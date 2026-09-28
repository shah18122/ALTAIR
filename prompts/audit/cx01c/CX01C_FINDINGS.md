# CX-01 — prioritised findings

Reader: claude-lead · Revision: 2026-09-15 ~12:45 IST (**FINAL for Phase 1 — all 22 review groups reported; 481/481 files covered**) · Baseline HEAD `a34af5c5c9fa8c7498e8cc4df00907548a41e1a5`

Full generated list: `CX01C_FINDINGS_INDEX.md` (321 rows at close: P1 16 · P2 107 · P3 198). Per-finding evidence, triggers, existing coverage and regression tests: `G*_findings.md`. Lead source verification: `LEAD_VERIFICATION.md` (V-nn).

## How to read this

- **Severity:**
  - **P0**: can cause wrong live orders, money loss or data corruption *today*
  - **P1**: serious defect or safety/latency blocker
  - **P2**: moderate
  - **P3**: minor
- **Latent** marks a defect in code no executable reaches yet. Its severity describes the impact once wired, and its current live impact is bounded. **No P0 exists at this revision, because no code path can place an order.**
- **Class:**
  - **CONFIRMED DEFECT** — exact lines plus a concrete trigger
  - **DESIGN GAP** — the design is missing something
  - **HYPOTHESIS** — needs runtime proof
- **Verification:** "✔ V-nn" means the lead re-read the cited source. Everything else is reviewer evidence not independently re-read.

## 1. Revalidation of the leads named in the brief

| Lead (brief / Codex) | Verdict | Evidence |
|---|---|---|
| ModelServer reuses inactive storage without protecting readers (Codex Q01) | **CONFIRMED** — P2, latent (tests only) | C08-001 |
| Generation metadata shared unsafely (Codex Q02) | **CONFIRMED** — P2, latent | C08-002; C08-023 (test cannot see it, SKIP→PASS) |
| SeqlockSnapshot copies a non-atomic payload concurrently (E003) | **CONFIRMED** (language-level data race) — P2, latent: the only executable use is single-threaded | C02-002 ✔ V-02 |
| Configuration refresh may spin without a bound | **CONFIRMED** unbounded, no pause/yield; impact HYPOTHESIS | C02-003 ✔ V-02 |
| Order-intent parsing has structural and numeric gaps | **CONFIRMED, and worse than stated**: a torn line merges two intents that parse (P1) | C13-005 ✔ V-04; C13-006/007/008 |
| Live price distribution is not an assembled strategy/risk/OMS pipeline | **CONFIRMED** | C15-004, C13-009, C03-002, C12-001 ✔ V-14 |
| ReplayTick, canonical tick storage and CSV bar replay follow different paths | **CONFIRMED**: four paths, none shared with a decision path | G03 §4.1; `CX01C_ARCHITECTURE.md` §8 |
| Placeholder live instrument specifications | **CONFIRMED** in both live-decode executables (`TOKEN%u`, lot 1, tick 1, scale 100) | `app/price_service_main.cpp:350-361`, `app/kite_ticker_main.cpp:243-254` (G03 §4.4, G15) |
| Stale depth and failover recovery semantics | **CONFIRMED**: stale depth applied as tradable (latent); an outage between polls is invisible. Unknown→NoneHealthy is **defensible**, but the comments contradict it | C03-001, C03-007, C03-008 |
| FeatureVector accepts a declared count larger than its storage (Q04) | **CONFIRMED** — `FeatureVector{1,600,ts}` then `set(550,…)` writes out of bounds | C05-001 |
| Book/flow builders reuse stale slots (Q05) | **CONFIRMED and extended** — no age gate anywhere | C05-002 |
| Synchronous desktop work and per-update table reconstruction block the Qt loop | **CONFIRMED**, and wider than stated: fits, reports and **subprocess waits** run on the GUI thread; the Forecast fetch can block up to ~65 s; buttons are not disabled (a second click re-enters); the Halt tab is unusable meanwhile. The per-frame rebuild is confirmed for the option chain, **not** per tick for the watchlist or pending intents (G16 quant pages pending) | C17-001, C17-007 |
| MPSC stalled producer / lock-free claim (E004) | **PARTIAL**: wedge and overstated claim confirmed; the unbounded CAS retry alone is not a defect | C02-007 |
| Latency budgets print OVER without failing (E005) | **CONFIRMED, extended to 10 sites.** Live example: `tsc_clock` printed OVER BUDGET and exited 0 | C02-006; baseline §5 |
| Aggregator unchecked cast (Q03) | **PARTIAL** | C08-006 |
| UI-001 … UI-004 (GUI-thread fits; seek/MarketClock; scrub max; stale candles) | **CONFIRMED** at checkpoint | C17-001…004 |
| E001 `round_to_tick` overflow at int64 extremes | **CONFIRMED** — P2, latent (no non-test caller) | C01-001 |
| E002 `apply_bps` long double == double on MSVC | **CONFIRMED on MSVC** — P3: `risk/cost.hpp` uses its own 128-bit arithmetic, and nothing in `risk/` calls `apply_bps` (G12) | C01-002, G12 §4 |
| E006 Pool double release / copyable allocators | **CONFIRMED** (free list corrupts, the same block is handed out twice; Pool and Arena copyable) — P2, latent (tests only) | C01-004 |
| (new) PlausibilityGate has no staleness bound | **CONFIRMED** — a tick hours behind the local clock is admitted as fresh; used by `feed/normaliser.hpp` (itself tests-only, C03-002) | C01-006 |
| UI-005 (Atlas family double-click opens page 0) | **CONFIRMED** — a family node has no page role, so a double-click opens page 0 (Live Grid). Atlas statuses: 26 rows spot-checked, **no status mismatch**. Two page indices are open questions (Hurst possibly page 27; walk-forward page 26), and the header comment's "32 pages" is stale. No Atlas change proposed (user instruction) | G18 |

## 2. Priority findings (confirmed P1)

| # | ID | Severity (lead) | Summary | Trigger → impact | Verified |
|---|---|---|---|---|---|
| 1 | **C13-005** | P1 latent | Interrupted intent append + next append = one line that parses as a mixed order | A torn `…"lots":50` followed by a complete record → **SELL 50 lots RELIANCE at ₹1.00** that nobody wrote; record B vanishes; `rejected == 0` | ✔ V-04 |
| 2 | **C13-002** | P1 latent | A rejected cancel/amend request makes the order terminally `Rejected` | Cancel refused because the order already filled → later fills refused → **engine believes it is flat while holding a position** | ✔ V-06 |
| 3 | **C13-001** | P1 latent | A partial fill during PendingCancel/Replace drops the pending state | CancelAck refused → cancelled order stays "live" with leaves 75; a re-cancel can then hit C13-002 | ✔ V-05 |
| 4 | **C13-010** | P1 latent (gate 7) | A stop exit on a gap is valued at the stop, not a reachable price | Long 750 @24,000, stops 23,800/24,000, tick 23,700 → reported P&L 0 instead of ≈ −₹2.25 lakh; a limit at the stop rests unfilled while the price falls. The test's "₹1.5 lakh benefit" is fictional | ✔ V-07 |
| 5 | **C17-010** | P1 latent | The typed confirmation hides limit price, order type and product; default limit ₹24,000 persists | Pick a ₹50 option, confirm REQUEST BUY → the intent carries a ₹24,000 limit that the user never saw | ✔ V-12 |
| 6 | **C14-001** | **P1 current** | Dataset writers keep 6 significant digits | Every `kite_fetch`/`kite_update` run rounds NIFTY/BANKNIFTY bars (23398.15 → 23398.1); **stored data already shows it**; every model evaluated on `dataset/` inherits it | ✔ V-11 |
| 7 | **C12-003** | P1 | charges.toml `[safety] block_on_unverified_schedule`, `[broker.*]`, `[implicit]` are never parsed | The file promises refusal of unverified schedules; the loader ignores the setting | ✔ V-13 |
| 8 | **C13-004** | P1 design gap | No "outcome unknown" order state, no idempotency key, no open-order reconciliation | A timeout on placement cannot be represented, so a future adapter must either guess or retry, including a forbidden blind retry at a backup broker | reviewer |
| 9 | **C13-009 / C15-004 / C12-001** | P1 design gap | No executable drains intents or runs pre-trade limits; the intent queue has no expiry | Any future drainer would act on stale intents; `check_order` has no non-test caller | ✔ V-14 |
| 10 | **C12-002** | P1 design gap | Conservation and daily-loss halts never trip the kill switch outside tests; the conservation identity is **true by construction** | A mis-reported fill cannot trip it | ✔ V-01 (C02-001) |

| 11 | **C11-001** | P1 (rule 7, research results) | The overnight-gap filter conditions night i on the session that FOLLOWS it | "Session below X" filter → filtered sweeps on the desktop and in `test_overnight_real` are biased upward by look-ahead; unfiltered results unaffected | ✔ V-16 |
| 12 | **C11-002** | P1 latent (gate 7) | Spot-hedged parity uses `df·(S−K)` instead of `S − df·K` | r 6.5%, 30 days, S ₹24,000 → conversion edge overstated ≈ ₹128/unit; tests use rate 0 and cannot see it | ✔ V-15 |
| 13 | **C11-003** | P1 latent (gate 7) | Parity/box costed as held to expiry, but STT on exercise is omitted; `parity.hpp` and `risk/cost.hpp` disagree on whether it applies | Held-to-expiry arbitrage looks cheaper than it is | reviewer |

**Lead re-rating:** C14-002 (ticker `--seconds` bypassed by heartbeats) moves from P1 to **P2**. It is an operational hang of a bounded-sample CLI; it does not corrupt data or touch orders. It relates to C15-002 (the price service stops after ~60 s and never reconnects).

## 3. Cross-cutting themes

These matter more than any single row, because each has several instances and will recur in new code unless a card addresses the pattern.

1. **Primitives, not a system.** The ROADMAP §5.1 path exists as tested pieces with no assembly: C15-004, C13-009, C03-002, C12-001/002. The backtest engine's strategy concept is implemented by no strategy (G05), and the flagging deploy harness is unconnected (G05). **Hard rule 6 (backtest = live) cannot currently be demonstrated.**
2. **Safety checks that cannot fire or are inert:**
   - conservation tautology (C02-001 ✔ V-01; the backtest ledger repeats it, C05-008)
   - inert `[safety]` config (C12-003 ✔)
   - `ALTAIR_STRICT_INVARIANTS` guard that cannot trigger (C02-011)
   - latency budgets that print but never fail (C02-006)
   - `monitor.hpp` `worst()` ranks "never observed" below Ok (C15-014)
   - **`config/altair.toml` has zero consumers anywhere** (C22-003). Every engine setting in it is inert, and `desktop/market_clock.hpp` documents hard-coded values pending an unwritten wiring card.
   - Task cards P1-02a/b/c and P1-03a still say BLOCKED although implemented (and their interfaces superseded), and several named tests don't exist under those names (C22-001).
   - the canary loss limit cannot fire before `min_observations` (C05-012)
3. **Concurrency primitives are unvalidated:**
   - seqlock race and unbounded spin (C02-002/003)
   - MPSC wedge (C02-007)
   - ModelServer slot ABA and generation race (C08-001/002)
   - TSan unavailable on MSVC (baseline B6)
   - possible GCC/Clang build blocker from an MSVC-only STL call (V-03, hypothesis)
   - Windows ASan green (129/129) is **not** race evidence
4. **Rule 11 (silent fixed bounds) is still being found:**
   - book level counts unchecked (C03-005)
   - VPIN clamp (C03-010)
   - `horizon_eval` writes past `double x[16]` (C08-010)
   - label uniqueness capped at 65,536 on the unsafe side (C08-014)
   - registry name truncation (C08-015)
   - clamped accessors (C13-013)
   - log truncation (C02-010)
   - expiry list capped at 8 (C17-012)
5. **Order lifecycle correctness.** C13-001/002/004/010/011/014/015 all need fixing **before** any OMS assembly (CX-02 before CX-03).
6. **Data integrity of research data:**
   - precision loss (C14-001 ✔)
   - unchecked rename and no coverage check in `kite_update` (C14-005/006)
   - the tick recorder drops depth, ignores write errors and has no identity map (C03-003/004)
   - the quote snapshot drops OI and timestamp (C14-008)
7. **Live / replay / stale honesty and time:**
   - stale depth tradable (C03-001)
   - `recv_ts` from `system_clock` against the documented TSC (C03-002)
   - the stream strip keys off NIFTY only, and the chain header stays LIVE after disconnect (C17-008)
   - bar-close replay possibly one interval early (G03 path D, hypothesis)
8. **Statistical evidence is overstated in places:**
   - iid SE on overlapping horizons in the forecast verdict shown in the UI (C08-007)
   - the band measured on the same period it is scored on (C08-009)
   - coin-flip SE (C08-012)
   - scale-dependent ADWIN without multiple-testing control (C05-011)

   - velocity standard error about 6.9× too small at order 2, so `velocity_is_significant` flags roughly 77% of pure-noise windows instead of 5% (C06-002, G06)
   - QLIKE silently drops zero-variance forecasts (G06)

   None of these flips the headline verdict (no model beats the random walk after cost), but each would overstate a future positive result.

   **Strategy statistics and costs (G10; callers are tests and `desktop/quant_pages.hpp` research pages only):**
   - `johansen_bivariate` uses a restricted statistic that is always too low, so it rejects too rarely and discards genuine pairs (C10-003).
   - The ADF test is wrong at 2 or more lags (C10-004).
   - The structural-break detector cannot see a break at the start of its window (C10-005).
   - A 5.5 bps cost labelled round-trip is charged per trade, which doubles directional costs (C10-013).
   - The cross-venue scan costs two trades where a same-day close-out takes four (C10-002).
   - A NaN rate or dividend yield is not refused and reaches an undefined integer conversion (C10-001).
   - Fundamentals rows do not record quarterly vs annual, so sector medians mix them (C10-007).
   - The real-NIFTY test drops overnight returns but still carries positions across them (C10-016).
   - Correct per G10: Engle–Granger MacKinnon critical values, half-life, basis `F − S·e^((r−q)T)` arithmetic, filing-date point-in-time fundamentals, causal rolling windows.

   **Analytics B (G07; all P3, none on an order path):**
   - `hurst_rs` on a constant series returns H = 0.950 ± 0.031, flagged significant, instead of refusing it as Degenerate. It is reachable from `strategies/regime.hpp` and `features/kinematics.hpp` (C07-003).
   - `sabr_min_density` reports false arbitrage (−3.32 vs a true +0.0100) from truncated strikes (C07-008).
   - `sabr_vol` refuses off-ATM strikes at ν = 0, which `valid()` accepts (C07-001).
   - VIX truncation counters count missing quotes, not dropped strikes (C07-005).
   - `Kalman1D` accepts NaN and stays NaN (C07-002).
   - `svi_fit` never returns NoConvergence (C07-009).
   - Verified correct by G07: the IV solver (honestly not Jäckel; ROADMAP still promises Jäckel at ~2 ns, C07-012), both SVI arbitrage checks, the Hagan core, Hurst bias correction, Kalman covariance positivity, rolling moments, and the VIX CBOE core (which has no production caller). Two-scale realised variance is **not implemented**, despite ROADMAP §3.8.

   **Analytics additions (G06, tests-only callers except `greeks.hpp`):**
   - If every HMM restart fails, `fit_hmm` indexes empty vectors instead of returning DidNotConverge, because -1e300 passes `isfinite`. A single ~77 σ outlier, such as a 10× price glitch, triggers it (C06-001).
   - The derivatives 4,096-point buffer keeps the **oldest** points and drops the newest, contrary to its comment. That is rule 11 on the unsafe side (C06-003).
   - The American pricer refuses low-vol puts on a paise overflow, and the chain sweep accepts an infinite strike and writes NaN.
   - Black-76/greeks formulas re-derived correct, including tail forms and T/σ refusal (G06 lead verdict).
9. **Credential surface wider than documented.** Six `app/` tools read key and secret; there are five session-file readers; the session file is written in place without restricted permissions (C14-010/011). Rotation of the exposed Kite key/secret remains outstanding (LEDGER P29, go-live).
12. **Risk and cost defaults fail open** (G12 final, reviewer evidence).
    - A missing rate key silently becomes 0.
    - Currency options are priced with futures exchange charges, about 100× too low.
    - All-zero risk limits approve everything.
    - Sizing accepts `edge_sigmas ≤ 0` and a Kelly divisor below 1, and has no quarter-Kelly ceiling.
    - The slippage fit accepts zero prices, which biases impact low.
    - There is no net-greeks limit.
    - `config/lot_size_history.csv` is read by nothing.
    - The Zerodha brokerage rate is a literal in desktop code.
    - The stress and Black-Litterman tests exit 0 when `dataset/` is missing.

    The STT schedule itself is correct: futures 0.02→0.05% and options 0.10→0.15% sell-side on premium from 2026-04-01; stamp duty is buy-side; the GST base and SEBI fee are right. See `G12_risk_charges_findings.md`.
11. **The "UI cannot trade" boundary is weaker than documented.** Its three enforcement points are all soft:
    - The gate-3 audit checks direct link libraries only, while the UI can start subprocesses that hold the session token and can write the intent queue (C17-018).
    - Rows labelled "watch only / cannot be ordered" still become requestable once found in the master (C17-021).
    - Dev accounts and the `--as admin` bypass are compiled into every build type except exactly `Release` (C17-015).

    Meanwhile the one mutating safety control misreports. A malformed or unreadable halt-request file displays "No halt requested", and the halt write result is unchecked (C17-014). A failed intent write is still logged PENDING (C17-011).
16. **Several core defects originate in the task cards, not in implementation drift** (G21; all acceptance tests named by the P0 cards exist).
    - P0-01 never requires `round_to_tick` to check the final `quot * t` for overflow (C21-001 → C01-001).
    - P0-05b never specifies double-release detection for `Pool` (C21-002 → C01-004).
    - P0-06b **mandates** the plain-payload seqlock idiom, a formal C++ data race, and P0-08a's `ConfigStore` inherits it (C21-003 → C02-002).
    - P0-09a's `ConservationLedger::check()` is tautological by design; the card discloses this in an architect's note, but the caveat never reaches the tests or CLAUDE.md (C21-004 → C02-001).

    **Consequence for Phase 2:** each fix needs a correction card that amends the original contract (PROTOCOL §7). Otherwise a future re-implementation from the card reintroduces the defect. Among the later cards (G22): P1-02a/b/c and P1-03a still read BLOCKED despite shipped, superseded interfaces (C22-001), and `Verdict` shipped as `ReconcileVerdict` without the required CONTRACT OBJECTION note (C22-002).
15. **Model code is not robust to bad inputs, and its tests assert less than they print** (G09; reviewer evidence).
    - A NaN training error is returned as 0.0, a perfect fit (C09-001).
    - GBDT has no finiteness checks: sorting NaN is undefined behaviour, and a NaN label yields a "successful" NaN model (C09-002).
    - GBDT `Frame` shape is never checked, so a short input reads out of bounds (C09-003).
    - GBDT `predict_row` allocates despite its hot-path contract (C09-004).
    - Leaf-wise growth is silently capped by `max_depth` (C09-005).
    - **Any `Causality` value other than `Masked`, including an out-of-range one, runs attention unmasked, which is a look-ahead path** (C09-007).
    - Real-data verdicts are printed rather than asserted (C09-020).
    - Four real-data tests pass on SKIP when `dataset/` is missing (C09-018).
    - Nothing is persisted or cost-netted (C09-016).
14. **Point-in-time instrument specs are not what live code uses** (G04; reviewer evidence).
    - Live binaries register placeholder specs (lot 1, tick 1); only `app/instruments_demo.cpp` runs the real master pipeline into the SpecStore (C04-001).
    - The store cannot hold two validity windows for one token, so a mid-life lot revision cannot be represented (C04-002).
    - There is no expiry calendar: the snapshot policy never detects a rollover, and loaders never set the stale flag (C04-003).
    - `research/tools/ingest_bars.py` drops the exchange from output paths, so NSE and BSE exports merge (C04-004).
    - Adding a FYERS source compiles but misbehaves: the reconciler keeps only Kite and XTS token slots, and failover can choose only between those two (C04-005).
    - A Kite lot of 75.5 becomes 75, and a zero lot or tick can reach the store unblocked (C04-008).
    - Kite underlying-name truncations are counted but never shown, and truncated names still merge (lead PARTIAL).
13. **Desktop pages say things their own numbers contradict** (G16).
    - The Markov model is hard-coded green "trained on real data" beside a walk-forward that finds no directional edge (C16-004).
    - The Cointegration page pairs the NIFTY future with whatever spot is selected, so BANKNIFTY and VIX test the wrong pair (C16-003).
    - Fixed verdict sentences can contradict the numbers computed above them.
    - Pages carry literals: lot size 75, cost 5.5 bps, a hard-coded `2026-09.csv`.
    - The scorecard's regime labels have a look-ahead.
10. **The GUI thread does engine-sized work:** **no code in `desktop/*.hpp` runs off the GUI thread** (G16). `neural_report` launches a test program and can block for up to 125 s (C16-001, **P1**). The Analytics panel runs 2,000 bootstrap passes in its constructor, so every startup waits (C16-002). Also: synchronous fits and reports (C17-001) and full chain-table rebuilds per price frame (C17-007). In-process with the engine, that is a latency hazard for any future engine work sharing the thread.

## 4. Regression tests to write first

Order follows the CX-02 dependency chain. Each is specified in its group file.

1. Intent queue: torn-prefix + next-record → 0 intents, `rejected ≥ 1` (C13-005). Plus huge digits, token > 2³²−1, duplicate keys, nested keys (C13-006/007).
2. Order state:
   - partial fill in PendingCancel then CancelAck → Cancelled, `refused == 0` (C13-001)
   - cancel Reject then fill → Filled (C13-002)
   - placement timeout → a distinct Unknown state (C13-004)
3. Exit ladder: gap below both stops → `level ≤ price` for a long (C13-010).
4. Order ticket: the confirmation text must contain limit price, order type and product; the limit must reset or require entry per contract (C17-010).
5. Dataset writers: 23398.15 round-trips exactly through `kite_update`/`kite_fetch` output (C14-001).
6. Conservation: an independent fill log with one fill off by one lot must trip (C02-001/C12-002).
7. Seqlock and ModelServer: latch-forced interleavings (reader paused across two publishes) asserting coherent prediction, digest, version and generation (C02-002, C08-001/002). Linux TSan run when an environment exists.
8. FeatureVector: `FeatureVector{1,600,ts}` refused, or clamped with a count (C05-001). A reused vector after a crossed book must not report `complete()` (C05-002).
9. L2Book: `bid_levels = 6` refused (C03-005). A stale-flagged depth must not replace a fresher book (C03-001).
10. PriceBus: a partial write followed by a cap trip must never drop the head frame mid-write (C15-001 ✔ V-09).
11. Latency gates: a budget miss must fail in a dedicated benchmark target (C02-006).
12. charges.toml: `block_on_unverified_schedule = true` with `last_verified = "UNVERIFIED"` → costing refused (C12-003).

## 5. Completeness and limits of this list

- **All 22 review groups reported.** 481/481 files and 141,880 lines are recorded fully read, and hashes are unchanged since inventory (`CX01C_COVERAGE_LEDGER.md`).
- The index holds **321 findings**. Retired-client groups G19/G20 found P3 issues only, and the protocol matches `server/`. Doc groups G21/G22 found P2/P3 contract drift, inert `config/altair.toml` (C22-003), and core defects that originate in the P0 cards (C21-001…004).
- **This is not a claim that all bugs were found.**
  - Review was static reading by independent reviewers. Severities and classes are theirs unless re-rated here.
  - The lead re-read source only for V-01…V-16.
  - No race detector, UB sanitizer or Linux compiler was available, so language-level races and UB remain possible beyond those found (C02-002, C08-001, C13-006, C01-001).
  - P3 rows in particular carry reviewer evidence only.
