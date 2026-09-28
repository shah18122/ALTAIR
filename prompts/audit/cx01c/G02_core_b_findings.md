# CX-01 G02_core_b — findings

Reader: claude-subagent:G02_core_b · Date: 2026-09-14 · Baseline HEAD: a34af5c5c9fa8c7498e8cc4df00907548a41e1a5

## 1 Scope & coverage

20 assigned files, 4,194 lines, **all fully read** with the Read tool in chunks of 300 lines or fewer. The sha256 of every file matched the partition before and after reading. HEAD equals the baseline. Nothing was built or run.

| Module | Files |
|---|---|
| core/config | CMakeLists.txt (47), config.hpp (314), store.hpp (79), toml_source.hpp (67), toml_source.cpp (209), tests/test_config.cpp (432), tests/test_toml_source.cpp (266) |
| core/invariant | CMakeLists.txt (16), conservation.hpp (315), tests/test_conservation.cpp (329) |
| core/lockfree | CMakeLists.txt (21), spsc_ring.hpp (160), mpsc_ring.hpp (139), seqlock.hpp (107), tests/test_spsc_ring.cpp (257), tests/test_mpsc_seqlock.cpp (403) |
| core/log | CMakeLists.txt (21), binlog.hpp (294), decoder.hpp (264), tests/test_binlog.cpp (454) |

I also read context outside the group, which is **not** claimed as coverage:
- app/main.cpp 1-220 and 355-464
- core/types/units.hpp 100-149 and 240-279
- root CMakeLists.txt 70-149
- app/warm_restart.hpp 140-169
- baseline_logs/build_default.log
- grep hits for callers across the engine directories

## 2 Findings table

| ID | Sev | Class | File:line | Title |
|---|---|---|---|---|
| C02-001 | P2 | DESIGN GAP | core/invariant/conservation.hpp:182-187, 207-208, 222-223 | The conservation check is true by construction and cannot trip on any accounting error; a refused fill is not latched |
| C02-002 | P2 | CONFIRMED DEFECT (language-level UB); impact HYPOTHESIS | core/lockfree/seqlock.hpp:52, 71 | Seqlock payload is written and read concurrently as a plain object, which is a data race (E003) |
| C02-003 | P3 | HYPOTHESIS | seqlock.hpp:87-92; core/config/store.hpp:57-59 | Unbounded busy-spin with no pause, yield or deadline on an ALTAIR_HOT path |
| C02-004 | P2 | CONFIRMED DEFECT | core/config/store.hpp:37-44, 53 | publish() does not enforce a rising version, so readers silently keep stale or empty config |
| C02-005 | P2 | DESIGN GAP | core/config/config.hpp:75-92, 261-266 | ConfigHandle is a bare index; after a refresh it silently reads a different key |
| C02-006 | P2 | DESIGN GAP | 5 test files, 10 sites (see detail) | Latency budgets print OVER and never fail ctest (E005, extended) |
| C02-007 | P3 | CONFIRMED DEFECT (latent, tests-only) | core/lockfree/mpsc_ring.hpp:74-87, 94-103 | A producer stalled between reservation and publish blocks the consumer and fills the ring; the "lock-free" claim is overstated (E004) |
| C02-008 | P3 | CONFIRMED DEFECT | core/config/toml_source.cpp:24-41, 51-82; config.hpp:281-286 | Distinct TOML keys can flatten to the same key and silently overwrite; every set error is reported as SnapshotFull |
| C02-009 | P3 | CONFIRMED DEFECT | toml_source.cpp:101-109, 166-168, 189-206 | Loader leaves a partial load behind on error; a short file read is parsed as success |
| C02-010 | P3 | DESIGN GAP / latent defect | core/log/decoder.hpp:149-151, 213-215, 33-38; binlog.hpp:135-138, 248-254 | Decoder trusts arg_count (out-of-bounds read); text truncation is silent; log drops and invalid site ids are never shown in the binary |
| C02-011 | P3 | CONFIRMED DEFECT | conservation.hpp:23-25; CMakeLists.txt:71-73 | The ALTAIR_STRICT_INVARIANTS `#error` guard cannot fire for the drift it claims to stop |

## 3 Finding details

### C02-001 — P2 DESIGN GAP — the conservation check cannot fail

**Evidence.**
- on_fill updates all three terms from the same inputs (conservation.hpp:182-187):
  ```
  fills_ += notional;
  costs_ += cost;
  cash_ -= outflow;
  ```
  where outflow = notional + cost (:172).
- on_cash_adjustment does `adjustments_ += amount; cash_ += amount;` (:207-208).
- check() tests `fills_ + costs_ + (cash_ - initial_cash_ - adjustments_) == 0` (:222-223).

**Why it cannot fail.** By induction from the starting state (F = K = A = 0, C = I), every public mutation leaves that expression at exactly 0. The only ways check() returns CashConservation are memory corruption, or signed overflow inside check() itself. The intermediate sums at :222-223 are not overflow-checked; that needs magnitudes above 9.2e18 paise, e.g. large adjustments, so it is unrealistic.

**What it cannot detect.** A missed fill, a double-reported fill, a wrong price or quantity, a wrong cost (for example STT on the wrong side), a sign error made by the caller, or any drift from broker cash. The caller feeds the same (qty, px, cost) into both sides of the identity.

**Refused fills are not latched either.** on_fill refuses Overflow and negative cost (:150-151, :156-180) and returns an error, but it does not trip. The only executable caller drops that error silently (app/main.cpp:141-149, outside this group):
```
if (fill.has_value()) {
    ++r.fills;
```
There is no else branch. A fill the ledger refused is therefore absent from the books, and the next check_and_trip() still passes.

**Trigger.** A broker fill of 11 units is reported to the ledger as 10 units (or twice, or with a cost of 0). check_and_trip() passes on every tick.

**Impact.** The kill switch that CLAUDE.md describes ("A breach trips the kill switch") is unreachable from real accounting errors. The latch works, but only via a manual trip().

**Coverage.** test_conservation.cpp never produces CashConservation from check():
- :245 is a manual trip()
- :58-61 is the negative-cost return value
- the 10k-operation test (:142-201) passes by construction

**Regression test.**
1. Keep an independent cash expectation built from an external fill log (the OMS or broker record).
2. Inject one fill into the ledger with qty off by 1.
3. Require that the reconciliation trips.
4. Separately, require that an on_fill result of Overflow or negative cost either latches or is escalated by the caller.

**Severity.** P2, because no live order path uses this ledger today. It becomes P1 if any go-live gate counts check_and_trip as the conservation kill switch.

### C02-002 — P2 CONFIRMED DEFECT (UB per [intro.races]), impact HYPOTHESIS — seqlock data race

**Evidence.** The writer does `value_ = v;` (seqlock.hpp:52) while a reader does `out = value_;` (:71). value_ is a plain T (:104). Comparing s0 == s1 after the copy (:78) throws away a torn result, but it does not remove the concurrent non-atomic access, which is undefined behaviour in C++.

**Trigger.** One thread calls store() while another calls try_load() on the same SeqlockSnapshot.

**Where it happens today.**
- Only in tests: test_mpsc_seqlock.cpp:224-289, and test_config.cpp:270-330 through ConfigStore.
- SeqlockSnapshot's only non-test user is ConfigStore (store.hpp:74).
- ConfigStore's only executable user is app/main.cpp:91-95, which publishes and refreshes on the same thread, once.
- No cross-thread writer/reader pair exists in any executable.

**Impact.** No miscompilation is known on MSVC 19.51. The compiler is nonetheless entitled to assume value_ is stable during the copy. TSAN cannot run on this toolchain: the tsan preset stops with FATAL_ERROR on MSVC (CMakeLists.txt:117-136). The suite has therefore never been race-checked, and under GCC/Clang TSAN both tests would be expected to report this race.

**Reproducer.** Build `mpsc_seqlock` and `config` with `-fsanitize=thread` on Linux. Expect a race report on seqlock.hpp:52 against :71.

**Fix direction.** Choose one:
- a payload of per-word atomics
- an atomic pointer swap between preallocated immutable snapshots
- a documented, sanitizer-annotated byte copy

**Severity note.** Codex rated this P1. I recommend P2 until a cross-thread hot-reload caller exists.

### C02-003 — P3 HYPOTHESIS — unbounded spin on the hot path

**Evidence.**
- `while (!try_load(out)) { // spin }` at seqlock.hpp:89-91; the comment at :84-85 says the missing cap is deliberate.
- store.hpp:57-59 repeats the loop inside `ALTAIR_HOT bool refresh`.
- Neither loop has `_mm_pause`, a yield, a deadline, or a counter.

**Trigger.** The writer is preempted between `seq_.store(s + 1)` (:47) and `seq_.store(s + 2)` (:56). This window covers a copy of a ~9.2 KB ConfigSnapshot. Meanwhile a reader calls refresh().

**Impact.** The reader burns 100% CPU for at least a scheduler quantum (roughly 15-30 ms on a default Windows desktop). If the reader shares a core with the writer at equal or higher priority, it can delay the writer indefinitely.

**Where it happens today.** Nowhere: main.cpp publishes before refreshing on the same thread, so the loop never iterates. SeqlockSnapshot::load() has no non-test caller.

**Coverage.** The test_config reader yields only when refresh() returns false (:295-305). The internal spin is neither bounded nor measured.

**Reproducer.**
1. Pin the writer and reader to one core, with the writer at lower priority.
2. Publish in a loop from the writer.
3. Record the reader's maximum refresh() latency and spin count.
4. Assert a bound, or require an explicit refusal once N attempts or D ns are exceeded.

### C02-004 — P2 CONFIRMED DEFECT — publish() does not enforce version monotonicity

**Evidence.**
- The rising-version rule is a precondition in the comment only (store.hpp:37-38).
- publish() stores the snapshot and its version unconditionally (:40-43).
- refresh() decides solely on `version_ ... == local.version()` (:53). content_hash is never compared.

**Trigger A.** publish(A, version 1). The reader refreshes and gets A. Then publish(B, version 1), with different content. refresh() returns false forever and the reader keeps A.

**Trigger B.** On a fresh store, a loader forgets set_version, so the snapshot has version 0. The reader's default local copy also has version 0, so refresh() returns false and the reader runs on an empty snapshot.

**Impact.** Stale or empty config is used as if current, which breaks rule 9 (fail loud). In app/main.cpp:100-105 (outside this group) lookups use `value_or(...)` defaults, which would mask trigger B completely.

**Coverage.** test_config.cpp:208-242 only publishes rising versions (1 then 2).

**Regression test.** Publish the same version with a different content_hash, then a version lower than the current one. Expect publish to refuse with an error (or a counted rejection), and expect refresh to never report "unchanged" while the content differs.

### C02-005 — P2 DESIGN GAP — stale ConfigHandle reads the wrong key

**Evidence.**
- ConfigHandle holds only `std::uint32_t idx_` (config.hpp:91).
- `at()` checks only `h.index() >= count_` (:262).
- The rule "A handle is only valid for the snapshot that produced it" (:73) is not enforced anywhere.
- store.hpp's refresh() returns true when it replaces the copy, but nothing invalidates handles already resolved.

**Trigger.**
1. Version 1 holds keys {"a", "risk.max_lots"}; `h = find("risk.max_lots")` gives index 1.
2. Version 2 is built as {"risk.max_loss_paise", "risk.max_lots"}. A merge, or a load that iterates keys in a different order, produces this.
3. After refresh(local), `get_int(h)` returns the value of risk.max_loss_paise with `has_value() == true`.

**Impact.** A strategy reads someone else's limit with no signal. This is exactly the collision class that the comment at :54-57 says the design prevents.

**Where it happens today.** Latent: main.cpp resolves its handles once and never reloads.

**Coverage.** None.

**Regression test.** The trigger above, expecting NotFound or a stale-handle error. **Fix direction.** Store key_hash (or a snapshot generation) in the handle and verify it in at(); that is one extra compare.

### C02-006 — P2 DESIGN GAP — latency budgets never fail (E005 confirmed and extended)

**Evidence.** Each of these prints `ns < budget ? "OK" : "OVER"` and never increments `failures`:

| File | Lines |
|---|---|
| test_spsc_ring.cpp | 224-225 |
| test_mpsc_seqlock.cpp | 345-346, 360-361, 373-374 |
| test_config.cpp | 380-381, 402-403 |
| test_conservation.cpp | 290-291, 300-301 |
| test_binlog.cpp | 410-411 |

- Exit status comes only from `failures` (for example test_binlog.cpp:453).
- No FAIL_REGULAR_EXPRESSION or PASS_REGULAR_EXPRESSION exists in any core CMakeLists. The only set_tests_properties in the tree are models/CMakeLists.txt:164 and desktop/CMakeLists.txt:637.
- No FAIL/OVER scan exists in build.bat, CMakePresets.json or any *.ps1 found.

**Impact.** Gate 6 (latency regression fails) is not enforced by ctest for these modules. A green ctest says nothing about latency.

**Reproducer.** Make any budgeted operation slower than its budget (for example, add a sleep of a few ns) and ctest still passes.

**Fix direction.** Make budget misses fail in a dedicated benchmark target, with noise handling such as best-of-N, rather than silently in the unit test.

### C02-007 — P3 CONFIRMED DEFECT (latent) — MPSC reservation stall

**Evidence.**
- A producer claims a slot with `tail_.compare_exchange_weak(pos, pos + 1, ...)` (mpsc_ring.hpp:74), then writes the data (:85), then publishes with `slot->seq.store(pos + 1, ...)` (:87).
- The consumer returns "empty" when the slot at head has not been published: `if (dif != 0) return false;` (:101-102).
- A producer returns "full" when `dif < 0` (:78-79).

**Trigger.**
1. Producer A claims position p and is preempted before :87.
2. Producer B pushes p+1 through p+Capacity-1, and all of those publish.
3. The consumer's try_pop at head p returns false even though Capacity-1 records are ready.
4. The next push at p+Capacity sees slot p's seq == p, so dif < 0, and every producer is told "full".

The ring is wedged until A is scheduled again.

**Impact.**
- The comment at :59 ("Lock-free but not wait-free") overstates it: one stalled thread blocks everyone.
- try_pop cannot tell "empty" apart from "blocked".
- An unbounded CAS retry on its own (:74-82) is ordinary lock-free behaviour, since another producer made progress, and is not a defect.
- MpscRing is **not used outside tests**. The header (:5-6) intends it for strategy-to-OMS orders, where this would become P1.

**Coverage.** The 4-producer stress test (test_mpsc_seqlock.cpp:90-156) cannot provoke this deterministically.

**Reproducer.**
1. Add a test-only friend hook that performs the CAS without publishing.
2. Push Capacity-1 items from a second producer.
3. Assert `!try_pop` and `!try_push`.
4. Publish through the hook, then assert all Capacity items drain in order.

### C02-008 — P3 CONFIRMED DEFECT — TOML flattened-key collision and misreported errors

**Evidence.**
- join_key concatenates `prefix + '.' + piece` (toml_source.cpp:31-39), with no escaping of a '.' or NUL inside a quoted TOML key.
- set_bits silently overwrites an existing key (config.hpp:281-286).
- put_scalar counts both writes as `loaded` (:61, :69, :80).
- put_scalar maps every set_* error to SnapshotFull (:57-59, :65-67, :76-78).

**Trigger.** This document:
```
[feed]
"kite.ws_mode" = 1
[feed.kite]
ws_mode = 2
```
It is valid TOML. Both values flatten to `feed.kite.ws_mode`; one is lost, and the loader reports success with `loaded == 2`. Which one wins depends on toml++'s table iteration order, which I did not verify. `"" = 1` returns SnapshotFull instead of an empty-key error.

A possible second trigger, unverified: a quoted key containing an escaped ` `. fnv1a64 and strcmp stop at the NUL, so it would collide with the prefix key.

**Where it happens today.** config/*.toml currently contains no dotted quoted keys and no nan/inf values (grep). Separately, NaN and inf floats would be accepted silently by set_real.

**Coverage.** test_toml_source.cpp:53-74 checks that tables do not collapse into each other, but has no collision case.

**Regression test.** The document above must return an error such as DuplicateKey. `"" = 1` must not be reported as SnapshotFull.

### C02-009 — P3 CONFIRMED DEFECT — partial load on error; short read accepted

**Evidence, partial load.** walk() writes into `out` as it iterates (toml_source.cpp:101-109). A later KeyTooLong or SnapshotFull returns an error (:166-168) with earlier keys already stored. toml_source.hpp:58-59 does not document this non-atomicity. The test at test_toml_source.cpp:177-183 claims "nothing partial was stored", but only because the failing key is the document's only key.

**Evidence, short read.**
- The fseek return values are ignored (:189, :191).
- `got` from fread (:204) is never compared with `n`, and ferror is never checked.
- load_toml(buf.data(), got, out) is then called (:206).

**Trigger, partial load.** `a = 1` followed by a table whose flattened key exceeds 47 characters. The result is KeyTooLong with `a` already stored, if toml++ iterates `a` first.

**Trigger, short read.** The file shrinks between ftell and fread, for example an editor saving during a reload. A prefix that ends on a line boundary is valid TOML, so the call returns success with keys missing.

**Where it happens today.** The loader has no executable caller (tests only). It will matter once hot reload loads into a live snapshot.

**Regression test.** A two-key document with the second key too long must leave `out` unchanged. For the short read, stub the read path (or use a file truncated after the size is measured) and expect an I/O error.

### C02-010 — P3 DESIGN GAP / latent defect — log path bounds

**Evidence.**
- decode_message and decode_line index `r.arg_tags[next]` and `r.args[next]` for every `next < r.arg_count` (decoder.hpp:149-151, :213-215), with no clamp to kMaxLogArgs = 5. A record with arg_count 6 or more reads past both arrays, and past the 64-byte record for args[5] and beyond.
- Records produced by Logger are always 5 or fewer (binlog.hpp:169-172, :242). LogRecord is never persisted: it appears only in core/log and app/main.cpp. That makes this latent until records come from a file or shared memory.
- LogTextWriter::put silently drops characters beyond capacity (:33-38), and finish() gives no truncation indication. That is a fixed buffer truncating invisibly, against rule 11; app/main.cpp uses `logbuf[192]`.
- Logger::write accepts site_id == kInvalidSite, which register_site returns when the table is full (binlog.hpp:135-138). The decoder then returns 0, and main.cpp:163 counts only results above 0, so the record vanishes uncounted.
- Logger::dropped() (:280) is never printed by app/main.cpp (grep).

**Impact.** Log loss and truncation are not visible on screen in the only executable.

**Regression test.** Decode a record with arg_count = 200 and require either a refusal or exactly 5 arguments rendered, checked under ASAN. Also require that decode returns a truncation flag when the formatted line exceeds the buffer.

### C02-011 — P3 CONFIRMED DEFECT — ineffective STRICT_INVARIANTS guard

**Evidence.** The guard at conservation.hpp:23-25 is:
```
#if defined(NDEBUG) && defined(ALTAIR_STRICT_INVARIANTS) \
    && (ALTAIR_STRICT_INVARIANTS == 0) \
```
CMakeLists.txt:71-73 defines `ALTAIR_STRICT_INVARIANTS=1` only when the option is ON. With the option OFF the macro is **undefined**, so the guard never fires.

**Impact.** The comment at :14 says "this guard stops it drifting back", which is false. Functional impact is nil today because no code reads the macro (grep finds only CMake, the presets and this guard).

**Regression.** Add a CMake FATAL_ERROR when the `prod` preset has the option OFF, or define the macro as 0 when OFF.

## 4 Lead revalidation

| Lead | Verdict | Evidence |
|---|---|---|
| **E003** seqlock race and unbounded load() | **CONFIRMED** (race is UB, load() is unbounded). Severity **PARTIAL**: no cross-thread use in any executable | Race: seqlock.hpp:52 vs :71 (C02-002). Unbounded retry: :87-92 (C02-003). Users: SeqlockSnapshot only in store.hpp:74; ConfigStore only in app/main.cpp:91-95 (same thread, one publish, one refresh); load() only in tests. Cross-thread use exists only in test_mpsc_seqlock.cpp and test_config.cpp. TSAN has never run on this toolchain (CMakeLists.txt:117-136). Recommend P2, not P1 |
| **E004** MPSC stalled producer, CAS retry, progress claim | **PARTIAL** | Stall CONFIRMED (mpsc_ring.hpp:74-87, :101-102, :78-79), including the wedge where producers see full and the consumer sees empty. "Lock-free" claim at :59 overstated: CONFIRMED. Unbounded CAS retry: NOT a defect on its own. No executable uses MpscRing (C02-007) |
| **E005** OVER printed without failing | **CONFIRMED and extended** | Also in test_config.cpp, test_conservation.cpp, test_binlog.cpp and two more sites in test_mpsc_seqlock.cpp. No regex or script gate found (C02-006) |
| store.hpp refresh spin | **CONFIRMED** that it is unbounded; impact HYPOTHESIS | store.hpp:57-59 (C02-003) |
| store.hpp publication lifetime | **NOT CONFIRMED as a defect** | Snapshots are copied by value into seqlock storage; the old config is overwritten in place; readers hold private copies and retry during a swap. No pointers, so no use-after-free. Real hazards are C02-004 (version) and C02-005 (handles) |
| toml_source malformed input | **PARTIAL** | Parse errors return ParseFailed before any mutation (toml_source.cpp:155-162, tested). Collisions, partial loads and short reads are defects (C02-008, C02-009) |
| Config hash determinism | **NOT CONFIRMED as a defect** | Order-independent wrapping sum of splitmix-mixed (key_hash, bits, type) (config.hpp:237-249), tested at test_config.cpp:153-194; version excluded by design. Snapshot entry order depends on toml++ iteration, which the hash tolerates but handles do not (C02-005) |
| Config test WORKING_DIRECTORY | **CONFIRMED that it reads the source tree** | core/config/CMakeLists.txt:44-45 sets `${CMAKE_SOURCE_DIR}`, so test_toml_source.cpp:217 reads the live config/altair.toml; fallback at :220 is `../../config/altair.toml`. The test is registered only when tomlplusplus is found (vcpkg preset); the `default` preset has no toolchain file, so it is likely not run there (unverified) |
| binlog capacity, overflow and ownership | **Facts verified** | Capacity is a template parameter; the app uses 4096 (main.cpp:42). Overflow policy: refuse the newest record, count attempts in dropped_, mark the gap in the stream with dropped_before (saturates at 65535) (binlog.hpp:248-254). Decode output is bounded; input arg_count is trusted (C02-010). No decoder thread exists: main.cpp decodes inside the tick loop (:160-179) |
| conservation.hpp exactness, overflow, breach, callers | **PARTIAL** | Exactness holds, but tautologically (C02-001). Overflow is checked in on_fill, on_cash_adjustment and equity; check() intermediates are unchecked (unrealistic magnitudes). Breach: check_and_trip latches, and while latched on_fill/on_cash_adjustment refuse; on_fill errors do not latch. Callers outside tests: app/main.cpp only. feed/replay.hpp:20 includes it without use; app/warm_restart.hpp:147-153 re-derives the same identity from snapshot fields. No oms, risk or desktop caller |

## 5 Architecture facts

**Ownership.**
- core/lockfree provides the transport primitives.
- core/log provides binary logging on top of SpscRing.
- core/config provides the snapshot, a store built on the seqlock, and a TOML loader.
- core/invariant provides the per-instrument cash ledger.
- Everything is header-only except altair_config_toml, which is a static library.

**Threads.** No module in this group creates a thread. In the only executable (app/main.cpp run_session) these all run on **one thread**:
- ConfigStore publish and refresh
- Logger producer, SpscRing and LogDecoder
- the ledger

The multi-thread designs (SPSC feed-to-strategy, MPSC strategies-to-OMS, seqlock hot reload, off-thread decoder) are implemented but **not connected** anywhere.

**Queues, capacities and overflow policies.**

| Component | Capacity | Overflow policy |
|---|---|---|
| SpscRing | power of two, all slots usable | refuses (returns false); never overwrites |
| MpscRing | power of two | refuses on "full"; can wedge (C02-007) |
| Log ring in app | 4096 | drop newest and count; gap marked, saturating at 65535 |
| LogRegistry | 1024 sites | returns kInvalidSite; Logger does not check it |
| ConfigSnapshot | 128 entries, 47-char keys | refuses: Full / KeyTooLong |
| TOML file | 1 MiB | FileTooLarge |
| TOML strings, dates, arrays of tables | — | skipped and counted, first key named |

**Hot vs cold.**
- ALTAIR_HOT: SpscRing and MpscRing push/pop, seqlock store/try_load/load, ConfigStore::refresh, ConfigSnapshot find/get_*, Logger::write, ledger on_fill/check/check_and_trip.
- Warm: the decoder.
- Cold: the TOML loader.
- No allocation or exception on the hot paths. The TOML loader allocates (std::string, toml::table) and catches exceptions internally.

**Conventions.**
- Money: Notional and Price are int64 paise.
- Quantity: Qty units, signed (+ buy, - sell).
- Costs: always a debit; a negative cost is refused.
- Notional arithmetic: `notional_of` is overflow-checked (units.hpp:252-258), but StrongInt `+` and `-` wrap unchecked (units.hpp:125-130).
- Log time: raw TSC ticks, converted in the decoder; records stamped before the clock anchor print 0.
- Config: int64, double and bool only; money must be written as integer paise in the TOML.
- Instrument identity: none in this group; the ledger is single-instrument by contract.

**Implemented vs connected.**
- MpscRing and the TOML loader: tests only.
- SeqlockSnapshot: only via ConfigStore, on one thread.
- ConservationLedger: only in the Phase 0 replay/selftest binary path.
- Binary logger: only in app/main.cpp; no other engine module logs through it.

## 6 Test-suite observations

- Every suite is a plain main() with a failure counter and a correct exit code. There are no skip paths that still pass, apart from the fallback path in test_toml_source.cpp:218-234, which silently skips the FileNotFound check.
- Throughput and budget lines never fail (C02-006). test_mpsc_seqlock.cpp:319-322 prints a zero-retry note rather than asserting it.
- The concurrent tests were deliberately rebuilt to stop on evidence rather than a timer (test_mpsc_seqlock.cpp:190-204; test_config.cpp:246-250). They prove algorithmic consistency on x86/MSVC only. No race detector has ever run: tsan is refused on MSVC, and the asan preset does not detect races.
- Missing negative tests:
  - an equal or zero version publish (C02-004)
  - a stale handle after refresh (C02-005)
  - a stalled MPSC producer (C02-007)
  - a flattened-key collision, and a partial load with more than one key (C02-008, C02-009)
  - arg_count > 5, and an invalid site id (C02-010)
  - any reachable CashConservation breach (C02-001)
- test_toml_source is only built when tomlplusplus is found, so the `default` preset ctest likely omits it.

## 7 Open questions

1. Does oms/reconcile.hpp (outside this group) reconcile ledger cash against an independent broker figure? If yes, C02-001 is partly mitigated; if no, conservation has no independent check anywhere.
2. `std::atomic_thread_fence` (seqlock.hpp:50, 55, 73) is not a function I know from the standard library. The baseline build log shows the default preset up to date with exit code 0, which implies it compiles on MSVC 19.51, but I did not inspect the STL. Please confirm portability to GCC/Clang, which the required TSAN re-gate needs.
3. What is toml++'s table iteration order? It decides which value wins in C02-008 and whether the C02-009 partial-load trigger works as written.
4. test_toml_source.cpp:5 says "No check description here may contain the substring FAIL", which implies some external output scan. None was found in build.bat, the presets, the CMake files or *.ps1. Which gate is that referring to?
5. Is `toml_source` run by any gate that uses the vcpkg preset?
