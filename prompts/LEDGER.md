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
| ~~**P0-09b → P2-01**~~ **RESOLVED 2026-08-31** | Closed by P2-01. The normalised `Tick` carries `InstrumentId` and **has no token field at all**; `source` records which feed produced it as a diagnostic. Test 2 puts one contract in a `SpecStore` with two unrelated broker tokens and asserts both resolve to the same `InstrumentId`. `feed/replay.hpp`'s `ReplayTick` still carries the bare `token` — migrating the replayer to emit `Tick` is P2-06/P2-07, and the warning block stays in that header until it does. Original text: **A broker token is not an instrument key.** `ReplayTick.token` is a bare `uint32` documented as "a token". Kite's `instrument_token` and XTS's `ExchangeInstrumentID` are **different number spaces for the same contract**, so that field means different things depending on which feed produced the tick. P2-05 switches the primary feed mid-session: with a broker token in the tick, every instrument would appear to vanish and a stranger appear in its place, and the book, ledger and strategies would all follow it. The normalised `Tick` must carry the canonical `InstrumentId` from the spec store (ROADMAP §6.2); decoders map (source, broker_token) → InstrumentId and nothing downstream ever sees a broker token. The Phase 0 skeleton is safe only because it is single-source by construction. **Warning is in the header at the field.** | **P2-01 + P2-04** |
| ~~**P1-01**~~ **PARTLY RESOLVED 2026-08-31** | ~~**`ContractSpec` has no home for broker tokens.**~~ P1-01 gave it `token[kFeedSourceCount]` indexed by `FeedSource`, and **P1-06 is where the map is actually assembled**: the merged spec takes `token[Kite]` from the Kite source and `token[Xts]` from the XTS source, so the mapping is bidirectional through the store (`id_of(src, token)` and `token_of(id, src)`) and nothing compares a token across sources. What remains is only that the XTS half is untestable against real data until P1-05 has a sample file. Original text: **`ContractSpec` has no home for broker tokens.** ROADMAP §6.2 gives it an `InstrumentId` but no `kite_token` / `xts_instrument_id`. P1-04 and P1-05 are named "token map" cards so the intent exists, but P1-01 must decide *where* the mapping lives — fields on the spec, or a separate table — and it must be **bidirectional**: decode needs token→id, subscription needs id→token. | **P1-01** |
| ~~**P1-04 → P2-04**~~ **RESOLVED 2026-08-31** | The loop is now a sink-templated `detail::load_kite_dump_into`, with `load_kite_dump(…, Reconciler&, …)` as the correct path and `load_kite_dump_unreconciled(…, SpecStore&, …)` retained for single-source tests and named for what it skips. `KiteLoadReport::rejected_by_store` became `rejected_by_sink` — there are two sinks now and only one is a store. A test loads a dump into a Reconciler, adds a disagreeing NSE row, and asserts the symbol **blocks** rather than Kite quietly winning. Original text: **`load_kite_dump` writes the store directly; every later parser writes the Reconciler.** P1-04 predates P1-06, so it calls `SpecStore::add` — which means a Kite dump can populate the store without ever facing the three-way check. The four master-parser cards written 2026-08-31 all take `Reconciler&` instead, deliberately: a parser that *can* write the store is a parser that can bypass reconciliation. `load_kite_dump` needs a `Reconciler&` overload, and the direct-to-store form should become test-only. Not urgent while Kite is the only source; **must land before a second source does.** | **P1-05 or P2-04** |
| **P1-02a → risk / OMS** | **NSE F&O tick sizes are NOT uniformly 5 paise.** Real reconciled values from the 2026-08-31 files: NIFTY future **10**, BANKNIFTY **20**, ABB **50**, ABCAPITAL **5**. Any code carrying a 5-paise literal is wrong for most contracts, and an order rounded to the wrong tick is rejected by the exchange or fills at a price nobody intended. This is rule 1 earning its place: the tick comes from the spec store, per contract, always. Note also that **UDiFF carries no tick column at all**, so the value reaches the merged spec from the BROKER via P1-06's D10 — which is precisely the cross-check the three-way design exists for. | **P3 + P4** |
| **P2-08 → P2-09 / strategies** | **A crossed book is NOT an error, and rejecting one breaks every pre-open.** The P2-08 skeleton originally said a crossed or locked book should be refused. That is wrong: during NSE's pre-open call auction orders are collected **without matching**, so the book legitimately crosses while the equilibrium price is discovered. Refusing would leave the previous continuous-session book in place and present it as current — stale data masquerading as live, which is strictly worse than crossed data correctly labelled. The book now **stores and marks**; `is_tradable()` is the single question a consumer asks. P2-09 must gate every microstructure signal on it — an imbalance or microprice computed from a crossed book is arithmetic on a state that cannot be traded. | **P2-09 + P5/P6** |
| **P2-11 → P4 (OMS)** | **Kite's two price representations are INVERSES of each other, and nothing in the type system catches a mix-up.** The WebSocket sends **integer paise**; the REST order API takes **decimal rupees** — `OrderParams.Price` is a Go `float64` serialised by `query.Values` with no scaling at all. A price read off a tick as `250005` must go back out as `"2500.05"`. Getting it backwards is 100x in either direction and **does not look like a bug**: an order at 100x is rejected by the price band, one at 1/100th simply rests in the book looking patient. `kite::kite_paise_to_rupees` is the only sanctioned conversion, never forms a double, and is proven to be the exact inverse of P1-04's parser across every hundredth and both int64 extremes. **The OMS must never format a price any other way.** | **P4** |
| **P2-10 → live** | **A Kite session cannot be established without an interactive browser login.** Step 2 of the handshake needs a Zerodha user ID, password and TOTP, and no part of Altair asks for or can supply those. Smit performs the login, copies the `request_token` out of the redirect URL, and hands it to the exchange step. The `access_token` that comes back is a **daily** artifact — it expires the next morning, so it is never config and never an environment variable; it lives in `data/kite_session.json` (gitignored). The exchange POST itself is P2-10c, blocked on vcpkg. | **Smit + blocker 7** |
| **P2-04 → P2-05** | **`PlausibilityGate` is a per-SOURCE object and must never be shared with `require_monotonic` on.** It holds one `last_`. Instruments tick independently — NIFTY at 10:00:05 then BANKNIFTY at 10:00:04 is ordinary — so a shared monotonic gate rejects a large fraction of good ticks and looks exactly like a broken feed. `Normaliser` now **forces the flag off in its constructor** rather than trusting the caller, and tracks per-instrument ordering itself. P2-05 must not re-enable it when it wires the second source, and if it ever gives each source its own gate, each gate still keeps monotonic off. | **P2-05** |
| **P2-02 → P2-05 / P3** | **Kite's exchange timestamp is ONE-SECOND resolution, and two of its five modes carry none at all.** `b[60:64]` is a Unix time in whole seconds, so **sub-second exchange latency is not measurable from Kite at any point** — only `recv_ts` has nanosecond resolution and it measures our receipt, not the exchange. LTP and quote modes carry no timestamp whatsoever and are marked `TickFlag::NoExchangeTs`. Consequences: P2-05's stall detection must key on `recv_ts`, not `exchange_ts`; any exchange-latency metric must exclude `NoExchangeTs` ticks or it computes zero by construction; and ROADMAP §3's Nyquist argument stands on `recv_ts` alone. This is a property of the feed, not a defect to fix. | **P2-05 + P3** |
| **P7-00 → P6 / P8** | **The first bar-data drop has a hole exactly where the flagship strategy is graded.** NIFTY 1-minute is missing **15:16–15:27 on every day** (12 consecutive minutes, 4 of 4 days), while India VIX has all 375. That window is where a 10-minute-horizon label lives and where NIFTY and VIX stop being joinable bar for bar. **Do not interpolate** — a fabricated close-window bar is a fabricated measurement in the exact window the model is scored on. Either source those minutes from Kite historical or make every close-window label explicitly unavailable. Three smaller ones: India VIX daily `open` equals the previous close on 524/526 bars (a copy, not a measurement); NIFTY daily has 1'408 zero-volume backfilled bars before 1997-01-01; and TradingView row caps mean NIFTY 1m covers **4 trading days**, not four months. Full detail in `prompts/P7-00_dataset_bar_ingest.md` §6. | **P6 + P8** |
| ~~**P1-04 → P2-04**~~ **RESOLVED 2026-09-01 by P1-09** | The universe filter runs at `Reconciler::add`, before anything is stored, so an unfiltered dump can no longer decide the universe by row order. Measured on the real files: 106'150 + 30'488 rows in, 888 admitted, 444 contracts, all agreed. Original text: **`load_kite_dump` will overflow the store on a real dump.** `SpecStore::kMaxInstruments` is 8'192; the live Kite `instruments.csv` is ~100'000 rows. The throughput test makes this concrete: 10'000 rows in, **8'192 added, 1'808 rejected**. The loader behaves correctly — it reports `rejected_by_store` rather than failing — but a caller that hands it the raw dump gets a **silently truncated universe**, and which 8'192 survive depends on CSV row order. Nothing may hand it an unfiltered dump. The universe filter (config `[universe]`) must run **before** `add`, or `kMaxInstruments` must be raised to cover the whole dump (~100k × 176 B = ~18 MB, plus a 2x token index — affordable, but it makes the store a different object). Decide in P2-04. | **P2-04** |
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
| P0-08b | [`P0-08b_toml_source.md`](P0-08b_toml_source.md) | `core/config/toml_source` — TOML → snapshot loader | **DONE** | 1✓ 2✓ 3✓ 4✓ 5 n/a 6 n/a 7✓ 8 n/a | — |

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

## Phase 1 — Instrument master (10)

| Card | Deliverable | Status |
|---|---|---|
| P1-01 | `ContractSpec` type + point-in-time spec store — **DONE** `bf732a6` · 1✓ᵐ 2✓ 3✓ 4✓ 5✓ **6✓** 7✓ 8✓ · 57 checks | **DONE** |
| P1-02a | NSE F&O UDiFF bhavcopy → `ContractSpec` — **DONE** · `udiff_master.hpp` · 43 checks | **DONE** |
| P1-02b | NSE `fo_mktlots.csv` → lot-size table — **DONE** · months resolved from the header, blank ≠ zero · 34 checks | **DONE** |
| P1-02c | NSE `EQUITY_L.csv` → cash `ContractSpec` — **DONE** · series filter, quote-safe splitter · 32 checks | **DONE** |
| P1-03a | BSE derivatives master — **DONE**, and it is the SAME parser: BSE publishes the identical UDiFF layout. Only the stamped `Exchange` differs. (BSE **cash** scrip master remains a separate artefact.) | **DONE** |
| P1-04 | Kite instruments dump parser + token map — **DONE** · 1✓ᵐ 2✓ᶜ 3✓ 4✓ 5✓ 6✓ 7✓ 8✓ · 94 checks | **DONE** |
| P1-05 | XTS instruments master parser + token map | TODO |
| P1-06 | Three-way reconciler + disagreement flags + symbol blocking — **DONE** · 1✓ᵐ 2✓ᶜ 3✓ 4✓ 5✓ 6✓ 7✓ 8✓ · 77 checks | **DONE** |
| **P1-09** | Universe filter — **DONE** · runs at `Reconciler::add`, the single gate · 40 checks | **DONE** |
| P1-07 | Margin fetch (SPAN + ELM) + change detection → retrain trigger | BLOCKED — creds |
| **P1-08a** | Snapshot age + expiry-crossing + download-failure policy — **DONE** · 1✓ 2✓ᶜ 3✓ 4✓ 5✓ 6✓ 7✓ 8✓ · 50 checks | **DONE** |
| P1-08b | Master downloader (atomic replace, parse-verify) — card written | READY — needs the vcpkg `net` feature built (boost-beast + openssl), no longer blocked on vcpkg itself |

**Exit:** a full session's universe auto-loads pre-open with zero hardcoded lot
sizes; a deliberately corrupted source is caught and blocks only its symbol.

---

## Phase 2 — Feed & book (14)

| Card | Deliverable | Status |
|---|---|---|
| P2-01 | `Tick` / `DepthUpdate` normalised structs + wire schema — **DONE** · 1✓ 2✓ᶜ 3✓ 4✓ 5✓ 6✓ 7✓ 8✓ · 51 checks | **DONE** |
| P2-02 | Kite binary decoder — **DONE** · 1✓ 2✓ᶜᶜ 3✓ 4✓ 5✓ **6✓ 70 ns/packet** 7✓ 8✓ · 71 checks | **DONE** |
| P2-03 | XTS Socket.IO 1501/1502/1505 decoder | TODO |
| P2-04 | Normaliser: gating, per-instrument ordering, source routing — **DONE** · 1✓ 2✓ᶜ 3✓ 4✓ 5✓ **6✓ 11.4 ns/submit** 7✓ 8✓ · 47 checks. (The token→`InstrumentId` binding is in P2-02 — a decoder that cannot resolve a token cannot make a `Tick` at all.) | **DONE** |
| P2-05 | Failover watchdog + seamless primary switch — **DONE** · keys on `recv_ts` · 38 checks | **DONE** |
| P2-06 | Session tick store — writer — **DONE** · 65 B/tick, byte-exact round trip | **DONE** |
| P2-07 | Tick store reader — **DONE** (shipped with P2-06; the Parquet archiver is deferred to P6, where a research query actually needs it) | **DONE** |
| P2-08 | L2 order book, O(1) update, crossed-book handling — **DONE** · 1✓ 2✓ᶜ 3✓ 4✓ 5✓ **6✓ 29.4 ns/apply** 7✓ 8✓ · 48 checks | **DONE** |
| **P2-09a** | OBI, weighted OBI, microprice — **DONE** · 1✓ 2✓ 3✓ 4✓ 5✓ 6✓ 7✓ 8✓ · 43 checks | **DONE** |
| P2-09b | VPIN + Kyle λ — **DONE** · volume-bucketed, through-the-origin, both report sample size · 45 checks | **DONE** |
| P2-09c | Queue position — needs live order state; really Phase 4 | TODO — deferred to P4 |
| **P2-10a** | `broker/sha256.hpp` — FIPS 180-4, NIST-verified, cross-checked against `hashlib` — **DONE** · 24 checks | **DONE** |
| **P2-10b** | `broker/kite_session.hpp` — login URL + session checksum — **DONE** | **DONE** |
| P2-10c | `/session/token` POST + `data/kite_session.json` persistence — **DONE** · vcpkg `net` built (62 pkgs, 15 min) · `broker/https_client.hpp` + `broker/kite_login.hpp` + `altair_kite_login` · TLS verified end-to-end against api.kite.trade (HTTP 400 on a bogus token) · Python stopgap DELETED | **DONE** |
| **P2-11** | `broker/kite_api.hpp` — the whole Kite surface transcribed from gokiteconnect **v4.4.2**, plus the paise↔rupees converter — **DONE** · 31 checks | **DONE** |

**Exit:** 6 h of live NIFTY + BANKNIFTY captured, replayed bit-identically, zero
drops, p99 decode < 3 µs.

---

## Phase 3 — Analytics & cost (10)

| Card | Deliverable | Status |
|---|---|---|
| P3-01 | Black-76 price + 1st-order greeks — **DONE** · erfc tail, finite-difference verified · 44 checks | **DONE** |
| P3-02 | 2nd/3rd order greeks — **DONE** · every greek finite-differenced against P3-01 · vanna verified BOTH ways (Clairaut) · charm is the one that is NOT right-independent, by exactly r*df · 50 checks | **DONE** |
| P3-03 | Bjerksund–Stensland American pricer — **DONE** · verified against a 1500-step binomial · LOWER BOUND always, worst case 0.28 of a tick · **not on any NSE path: NSE options are European** · 26 checks | **DONE** |
| P3-04 | IV solver + **Black-Scholes on spot** — **DONE** · safeguarded Newton on the same pricer, no duplicated formula · refuses outside no-arb bounds · returns vega as the error propagator · 61 checks · 1.6 µs/solve | **DONE** |
| P3-05a | SVI slice + butterfly/calendar arbitrage checks — **DONE** · g<0 shown to be a NEGATIVE BUTTERFLY PRICE, 42/42 agreement · density integrates to 1.0 · 46 checks | **DONE** |
| P3-05b | SVI calibration, quasi-explicit — **DONE** · inner (a,c,d) linear + Nelder-Mead on (m,log sigma) · weights from P3-04's error bar, 97x better at the money · 43 checks | **DONE** |
| P3-06a | Rolling stats: mean, var, skew, kurt, z — **DONE** · Welford + Pebay, two-pass window · naive formula shown reporting 4.9x too much vol on a quiet high-priced counter · 55 checks | **DONE** |
| P3-06b | EWMA + EWMA variance, decayed in TIME — **DONE** · fixed-alpha shown spreading 90.5 price points on identical data · variance bias-corrected (was 76% low) · 45 checks | **DONE** |
| P3-06c | Hurst by rescaled range — **DONE** · Anis-Lloyd corrected (uncorrected reads +0.057 high at n=512) · std error floored at 1/sqrt(n) after it flagged pure noise as significant · 29 checks | **DONE** |
| P3-07 | Multi-scale derivatives — **DONE** · local polynomial, not smooth-then-difference · naive two-point difference shown reporting 500 paise/s on a series that is NOT MOVING · 34 checks | **DONE** |
| P3-08 | India VIX replication — **DONE** · model-free variance swap · flat 15-vol recovers 15.0028 · agrees with the SVI surface to 0.0025 sharing no code · 47 checks | **DONE** |
| P3-09 | **Cost calculator** — **DONE** · exact 128-bit integer paise, premium-vs-notional, side-aware · 46 checks · measured 5.95 bps round trip | **DONE** |
| P3-10 | Slippage learned from fill history — **DONE** · square-root impact law · fitting on FILLS ONLY understates cost 0.635 vs 0.900 truth · sizes on the pessimistic bound · 43 checks | **DONE** |

**Exit:** full-chain greeks < 50 µs; cost calculator matches a real contract note
to the paisa on 50 historical trades.

---

## Phase 4 — Risk, OMS, paper trading (9)

| Card | Deliverable | Status |
|---|---|---|
| P4-01 | **Policy numbers are Smit's, not the engine's.** `SizingInputs` now defaults every policy field to ZERO and `size_position` refuses zero, so the sizer cannot run until risk_fraction, kelly_divisor, target_vol, max_leverage and edge_sigmas are supplied. They belong in `config/` and are the trade handler's to set. | Smit |
| P4-01 | Sizing: fixed-fractional + ¼-Kelly + vol targeting — **DONE** · the MINIMUM binds and is named · rounds DOWN to whole lots · edge inside its error bar sizes to zero · 56 checks | **DONE** |
| P4-02 | Pre-trade limits + kill switch — **DONE** · every check runs, full violation mask · sticky switch needing an explicit token · conservation in exact paise (double misses a 1-paisa breach) · 49 checks | **DONE** |
| P4-03 | Portfolio greeks + sector + margin — **DONE** · aggregates in MONEY, not raw greeks · predicts a real reprice to 0.08% while the raw delta sum has the OPPOSITE SIGN · vega bucketed by expiry · 35 checks | **DONE** |
| P4-04 | Order state machine — **DONE** · `oms/` opened · duplicates are traffic, stale fills never walk the position back, a fill AFTER cancel-ack is accepted · 56 checks | **DONE** |
| P4-05 | Kite execution adapter — **DONE** · translation only, no transport, so it tests with no network · an UNKNOWN STATUS IS REFUSED (no default arm) · partial fill derived from quantities, Kite has no status for it · 51 checks | **DONE** |
| P4-06 | XTS execution adapter | **DEFERRED** — Smit set XTS aside ("avoid xts master sample for now"). No XTS spec sample, no credentials, and `oms/throttle.hpp` REFUSES `Venue::Xts` rather than falling back to Kite, so nothing routes there by accident until this lands |
| P4-07 | Router + per-broker throttle — **DONE** · three windows, tightest binds and SAYS WHICH · 2 orders/sec exhausts the DAILY budget in 25 min while the fast limits never bind · unavailable venue REFUSED, never a silent fallback · 31 checks | **DONE** |
| P4-08 | Reconciliation + orphan sweeper — **DONE** · an ORPHAN trips the kill switch, a ghost does not · quantity exact, price toleranced · unsorted input REFUSED (would fake orphans) · 37 checks | **DONE** |
| P4-09 | Exit ladder + auto square-off — **DONE** · RULE 8 enforced and MEASURED: tighter-stop-first is worth Rs 1,50,000 on one gap · stops can only tighten · clock beats target · 38 checks | **DONE** |

**Exit:** paper-trades a trivial strategy live 5 sessions, zero reconciliation
breaks, invariants never trip.

---

## Phase 5 — Feature registry & arbitrage (8)

| Card | Deliverable | Status |
|---|---|---|
| P5-01 | Feature registry — **DONE** · the hash covers ORDER, not just the set (a model's weights are positional) · horizon bands ENFORCED, not advisory · sealed is immutable · 41 checks | **DONE** |
| P5-02 | Feature builders: kinematics + distribution — **DONE** · an unready feature is ABSENT, not zero · warmup per FEATURE not per vector · Hurst fed returns, once, where it shows · 35 checks | **DONE** |
| P5-03 | Feature builders: book + flow — **DONE** · an empty book is NOT a balanced book, it fills NOTHING · a crossed book is data: flag present, levels absent · sample counts travel with the estimates · 24 checks | **DONE** |
| P5-04 | Feature builders: options + cross-asset + calendar — **DONE** · KNOW WHAT WRAPS: the session clock does NOT (a circle puts the open and the close 0.000 apart), the expiry cycle DOES (linear puts consecutive sessions 4x too far) · the trading calendar is passed IN, never derived from a timestamp · the IV band travels as its own feature: 25x wider in the wing, 2070x if the chain is flat · correlation on returns, never levels (0.492 vs 0.031 on independent walks) · 34 checks | **DONE** |
| P5-05 | Cash–futures basis + cross-venue scanner — **DONE** · HALF THE DIRECTIONS CANNOT BE TRADED: no short cash delivery in India, so the reverse carry is arithmetic and not edge · executability travels with every result, ordinal 0 is Unknown · no gross-edge field exists (rule 5) · **measured: a carry breaks even at 9.2% implied repo against a 6.5% real rate — a 270bp hurdle; a 60-paise NSE/BSE cross nets MINUS Rs 284** · a Rs 12 basis on a 2950 spot is a Rs 1.85 DISCOUNT · 33 checks | **DONE** |
| P5-06 | Put–call parity + box + butterfly scanner — **DONE** · **the textbook butterfly assumes an even ladder and NIFTY’s is not**: on a flat, convex, arbitrage-free chain the unweighted form reports **Rs 34.95 of fictional free money** at every 50→100 step change; the weighted form reports +4.06 · parity at the TOUCH, never the mid (the header has no mid accessor) · parity vs a FUTURE is symmetric, vs SPOT it inherits P5-05 · an unnamed hedge is REFUSED, not costed against a guess · **a box lends at 6.53% at the mid and −17.69% at the touch — 24 points of rate out of Rs 4.00; −88.5% at six days** · 40 checks | **DONE** |
| P5-07 | Calendar spread scanner — **DONE** · the calendar condition is at fixed **k**, so fixed k means a DIFFERENT STRIKE on each expiry; iterating strikes and reading each slice at its own moneyness IS the strike-aligned test wearing the other label (an early draft had the branches swapped — the test caught it) · **measured: index calendar 100.6% of margin (second order, reported as such); a stock going ex-dividend HIDES 81 of 401 genuine violations** — hiding, not inventing, which is the worse failure · falling VOL is not falling VARIANCE (30→18 vol is +209% variance) · a futures calendar is symmetric, like the box · 28 checks | **DONE** |
| P5-08 | Arbitrage opportunity log + post-cost edge report — **DONE** · **a log of winners cannot answer the phase’s question**: NoMispricing / CostExceedsEdge / Unreachable / StaleQuotes all present as an empty file, and point at the market, the schedule, the ACCOUNT and the FEED respectively · gross lives in the log and on no decision type (rule 5 intact) · exact aggregates + uniform RESERVOIR sample: 45.5% late vs an overwriting ring’s 100% and a stop-when-full log’s 0% · **winner’s curse calibrated: 200k pure-noise scans give a 4.6σ “best opportunity” at ratio 1.03 of what noise predicts** · 24 checks | **DONE** |

**Exit:** a full session logging every arbitrage opportunity with its post-cost
edge — the empirical answer to whether the edge exists.

---

## Phase 6 — Quant strategy + backtester (7 → 5 live, 2 withdrawn)

| Card | Deliverable | Status |
|---|---|---|
| P6-01 | ~~Indicator kernels: EMA, RSI, ATR, MACD, BB, Keltner, Supertrend~~ — **WITHDRAWN 2026-09-02, Smit.** Traced to the predecessor's `strategy.py` (113 EMA / 89 ATR / 85 MACD / 52 RSI / 25 pivot / 13 Supertrend mentions). Altair does not trade on technical parameters. `analytics/indicators.hpp` and its 22 checks deleted. The findings stand as review knowledge — Wilder's α is 1/N not 2/(N+1); true range includes the gap — but nothing in the tree computes them. The fixed-α EWMA it carried was in any case a worse duplicate of P3-06b's time-based `analytics/ewma.hpp`. | **WITHDRAWN** |
| P6-02 | ~~Ichimoku, Stochastic, Heikin-Ashi, pivots, market structure~~ — **WITHDRAWN 2026-09-02, Smit.** Same reason: `analytics/structure.hpp` and its 25 checks deleted. The look-ahead discipline it demonstrated is NOT withdrawn — `describes` vs `known_at` as separate fields is the pattern P6-05 and P6-06 enforce structurally, and the Chikou measurement (Rs 1,367 honest vs Rs 5,698 peeking, on a random walk) remains the clearest statement of rule 7 in the record. | **WITHDRAWN** |
| P6-03 | Regime detector — **DONE** · **a regime boundary is a threshold on a NOISY estimate**, and near the boundary is exactly where a conditional model switches behaviour · **measured on 4000 bars of a PURE RANDOM WALK: the naive rule labels 3,872 regimes (100%), the error bar leaves 273 (7.1%)** — and not zero, because a 2σ test fires ~5% on a true null by construction · on a genuinely persistent series it still says Trending on 3,868 of 3,872 · hysteresis + dwell cuts regime changes 119 → 25 · vol SE is 9.2% of the estimate at n=60, so a threshold 5% away is inside the bar · causal only, no method takes a series · thresholds have NO defaults · 22 checks | **DONE** |
| P6-04 | Score engine, regime-conditional, multi-timeframe — **DONE** · **ten correlated signals are not ten signals**: an ordinary 10-member EMA stack (periods 5–165, one series) MEASURES ρ̄ = 0.781 → **n_eff 1.25 of 10, naive SE understates by 2.83x** · sizes on the LOWER BOUND, and a score whose interval straddles zero does not fire · no gross field exists (rule 5) — cost is subtracted before the number is constructed · an incomplete regime is REFUSED, never falls back to the unconditioned model · correlation and the paise-per-score scale have no defaults · unready signals are excluded, not zeroed · 24 checks | **DONE** |
| P6-05 | Tick-level backtest replayer, cost-aware — **DONE** · **the fill model IS the backtest** · **measured: the same edgeless strategy, same tape, 400 fills — filling at the MID is worth Rs 1,499.82, exactly Rs 3.75/fill = the 5-paise half-spread × the 75 lot** · **a limit order with 600 units ahead fills NOTHING across 300 units traded at its price, where a front-of-queue order is done in three prints; the naive “touched = filled” rule books 399 fills on one order** · `FillModel::Unspecified` = 0 refuses to run · conservation checked EVERY fill by two independent routes, exact in paise · the strategy’s on_tick has no tape, index, length or peek — rule 7 is structural · 27 checks | **DONE** |
| P6-06 | Walk-forward harness + purged K-fold + embargo — **DONE** · **contiguous 5-fold leaks 0.39% of training samples; SHUFFLED 5-fold leaks 100%, and purging it honestly leaves ZERO usable training data** — proof the split was never valid · **1-NN on data with no relationship: shuffled reports +0.1656 out-of-sample correlation, purged contiguous +0.0041** · the embargo closes a path purging leaves open (features look back INTO the test block) and has no default · walk-forward gap ≥ horizon enforced; expanding vs rolling are different claims, not a knob · 26 checks | **DONE** |
| P6-07 | Monte Carlo (GBM, Heston, bootstrap, jump-diffusion) + metrics — **DONE** · **an IID bootstrap keeps the mean, spread and Sharpe to three decimals and loses the DRAWDOWN** — a drawdown is a property of the ORDER of returns and nothing else · **max drawdown scales as sqrt(n): 5.00% → 10.13% → 19.90% over 250/1000/4000 steps, ratio 3.98 against a theoretical 4.00** — so two backtests of different lengths cannot be compared on it · **sqrt(252) assumes independence; Lo (2002) puts the naive annualised Sharpe 52.5% too high at ρ=0.4**, and too LOW for a mean-reverter · Itô correction + jump compensator applied (jumps change RISK, not drift) · full-truncation Heston stays finite with Feller violated · Sortino divides by n, not by the downside count · block length has no default · 27 checks | **DONE** |

**Exit:** ported strategy reproduces the predecessor's backtest within tolerance,
then is re-run under post-April-2026 STT rates. *(Harness complete; the port
itself waits on a real tape.)*

---

## Phase 7 — Research pipeline (5) · *parallel from here*

| Card | Deliverable | Status |
|---|---|---|
| P7-00 | TradingView bar exports → `dataset/` partition + coverage validation — **DONE** · 17'327 bars, 41 partitions | **DONE** |
| P7-01 | Paper ingest → feature card → registry — **DONE** · **the registry counts ATTEMPTS, not successes**: 50 ingested, 10 trials, 1 replicated, and the 9 rejected STAY — a registry that dropped its failures would hand the deflated Sharpe a 1 and report the survivor as significant · a claimed effect with no venue/universe/period is REFUSED — a 1965-2009 US futures Sharpe is a fact about that market and a HYPOTHESIS about NSE 2026 · lifecycle is an enumerated state machine, so Ingested→Promoted is impossible however promising it looked | **DONE** |
| P7-02 | Feature-card → C++ builder scaffold generator — **DONE** · generates the CONTRACT (slots, band, warmup, `std_error`, the kSkip absence discipline) and **stops at the transform with an `#error`** · a generated scaffold that COMPILES is one that can ship with a plausible confident wrong transform inside it — the compile error means the only way to a building binary is for somebody to have read the paper · a buffer too small is refused rather than truncated (truncation could lose the `#error`) | **DONE** |
| P7-03 | Replication harness — **DONE** · **the same Sharpe of 1.50 over 1000 observations is 1.0000 deflated after 5 trials and 0.0000 after 20** — the null bar rises 0.000 → 1.193 → 1.901 → 2.276 → 2.766 across 1/5/20/50/200 trials · **non-normality decides marginal cases: at SR 2.50 the deflated value is 0.9998 for normal returns and 0.9591 at skew −1.2, kurtosis 9** · `preregister` before `evaluate` is a state machine, not a convention — a threshold chosen after seeing the result is not a threshold, and it cannot be registered twice | **DONE** |
| P7-04 | Paper → live promotion gate — **DONE** · a CONJUNCTION of four terms — replicated status, deflated-significant, out-of-sample, net-of-cost edge — each able to veto, and the gate returns WHICH · “did not promote” is four different pieces of news and only one of them means the idea was wrong · ordinal 0 is Unknown on both enums, so zeroed evidence does not promote · 33 checks across P7-01..04 | **DONE** |

**Exit:** three papers ingested, implemented, replicated or rejected with evidence.

---

## Phase 8 — ML stack (12)

| Card | Deliverable | Status |
|---|---|---|
| P8-01 | Triple-barrier label builder — **DONE** · **the FIRST TOUCH wins**: two independent existence tests with the target preferred label a path that stopped out on bar 2 and recovered on bar 6 as a WIN — **802 of 2,852 labels flip, 28.1%** · this is CLAUDE.md hard rule 8 in the labelling path (the ordering bug that cost ~₹41K), and worse, because the model trained on it goes looking for more of them · within one bar the STOP is assumed · **the label window ends at the TOUCH: mean 2.92 bars against a 30-bar horizon, so purging on the horizon over-purges by 926%** · **2,000 consecutive labels are 558.9 effective observations, 0.279 each — deflated Sharpe fed the row count overstates evidence 3.6x** · fixed-width barriers are a REGIME label (11.5% vs 100% touched across regimes) · vertical policy has no default · 26 checks | **DONE** |
| P8-02 | Dataset builder: tensor assembly — **DONE** · **a scaler fitted on the whole dataset leaks the test set into every TRAINING row**: on a feature shifting +3.0 at the boundary the leaky fit gives mean +0.9497 / sd 1.7123 against an honest +0.0376 / 1.0018, and **every training row changes — mean |gap| 0.5683, worst 1.9552** · nothing is copied and the fold boundary is identical, which is why it survives every other precaution · `fit` before `transform`, no refit, and out-of-range transform requires saying `applying_to_holdout` · **a tensor has no way to say ABSENT**, so MissingPolicy has no default and zero-fill is not offered — DropRow removes 60/600 warmup rows non-randomly, MaskAndImpute keeps them with a mask · imputation uses the FITTED mean, after the fit, or it is the same leak in a different coat · sample weights are P8-01 uniqueness (600 rows = 240 effective) · shapes checked, softmax subtracts the max (finite at logit 800), init is seeded and reproducible · 30 checks | **DONE** |
| P8-03 | LibTorch training harness (loop, checkpoint, early stop, LR schedule) — **DONE** (plain C++23 behind a stable interface; LibTorch backend drops in) · **early stopping turns the validation set into training data**: keeping the best of E epochs is a selection over E candidates, so on PURE NOISE with a model that does not learn the reported validation MSE sits **0.0015 / 0.0138 / 0.0175 BELOW the true 1.2134 at 10 / 50 / 200 epochs** — same extreme-value arithmetic as P5-08 and P7-03, applied to epochs · the fix is a third split touched ONCE: 0.8953 against its own true 0.8861, gap **+0.0092 — above**, because nothing was selected on it · `consume_test` refuses a second read · best checkpoint restored, not the last (best was epoch 55 of 60) · sample weights applied in the loss · schedule has no default · 24 checks | **DONE** |
| P8-04 | LSTM model — **DONE** (forward pass exact; ridge readout over a fixed reservoir, BPTT is what a LibTorch backend adds — stated in the header, not implied) · **the forget-gate bias decides whether there is any long-range memory, and ZERO is what you get by not choosing**: half-life 1.00 step at bias 0 against 2.21 at bias 1, and **9.5e-07 vs 1.9e-03 retained after 20 steps — 1994x**, from one constant · `init` takes the bias as a required argument, no overload without it · gates written out rather than packed, because the packed form is where the forget bias silently ends up at zero · h stays in [-1,1] · reversing a sequence changes the encoding (a recurrence that is order-invariant is not recurring, and no shape check would say so) · beats the predict-the-mean baseline at **16% of its MSE** | **DONE** |
| P8-05 | GRU model — **DONE** · **the update gate has the OPPOSITE sign to the LSTM’s forget gate**: h = (1−z)h + zn, so retaining means a SMALL z and the bias that lengthens memory is NEGATIVE — copying the LSTM’s +1 across, which looks like consistency, gives a half-life of **0.53 steps against 2.21** · no separate cell state: an LSTM can hold a value while emitting nothing about it, a GRU must speak to remember · same `Model` interface, also beats the baseline · 17 checks across P8-04/05 | **DONE** |
| P8-06 | TFT model — **DONE** (gated residual networks, variable selection, interpretable attention; the full multi-horizon TFT is what a LibTorch backend completes) · **a softmax over features is NOT feature importance**: identical gate logits over decisive {5.0, 4.0} and useless {0.001, 0.001} features give **bit-identical weights {0.550, 0.450}** and magnitudes differing by three orders — so `variable_selection` reports both, and `concentration()` says when the softmax selected nothing · GLU lets a sub-network switch off rather than be learned around, which is what makes the architecture work on the small data a trading series always is | **DONE** |
| P8-07 | MLP cross-sectional model — **DONE** · **a cross-sectional model needs cross-sectional normalisation, and P8-02’s time-series scaler is the wrong tool here** · measured: a market-wide 3σ shock moves every time-series z-score by **3.0000** and every cross-sectional rank by **0.0000** — the model fed z-scores sees 50 strong sell signals on a day when nothing changed relative to anything · rank not z-score (a 50-name cross-section is fat-tailed and one outlier dominates a mean but not an ordering) · midrank for ties · a ragged universe is refused, because rank 7 of 20 ≠ rank 7 of 200 | **DONE** |
| P8-08 | Temporal CNN + attention autoencoder — **DONE** · **attention without a causal mask is a look-ahead bug with excellent accuracy**: with the target hidden at t+1 the unmasked model recovers it to **0.0010** against the mask’s 4.4948 · the mask is −∞ BEFORE the softmax, not zeroing after — zeroing after leaves the future in the normalising sum so every visible weight is wrong · **“same” padding centres the kernel: 2 outputs BEFORE a spike move, forward reach (k−1)/2·dilation = 4 samples**, and “same” is what every library defaults to because it is right for images · Causality and Padding both have no default · 26 checks across P8-06/07/08 | **DONE** |
| P8-09 | Model registry: immutable, versioned, hash-addressed — **DONE** · **a weights-only hash is not a model hash**: two models with byte-identical weights against different feature registries multiply different columns and a weights hash reports a match — `ModelKey` covers weights + feature_version + architecture + training window + seed · `check_vector` refuses a FeatureVersionMismatch at serving time, the only place it is detectable · immutable: re-registering an identical key is a no-op, a different key is a new version, and a recorded test loss cannot be overwritten with a better one · Registered → Shadow → Canary → Live is enumerated, so nothing goes straight to Live (CLAUDE.md’s guard against slow-motion self-inflicted loss) | **DONE** |
| P8-10 | ONNX / TorchScript serving, warm + pinned, hot-swap — **DONE** (the serving CONTRACT those runtimes plug into) · **an in-place weight swap lets a reader straddle two models**: measured **12,377 of 13,569 concurrent reads TORN**, against **0 of 20,002 across 18,711 swaps** double-buffered with one release/acquire index · a torn read is a forecast from a model that never existed and cannot be reproduced from rule 10’s tuple, and it does not crash · `publish` refuses an unwarmed slot — the first inference after a swap is the slow one and would otherwise land on the first tick after a promotion · serving refuses a FeatureVersion mismatch and an ABSENT feature (no zero substituted) · no allocation on the path | **DONE** |
| P8-11 | Aggregator: per-model × per-timeframe weights, horizon-match enforcement — **DONE** · **two forecasts at different horizons are not two opinions about one thing** — a 375-bar forecast is **6.1x** a 10-bar one for identical signal strength purely from sqrt(T), so an unweighted mean is dominated by the longest member regardless of quality · mismatched horizons are REFUSED; `rescale_horizon` is explicit because the sqrt rule assumes independent increments, which is what a forecasting model exists to violate · mismatched feature registries refused too · unready members excluded, not zeroed | **DONE** |
| P8-12 | Confidence intervals + edge lower bound — **DONE** · **five models on the same features, data and labels are worth 1.19 votes**: se 0.0447 → **0.0917** at a measured ρ=0.8 (P6-04’s correction applied to model outputs) → **0.1095** once the WEIGHTS’ own estimation error is added in quadrature, which an interval carrying only member uncertainty omits · lower bound 0.3553 → 0.3083 → 0.2905 as the honest uncertainty lands · net of round-trip cost before anything downstream sees it (rule 5), no pre-cost accessor in paise · 34 checks across P8-10/11/12 | **DONE** |

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

## Build

`build.bat` wraps `cmake --preset`. Added 2026-08-31 after discovering the
documented build had **never been run**: 16 targets had been built with ad-hoc
`cl` invocations, and `build/default` held a stale configure predating
`instruments/`. Neither `cmake` nor `ninja` is on PATH on this box (both ship
inside VS Build Tools) and `vcvars64.bat` cannot export into PowerShell. First
real run: 40 compile/link steps, **0 warnings**, 19/19 tests.

Two cmd.exe traps cost a cycle each and are worth remembering: a batch file
with UTF-8 box-drawing in comments is unparseable, and the VS path contains
`(x86)`, so echoing it inside a parenthesised `if` block closes the block early
and produces the baffling `\Microsoft was unexpected at this time`.

---

## Toolchain

vcpkg installed natively on Windows 2026-08-31 at `C:/PycharmProjects/vcpkg`,
`builtin-baseline` pinned to `30ef65ca`. `build.bat` sets `VCPKG_ROOT` itself.

**WSL is NOT enabled** and vcpkg never needed it. `wsl.exe` exists (it always
does) but the optional Windows component is missing; enabling it takes
`wsl.exe --install --no-distribution` **as Administrator** plus a reboot. That
matters only for the Linux half of gate 1, which remains unevidenced.

Two presets, both green and both warning-free:

| Preset | Deps | Tests |
|---|---|---|
| `default` | none | 30 — the tree still builds with no vcpkg at all |
| `vcpkg` | `config` (tomlplusplus) | 31 — adds the P0-08b TOML loader |

The `testing` feature (Catch2 + Google Benchmark) is deliberately **off**: the
28 test binaries use a plain `int main()`, and migrating them is a card nobody
has written. Turn it on when one does, not before.

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
4. **Never route backslash escapes through a bash heredoc in this
   environment.** `\n` inside a `<<'EOF'` heredoc still collapses to a real
   newline, silently breaking C string literals. It cost three build cycles
   before the pattern was recognised. Use the Edit tool for anything with
   escapes.
5. **Put the evidence requirement in the loop's termination condition**, never
   in an assertion after it, or a loop that ran zero times passes vacuously.

---

## Blockers

| # | Blocks | Question | Owner |
|---|---|---|---|
| ~~8~~ | ~~No version control~~ | **RESOLVED 2026-08-31.** `git init`, 19 commits, one per card to PROTOCOL §9, ledger Commit column filled with real hashes. History is reconstructed (the work predates the repo) so intermediate commits are not individually buildable; only HEAD is verified. Commits from here are real. | — (closed) |
| ~~**7**~~ | ~~P0-08b~~ **RESOLVED 2026-08-31** | **vcpkg installed natively on Windows** — WSL was *not* actually enabled (the optional component is missing and needs an admin install plus a reboot), but vcpkg never needed it. Cloned to `C:/PycharmProjects/vcpkg`, bootstrapped, `builtin-baseline` pinned to `30ef65ca`, `tomlplusplus` built, `build.bat` sets `VCPKG_ROOT` automatically, and a `vcpkg` **test** preset was added (there was none, so `ctest --preset vcpkg` had never been runnable). The manifest also had to be fixed: it carried `_comment_*` fields that vcpkg rejects outright, which means **vcpkg.json had never once been validated**. Original text: **vcpkg is not installed.** `VCPKG_ROOT` is unset, there is no vcpkg tree on the box, and `builtin-baseline` in `vcpkg.json` is unpinned. P0-08b needs `tomlplusplus`; the `testing` feature (Catch2 + Google Benchmark) has never been switchable either, which is why all ten cards so far use a plain `int main()`. Until this is fixed, **no card with a dependency can have gates 1 or 4 verified**. Install vcpkg, set `VCPKG_ROOT`, pin `builtin-baseline` to a commit SHA. | **Smit** |
| ~~**9**~~ **RESOLVED 2026-09-01** | ~~P1-02, P1-03, P1-05~~ **The master files were public downloads all along** — no credentials, no broker session. `fo_mktlots.csv`, `EQUITY_L.csv`, the NSE and BSE UDiFF bhavcopies and the full Kite dump were all fetched directly. Phase 1's blocker was never auth; it was that nobody had tried. XTS (P1-05) remains set aside by choice. Original text: **STILL OPEN after the 2026-08-31 data drop** — that drop was *bar* data (OHLCV), which is a different artifact and unblocks nothing here. **One sample master file each.** NSE contract master + `fo_mktlots`, the BSE equivalent, and one XTS instrument master dump. Any recent day; content does not matter, the **exact byte layout** does — column order, header text, date format, whether numbers are rupees or paise, what an empty field looks like. A parser written against a guessed schema is the confident-plausible-wrong failure the protocol exists to catch, and no test can find it because the test would share the guess. Drop them anywhere; `data/masters/` is gitignored. | **Smit — files** |
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

### Phase 1 gate record — 2026-08-31

Run against PROTOCOL §10. **Verdict: NO-GO.**

Nothing built is broken — 3'012 assertions across 17 test binaries, 51 loaded
runs, zero failures, zero warnings, and the whole tree now builds and tests
through the *documented* path rather than ad-hoc scripts. The gate fails on
coverage, not on quality.

| § | Check | Result |
|---|---|---|
| 1 | Every card in the phase DONE | **NO** — 5 of 9. P1-02a/b/c and P1-03a blocked on sample files (blocker 9); P1-05 on an XTS sample; P1-07 on credentials; P1-08b on vcpkg |
| 2 | Zero warnings, `/W4` | **YES** — 40 compile/link steps, 0 warnings |
| 3 | `ctest` green | **YES** — 19/19, three consecutive loaded runs |
| 4 | Contracts honoured | **YES** — P1-04, P1-06 and P1-08a cards each amended where the contract, not the code, was wrong |
| 5 | No hot-path allocation | **YES** — every Phase 1 path is pre-open; none is `ALTAIR_HOT` |
| 6 | Latency budgets | **YES** — `id_of` 1.26 ns; Kite parse 2.8 M rows/s |
| 7 | Numerical / financial | **YES** — no floating point in any parser; exact integer comparison in the reconciler |
| 8 | Physics | **YES** — IST day boundaries via P0-02 throughout; every weekday claim verified against an independent implementation |
| 9 | Exit criterion: *"a full session's universe auto-loads pre-open with zero hardcoded lot sizes; a deliberately corrupted source is caught and blocks only its symbol"* | **PARTIAL** — the second half is **demonstrated**: `altair --instruments` shows a lot-size disagreement blocking exactly one symbol while five others load. The first half cannot be met without the master files |

**Blocking the gate:** blocker 9 (four sample files), blocker 7 (vcpkg),
credentials for P1-07. Every one of them is an input, not a defect.

---

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
