# CX-01 G11_strategies_b — findings

Reader: claude-subagent:G11_strategies_b · Date 2026-09-14 · Baseline a34af5c5c9fa8c7498e8cc4df00907548a41e1a5

## 1 Scope & coverage

13 files, 4,794 lines assigned. **All 13 are fully read.** Every file was read in chunks of 300 lines or fewer, and its sha256 before and after reading matches the partition inventory. No file changed during the review.

| File | Lines | Status |
|---|---|---|
| strategies/parity.hpp | 450 | fully read, reviewed |
| strategies/oppty_log.hpp | 318 | fully read, reviewed |
| strategies/overnight.hpp | 419 | fully read, reviewed |
| strategies/regime.hpp | 336 | fully read, reviewed |
| strategies/score.hpp | 303 | fully read, reviewed |
| strategies/vix_forecast.hpp | 220 | fully read, reviewed |
| strategies/tests/test_parity.cpp | 444 | fully read, reviewed |
| strategies/tests/test_oppty_log.cpp | 321 | fully read, reviewed |
| strategies/tests/test_overnight.cpp | 416 | fully read, reviewed |
| strategies/tests/test_overnight_real.cpp | 460 | fully read, reviewed |
| strategies/tests/test_regime.cpp | 361 | fully read, reviewed |
| strategies/tests/test_regime_real.cpp | 412 | fully read, reviewed |
| strategies/tests/test_score.cpp | 334 | fully read, reviewed |

Files outside the group were consulted only to check a claim, not reviewed: risk/cost.hpp:17-19, core/types/units.hpp (Qty), strategies/basis.hpp (Executability, ShortCashCapability), analytics/hurst.hpp (Anis-Lloyd correction present), analytics/rolling.hpp (`at()` is oldest first), desktop/quant_pages.hpp:3620-3694, plus caller greps across first-party dirs. No dataset file was opened.

## 2 Findings table

| ID | Sev | Class | File:lines | One line |
|---|---|---|---|---|
| C11-001 | P1 | CONFIRMED DEFECT | overnight.hpp:246-254, 303-305 | The session filter reads the session that comes AFTER the night (look-ahead), while the comments say it reads the session before |
| C11-002 | P1 | CONFIRMED DEFECT | parity.hpp:222-225, 240-241 | Spot-hedged parity discounts the spot price: df*(S-K) instead of S - df*K, so every conversion is overstated by S*(1-df) |
| C11-003 | P1 | CONFIRMED DEFECT | parity.hpp:278-287, 434-446 | Parity and box are costed as held to expiry, but STT on exercise (charged on intrinsic value) is left out; the comment says it doesn't apply |
| C11-004 | P2 | CONFIRMED DEFECT | parity.hpp:402-424 | `scan_box` only prices the long box; a rich box, sold as a short box, is never detected |
| C11-005 | P2 | CONFIRMED DEFECT | oppty_log.hpp:170-173 | `classify` uses \|gross\|, so a negative touch deviation is filed as CostExceedsEdge or Unreachable ("real mispricing") |
| C11-006 | P2 | CONFIRMED DEFECT (statistical) | oppty_log.hpp:224-228 | `best_over_noise` ignores the mean and assumes independent draws; it is wrong whenever the mean net edge isn't zero |
| C11-007 | P3 | CONFIRMED DEFECT | regime.hpp:180-186, 265-271 | Hysteresis lets the regime jump straight to the opposite side on the looser exit threshold |
| C11-008 | P3 | CONFIRMED DEFECT | regime.hpp:315-334 | `liquidity_regime` turns NaN inputs into a decided `Normal`; the doc says it takes std::expected and refuses |
| C11-009 | P3 | DESIGN GAP | vix_forecast.hpp:107-108, 160-162, 197-199 | Realised vol is silently clamped to 0 (the unsafe side); wrong error code; the log forecast is a median with no bias note |
| C11-010 | P3 | DESIGN GAP | score.hpp:118-121, 237-240 | `paise_per_score` has no quantity dimension yet is subtracted from a position-total `Notional` |
| C11-011 | P3 | DESIGN GAP | parity.hpp:304-359; overnight.hpp:186-197 | Pre-cost outputs: the butterfly margin carries no cost or executability, and overnight cost is a caller-supplied bps figure |
| C11-012 | P3 | CONFIRMED DEFECT (hygiene) | parity.hpp:288-289, 432, 443-445; oppty_log.hpp:252-255 | Rounding is biased on negatives; rate is not checked finite before an int64 cast; `<limits>` isn't included; enum-indexed arrays have no bound check |
| C11-013 | P3 | HYPOTHESIS | tests/test_regime_real.cpp:54-75, 174, 359 | The comment says closes are in paise, but they're parsed with no x100 and truncated into `Price`; if the CSV is in rupees, detector inputs are quantised to whole rupees |
| C11-014 | P3 | DESIGN GAP | tests/test_overnight_real.cpp:156-160, 247-248, 278-281, 334-335 | Hard-fails (exit 1) when the gitignored dataset is missing, and asserts market-dependent verdicts |

## 3 Finding details

### C11-001 — P1 — overnight filter reads the FOLLOWING session (look-ahead, rule 7)
**Evidence.** `session_returns` sets `p = bars[i-1]`, `b = bars[i]` and pushes `overnight = log(b.open/p.close)` and `session = log(b.close/b.open)` (overnight.hpp:246-254). Output index j therefore pairs night j (close of j to open of j+1) with the session of bar j+1, which begins when night j ends. `run_overnight` then does:
```
if (!(r->session[i] < spec.session_below_bps)) { continue; }
taken.push_back(night[i] * spec.size);
```
(303-305). The header says otherwise: the spec doc (117-119) says "the session that PRECEDED it", 265-268 say "from session i's own open-to-close return", and 297-299 say it "ends at the close, i.e. strictly before night i begins". test_overnight_real.cpp:387 prints "filtering on the preceding session".

**Trigger.** Any finite `session_below_bps`. The default (infinity) takes every night, so the unfiltered numbers are unaffected.

**Impact.** The filter reads a return that is not known until about six hours after the position is closed. It also shares the price `open[j+1]` with the outcome: a high open raises night j and mechanically lowers session j. So "session below X" preferentially keeps big gap-up nights, inflating `mean_taken_bps`, `breakeven_rt_bps` and every filtered row. That affects the desktop sweep (quant_pages.hpp:1350) and test_overnight_real.cpp [5] (398-417). The test prints "best conditional mean in the sweep" as if it were a causal result. No oms/ path.

**Existing coverage.** test_overnight.cpp [2] (145-175) scales `open`, `close` and `first_bar_close` of bar 600 by one factor. That leaves every session return unchanged by construction, so it cannot detect an index misalignment. Tests [3] and [4] use independent noise, where the bias does not show.

**Regression test.** Perturb ONLY `bars[k+1].close`. That changes `session[k]` and leaves night k's outcome, fixed at `open[k+1]`, unchanged. Assert that the take/skip decision for night k does not move; the current code fails. Second check: on noise where a shared open shock enters night and next session with opposite sign, assert the conditional mean under `session_below_bps = 0` equals the unconditional mean within sampling error. Fix direction: filter night j on `session[j-1]`, and skip j = 0.

### C11-002 — P1 — spot-hedged parity uses df*(S-K)
**Evidence.** `hedge_buy = df * (q.hedge.ask - K)` and `hedge_sell = df * (q.hedge.bid - K)` (222-225). `mid_deviation` uses the same form (240-241). Against a future, C - P = df*(F - K) is correct. Against the spot (`HedgeLeg::Spot`, accepted at 268-273), European parity is C - P = S - PV(div) - df*K, but the code discounts S too.

**Trigger.** `hedge_leg = Spot` with `rate > 0` and `t > 0`. Example: S = 24,000, r = 6.5%, T = 30/365 gives 1 - df = 0.00533. The conversion deviation is overstated by about Rs 128 per unit, about Rs 9,600 gross on qty 75; reversals are understated by the same amount. Dividends are also ignored.

**Impact.** A fictitious "rich synthetic" conversion shows up on any spot-hedged chain. It is marked Executable, because a conversion needs no short cash (246-249). Desktop caller: quant_pages.hpp:3682-3685 passes `rich_spot` with `q.rate = rate` (3644). The value of `rate` is set above line 3620, which I did not read.

**Coverage.** test_parity.cpp tests 2 and 3 set `q.rate = 0.0` (213, 280). At that rate df = 1 and the two forms coincide.

**Regression test.** Spot hedge, rate 0.065, T = 30/365, S = K, with C and P chosen so that C - P = S - df*K exactly at the mid. Assert `|mid_deviation| < 1` paise. The current code gives about 12,800.

### C11-003 — P1 — held-to-expiry bill omits STT on exercise (rule 5)
**Evidence.** parity.hpp:278-280 says "the options do not pay STT on exercise for the buyer, so the entry legs are the bill". risk/cost.hpp:17-19 contradicts this: "STT on an exercised option is on intrinsic value, not premium" and it is "a separate card". A grep for exercise/intrinsic in config/*.toml and risk/*.hpp finds nothing else. `scan_box` prices only the four entry legs (434-442).

**Trigger.** Any parity or box position held to expiry with an ITM long leg. For a long box, the long legs C(K1) and P(K2) have combined intrinsic of at least K2 - K1 at expiry, growing without bound once the underlying moves past a wing. Physically settled stock options add delivery charges on top.

**Impact.** Cost is under-reported on exactly the structures whose touch edge is a few rupees, which biases Phase 5's opportunity count upward.

**Coverage.** None. The test schedule (test_parity.cpp:44-75) has no exercise charge.

**Regression test.** A 200-point box with +Rs 1.00 per unit of touch edge. Assert that `net` includes an exercise-STT term on at least the box width. This needs a cost-model card first; until then the scanners should refuse or flag the held-to-expiry assumption.

### C11-004 — P2 — box scanner is one-directional
**Evidence.** Only "Buy the box: buy C(K1), sell C(K2), sell P(K1), buy P(K2)" is computed (419-424). The doc at 402-404 says "both directions are always reachable".

**Trigger.** A box whose sell-side value, C1.bid - C2.ask - P1.ask + P2.bid, exceeds the discounted payoff. `gross_value` is then negative and `actionable()` is false.

**Impact.** Half of the box arbitrage space (borrowing below the rate) is invisible, which biases the Phase-5 verdict toward "no edge".

**Coverage.** test_parity.cpp test 4 covers only a fair long box.

**Regression test.** Quote a box Rs 5 above the discounted payoff with Rs 0.10 spreads. Assert a positive-net short-box opportunity is reported.

### C11-005 — P2 — verdict classifier uses |gross|
**Evidence.** oppty_log.hpp:170-173:
```
const std::int64_t g = gross.raw() < 0 ? -gross.raw() : gross.raw();
if (g <= noise_floor)                      { return OpportunityVerdict::NoMispricing; }
if (ex != Executability::Executable)       { return OpportunityVerdict::Unreachable; }
```
**Trigger.** `scan_parity` reports the better of two directions, and on a well-quoted chain that is negative (test_parity.cpp:226 asserts `touch_deviation < 0`). Example: gross = -22,500 paise, cost 5,000, Executable, floor 375 gives CostExceedsEdge, which the enum doc (100-101) defines as "There was a real mispricing and the bill was larger". Separately, Unreachable is returned before any net check (172), although its doc (103) says "The bill was smaller".

**Impact.** The P5-08 exit-criterion table moves fairly priced observations out of NoMispricing into CostExceedsEdge or Unreachable. That is the mis-direction the header says the log exists to prevent.

**Coverage.** test_oppty_log.cpp tests 1-3 use only non-negative gross. Test 4 classifies negative noise but asserts no verdict counts.

**Regression test.** `classify(Notional{-22500}, Notional{5000}, Executable, 375)` must not be CostExceedsEdge. `classify(Notional{900}, Notional{5000}, ShortCashUnavailable, 100)` must not be Unreachable if Unreachable means the bill was smaller.

### C11-006 — P2 — winner's-curse yardstick ignores location and dependence
**Evidence.** `best_over_noise` returns `best_net / expected_extreme_from_noise(observed, net_sd())` (224-228). For noise with mean mu, the expected maximum is mu + sd*E[max Z]. `observed` also counts tick-by-tick, strike-by-strike observations, which are strongly autocorrelated, so the effective n is far below `observed`.

**Trigger.** Net edges normally have a negative mean, because cost is subtracted. Example: mu = -Rs 300, sd = Rs 50, best = +Rs 50, a 7-sigma outlier, gives a ratio of about 0.23 ("nothing there"). With mu > 0, pure noise gives a ratio above 1.

**Coverage.** test_oppty_log.cpp test 4 (248-307) uses mean-zero iid noise only.

**Regression test.** Shift test 4's noise by -30,000 paise and assert the yardstick still reads about 1. Plant one outlier at mean + 8 sd and assert it reads well above 1.

### C11-007 — P3 — hysteresis permits a direct flip on the exit threshold
**Evidence.** `need = (current_ == Trending || current_ == MeanReverting) ? t.exit_sigma : t.min_sigma;` is applied to both sides (180-186). VolDetector does the same with `0.5 * t.min_sigma` from High or Low (265-271).

**Trigger.** min_sigma 2, exit 1, current Trending, margin -1.2: MeanReverting is proposed, although entering it should require -2.

**Impact.** The damping is weaker than specified. `min_bars` still applies and consecutive windows overlap in 511 of 512 samples, so one-bar reversals are rare.

**Coverage.** test_regime.cpp test 2 and test_regime_real.cpp §4 count flips; neither exercises a direct opposite-side flip.

**Regression test.** Put `current_` in Trending, feed returns giving a margin of -1.2 sigma, and assert the proposed label is RandomWalk.

### C11-008 — P3 — liquidity_regime accepts NaN as Normal
**Evidence.** The signature takes plain doubles (321-323), but the doc (317-320) says it "takes them as `std::expected` and refuses". With NaN spread and depth >= min_depth, every comparison is false and it returns `Normal`. NaN depth also yields Normal. The result makes `MarketRegime::complete()` true, which `score.hpp::combine` accepts.

**Callers and coverage.** No caller found; no test.

**Regression test.** `liquidity_regime(std::nan(""), 1e6, 10, 2, 100)` must return Unknown.

### C11-009 — P3 — vix_forecast rough edges
- `realised_from_implied` silently clamps a negative result to `0.0` (198-199). Under rule 11's "clamp toward the SAFE side", a zero realised-vol forecast is the unsafe side; it should refuse. The only caller is risk/tests/test_hedge_book.cpp:385.
- `fit_vix` with `VixSpace::Unspecified` returns `BadHorizon` (107-108), the wrong error code.
- In log space, `forecast_vix` returns exp(E[log VIX]), which is the median; the mean is higher by exp(sigma_h^2/2). This is undocumented. No h-step interval exists. desktop/data/fits.hpp:359-370 uses h = 1 with the one-step `residual_sd`, which is consistent.
- `fit_vix` fits in-sample on whatever slice it is given, so causality is the caller's responsibility. desktop/data/fits.hpp was not reviewed.

### C11-010 — P3 — score dimensional ambiguity
`paise_per_score` is "Expected move, in paise, per unit of score" (118-121): a bare double with no quantity. `net = gross - static_cast<double>(round_trip.raw())` (239) subtracts a position-total `Notional`. test_score.cpp:94 and 201 pair "Rs 40 of expected move per unit score" with "Rs 8 round trip" and never state a quantity. Unless calibration bakes the position size into the scale, a per-unit move is being compared with a per-position bill (gate 8, rule 2). `correlation_inflation` also assumes equal, same-sign weights; mixed-sign weights with rho > 0 overstate error, which is the safe side. `combine` has no non-test caller.

### C11-011 — P3 — pre-cost outputs outside the order path
- `butterfly_margin` returns a touch margin with no cost and no `Executability` (304-359), and desktop displays it (quant_pages.hpp:3372, chain_panel.hpp).
- The overnight strategy nets a caller-supplied bps round trip (`net_at` and `excess_at`, 186-197) rather than using the cost calculator, although flat per-order brokerage does not scale with bps. `SessionBar` prices are `double` (81-95).

Both are research outputs, hence P3.

### C11-012 — P3 — hygiene
- `static_cast<std::int64_t>(x + 0.5)` rounds negative values toward zero (parity.hpp:288-289, 443-445). score.hpp:243 does this correctly.
- `q.rate` is never checked finite. A NaN reaches a double-to-int64 cast, which is UB.
- parity.hpp:432 uses `std::numeric_limits` without `<limits>`.
- oppty_log.hpp:252-255 indexes arrays by enum value with no bound check. A corrupted enum means an out-of-bounds write, though typed code cannot produce one.

### C11-013 — P3 — HYPOTHESIS — daily closes possibly in rupees, truncated to integer Price
test_regime_real.cpp:54 documents "Closes from the daily CSV, in paise", but `load_closes` does `atof` with no x100 (69). The value is then truncated into `Price{static_cast<std::int64_t>(closes[i])}` (174, 296, 359). If all.csv holds rupees (I did not open it, per the rules), every detector input is quantised to whole rupees. Near NIFTY 250 in the early 1990s, rounding to a whole rupee is noise comparable to a daily move. That quantisation noise pushes differenced series toward mean reversion and biases the Hurst estimate, and the vol figures printed "in paise" would really be rupees.

**To confirm.** Check the units of the all.csv close column, or compare against the other daily loaders.

### C11-014 — P3 — real-data overnight test hard-fails without data and asserts market facts
test_overnight_real.cpp:156-160 returns 1 ("FAILED -- dataset incomplete") when fewer than 2,000 sessions load. `dataset/` is gitignored, and test_regime_real.cpp:139-146 SKIPs with exit 0 in the same situation, so the two tests disagree.

It also asserts properties of the market rather than of the code:
- t > 3 (247-248)
- median > mean (278-281)
- a majority of years positive (334-335)
- both exits cross buy-and-hold inside a 0-12 bps sweep (381-382)

These can fail when new data arrives, with no code change. The open is also taken from the first CSV row of each day, with no check that it is the 09:15 bar (104-117). A day missing its first bars would silently use a later price as the auction print.

## 4 Lead revalidation

- **parity.hpp:**
  - The parity formula against a future, df*(F-K), is correct. Against the spot it is wrong (C11-002).
  - Four legs are costed: both options on premium with Segment::Opt, plus the hedge in and out on notional, with delivery for a spot hedge.
  - The option STT side comes from the ChargeSchedule; the test sets Sell at 0.15% of premium (test_parity.cpp:52-53). No side is hard-coded in the header.
  - STT on exercise is missing (C11-003).
  - The butterfly is correctly spacing-weighted, with strikes supplied by the caller.
  - The box's implied rate, ln(payoff/cost)/T, is correct, but only the long box is scanned (C11-004).
- **oppty_log.hpp:**
  - The capacity bound is honoured in the rule 11 "visible truncation" form: aggregates are exact over every observation, `observed` versus `retained` is reported, and the reservoir is a correct Algorithm R.
  - No allocation; `push` is `ALTAIR_HOT` and allocation-free.
  - The defects are in classification (C11-005) and in the noise yardstick (C11-006).
- **overnight.hpp / test_overnight_real.cpp:**
  - Gap definitions are correct: log returns, with the conservation identity exact and tested.
  - The unfiltered close-to-open measurement has no look-ahead; the filter does (C11-001).
  - Cost is a caller bps figure, not the P3-09 calculator (C11-011).
  - The real test honestly lists the fill-at-close and fill-at-auction assumptions as unmodelled (432-447).
- **regime.hpp / test_regime_real.cpp:**
  - Labels are computed causally: push, then call, over a rolling window with `at()` oldest first, and Hurst uses the Anis-Lloyd correction.
  - The real test conditions forward returns strictly after the call bar (152-155), and its whole-sample vol quantiles (366-367) are printed only, not used for labels.
  - Defects: hysteresis flaw (C11-007), NaN liquidity (C11-008), a units question (C11-013).
- **score.hpp:** contains NO ported RXT thresholds. Every threshold, weight, correlation, scale and k is supplied by the caller and refused if absent. Cost is subtracted before any field exists, subject to the units ambiguity in C11-010. `kMaxSignals` refuses input over the bound (191).
- **vix_forecast.hpp:**
  - The AR(1)/OU fit is ordinary least squares on the supplied slice, in-sample; causality is the caller's job.
  - The horizon is iterated h times on the conditional mean, in log space a median (C11-009).
  - Half-life -ln2/ln b is correct, and the residual dof n-3 is correct.

## 5 Architecture facts
- **Order path.** No file in this group is included by app/ or oms/ (grep), and none places an order.
- **Implemented and connected to the desktop:**
  - parity.hpp: quant_pages.hpp demo pages; chain_panel.hpp references butterfly_margin.
  - overnight.hpp: quant_pages.hpp overnight page, including the filtered sweep affected by C11-001.
  - regime.hpp: header included by quant_pages.hpp and flagging/scorecard.hpp; the detectors themselves are instantiated only in tests.
  - vix_forecast.hpp: desktop/data/fits.hpp, reached from panels.hpp.
- **Implemented, tests only:**
  - oppty_log.hpp.
  - score.hpp `combine`: models/aggregator.hpp includes the header, but no `combine(` call was found.
  - regime.hpp `liquidity_regime`: no caller at all.
- **Cost integration:**
  - parity and box call `compute_cost` per leg (P3-09), but without exercise STT.
  - score takes a caller `Notional` round trip.
  - overnight takes a caller bps figure.
  - butterfly has no cost.
  - The opportunity log is fed by nothing outside tests, so the Phase 5 "session of every opportunity" deliverable is not wired to any executable.
- **Hot path.** Only `OpportunityLog::push` is marked `ALTAIR_HOT`, and it allocates nothing. overnight.hpp allocates (vectors, strings) and is cold research code. `TrendDetector::call` copies the window into a `thread_local` buffer on every call: O(N) per call, no heap.

## 6 Test-suite observations
- **All four synthetic tests** (parity, oppty_log, overnight, regime, plus score) print narrative and assert. None reads the dataset.
- **Data sources.**
  - test_overnight_real.cpp reads `ALTAIR_DATASET_DIR/spot/nifty/5m/*.csv`.
  - test_regime_real.cpp reads `ALTAIR_DATASET_DIR/spot/nifty/1d/all.csv`.
  - Neither file was opened by this reviewer.
- **Blind spots that let the findings through:**
  - rate = 0 in the parity tests hides C11-002.
  - The overnight causality test is structurally unable to see C11-001.
  - No short-box test (C11-004), no negative-gross classification test (C11-005), no non-zero-mean noise test (C11-006).
  - No direct-flip hysteresis test (C11-007), no `liquidity_regime` test (C11-008), no exercise-cost test (C11-003).
- **Tautological check.** test_score.cpp:175-178 asserts `std_error == naive * correlation_inflation(...)`, which restates the implementation. The equal-weight, unit-variance case happens to make the closed form exact, so it is not wrong, but it does not independently test the claim.
- **Verdicts.**
  - test_regime_real prints a verdict ("does not separate" versus "separates") without asserting it, which is the right shape.
  - test_overnight_real asserts market facts and hard-fails on missing data (C11-014).

## 7 Open questions
1. What value does `rate` have at desktop/quant_pages.hpp:3644, set above line 3620 and not read here? If non-zero, the spot row on that page shows the C11-002 artefact today.
2. Is an exercise/assignment cost card scheduled (risk/cost.hpp:17-19)? Until then, should `scan_parity` and `scan_box` refuse, or mark the held-to-expiry assumption?
3. What units is the close column of `dataset/spot/nifty/1d/all.csv` in (C11-013)?
4. Does anything outside the files grepped here construct `Observation`s from live scanners, such as a Phase-5 session runner under backtest/ or app/? None was found.
