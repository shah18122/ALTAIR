# Task Card Ledger

115 cards. One card = one DeepSeek prompt = one review = one commit.
Status: `TODO` · `SENT` · `REVIEW` · `CORRECTION` · **`DONE`** · `BLOCKED`

**Progress: 14 / 115 · Phase 0 done bar one, Phase 1 started · 12%**

> Count corrected 2026-08-29: the header read 96 while the per-phase headings
> summed to 110 — the per-phase numbers were right and matched the rows, only the
> grand total was stale. `PROTOCOL.md` said 93. Both fixed, along with
> `CLAUDE.md`. The P0-05, P0-06, P0-08 and P0-09 splits took Phase 0 from 9 cards to 13
> and the total to **115** (14·7·9·10·9·8·7·4·12·7·8·14·6). All three agree.

### Carried debt

| From | Item | Resolve in |
|---|---|---|
| P1-02 / P1-03 / P1-05 / P1-07 | **Four Phase 1 cards cannot be implemented here.** P1-02 and P1-03 parse exchange master files I have no sample of; P1-05 needs an XTS schema I do not have; P1-07 needs broker credentials. Writing a wire-format parser against a guessed schema is exactly the confident-plausible-wrong failure the protocol exists to stop. **Cards can be written now; implementation needs one sample file each (P1-02/03/05) and credentials (P1-07).** | Smit — sample files |
| P0-01 | `apply_bps` uses `long double`, which is 64-bit on MSVC. Exact for realistic single-trade magnitudes (₹100 cr turnover × 15 bps ≪ 2⁵³) and all five specified tie cases verified correct. **Accumulated session turnover is a different question** — revisit with scaled-integer arithmetic. | P3-09 |
| P0-01 … P0-10 | **Gate 1 verified on MSVC only.** No GCC or Clang on the dev box (no clang-cl in the VS install, no WSL distro), so `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wold-style-cast` is unverified for all thirteen cards. MSVC `/W4 /permissive-` and `/analyze` are clean, and `/Wall` shows only C4514 (off-by-default). PROTOCOL §10 requires a clean build on Windows **and** Linux. | Phase 0 gate |
| P0-03 | `mul_shift32`'s portable 64×64→128 branch is **dead code on this box** — MSVC x64 takes `_umul128` and GCC/Clang take `unsigned __int128`. It is exercised by no test and no compiler here. Reachable only on an exotic target; verify before trusting any such build. | Phase 0 gate |
| P0-04 | `to_utc` rejects `raw == INT64_MIN` for `TimeUnit::Nanos`, where it would in fact be representable. Deliberately conservative: the bound is written as `raw < -limit` so `-INT64_MIN` is never formed. Costs one representable value at year 1677. | — (accepted) |
| **P0-01 / CLAUDE.md rule 3** | **Integer paise is NOT sufficient for currency derivatives.** Confirmed against Zerodha's own client (`gokiteconnect/ticker/ticker.go`, `convertPrice`): the wire price divisor is **segment-dependent** — `NseCD` divides by 10'000'000 and `BseCD` by 10'000, while everything else divides by 100. So an NSE-CD wire integer is in units of 10⁻⁷ rupees, **five decimal places finer than a paisa**, and storing it in `Price` (integer paise) truncates silently. Does **not** bite today: the configured universe is indices, index F&O and stock futures — no CDS. It becomes live the moment a USDINR contract is added. `ContractSpec` must carry a `price_scale` (wire units per rupee) and P2-02 must normalise, or the universe must **block** currency derivatives outright (rule 9). Decide in P1-01; enforce in P2-02. | **P1-01 + P2-02** |
| **P0-09b → P2-01** | **A broker token is not an instrument key.** `ReplayTick.token` is a bare `uint32` documented as "a token". Kite's `instrument_token` and XTS's `ExchangeInstrumentID` are **different number spaces for the same contract**, so that field means different things depending on which feed produced the tick. P2-05 switches the primary feed mid-session: with a broker token in the tick, every instrument would appear to vanish and a stranger appear in its place, and the book, ledger and strategies would all follow it. The normalised `Tick` must carry the canonical `InstrumentId` from the spec store (ROADMAP §6.2); decoders map (source, broker_token) → InstrumentId and nothing downstream ever sees a broker token. The Phase 0 skeleton is safe only because it is single-source by construction. **Warning is in the header at the field.** | **P2-01 + P2-04** |
| ~~**P1-01**~~ **PARTLY RESOLVED 2026-08-31** | ~~**`ContractSpec` has no home for broker tokens.**~~ P1-01 gave it `token[kFeedSourceCount]` indexed by `FeedSource`, and **P1-06 is where the map is actually assembled**: the merged spec takes `token[Kite]` from the Kite source and `token[Xts]` from the XTS source, so the mapping is bidirectional through the store (`id_of(src, token)` and `token_of(id, src)`) and nothing compares a token across sources. What remains is only that the XTS half is untestable against real data until P1-05 has a sample file. Original text: **`ContractSpec` has no home for broker tokens.** ROADMAP §6.2 gives it an `InstrumentId` but no `kite_token` / `xts_instrument_id`. P1-04 and P1-05 are named "token map" cards so the intent exists, but P1-01 must decide *where* the mapping lives — fields on the spec, or a separate table — and it must be **bidirectional**: decode needs token→id, subscription needs id→token. | **P1-01** |
| **P1-04 → P2-04** | **`load_kite_dump` writes the store directly; every later parser writes the Reconciler.** P1-04 predates P1-06, so it calls `SpecStore::add` — which means a Kite dump can populate the store without ever facing the three-way check. The four master-parser cards written 2026-08-31 all take `Reconciler&` instead, deliberately: a parser that *can* write the store is a parser that can bypass reconciliation. `load_kite_dump` needs a `Reconciler&` overload, and the direct-to-store form should become test-only. Not urgent while Kite is the only source; **must land before a second source does.** | **P1-05 or P2-04** |
| **P7-00 → P6 / P8** | **The first bar-data drop has a hole exactly where the flagship strategy is graded.** NIFTY 1-minute is missing **15:16–15:27 on every day** (12 consecutive minutes, 4 of 4 days), while India VIX has all 375. That window is where a 10-minute-horizon label lives and where NIFTY and VIX stop being joinable bar for bar. **Do not interpolate** — a fabricated close-window bar is a fabricated measurement in the exact window the model is scored on. Either source those minutes from Kite historical or make every close-window label explicitly unavailable. Three smaller ones: India VIX daily `open` equals the previous close on 524/526 bars (a copy, not a measurement); NIFTY daily has 1'408 zero-volume backfilled bars before 1997-01-01; and TradingView row caps mean NIFTY 1m covers **4 trading days**, not four months. Full detail in `prompts/P7-00_dataset_bar_ingest.md` §6. | **P6 + P8** |
| **P1-04 → P2-04** | **`load_kite_dump` will overflow the store on a real dump.** `SpecStore::kMaxInstruments` is 8'192; the live Kite `instruments.csv` is ~100'000 rows. The throughput test makes this concrete: 10'000 rows in, **8'192 added, 1'808 rejected**. The loader behaves correctly — it reports `rejected_by_store` rather than failing — but a caller that hands it the raw dump gets a **silently truncated universe**, and which 8'192 survive depends on CSV row order. Nothing may hand it an unfiltered dump. The universe filter (config `[universe]`) must run **before** `add`, or `kMaxInstruments` must be raised to cover the whole dump (~100k × 176 B = ~18 MB, plus a 2x token index — affordable, but it makes the store a different object). Decide in P2-04. | **P2-04** |
| ~~**P1-02 / P1-03 / P1-05**~~ **POLICY DECIDED 2026-08-31** | The `on_download_failure` half of this row is **closed** by P1-08a: the policy is a function of snapshot age **in trading days** AND whether an F&O expiry has been crossed — age alone gets it exactly backwards on the one day a month it matters, because rollover is when lot sizes change. Withheld sources are simply not fed to the Reconciler, so P1-06's existing `MissingPrimary`/`NoBroker` verdicts do the blocking and no second blocking path exists to disagree with the first. No snapshot at all **halts**. The scraping-fragility half of this row remains open. Original text: **Symbol mapping depends on files downloaded from the exchange, not on a broker API.** ROADMAP §6.1 already makes the NSE and BSE contract masters *primary* for lot size, tick size and expiry, with Kite/XTS as cross-check — so XTS symbol mapping cannot be satisfied by the XTS API alone. That makes these scraping cards, the most fragile kind: NSE's endpoints want specific headers and cookies, payloads are gzipped, and URLs move without notice. `config/altair.toml` sets `snapshot_dir` and `keep_snapshots_days = 3650`, which is the right shape — but there is **no `on_download_failure` policy**. There is one for *disagreement* (`block_symbol`) and none for *unavailability*. Pre-open is 08:15 and the market opens 09:15: decide now whether a failed fetch falls back to the last good snapshot with a loud flag, or halts the session. Rule 9 says it must not silently start with no specs. | **P1-01 (policy) + P1-02** |
| ~~suite-wide~~ | ~~One unreproduced suite failure.~~ **RESOLVED 2026-08-29.** It was the P0-06b seqlock test asserting `successes == 2 * kTargetPerReader`. The reader loop continues while *either* its success target or its value-change target is unmet, so a reader holding 100'000 successes but still short on changes keeps sampling and **overshoots** — roughly 1 run in 36, which is why 96 earlier runs missed it. The loop was correct; the assertion should have been `>=`. Found by running the full tree under deliberate CPU load with every FAIL line captured, which is now the standing technique. **72 loaded suite runs since, zero failures.** | — (closed) |

---

## Phase 0 — Foundation (14)

| Card | File | Deliverable | Status | Gates | Commit |
|---|---|---|---|---|---|
| P0-01 | [`P0-01_core_types.md`](P0-01_core_types.md) + [`c1`](P0-01c1_correction.md) | `core/types` — dimensional units, exact paise money, tick rounding | **DONE** | 1✓ 2✓ 3✓ 4✓ 5✓ 6 n/a 7✓ 8✓ | `eeab54b` |
| P0-02 | [`P0-02_core_time_timestamp.md`](P0-02_core_time_timestamp.md) | `core/time/timestamp.hpp` — affine time algebra, IST, floor semantics | **DONE** | 1✓ᵐ 2✓ 3✓ 4✓ 5✓ 6 n/a 7✓ 8✓ | `03c3062` |

> **P0-02 process note.** Implemented by Claude, not DeepSeek, at Smit's
> instruction on 2026-08-29 — a deliberate departure from the role split in
> `CLAUDE.md` §"Your role in this repo". Recorded here because the ledger and the
> git history must line up (PROTOCOL §9), and because a card whose author and
> reviewer are the same actor has weaker independence than the other 109.
> Gate 2 was still run mechanically (52/52 contract declarations matched verbatim)
> rather than by self-assertion. ᵐ = gate 1 on MSVC only; see carried debt.
>
> Three defects were found in the card during pre-send review and fixed before
> implementation: an int32 overflow (UB) in spec item 8, an uncompilable
> `Duration / scalar` in item 9 (`StrongInt` defines no `operator/`), and a
> "14 hours" comment for a 22-hour span. Literal coverage was also added to §6 —
> the six `operator""` literals were in the contract but untested.
| P0-03 | [`P0-03_core_time_tsc_clock.md`](P0-03_core_time_tsc_clock.md) | `core/time/tsc_clock` — invariant-TSC detect, calibration, drift uncertainty | **DONE** | 1✓ᵐ 2✓ 3✓ 4✓ 5✓ **6✓** 7✓ 8✓ | `6d9c8f1` |

> **P0-03 bench (gate 6, first live one).** `TscClock::now()` p50 = 16 ns,
> **p99 = 18 ns** against a 25 ns budget, on an invariant TSC at 2.112 GHz.
> Calibrated `ns_per_tick = 0.473469443 ± 0.000002806` (5 × 10 ms samples,
> relative stderr 5.9e-6). `now()` landed 1.4 µs from `system_clock`.
>
> The first harness reported p99 = 121 ns and looked like a budget failure. It
> was measuring itself: bracketing `now()` with two ordered TSC reads costs a
> full RDTSCP (~12 ns here), and a `volatile` sink was forcing a store of the
> whole `Timestamp`. The harness now measures back-to-back `now_ticks()` as a
> baseline and subtracts it. **Any future latency card must do the same** — an
> unsubtracted bracket overstates a sub-20 ns operation by ~7x.
>
> **A 1-in-20 flake, found later and fixed.** `test_tsc_uncertainty_propagates`
> asserted `sigma(10ms) < 11 * sigma(1ms)`. Since
> `sigma(d) = |d|*rel + ns_per_tick`, at 1 ms the constant one-tick term (0.47 ns)
> and the integer rounding of a ~1 ns result are the same order as the signal: a
> tight calibration puts sigma(1ms) at 1.49 ns (rounding **down** to 1) and
> sigma(10ms) at 10.67 (rounding **up** to 11), and the bound fails on rounding
> alone. Now measured at 100 ms / 1000 ms where the floor is negligible.
> Verified 25 consecutive loaded runs. **Assert a ratio only where the
> proportional term dominates the constant one.**
>
> Implemented by Claude, not DeepSeek (as with P0-02). Gate 2 run mechanically:
> 24/24 contract declarations matched verbatim. ᵐ = gate 1 on MSVC only.
| P0-04 | [`P0-04_core_time_exchange_ts.md`](P0-04_core_time_exchange_ts.md) | `core/time/exchange_ts` — per-source epoch normalisation + plausibility gate | **DONE** | 1✓ᵐ 2✓ 3✓ 4✓ 5✓ 6 n/a 7✓ 8✓ | `016c390` |

> **P0-04 notes.** Header-only and fully `constexpr`, so the epoch arithmetic is
> proven by three namespace-scope `static_assert`s rather than only at runtime.
> 54 checks. Gate 6 n/a — normalisation is a handful of integer ops and is
> covered by the 1 µs "wire decode → normalised tick" budget in ROADMAP §11,
> which P2-04 must measure end to end.
>
> **The IST-naive trap is the reason this card exists.** A feed that encodes IST
> wall-clock as though it were UTC emits a value 19800 s *larger* than the truth
> — every tick stamped 5 h 30 m into the future, which is the one direction that
> injects look-ahead. The correction is a **subtraction**; adding would double
> the error to 11 h. Anchored on the P0-02 instant: naive raw `1787908500` s,
> truth `1787888700` s, difference exactly `kIstOffset`.
>
> Two decisions worth carrying forward: **no session-time constant ships** (they
> change — muhurat, special sessions — and CLAUDE.md rule 1 bans baked-in market
> parameters), and `admit()` does **not** enforce the session by default, because
> a pre-open tick is a valid instant and conflating "implausible" with "outside
> trading hours" would silently drop legitimate data.
>
> Two bugs were found in the card's own test expectations during pre-flight and
> fixed before implementing: a floor/ceiling sequence that walks backwards in
> time would have tripped the monotonic check it wasn't testing, and the
> "fails both floor and ceiling" case as first written failed only one — it needs
> `local_now` *below* the floor for both to trip. Implemented by Claude, not
> DeepSeek. Gate 2 run mechanically: 29/29 declarations verbatim. ᵐ = MSVC only.
| P0-05a | [`P0-05a_core_mem_page_alloc.md`](P0-05a_core_mem_page_alloc.md) | `core/mem/page_alloc` — OS pages, hugepage attempt, honest reporting | **DONE** | 1✓ᵐ 2✓ 3✓ 4✓ 5✓ 6 n/a 7 n/a 8✓ | `bac0264` |
| P0-05b | [`P0-05b_core_mem_arena_pool.md`](P0-05b_core_mem_arena_pool.md) | `core/mem/arena` + `core/mem/pool` — O(1) bump and free-list allocators | **DONE** | 1✓ᵐ 2✓ 3✓ 4✓ 5✓ **6✓** 7 n/a 8✓ | `066a679` |

> **P0-05 was split.** As scoped in this ledger it needed six files
> (`page_alloc.hpp/.cpp`, `arena.hpp`, `pool.hpp`, a `CMakeLists.txt`, tests) and
> PROTOCOL §8 splits at four. Suffixed `a`/`b` rather than renumbered, so
> P0-06…P0-09 keep their identity — precedent: `P0-01c1`. **Card total 110 → 111**,
> corrected here, in `PROTOCOL.md`, and in `CLAUDE.md`.
>
> **P0-05a — the hugepage decision.** A refused hugepage is **not** an error: it
> falls back to ordinary pages, succeeds, and `backing()` reports the truth. That
> looks inconsistent with P0-03, where a missing invariant TSC *is* an error. The
> distinction is what the fallback changes — `steady_clock` for a TSC changes the
> *meaning* of every measurement, while 4 KiB pages are semantically identical to
> 2 MiB ones and only slower. **Degrade silently on performance, fail loud on
> meaning.** On this box the 4 MiB huge request was REFUSED (Windows needs
> `SeLockMemoryPrivilege`), so the fallback path is the one the tests actually
> exercised — verified, not assumed. `page_size` 4096, `huge_page_size` 2097152.
>
> **P0-05b bench (gate 6).** `Arena::allocate(32,8)` **2.51 ns/op** (budget 10),
> `Pool::acquire`+`release` **2.21 ns/pair** (budget 15). These are *below* the
> ~12 ns RDTSCP pair that P0-03 used to measure, so subtracting the harness is no
> longer sufficient — the quantum exceeds the signal. Timed in **batches** of 1e6
> between one clock pair instead. Two techniques now on the shelf: subtract the
> harness for ~20 ns operations, batch for ~2 ns ones.
>
> **A real implementation bug, caught by the card's own test.** `Arena::allocate`
> first aligned the *offset* rather than the *address*, which is only correct when
> the span's base is at least as aligned as the request — a 64-byte-aligned buffer
> happily returned a "128-aligned" pointer that was nothing of the sort. Fixed to
> align the address, which **cost `allocate` its `constexpr`**: `reinterpret_cast`
> is not permitted in a constant expression and no constexpr path can inspect a
> pointer's alignment. The contract was amended rather than the test weakened.
> A second failure was the reverse — the test asserted `high_water() == 600` where
> the truth is 604, because 8-byte alignment padding is genuinely charged. The
> card now asserts 604 and says why.
>
> Both implemented by Claude, not DeepSeek. Gate 2 run mechanically: 15/15, 19/19,
> and 21/21 declarations verbatim across the three headers. Gate 7 n/a — no money,
> prices, or time arithmetic in either card. ᵐ = gate 1 on MSVC only.
| P0-06a | [`P0-06a_core_lockfree_spsc_ring.md`](P0-06a_core_lockfree_spsc_ring.md) | `core/lockfree/spsc_ring` — the feed→strategy hot path | **DONE** | 1✓ᵐ 2✓ 3✓ 4✓ 5✓ **6✓** 7 n/a 8✓ᵗ | `f75717e` |
| P0-06b | [`P0-06b_core_lockfree_mpsc_seqlock.md`](P0-06b_core_lockfree_mpsc_seqlock.md) | `core/lockfree/mpsc_ring` + `seqlock` — many writers, one reader | **DONE** | 1✓ᵐ 2✓ 3✓ 4✓ 5✓ **6✓** 7 n/a 8✓ᵗ | `0e2fb84` |

> **P0-06 was split**, same reason as P0-05: three lock-free structures need
> five-plus files and well over eight tests. **Card total 111 → 112.**
>
> **ᵗ — READ THIS BEFORE TRUSTING EITHER CARD.** The memory orderings are
> **unvalidated**. Neither TSAN nor ASAN runs here (the root `CMakeLists.txt`
> guards both with `AND NOT MSVC`; no GCC, Clang, or WSL on the box). Worse,
> **x86 is TSO** — `acquire`/`release` compile to plain `mov`, so a missing or
> wrong ordering annotation is *completely invisible* to every test that runs on
> this machine and would surface only on ARM. Every green result below is
> evidence about the **algorithms**, not about their ordering. This is why both
> cards specify the orderings verbatim rather than leaving them to the
> implementer: they cannot be discovered by testing on this hardware. **The
> Phase 0 gate must re-run all of `core/lockfree` under TSAN on Linux/Clang.**
>
> **P0-06a bench.** SPSC `try_push`+`try_pop` **2.80 ns/pair** (budget 20);
> two-thread stress moved 1'000'000 elements at **268 M elem/s**, with the
> consumer verifying the *full sequence*, not just the count — a count-only check
> passes a matched drop-and-duplicate.
>
> **P0-06b bench.** MPSC push+pop **11.94 ns/pair** (budget 40) — the CAS costs
> ~4x the SPSC store, which is the expected shape. `Seqlock<Big>::store`
> 1.61 ns, `try_load` 0.63 ns (budget 20 each). Both seqlock figures are
> L1-hot and uncontended, i.e. best case; real cross-core numbers will be worse.
> MPSC 4-producer stress: 1'000'000 elements, per-producer ordering preserved,
> no duplicates, 8.9 M elem/s (dominated by `yield()` in the spin loops).
>
> **The seqlock tear test took four attempts to become evidence.** Worth reading
> in full — every failure mode here is one a future concurrency card can repeat.
>
> 1. Readers free-running against a tight-loop writer, asserting
>    `successes > 100'000`. The writer held an odd sequence for most of its
>    ~1.6 ns cycle, so readers retried **99.98%** of the time and "zero tears"
>    rested on **1'330 samples**. Green, and nearly vacuous.
> 2. Added a writer gap → ~200'000 successes, but the assertion was **flaky**:
>    56'000–206'000 with machine load, failing ~1 run in 3. `tears == 0` passed
>    every time; the seqlock was never the problem.
> 3. Made each reader run to a *fixed* 100'000 successes. Then, **under CPU
>    load, it reported genuine TORN READS** — 2 failures in 12. The cause was
>    **not** the seqlock: a default-constructed `SeqlockSnapshot<Big>` holds
>    `{0,0,0,0}`, which fails the `{n,n+1,n+2,n+3}` checker, so a reader that
>    outran the writer's first store logged 200'000 phantom tears. **A false
>    positive that would have sent someone hunting a memory-ordering bug that
>    does not exist.** Fixed with a `writer_ready` barrier.
> 4. That exposed the last hole: with the writer descheduled, readers drained
>    all 200'000 reads of one *frozen* tuple, satisfying the pattern trivially.
>    Asserting "the value moved" afterwards just reintroduced flakiness (1 in 14).
>
> **Final form: the evidence requirement lives in the loop's termination
> condition, not in an assertion after it.** Each reader runs until it has both
> 100'000 successes and 1'000 observed value changes, with an attempt cap as a
> loud backstop. **16 consecutive runs under deliberate CPU load, zero failures.**
>
> Three transferable rules, now in the card: assert on the invariant and
> *construct* the sample size; never let an unwritten initial state satisfy or
> violate the checker by accident; put the evidence requirement in the loop.
>
> Both implemented by Claude, not DeepSeek. Gate 2 mechanical: 20/20, 14/14,
> 10/10 declarations verbatim. Gate 7 n/a — no money, price, or time arithmetic.
| P0-07 | [`P0-07_core_log.md`](P0-07_core_log.md) | `core/log` — binary async logger + off-thread decoder | **DONE** | 1✓ᵐ 2✓ 3✓ 4✓ 5✓ **6✓** 7 n/a 8✓ | `78545fa` |

> **P0-07 bench.** `Logger::write(u64, i64, f64)` **23.14 ns/call** against a
> derived 40 ns budget — roughly 12 ns of that is the irreducible ordered TSC
> read, and the figure includes the paired `try_pop` that keeps the ring drained.
> `LogDecoder::decode_line` is **210 ns**, 9.1x the write: off the hot path by
> design, and one consumer thread still absorbs ~4.8 M records/s. End to end,
> 200'000 records logged *and decoded* at **19.4 M rec/s**. 78 checks.
> ROADMAP §11 has no log line; the budget is derived from the stages a log call
> sits *inside* (wire decode 1 µs, risk checks 3 µs).
>
> **Four decisions, all made in the card rather than left open.** No string
> arguments — a string is the allocation this design exists to avoid, and Altair
> symbols are `uint32` tokens anyway. Fixed 64-byte records, one cache line, so
> the transport is P0-06a's `SpscRing` rather than a bespoke byte ring. The hot
> path stores **raw TSC ticks** and the decoder converts, keeping a 128-bit
> multiply off the tick path. And no globals or macros: a global registry brings
> a static-initialisation-order problem and makes the whole thing untestable.
>
> **`dropped()` counts attempts, not losses** — a semantic the first test got
> wrong. The two-thread test spins retrying on a full ring, so every failed
> attempt increments the counter even though the retry delivers the record;
> asserting `dropped() == 0` looked obviously right and was obviously wrong.
> The doc comment now says so explicitly, because the counter (and
> `dropped_before` with it) is only meaningful under the intended
> fire-and-forget use. Delivery is proved by the ordering check instead.
>
> One card expectation was also wrong: `"spec {:.2f}"` with one argument yields
> `"spec {:.2f} (+1 unused)"`, not the bare string. `{:.2f}` is not a
> placeholder to this decoder, so the argument genuinely goes unconsumed — and
> reporting that is the right behaviour, since it surfaces the unsupported spec
> instead of silently swallowing a value the author expected to see.
>
> Implemented by Claude, not DeepSeek. Gate 2 mechanical: 38/38 and 7/7
> declarations verbatim. Gate 7 n/a — no money, price, or time arithmetic beyond
> the tick→ns conversion P0-03 already owns. ᵐ = gate 1 on MSVC only.
| P0-08a | [`P0-08a_core_config_store.md`](P0-08a_core_config_store.md) | `core/config` — versioned, hashed, hot-swappable config store | **DONE** | 1✓ᵐ 2✓ 3✓ 4✓ 5✓ **6✓** 7 n/a 8✓ | `6570bc8` |
| P0-08b | — | `core/config/toml_source` — TOML → snapshot loader | **BLOCKED** | — | — |

> **P0-08 was split, and the second half is BLOCKED.** The practical reason:
> **there is no vcpkg on this box** — `VCPKG_ROOT` unset, vcpkg not installed,
> `builtin-baseline` unpinned in `vcpkg.json`. A card requiring `tomlplusplus`
> could not be built or tested here, so gates 1 and 4 would be green on faith.
> P0-08b is therefore recorded as **blocker #7**, not quietly skipped.
>
> The architectural reason stands regardless: the config *mechanism* — snapshot
> swap, version, content hash, typed lookup — has nothing to do with the file
> format. Splitting at that seam left this half needing **no dependency at all**,
> and it means replay can feed config from a captured snapshot instead of
> re-parsing a file, which rule 6 requires anyway. Card total 112 → 113.
>
> **`content_hash()` is order-independent, and that is a rule-10 requirement,
> not a nicety.** Two snapshots built from the same pairs in different order hash
> identically; a commutative sum of splitmix64-mixed entry words gets there.
> An order-dependent hash would mean the same config file read twice could
> produce two different hashes — and "every live decision reproducible from
> `config_hash`" would simply be false. The type participates in the hash too:
> `set_int("k", 1)` and `set_bool("k", true)` store identical bits and must not
> collide.
>
> **Keys are matched on text, not on hash.** The 64-bit FNV-1a is a fast reject
> only; `find` then `strcmp`s. Matching on the hash alone would silently hand a
> strategy someone else's limit, with nothing to say so.
>
> **No type coercion on read.** `get_bool` on an `Int` is `WrongType`, not
> `true`; `get_int` on a `Bool` is `WrongType`, not `1`. A missing key is
> `NotFound`, never a default. Rule 9 — a mistyped config read is a limit read
> as the wrong quantity.
>
> **Bench.** `refresh()` unchanged — the steady-state hot path — **0.63 ns**
> (budget 5): one acquire load and a compare, no copy. `get_int(handle)`
> **1.10 ns** (budget 5). `find()` scanning 128 entries is **107.6 ns**, i.e.
> the handle is **98x cheaper**, which is the entire justification for resolving
> once at startup. `sizeof(ConfigSnapshot)` is 9232 bytes — far too large to copy
> per tick, which is exactly why `refresh` copies only when the version moves.
>
> Built on P0-06b's seqlock rather than inventing a second mechanism. The
> concurrent test reuses the P0-06b construction — terminate on the evidence, not
> a timer — and observed 2'000 distinct versions with zero inconsistent
> snapshots. 74 checks, clean on the first run. Implemented by Claude, not
> DeepSeek. Gate 2 mechanical: 39/39 and 8/8 verbatim. ᵐ = gate 1 on MSVC only.
| P0-09a | [`P0-09a_core_invariant.md`](P0-09a_core_invariant.md) | `core/invariant` — the conservation ledger (§3.6 laws 1 and 2) | **DONE** | 1✓ᵐ 2✓ 3✓ 4✓ 5✓ **6✓** **7✓** 8✓ | `01b12a1` |
| P0-09b | [`P0-09b_feed_replay.md`](P0-09b_feed_replay.md) | `feed/replay` — a replayer that cannot expose a future tick | **DONE** | 1✓ᵐ 2✓ 3✓ 4✓ 5✓ **6✓** 7 n/a 8✓ | `76228d4` |

> **P0-09 was split**, the fourth and last of Phase 0. `core/invariant` is
> 3 files; `feed/replay` needs 4 of its own including the root `CMakeLists.txt`
> edit. Card total 113 → 114; Phase 0 finishes at 13.
>
> **Scope cut, stated rather than stubbed.** ROADMAP §3.6 lists four
> conservation laws. Two of them — net delta vs hedge, and order-book depth
> monotonicity — need greeks (P3) and an L2 book (P2-08). They are **not
> stubbed**: a check that always passes is worse than an absent one. P0-09a
> delivers the two cash laws and the enforcement shape the other two will reuse.
>
> **The sign convention is the gate-7 content here.** A buy debits cash by
> `notional + cost`. A sell has a *negative* notional, so cash rises by
> `|notional|` — **and the cost still debits**. Crediting the cost on the sell
> is the classic error, and it makes every short look better by exactly twice
> the charge. The test pins it: a round trip gaining ₹1000.00 with ₹246.90 of
> costs must realise **75'310 paise**, not 100'000.
>
> `check()` uses `==`, not a tolerance. That is only possible because rule 3 put
> every money amount in integer paise — an epsilon here would hide precisely the
> class of bug the invariant exists to catch. Verified over 10'000 operations of
> a fixed-LCG walk (never `std::random`, so a failure reproduces), with the
> identity re-checked after **every** one and the accumulators compared against
> totals the test kept independently.
>
> **A breach latches.** Once tripped, fills, adjustments and checks all return
> `Latched` — rule 9's kill switch. A second trip does not overwrite the first,
> because the first breach is the diagnosis and later ones are its consequences.
> `clear_breach()` lifts the latch and touches no balance and no count.
>
> **P0-09b: look-ahead is prevented structurally, not by discipline.** There is
> no `peek()`, no `at()`, no `operator[]`, no `begin()`, no `data()`, no
> iterator; `next()` returns **by value** so a caller cannot do pointer
> arithmetic off a reference. The tests assert those absences with named
> concepts — the same technique P0-01 used for `Price * Lots`. Look-ahead is the
> most expensive bug in this domain precisely because it does not crash: it
> produces a *better* result, and the better result is what gets believed.
>
> `now()` returns the timestamp of the tick **last delivered**, `Timestamp::epoch()`
> before the first, and never a wall clock. seqno must strictly increase;
> timestamps may repeat, because two trades can share a nanosecond but two ticks
> cannot share a sequence number.
>
> **Bench.** `on_fill` **13.40 ns**, `check_and_trip` **0.31 ns** (budget 20
> each; ROADMAP §11 gives all risk checks 3 µs and this runs inside it).
> `Replayer::next()` **3.61 ns** (budget 10). The end-to-end null-strategy run —
> 1'000 ticks, conservation armed and checked on every tick — moved at
> **111 M ticks/s**. 82 + 55 checks. Implemented by Claude, not DeepSeek. Gate 2
> mechanical: 26/26 and 21/21 verbatim. ᵐ = gate 1 on MSVC only.

| P0-10 | [`P0-10_app_entry_point.md`](P0-10_app_entry_point.md) | `app/` — the `altair` binary; the Phase 0 exit criterion | **DONE** | 1✓ᵐ 2✓ 3✓ 4✓ 5✓ 6 n/a 7 n/a 8✓ | `22a64c5` |

> **P0-10 — the exit criterion now actually runs.** Added to close gate item 7:
> ROADMAP §12 asks for `altair --replay sample.tick` end to end with a null
> strategy, and there was no binary. Card total 114 → 115; Phase 0 finishes at 14.
>
> ```
> altair --replay data/ticks/sample.tick
>   clock source     : InvariantTsc
>   ticks replayed   : 500000 of 500000
>   throughput       : 54.23 M ticks/s
>   fills            : 500      log lines decoded: 500
>   final position   : 0 units  realised delta: 4203 paise
>   clock == tick    : yes
>   invariants       : held on every tick
> ```
>
> **This is the first thing that proves the modules compose.** It wires nine of
> them — `TscClock`, `PageBlock`+`Arena`, `SpscRing`+`Logger`+`LogDecoder`,
> `ConfigStore`+`ConfigSnapshot`, `ConservationLedger`, `Replayer` — and a wiring
> bug shows up here or nowhere. Log records are **drained and decoded inside the
> replay loop**, deliberately: a logger whose consumer never runs is a ring that
> silently fills, and no unit test catches that.
>
> The `+4203 paise` is **noise, not alpha**. 500 alternating one-lot round trips
> on a random walk, gross ~9203 paise against 5000 paise of costs. Anything that
> looked like edge in Phase 0 would be a bug.
>
> **Two deliberate skeletons.** The session file is a magic, a version and a flat
> `ReplayTick` array — P2-06/07 defines the real mmap'd columnar store, and a real
> capture drops in behind the same reader. And `--gen` exists because there is no
> captured tick data yet; the alternative was an exit criterion nobody could run.
> `BadTickSize` rejects rather than reinterprets: if a later card changes
> `ReplayTick`, every existing file becomes garbage that would otherwise be read
> as though it were fine.
>
> **The binary opens no socket and reads no credential**, by design. A Phase 0
> binary that could reach a broker is one that could place an order.
>
> Gate 1 initially failed on MSVC's C4996 for `fopen`. Fixed with a portable
> `detail::session_fopen` (`fopen_s` on MSVC, `fopen` elsewhere) rather than
> `_CRT_SECURE_NO_WARNINGS` — the warning is real advice, and suppressing a whole
> class of it to silence five call sites is the wrong trade. Gate 2 then flagged
> two signatures: header-only free functions **must** be `inline` or every TU
> including the header is a duplicate-symbol link error. The card was
> under-specified, not the code wrong; the contract now says `inline`.
>
> 26 self-test checks, registered as `app_selftest`. Gate 6 n/a — this is a
> harness around the budgeted stages, not a stage. Gate 7 n/a — the null strategy
> has no financial content. Implemented by Claude, not DeepSeek. Gate 2
> mechanical: 15/15 verbatim. ᵐ = gate 1 on MSVC only.

**Exit:** `altair --replay sample.tick` runs end to end with a null strategy;
latency harness green; all invariants armed. — **MET** as of 2026-08-31, on a
synthetic session; a captured one is still needed (blocker #6).

---

## Phase 1 — Instrument master (9)

| Card | Deliverable | Status |
|---|---|---|
| P1-01 | `ContractSpec` type + point-in-time spec store — **DONE** `bf732a6` · 1✓ᵐ 2✓ 3✓ 4✓ 5✓ **6✓** 7✓ 8✓ · 57 checks | **DONE** |
| P1-02a | NSE F&O contract master parser (**primary** for NSE F&O) — card written | BLOCKED — sample |
| P1-02b | NSE `fo_mktlots.csv` → lot-size table (wide format, rolls monthly) — card written | BLOCKED — sample |
| P1-02c | NSE `EQUITY_L.csv` → cash `ContractSpec` — card written | BLOCKED — sample |
| P1-03a | BSE scrip + contract masters (**primary** for BSE) — card written | BLOCKED — sample |
| P1-04 | Kite instruments dump parser + token map — **DONE** · 1✓ᵐ 2✓ᶜ 3✓ 4✓ 5✓ 6✓ 7✓ 8✓ · 94 checks | **DONE** |
| P1-05 | XTS instruments master parser + token map | TODO |
| P1-06 | Three-way reconciler + disagreement flags + symbol blocking — **DONE** · 1✓ᵐ 2✓ᶜ 3✓ 4✓ 5✓ 6✓ 7✓ 8✓ · 77 checks | **DONE** |
| P1-07 | Margin fetch (SPAN + ELM) + change detection → retrain trigger | BLOCKED — creds |
| **P1-08a** | **Snapshot age + expiry-crossing + download-failure policy — card written** | **TODO — IMPLEMENTABLE NOW** |
| P1-08b | Master downloader (atomic replace, parse-verify) — card written | BLOCKED — vcpkg |

**Exit:** a full session's universe auto-loads pre-open with zero hardcoded lot
sizes; a deliberately corrupted source is caught and blocks only its symbol.

---

## Phase 2 — Feed & book (9)

| Card | Deliverable | Status |
|---|---|---|
| P2-01 | `Tick` / `DepthUpdate` normalised structs + wire schema | TODO |
| P2-02 | Kite binary decoder | TODO |
| P2-03 | XTS Socket.IO 1501/1502/1505 decoder | TODO |
| P2-04 | Normaliser + spec-store binding (token → ContractSpec) | TODO |
| P2-05 | Failover watchdog + seamless primary switch | TODO |
| P2-06 | mmap'd columnar tick store (writer) | TODO |
| P2-07 | Tick store reader + Parquet archiver | TODO |
| P2-08 | L2 order book, O(1) update, crossed-book guard | TODO |
| P2-09 | OBI, weighted OBI, microprice, VPIN, Kyle λ, queue position | TODO |

**Exit:** 6 h of live NIFTY + BANKNIFTY captured, replayed bit-identically, zero
drops, p99 decode < 3 µs.

---

## Phase 3 — Analytics & cost (10)

| Card | Deliverable | Status |
|---|---|---|
| P3-01 | Black–Scholes greeks, 1st order, SIMD | TODO |
| P3-02 | 2nd/3rd order greeks (Vanna, Volga, Charm, Veta, Speed, Zomma) | TODO |
| P3-03 | Bjerksund–Stensland American pricer | TODO |
| P3-04 | Jäckel IV solver | TODO |
| P3-05 | SVI surface fit + arbitrage-free checks | TODO |
| P3-06 | Rolling stats: mean, var, skew, kurt, z, EWMA, Hurst | TODO |
| P3-07 | Derivatives: velocity, acceleration, jerk, multi-scale | TODO |
| P3-08 | India VIX replication | TODO |
| P3-09 | **Cost calculator** — full stack, effective-dated `charges.toml` | TODO |
| P3-10 | Slippage + Kyle-λ impact learned from fill history | TODO |

**Exit:** full-chain greeks < 50 µs; cost calculator matches a real contract note
to the paisa on 50 historical trades.

---

## Phase 4 — Risk, OMS, paper trading (9)

| Card | Deliverable | Status |
|---|---|---|
| P4-01 | Sizing: fixed-fractional + ¼-Kelly + vol targeting, lot-size aware | TODO |
| P4-02 | Pre-trade limit checks + kill switch | TODO |
| P4-03 | Portfolio greeks + sector exposure + margin utilisation | TODO |
| P4-04 | Order state machine | TODO |
| P4-05 | Kite execution adapter | TODO |
| P4-06 | XTS execution adapter | TODO |
| P4-07 | Smart router + per-broker throttle | TODO |
| P4-08 | Reconciliation + orphan sweeper | TODO |
| P4-09 | Exit ladder + auto square-off | TODO |

**Exit:** paper-trades a trivial strategy live 5 sessions, zero reconciliation
breaks, invariants never trip.

---

## Phase 5 — Feature registry & arbitrage (8)

| Card | Deliverable | Status |
|---|---|---|
| P5-01 | Feature registry: versioned, hashed, horizon-banded | TODO |
| P5-02 | Feature builders — kinematics + distribution | TODO |
| P5-03 | Feature builders — book + flow | TODO |
| P5-04 | Feature builders — options + cross-asset + calendar | TODO |
| P5-05 | Cash–futures basis + cross-venue scanner | TODO |
| P5-06 | Put–call parity + box + butterfly scanner | TODO |
| P5-07 | Calendar spread scanner | TODO |
| P5-08 | Arbitrage opportunity log + post-cost edge report | TODO |

**Exit:** a full session logging every arbitrage opportunity with its post-cost
edge — the empirical answer to whether the edge exists.

---

## Phase 6 — Quant strategy + backtester (7)

| Card | Deliverable | Status |
|---|---|---|
| P6-01 | SIMD indicators: EMA, RSI, ATR, MACD, BB, Keltner, Supertrend | TODO |
| P6-02 | Ichimoku, Stochastic, Heikin-Ashi, pivots, market structure | TODO |
| P6-03 | Regime detector | TODO |
| P6-04 | Score engine, regime-conditional, multi-timeframe | TODO |
| P6-05 | Tick-level backtest replayer, cost-aware | TODO |
| P6-06 | Walk-forward harness + purged K-fold + embargo | TODO |
| P6-07 | Monte Carlo (GBM, Heston, bootstrap, jump-diffusion) + metrics | TODO |

**Exit:** ported strategy reproduces the predecessor's backtest within tolerance,
then is re-run under post-April-2026 STT rates.

---

## Phase 7 — Research pipeline (5) · *parallel from here*

| Card | Deliverable | Status |
|---|---|---|
| P7-00 | TradingView bar exports → `dataset/` partition + coverage validation — **DONE** · 17'327 bars, 41 partitions | **DONE** |
| P7-01 | Paper ingest → feature card → `registry.json` | TODO |
| P7-02 | Feature-card → C++ builder scaffold generator | TODO |
| P7-03 | Replication harness: pass/fail on your data | TODO |
| P7-04 | Paper → live promotion gate (replicated + post-cost edge) | TODO |

**Exit:** three papers ingested, implemented, replicated or rejected with evidence.

---

## Phase 8 — ML stack (12)

| Card | Deliverable | Status |
|---|---|---|
| P8-01 | Triple-barrier label builder | TODO |
| P8-02 | Dataset builder: tensor assembly from the tick archive | TODO |
| P8-03 | LibTorch training harness (loop, checkpoint, early stop, LR schedule) | TODO |
| P8-04 | LSTM model | TODO |
| P8-05 | GRU model | TODO |
| P8-06 | TFT model | TODO |
| P8-07 | MLP cross-sectional model | TODO |
| P8-08 | Temporal CNN + attention autoencoder (uneven-pattern discovery) | TODO |
| P8-09 | Model registry: immutable, versioned, hash-addressed | TODO |
| P8-10 | ONNX / TorchScript serving, warm + pinned, hot-swap | TODO |
| P8-11 | Aggregator: per-model × per-timeframe weights, horizon match | TODO |
| P8-12 | Confidence intervals + edge lower bound | TODO |

**Exit:** 10-minute forecast beats persistence out-of-sample **after costs**.
If it does not, that is a valid result — stop and say so.

---

## Phase 9 — Flagging, drift, auto-correction (7)

| Card | Deliverable | Status |
|---|---|---|
| P9-01 | Flag engine: per-model per-horizon per-regime scorecards | TODO |
| P9-02 | Scorecard store + session-close online weight update | TODO |
| P9-03 | Drift: PSI + KS | TODO |
| P9-04 | Drift: ADWIN + Page-Hinkley | TODO |
| P9-05 | Shadow deploy harness | TODO |
| P9-06 | Canary + auto-rollback | TODO |
| P9-07 | Quarterly scheduler + spec-change and drift triggers | TODO |

**Exit:** a deliberately poisoned model is auto-detected, de-weighted, and rolled
back with no human action.

---

## Phase 10 — Hedge book & VIX (8)

| Card | Deliverable | Status |
|---|---|---|
| P10-01 | Fundamentals ingest, point-in-time | TODO |
| P10-02 | DCF engine (FCFF + FCFE, three-stage) | TODO |
| P10-03 | Relative multiples + quality factor screens | TODO |
| P10-04 | Engle–Granger + Johansen cointegration | TODO |
| P10-05 | Pair selection + half-life + structural break detection | TODO |
| P10-06 | Sector and β neutralisation at portfolio level | TODO |
| P10-07 | VIX forecast model | TODO |
| P10-08 | Vega sizing + tail hedge | TODO |

**Exit:** sector-neutral book runs a month in paper with |β| < 0.1.

---

## Phase 11 — UI (14)

> **Split by the one-component-one-directory rule.** P11-01 is `server/` —
> backend only, renders nothing. P11-02..P11-14 are `client/` — desktop only,
> talks to the server over the wire protocol and links no engine header. No
> card in this phase may span both.

| Card | Deliverable | Status |
|---|---|---|
| P11-01 | uWebSockets server + binary delta frame protocol | TODO |
| P11-02 | SPA shell: tabs, theming, PIN lock, layout persistence | TODO |
| P11-03 | Grid core: virtual scroll, 1 M rows, incremental cell patch | TODO |
| P11-04 | Grid filters: type-aware, per column, chips, URL state | TODO |
| P11-05 | Grid sort, grouping, aggregation, pivot | TODO |
| P11-06 | Grid columns: pin, reorder, resize, groups, derived expressions | TODO |
| P11-07 | Grid formatting: conditional, heatmap, sparklines, flash-on-tick | TODO |
| P11-08 | Grid keyboard, range select, status bar, saved views | TODO |
| P11-09 | Export: CSV/TSV/JSON + native XLSX + clipboard + PDF | TODO |
| P11-10 | WebGL chart core: candlestick, volume, overlays, subplots, crosshair | TODO |
| P11-11 | Depth ladder + order-book heatmap + footprint | TODO |
| P11-12 | IV smile/surface + greeks-vs-strike + net gamma profile | TODO |
| P11-13 | Prediction cones, flag markers, replay scrubber | TODO |
| P11-14 | Panels: option chain + strategy builder, cost, scorecards, risk, audit | TODO |

**Exit:** full dashboard drives a live paper session at 60 fps with 1 M audit rows
in the grid and every export byte-correct.

---

## Phase 12 — Production (6)

| Card | Deliverable | Status |
|---|---|---|
| P12-01 | Linux deployment: CPU isolation, hugepages, io_uring, NIC tuning | TODO |
| P12-02 | Process supervision, crash recovery, warm restart from state | TODO |
| P12-03 | Monitoring + alerting (latency, drops, drift, PnL, invariants) | TODO |
| P12-04 | Daily reconciliation vs broker contract notes | TODO |
| P12-05 | Disaster recovery + position-flattening runbook | TODO |
| P12-06 | Go-live checklist + staged capital ramp | TODO |

**Exit:** live with 10% of intended capital.

---

## Reference findings — `gokiteconnect`, read 2026-08-31

Zerodha's own Go client, cloned shallow to `research/reference/gokiteconnect`
(gitignored — read for protocol facts, **not vendored**, and no code copied).
Two findings, both material.

**1. Kite has TWO time formats with DIFFERENT semantics, which validates P0-04's
per-source design.**

| Source | Wire form | `SourceTimeSpec` |
|---|---|---|
| Binary ticker (WebSocket) | `uint32` big-endian **seconds since the Unix epoch, genuine UTC** — `ticker.go` builds it with `time.Unix(...)`, which is UTC by definition | `{Seconds, Duration{0}, ist_naive = false}` |
| REST / CSV (orders, instruments) | zoneless strings `"2006-01-02 15:04:05"`, parsed with `ParseInLocation(..., Asia/Kolkata)` — i.e. **IST wall-clock with no zone** | effectively **`ist_naive = true`** once turned into an integer |

So the binary feed is *not* IST-naive and the REST path *is*. Hardcoding one
answer per broker — which P0-04 explicitly refused to do — would have been wrong
for whichever half it did not cover. Field offsets in full mode:
`exchange_timestamp` at `b[60:64]`, `last_trade_time` at `b[44:48]`, both
`uint32` big-endian; index-full puts the timestamp at `b[28:32]`.

Minor: those are **`uint32` seconds**, so the field itself survives to 2106 —
but anything that reads it into an `int32` breaks in 2038. P2-02 must widen on
read, which `to_utc(std::int64_t, ...)` already forces.

**2. The price scale is segment-dependent — see carried debt above.** This is
the more serious of the two and contradicts CLAUDE.md rule 3 as written.

---

## Standing test techniques

1. **Run the full tree under deliberate CPU load and capture every `FAIL`
   line.** Four spinners, several passes. This is what finally caught the
   P0-06b overshoot after 96 clean unloaded runs.
2. **No check description may contain the substring `FAIL`.** P1-06 briefly had
   one reading "a verdict read before reconcile() FAILS", which showed up as a
   phantom failure in technique 1 — and then did it again from a stale binary,
   proving the point twice in one session. Reworded to "is REFUSED".
3. **Rebuild before sweeping.** A stale `.exe` reports the previous revision's
   behaviour with total confidence.
4. **Put the evidence requirement in the loop's termination condition**, never
   in an assertion after it, or a loop that ran zero times passes vacuously.

---

## Blockers

| # | Blocks | Question | Owner |
|---|---|---|---|
| ~~8~~ | ~~No version control~~ | **RESOLVED 2026-08-31.** `git init`, 19 commits, one per card to PROTOCOL §9, ledger Commit column filled with real hashes. History is reconstructed (the work predates the repo) so intermediate commits are not individually buildable; only HEAD is verified. Commits from here are real. | — (closed) |
| **7** | **P0-08b, and every later card with a dependency** | **vcpkg is not installed.** `VCPKG_ROOT` is unset, there is no vcpkg tree on the box, and `builtin-baseline` in `vcpkg.json` is unpinned. P0-08b needs `tomlplusplus`; the `testing` feature (Catch2 + Google Benchmark) has never been switchable either, which is why all ten cards so far use a plain `int main()`. Until this is fixed, **no card with a dependency can have gates 1 or 4 verified**. Install vcpkg, set `VCPKG_ROOT`, pin `builtin-baseline` to a commit SHA. | **Smit** |
| **9** | **P1-02, P1-03, P1-05 — the last 3 unbuilt Phase 1 cards** | **STILL OPEN after the 2026-08-31 data drop** — that drop was *bar* data (OHLCV), which is a different artifact and unblocks nothing here. **One sample master file each.** NSE contract master + `fo_mktlots`, the BSE equivalent, and one XTS instrument master dump. Any recent day; content does not matter, the **exact byte layout** does — column order, header text, date format, whether numbers are rupees or paise, what an empty field looks like. A parser written against a guessed schema is the confident-plausible-wrong failure the protocol exists to catch, and no test can find it because the test would share the guess. Drop them anywhere; `data/masters/` is gitignored. | **Smit — files** |
| 2 | Phase 4 | Kite/XTS rate limits and OPS caps on your accounts | Smit — ask brokers |
| 4 | Phase 4 | Capital and per-strategy allocation | Smit |
| 5 | Phase 8 | GPU available for training? | Smit |
| 6 | Phase 8 | Historical L2 tick data source and depth | Smit |
| 1 | Phase 12 | Confirm prod OS (assumed Linux) | Smit |

---

## Phase gate log

| Phase | Date | Verdict | Notes |
|---|---|---|---|
| 0 | 2026-08-29 | **NO-GO** | 12 of 13 cards DONE, 711 assertions, every budget met. Four checklist items unevidenced. |
| 0 | 2026-08-31 | **NO-GO** (re-gate) | 13 of 14 DONE, 737 assertions across 13 targets. **Item 7 now PASSES** — `altair --replay` runs 500'000 ticks end to end at 54.23 M ticks/s with invariants armed. Three items remain, all needing tooling this box does not have. |

### Phase 0 gate record — 2026-08-29

Run against PROTOCOL §10. **Verdict: NO-GO.** Not because anything is broken —
everything built is green and within budget — but because four checklist items
cannot be evidenced, and passing a gate on unevidenced items is how a phase gate
stops meaning anything.

| § | Check | Result |
|---|---|---|
| 1 | Every card in the phase DONE | **NO** — P0-08b BLOCKED on vcpkg (blocker #7) |
| 2 | Clean build on Windows **and** Linux | **NO** — MSVC only; no GCC/Clang/WSL on the box |
| 3 | `ctest` green, no skipped tests | ✅ 711 assertions, 12 suites, 72 loaded runs, 0 failures |
| 4 | Latency suite within ROADMAP §11 | ✅ every budget met, table below |
| 5 | Replay regression vs golden outputs | ⚠️ **now possible, not yet established.** `--gen` and the replay are both deterministic — two runs of the same session produce identical output apart from wall time — so a golden can be captured. None has been. |
| 6 | Invariants armed over a full replayed session | ⚠️ armed and checked on every tick of a **500'000-tick synthetic** session; no real capture (blocker #6) |
| 7 | ROADMAP §12 exit criterion demonstrably met | ✅ **PASSES as of 2026-08-31.** `altair --replay` ran 500'000 ticks end to end, 54.23 M ticks/s, invariants held on every tick, exit 0 |
| 8 | Written go/no-go, deferrals recorded | ✅ this record |

**Latency, all measured on MSVC 19.51 x64 `/O2`, invariant TSC at 2.112 GHz:**

| Operation | Measured | Budget |
|---|---|---|
| `TscClock::now()` p99 | 18 ns | 25 |
| `Arena::allocate(32,8)` | 2.87 ns | 10 |
| `Pool::acquire`+`release` | 2.20 ns | 15 |
| `SpscRing` push+pop | 2.48 ns | 20 |
| `MpscRing` push+pop | 15.76 ns | 40 |
| `Seqlock::store` / `try_load` | 8.98 / 0.47 ns | 20 |
| `Logger::write(3 args)` | 23.14 ns | 40 |
| `ConfigStore::refresh` unchanged | 0.63 ns | 5 |
| `ConservationLedger::on_fill` | 13.40 ns | 20 |
| `Replayer::next()` | 3.61 ns | 10 |

**What must happen before Phase 0 can be re-gated:**

1. **Install vcpkg** (blocker #7). Unblocks P0-08b, and also the `testing`
   feature — Catch2 and Google Benchmark have never been switchable, which is
   why all twelve cards use a plain `int main()`.
2. **Build on Linux with GCC or Clang.** `-Wall -Wextra -Wpedantic -Wconversion
   -Wsign-conversion -Wold-style-cast` is unverified across every card, and
   P0-03's portable 128-bit multiply is dead code that no compiler here reaches.
3. **Run `core/lockfree` under TSAN.** The memory orderings in P0-06a/b are
   *unvalidated* — x86 is TSO, so a wrong ordering is invisible on this hardware
   and would surface only on ARM. This is the single largest correctness risk in
   the phase.
4. ~~An `app/` target with `--replay`.~~ **DONE 2026-08-31** (P0-10). Item 7
   passes. Items 5 and 6 still need a **captured session** (blocker #6) — the
   synthetic generator makes the criterion runnable, not the regression real.
   A golden output can now be captured whenever you want one; the replay is
   deterministic.

**What is genuinely done:** eleven modules, 3'800 lines of implementation and
3'700 of tests, every one written as a task card first and reviewed against the
eight gates. The type system enforces dimensional units, affine time, and
structural no-look-ahead. Conservation is checked in exact integer paise on
every tick. Nothing on the hot path allocates.
