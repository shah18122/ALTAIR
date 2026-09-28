# CX-01 Phase 1 — G01_core_a_build findings

Reader: claude-subagent:G01_core_a_build · Date: 2026-09-14 · Baseline HEAD a34af5c5c9fa8c7498e8cc4df00907548a41e1a5
Audit only. No build, no test run, no executable run. "CONFIRMED" below means confirmed by reading the code plus hand-derived arithmetic. Nothing was executed.

## 1. Scope & coverage

- Assigned: 34 files, 5,217 lines (partition line counts).
- Fully read: 34/34 files, every line, in chunks of at most 300 lines (ranges are in the coverage JSON). All `sha256_before` values match the partition inventory.
- Partial or unread: none.
- Outside my files, I used only targeted greps to establish callers (app/main.cpp:95-135 via sed; feed/normaliser.hpp and risk/cost.hpp via grep hits). I did not review those files.

## 2. Findings table

| ID | Sev | Class | File:line | Summary |
|---|---|---|---|---|
| C01-001 | P2 | CONFIRMED DEFECT (latent) | core/types/units.hpp:350-372 | `round_to_tick` can hit signed overflow (UB) in `quot * t` at the int64 extremes instead of returning `Overflow` |
| C01-002 | P3 | CONFIRMED DEFECT (latent, MSVC) | core/types/units.hpp:322-328 | `apply_bps` range guard rounds to 2^63 when long double is 64-bit, so the cast of exactly 2^63 is UB |
| C01-003 | P3 | CONFIRMED DEFECT (derived, latent) | core/types/units.hpp:407-421, 429-443 | `price_from_rupees` / `notional_from_rupees` reject valid paise values at or above ~Rs 1.31 lakh (a 1e-9 tolerance is smaller than one ulp) |
| C01-004 | P2 | CONFIRMED DEFECT (latent) | core/mem/pool.hpp:105-118; implicit copy | Releasing an owned block twice makes the free list self-loop, so the same block is handed out twice, and `in_use_` underflows. Pool and Arena are copyable, which aliases the allocator |
| C01-005 | P3 | CONFIRMED (test gap) | core/mem/tests/test_arena_pool.cpp:307-308, 328-329 | Latency budgets only print OK/OVER and never fail; latency_gate is not used |
| C01-006 | P2 | DESIGN GAP | core/time/exchange_ts.hpp:199-228 | PlausibilityGate has no staleness bound, so a tick far behind `local_now` is admitted as fresh |
| C01-007 | P3 | CONFIRMED DEFECT | core/time/exchange_ts.hpp:192, 209, 215-226 | Unchecked `local_now + max_future_skew`, no Config validation; one future tick poisons the monotonic watermark |
| C01-008 | P3 | HYPOTHESIS | core/time/tsc_clock.cpp:229-287, 323-326 | TscClock anchors once and never re-anchors; accepted calibration error up to 0.1% can mean seconds of drift in `now()` over a session |
| C01-009 | P3 | CONFIRMED DEFECT | core/time/tsc_clock.cpp:296, 328-339 | The fallback clock stores a tick quantum as a ratio stderr, so `uncertainty_of` reports 100% relative error |
| C01-010 | P3 | HYPOTHESIS | core/time/tsc_clock.cpp:316-326 | A backwards TSC delta wraps unsigned, giving a ~195-year Duration with no guard |
| C01-011 | P3 | DESIGN GAP | CMakeLists.txt:47-69, 93-115; CMakePresets.json:49-57 | Warnings are not errors (gate 1 not enforced); the "Address/UB" preset has no UB checks on MSVC |
| C01-012 | P3 | DESIGN GAP | CMakeLists.txt:51-52 | `/GS-` turns off stack cookies in the shipped RelWithDebInfo/Release binary |
| C01-013 | P3 | DESIGN GAP | .gitignore:41-46, 99-129 | `data/` is protected file by file, not as a directory; new operational or credential files there are committable |
| C01-014 | P3 | DESIGN GAP | cmake/BoundsAudit.cmake:67-68, 124-147; NamespaceAudit.cmake:46-64; AtlasAudit.cmake:25-120 | The configure-time audits catch far less than a "clean" message suggests |
| C01-015 | P3 | DESIGN GAP / CONFIRMED | core/types/units.hpp:112-138, 262-270 | Same-dimension StrongInt arithmetic is unchecked (UB); `qty_of` accepts LotSize 0 but `lots_of` refuses it |

## 3. Finding details

### C01-001 — round_to_tick overflow (P2, CONFIRMED DEFECT, latent)
- Evidence: units.hpp:357-359 `} else if (mode == RoundMode::Up) { if (rem > 0) { ++quot; }` then :372 `return Price{quot * t};` There is no `mul_overflows` check, although the function returns `std::expected<Price, ArithError>`.
- Trigger: `round_to_tick(Price::max(), Price{2}, Up)`: quot = 2^62-1, rem = 1, ++quot gives 2^62, and 2^62·2 = 2^63 overflows. `Nearest` with the same input hits the same path (abs_rem 1 >= t-abs_rem 1). `round_to_tick(Price::min(), Price{3}, Down)`: quot = -3074457345618258602, rem = -2, --quot, and ·3 = -2^63-1 underflows.
- Impact: UB; in practice it wraps to INT64_MIN, a price with the opposite sign. It is not reachable from real market prices, but it is reachable from sentinel values (`Price::max()` used as "no quote"). No caller outside core/types/tests (grep over all engine directories). If an order-price path adopts it, severity rises.
- Tests: test_units.cpp:204-222 covers only ±10007/5. There are no extrema tests.
- Regression test: for every mode, `round_to_tick(Price::max(), Price{2}, m)` and `round_to_tick(Price::min(), Price{3}, m)` should return either a value that is on tick and within range, or `ArithError::Overflow`. Run under clang UBSan.

### C01-002 — apply_bps bound on MSVC (P3, CONFIRMED DEFECT, latent)
- Evidence: units.hpp:323 `const auto mx = static_cast<long double>(std::numeric_limits<std::int64_t>::max());` :324 `if (shifted < mn || shifted > mx)` :328 `static_cast<std::int64_t>(shifted)`. On MSVC, long double is IEEE double, so `mx` is 2^63 and `shifted == 2^63` passes the guard.
- Trigger (derived): `apply_bps(Notional::max(), Bps{10000.0})`. `n` becomes 2^63; 2^63·10000 = 2^67·625 is exact; ·1e-4 rounds to 2^63; +0.5 is still 2^63; the check `> mx` is false; the cast is UB (cvttsd2si returns INT64_MIN).
- Impact: magnitudes of 9.2e16 rupees are unreachable in real money. risk/cost.hpp:9-12 says the cost calculator replaced `apply_bps` with scaled-integer rates. apply_bps has no non-test caller (grep). This is also the carried debt recorded in CLAUDE.md.
- Regression test: `apply_bps(Notional::max(), Bps{10000})` should return `Overflow`. Use the exclusive bound `shifted >= 9223372036854775808.0L` as the check.

### C01-003 — rupee conversion refuses valid paise (P3, CONFIRMED by IEEE-754 derivation, latent)
- Evidence: units.hpp:407 `const double scaled = rupees * 100.0;` :418 `if (detail::abs_d(scaled - rounded) > 1e-9) {` (the same at :440).
- Mechanism: for rupees in [2^k, 1.28·2^k), `scaled` has exponent k+6. The representation error of `rupees` (up to 2^(k-53)) times 100 can exceed half an ulp of `scaled`, so `scaled` lands one ulp (2^(k-46)) off the integer. For k >= 17 that ulp exceeds 1e-9, and the value is refused.
- Worked example: 131072.05 is stored as 131072.05 - 0.4·2^-35. ×100 = 13107205 - 40·2^-35, which rounds to 13107205 - 64·2^-35 (distance 24 versus 40). `rounded` = 13107205, and |diff| = 1.86e-9 > 1e-9, so the call returns NotRepresentable.
- Affected bands: Rs [131072, 167772.16), [262144, 335544.32), [524288, 671088.64), [1048576, 1342177.28), ... NIFTY-lot notionals (~Rs 18.75 lakh) fall in the k=20/21 bands.
- Impact: this is a loud refusal, not a wrong value, but valid prices and notionals are rejected. No non-test caller (grep). The static_asserts in test_units.cpp:81-86 only use 25000.50 and -100.25.
- Regression test: `static_assert(price_from_rupees(131072.05).value() == Price{13107205});` and `notional_from_rupees(1100000.05)` should have a value. Compare against a tolerance scaled to the ulp of `scaled` (or < 0.5 paise), or parse decimal text instead.

### C01-004 — Pool double release and copyable allocators (P2, CONFIRMED DEFECT, latent)
- Evidence: pool.hpp:109-117 `if (!owns(p)) { ++foreign_; return; }` then `*reinterpret_cast<void**>(p) = free_; free_ = p; --in_use_;`. `owns()` (:122-136) checks only range and alignment, not whether the block is currently allocated.
- Trigger: `a = acquire(); release(a); release(a);` gives `*a = a` (a self-loop) and `in_use_` = SIZE_MAX. The next two `acquire()` calls both return `a`: two owners of one block. Releasing a block that was never acquired pushes a duplicate node the same way.
- Copying: `Pool` and `Arena` declare no copy control, so they are implicitly copyable. `Pool q = *p;` gives two free lists threading the same memory, and they hand out the same blocks. `std::expected<Pool>` moves are copies of the raw pointers.
- Impact: silent memory aliasing or corruption. Pool has no user outside tests (grep: `mem/pool.hpp` is included only by test_arena_pool.cpp; `\bPool\b` has no engine hits). Arena is used by app/main.cpp:108-112 (selftest scratch memory).
- Tests: test_arena_pool.cpp:238-273 covers foreign and mid-block release, not double release or copying.
- Regression test: after acquire/release/release, expect `in_use()==0`, `foreign_releases()` (or a new `double_releases()`) == 1, and two acquires returning distinct pointers. Also `static_assert(!std::is_copy_constructible_v<Pool>)` (and Arena). Note that detecting double release needs an allocation bitmap or a debug tag, which is a design decision for the card.

### C01-005 — latency budgets that cannot fail (P3, CONFIRMED test gap) — Codex E005, mem part
- Evidence: test_arena_pool.cpp:307-308 `ns < 10.0 ? "OK" : "OVER"` and :328-329 `ns < 15.0 ? "OK" : "OVER"`. `failures` is never touched. test_tsc_clock.cpp:299-301 prints `"OVER BUDGET — review gate 6"` and is documented as deliberately unasserted at :242-243.
- latency_gate.hpp semantics: `latency_gate_active()` is true iff NDEBUG is defined. No CMakeLists in the repo sets or unsets NDEBUG (grep), so it follows CMake's defaults: `default` (RelWithDebInfo) and `prod` (Release) define NDEBUG and the gate is active; `debug`/`asan`/`tsan` (Debug) do not, and it prints SKIP with the ratio. The header is sound. The mem and tsc tests just do not use it, so ctest never fails gate 6 for Arena, Pool, or TscClock in any preset. `prod` builds no tests.
- Fix/test: in `default`, wrap these in `if (latency_gate_active()) check(ns < budget, ...) else latency_not_measured(...)`. Planting a `sleep` in the loop should fail `ctest --preset default`.

### C01-006 — no staleness bound in PlausibilityGate (P2, DESIGN GAP)
- Evidence: exchange_ts.hpp:201-223 checks, in order, `exchange_ts < cfg_.floor`, `exchange_ts > local_now + cfg_.max_future_skew`, `exchange_ts < last_` (if monotonic), and the session. Config (:164-182) has no maximum lag.
- Trigger: `PlausibilityGate g{{kFloor, 2s, true, false, {}}}; g.admit(kNow - hours(3), kNow)` is admitted on a fresh gate, or after a reconnect `reset()`, or with `require_monotonic=false`. The only lower bound is the absolute `floor` (e.g. 2020-01-01).
- Impact: stale data is admitted as fresh at the one gate the header calls "the single place a bad clock can poison everything downstream". It is connected: feed/normaliser.hpp:38,125,183 owns a PlausibilityGate. Whether the normaliser or strategies add their own staleness check is outside this group (see open question 1).
- Tests: test_exchange_ts.cpp has no stale-tick case.
- Regression test: add `max_past_lag` and `TsReject::Stale`. Then `admit(kNow - max_past_lag - 1ns, kNow)` should return `Stale`, and exactly `max_past_lag` should be admitted.

### C01-007 — ceiling overflow and watermark poisoning (P3, CONFIRMED DEFECT)
- Evidence: :209 `if (exchange_ts > local_now + cfg_.max_future_skew)` uses Timestamp `operator+`, which is unchecked (timestamp.hpp:78-81). The constructor at :192 accepts any Config.
- Trigger: `max_future_skew = Duration::max()` (a natural way to say "no ceiling") with `local_now` around 1.79e18 overflows; with wraparound every tick is rejected as AfterCeiling. A negative skew rejects ticks that are already current.
- Separately, with `require_monotonic`, one tick stamped `local_now + skew` advances `last_` (:226), and every correctly stamped tick is then `Regressed` for up to `skew`.
- Impact: fails closed (lost ticks), no wrong acceptance. There is no test for skew extremes.
- Regression test: `Config{floor, Duration::max(), ...}` should be refused at construction (or the addition saturated), and `admit(kNow, kNow)` should be admitted.

### C01-008 — TscClock drifts from UTC over a session (P3, HYPOTHESIS)
- Evidence: tsc_clock.cpp:229-272 uses 5×10 ms windows against `steady_clock` and accepts `sd/mean <= 1e-3`. :284-285 anchors once. :325 `return cal_.anchor + ticks_to_duration(now_ticks() - cal_.anchor_ticks);`. There is no re-anchor API, and `cal_` is set only by `create()`.
- Reasoning: the ratio error at the acceptance edge is about sd/√5, i.e. about 4.5e-4 relative. Over a 22,500 s session that could be ~10 s of `now()` error. A healthy host is at the ppm level, which is still ~0.1 s per session plus oscillator drift against NTP-disciplined UTC. On Windows, steady_clock is QPC, which is itself usually TSC-derived, so the calibration measures read jitter rather than frequency error against UTC. `uncertainty_of()` covers only span-ratio error; nothing reports the uncertainty of `now()` against UTC.
- Connected: app/main.cpp:364,451 create a TscClock (with fallback).
- Impact: if `now()` feeds `PlausibilityGate::admit(local_now)`, drift beyond `max_future_skew` either rejects every live tick (clock slow) or widens the look-ahead window (clock fast). Unverified (open question 1).
- Test: test_tsc_clock.cpp:204-219 checks within 100 ms right after creation only.
- Reproducer: run a clock for 6 h and record `clk.now() - system_clock` every minute. Expected: |drift| bounded by a documented figure, or a periodic re-anchor.

### C01-009 — fallback clock uncertainty is 100% (P3, CONFIRMED DEFECT)
- Evidence: :294 `cal.ns_per_tick = 1.0;` :296 `cal.ns_per_tick_stderr = steady_tick_ns();` (1.0 ns when `steady_clock::period` is `nano`, as on MSVC STL and libstdc++); :332-337 `rel = stderr / ns_per_tick` gives 1.0 and `sigma = mag*rel + ns_per_tick`.
- Trigger: `TscClock::create_fallback().uncertainty_of(millis(200))` returns 200,000,001 ns (100%).
- Impact: on any fallback host (VM/CI) every span is reported as "error bar straddles zero". Analytics and logging only.
- Tests: test_tsc_fallback_clock (:221-238) checks only `stderr > 0`. The 1% check at :163 runs only against `clock_under_test`, which is the TSC clock on this box.
- Regression test: `create_fallback().uncertainty_of(millis(200)) < millis(2)`. The ratio stderr for fallback is 0 (ticks are ns); the quantum belongs in the additive term.

### C01-010 — backwards TSC delta wraps (P3, HYPOTHESIS)
- Evidence: :320 `ticks_to_duration(now_ticks() - t0_ticks)` and :325 do unsigned subtraction with no sign check. mul_shift32 of 2^64-k gives about ns_per_tick·2^64, which is ~6.1e18 ns (~195 y) at 3 GHz; below ~2 GHz it passes 2^63 and converts to a negative value.
- Trigger: `t0` read on a core whose TSC is ahead of the reading core (unsynchronised TSC_ADJUST, multi-socket, VM migration). The header (:119) promises monotonicity only "on a single core".
- Tests: the monotonic loops (:92-113) are single-threaded.
- Reproducer: `clk.ticks_to_duration(std::uint64_t{0} - 100)` gives ~6.1e18. Expected: a refused value (or a signed delta) so a 195-year latency sample cannot enter a histogram.

### C01-011 — build gates weaker than their names (P3, DESIGN GAP)
- No `/WX`, `-Werror` or `COMPILE_WARNING_AS_ERROR` in any CMakeLists (grep). CMakeLists.txt:48 sets `/W4` only, so review gate 1 ("zero warnings") is not enforced by the build.
- The `asan` preset's displayName (CMakePresets.json:50) is "Debug + Address/UB sanitizer", but CMakeLists.txt:100 adds only `/fsanitize=address` on MSVC, the only toolchain in use. Signed-overflow defects such as C01-001/002/015 are invisible to every preset on this box. The configure message at :107 is honest; the preset name is not.
- The TSan refusal is correct: :117-136 is a FATAL_ERROR on MSVC.

### C01-012 — /GS- in shipped configurations (P3, DESIGN GAP)
- CMakeLists.txt:51-52 `$<$<CONFIG:RelWithDebInfo>:/O2 /Oi /Ot /GS- /Zi>` / `$<$<CONFIG:Release>:... /GS->`. Stack buffer-overrun detection is off in `default` and `prod`, the binaries that parse broker/network input and hold live positions. It is a performance choice with no recorded rationale. Consider `/GS` for broker/feed/oms/server targets, or document the decision.

### C01-013 — .gitignore protects data/ file by file (P3, DESIGN GAP)
- Ignored: `.env`, `.env.*`, `config/secrets.toml`, `**/kite_session.json`, `**/*credentials*`, `**/*api_key*`, `**/kite_account.json`, `**/kite_quotes.json`, `**/kite_ticks.json`, `data/order_intents.jsonl`, `data/kill_request.json`, `data/live_forecast_scratch/`, `data/ticks|instruments|fundamentals|archive|masters/`, `data/instruments.csv`, `**/*.altick`.
- Every `"data/..."` literal found in broker/oms/desktop/app/feed/server code is covered, and `git ls-files data` is empty. But `data/` itself is not ignored, so a new session, token, fills or audit file written there by a future card is committable by `git add -A`.
- `git status` also shows untracked, un-ignored `GETSClient_5.0.191022_64bit_040924/`, `change_by_codex.txt` and `idea.txt`. Their contents were not inspected.
- Suggest: `data/*` with explicit `!` exceptions.

### C01-014 — configure-time audits: narrow recall (P3, DESIGN GAP)
- BoundsAudit detects three textual shapes only. It misses `if (x > kMax) x = kMax;`, `std::clamp`, and `std::min<T>(x, kMax)` (the regex is `std::min *\(`). It also misses `std::min(static_cast<int>(x), kMax)`, because `[^)]*` stops at the first `)` (:145). It skips `research/` (built, CMakeLists.txt:221), all `/tests/` and `/tools/` paths, and any text on a line starting with `//`. One comment containing `RULE 11` (any text, e.g. "RULE 11: TODO") exempts every clamp in the next 12 lines (:110-114).
- NamespaceAudit (:46-47) skips `desktop/` and `app/`, which are compiled in the same process with engine headers. It sees only column-zero `enum class|struct|class` and `inline constexpr k...`; plain `constexpr`, `using` aliases, plain `enum` and indented declarations are invisible. It cannot distinguish `altair::detail::X` from `altair::X` (false positives only).
- AtlasAudit checks only that a named path exists (:95), not that the model is in it. It runs as a top-level `include()` rather than a function, so `_atlas_text`, `_row`, `_path`, `_status`, `_bad`, `_rows` leak into every subdirectory scope.
- All three do fail loud when they detect something. The gap is that "clean" means much less than the headers' confidence suggests, and BoundsAudit's own header concedes it (:20-23).

### C01-015 — unchecked same-dimension arithmetic; qty_of accepts LotSize 0 (P3)
- units.hpp:112-138: `operator+/-/+=/-=`, `operator*(StrongInt, Rep)` and unary `-` compute `static_cast<Rep>(a.v_ + b.v_)` with no check (UB on overflow), while every cross-dimension operation returns `expected`. Session accumulators of `Notional` (e.g. `ConservationLedger ledger{Notional{cash0}}`, app/main.cpp:127) have no overflow detection. Real magnitudes are far below the limit, but `Price * k` with `k` taken from input is not protected.
- :262-270 `qty_of(Lots{5}, LotSize{0})` returns `Qty{0}` (CONFIRMED by reading: only `ls < 0` is refused), whereas `lots_of` returns DivideByZero for 0. A zero lot size from a bad spec silently becomes a zero-quantity order, against rule 9. Callers of `qty_of` were not surveyed.
- Regression test: `qty_of(Lots{5}, LotSize{0})` should return NotRepresentable.

## 4. Lead revalidation

| Lead | Verdict | Evidence |
|---|---|---|
| Codex E001 round_to_tick | **CONFIRMED** (also Down at Price::min()) — severity P2 not P1: no production caller | C01-001; grep shows callers only in core/types/tests |
| Codex E002 apply_bps long double | **CONFIRMED** on MSVC; trigger Notional::max()×10000 bps — severity P3: unreachable magnitude and superseded by risk/cost.hpp scaled integers | C01-002 |
| Codex E006 pool double release / copy | **CONFIRMED** (both parts; Arena is copyable too) — P2 latent: Pool has no non-test user | C01-004 |
| Codex E005 (mem part) | **CONFIRMED** for test_arena_pool.cpp:307/328; the same pattern in test_tsc_clock.cpp:299-301. latency_gate.hpp NDEBUG semantics are CORRECT for all presets (no CMake overrides NDEBUG); these two tests do not use it | C01-005 |
| core/time TSC calibration/drift | PARTIAL: stderr is propagated for spans; the fallback stderr is dimensionally wrong (C01-009); no now()-vs-UTC uncertainty or re-anchor (C01-008); wrap on backwards delta (C01-010) | |
| exchange_ts plausibility gate | PARTIAL: floor, ceiling, monotonic and session are correct and tested; no staleness bound (C01-006); unchecked ceiling add (C01-007). `to_utc` is overflow-safe and IST-naive correction subtracts (correct) | |
| IST handling | NOT a defect: constant +19800 s, floor-mod for pre-epoch, `ist_time_of_day` widens before multiplying; tested including compile-time. Preconditions (no validation of hh/mm/ss; t within 5h30 of max) are documented | timestamp.hpp:254-301 |
| Timestamp affine algebra | NOT a defect: Timestamp+Timestamp and Timestamp*scalar are static_assert'ed ill-formed (test_timestamp.cpp:48-59); arithmetic is unchecked by documented precondition | |
| TSan on MSVC | CONFIRMED refused (FATAL_ERROR) | CMakeLists.txt:117-136 |
| MSVC ASan | Real: /fsanitize=address, /RTC1 stripped, runtime DLL staged before every exe, FATAL if DLL missing. No UBSan (C01-011) | CMakeLists.txt:93-107; AsanRuntime.cmake |
| .gitignore secrets/state | PARTIAL: .env, kite_session.json, credentials/api_key, order_intents.jsonl and kill_request.json are protected; data/ is not protected wholesale (C01-013) | |

## 5. Architecture facts

- **Ownership:** core/types (header-only INTERFACE `altair_types`): dimensional units and exact paise money. core/time (STATIC `altair_time`): timestamp.hpp (affine time + IST), tsc_clock.cpp (clock), exchange_ts.hpp (per-source normalisation + gate). core/mem (STATIC `altair_mem`): PageBlock (OS pages), Arena, Pool. core/testing/latency_gate.hpp: test-only helper. core/cache_topology.hpp: cold probe. `altair_core` umbrella links whichever of types/time/mem/lockfree/log/config/invariant exist (core/CMakeLists.txt:24-29, EXISTS-guarded).
- **Hot vs cold:** ALTAIR_HOT expands to `[[gnu::hot]]` on GCC/Clang and to nothing on MSVC (units.hpp:10-14), so on this box it is a marker only. Hot: notional_of/qty_of/lots_of/price_of/apply_bps/round_to_tick, Arena::allocate, Pool::acquire/release, TscClock::now/now_ticks/elapsed_since, `to_utc`, `PlausibilityGate::admit`. None allocate or throw. Cold: TscClock::create (~50 ms busy-wait), PageBlock::reserve (syscalls), read_cache_topology (allocates a std::vector on Windows), page_size (magic static).
- **Threads / shared state:** TscClock is immutable after construction and safe to share. `features()` is a magic static. `g_use_rdtscp` is a zero-initialised bool set during dynamic init (tsc_clock.cpp:116-118); an early read takes the LFENCE+RDTSC path, which is correct. PlausibilityGate, Arena and Pool are single-thread by contract with no atomics; PlausibilityGate Stats are plain uint64. PageBlock is move-only RAII. No queues or rings in this group.
- **Conventions:** Price = int64 paise per unit; Qty = units; Lots = contracts; LotSize = units/contract (never a literal); Notional = int64 paise; Rate continuously compounded; Years ACT/365F; Vol fraction; Bps 1e-4. Timestamp = int64 ns since Unix epoch UTC. IST = fixed +05:30. Session windows = ns since IST midnight from config, no shipped NSE/BSE constants. SourceTimeSpec {unit, epoch_offset, ist_naive} per feed from config. TscCalibration Q32.32 ns/tick.
- **Implemented vs connected:** round_to_tick, is_on_tick, apply_bps, price_from_rupees and notional_from_rupees are test-only. Pool is test-only. Arena + PageBlock are used by app/main.cpp:108-112. TscClock::create / create_fallback are used by app/main.cpp:364-365, 451-452. PlausibilityGate is used by feed/normaliser.hpp. cache_topology is used by analytics/chain_soa.hpp and analytics/bench/bench_chain.cpp. latency_gate is used by feed (kite_decoder, normaliser), book (l2_book) and analytics (rolling, ewma) tests.
- **Build/presets:** C++23, default build type RelWithDebInfo, `/W4 /permissive- /arch:AVX2` (native arch ON by default, so binaries are host-specific). Presets: `default` (RelWithDebInfo, tests ON, bench OFF, no vcpkg); `vcpkg` (+toolchain, feature `config`, bench ON); `net` (`config;net`, jobs 4); `debug`; `asan` (Debug+ASan); `tsan` (FATAL on MSVC); `prod` (Release, tests OFF). vcpkg.json features `testing`, `math`, `storage`, `ui` and `ml` are selected by no preset.
- **Optional pieces** (QUIET find_package, so the target degrades or skips when absent): tomlplusplus (core/config, risk), OpenSSL (broker), Boost headers (app, server, desktop), Qt6 6.8 (desktop). `ALTAIR_BUILD_BENCH` gates analytics bench. `ALTAIR_ENABLE_TORCH` is declared and printed but has no consumer in any CMakeLists (grep).
- **build.bat:** hard-coded VS 18 BuildTools path. Uses `%ERRORLEVEL% NEQ 0` (catches the -1 link failure). For `prod`, any non-zero ctest exit is turned into exit 0 with a NOTE; there is no `prod` test preset, so ctest always errors there.

## 6. Test-suite observations

- Every test in the group is a plain `main()` with `check()` incrementing `failures` and returning non-zero, so ctest exit codes are meaningful. There is no Catch2, despite comments promising it; `tests/testkit/` is an empty placeholder and `smoke.cpp` checks only C++23/expected/span.
- test_units.cpp: good static type-safety probes (concept workaround for MSVC). Missing: int64 extrema for round_to_tick/is_on_tick, apply_bps near 2^63, NaN/inf Bps, large-magnitude rupee conversions (C01-003), `qty_of` with LotSize 0.
- test_arena_pool.cpp: missing double release, release of never-acquired blocks, copy traits. Latency is print-only (C01-005).
- test_page_alloc.cpp: property-based and portable. `check(true, "survived ...")` (:197) is a reached-line assertion, not a check. The huge-page branch is environment-dependent; either branch passes.
- test_tsc_clock.cpp: real sleeps (200 ms) with 2% and 100 ms tolerances, so it can flake on loaded hosts. Calibration checks `skip` and pass on hosts without invariant TSC. Fallback uncertainty is untested (C01-009). No cross-thread/cross-core monotonicity test. Latency is print-only.
- test_exchange_ts.cpp: thorough on conversion overflow, IST-naive direction and check order. Missing: stale ticks (C01-006), skew extremes (C01-007), watermark poisoning.
- test_timestamp.cpp: solid, including compile-time IST checks and `hours(kMaxHours)` exactness.

## 7. Open questions

1. feed/normaliser.hpp:67,91,140-142 takes `local_now` as a parameter from its caller and passes it straight to `gate_.admit`. Do the normaliser's callers (feed/app) supply `TscClock::now()`, and what `max_future_skew` does config set? This decides whether C01-008 escalates. Is there a staleness check downstream of the gate (C01-006)?
2. Linux: `MAP_HUGETLB` with a system default hugepage size other than 2 MiB (page_alloc.cpp:53-55, 180) — does `munmap(p, hsz)` release a 1 GiB mapping correctly?
3. In the `default` preset (no vcpkg), which targets skip silently when tomlplusplus, OpenSSL, Boost or Qt are absent, and does each print that it skipped?
4. Does any subdirectory CMakeLists read `_path`, `_row`, `_bad` or `_status`, which leak from AtlasAudit.cmake?
5. What is in the untracked `GETSClient_*` tree? It is not ignored, so is anything credential-bearing at risk of a `git add -A`? (Not inspected; out of scope.)
