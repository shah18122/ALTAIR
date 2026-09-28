# CX-01 G10_strategies_a — findings

Reader: claude-subagent:G10_strategies_a · Baseline a34af5c · Date 2026-09-14

## 1 Scope & coverage

13 files assigned, 4,668 lines. **All 13 fully read** in chunks of 300 lines or fewer.
sha256 before and after matched the partition inventory for every file.

| File | Lines | Status |
|---|---|---|
| strategies/CMakeLists.txt | 96 | fully read |
| strategies/basis.hpp | 403 | fully read |
| strategies/calendar.hpp | 328 | fully read |
| strategies/cointegration.hpp | 486 | fully read |
| strategies/fundamentals.hpp | 297 | fully read |
| strategies/meanrev.hpp | 210 | fully read |
| strategies/momentum.hpp | 317 | fully read |
| strategies/tests/test_basis.cpp | 408 | fully read |
| strategies/tests/test_calendar.cpp | 400 | fully read |
| strategies/tests/test_cointegration.cpp | 363 | fully read |
| strategies/tests/test_directional.cpp | 546 | fully read |
| strategies/tests/test_directional_real.cpp | 501 | fully read |
| strategies/tests/test_fundamentals.cpp | 313 | fully read |

Nothing was built or run. Callers were found with Grep only; desktop/ code
outside the grep context lines was not reviewed (another group owns it).

## 2 Findings table

| ID | Sev | Class | File:lines | Title |
|---|---|---|---|---|
| C10-001 | P2 | CONFIRMED DEFECT | basis.hpp:133-136,165,183,307-311; calendar.hpp:205-207,269,287-296 | Non-finite rate / dividend yield / tenor not refused; NaN is cast to int64 (UB) |
| C10-002 | P2 | CONFIRMED DEFECT | basis.hpp:29-31,357-363,388-399 | Cross-venue scan costs two opening legs, though the file says both venues are squared off intraday |
| C10-003 | P2 | CONFIRMED DEFECT | cointegration.hpp:288-349 | `johansen_bivariate` is not the Johansen trace statistic |
| C10-004 | P2 | CONFIRMED DEFECT | cointegration.hpp:128,194-230,282 | ADF with lags >= 2 is not ADF(p) |
| C10-005 | P2 | CONFIRMED DEFECT (analytic) | cointegration.hpp:449-484 | CUSUM-of-squares cannot see a break at the start of its test window; `at` uses later data |
| C10-006 | P3 | CONFIRMED DEFECT | cointegration.hpp:90,186,269,314,365,455; calendar.hpp:141-143; fundamentals.hpp:120-122 | Refusals name the wrong bound or reason |
| C10-007 | P2 | DESIGN GAP | fundamentals.hpp:80-107,138-156,188-247 | No period type: quarterly and annual rows compete in `as_of` and in sector medians |
| C10-008 | P3 | CONFIRMED DEFECT | fundamentals.hpp:269-294 | `relative_to_sector` accepts a negative median and NaN peers |
| C10-009 | P3 | CONFIRMED DEFECT | meanrev.hpp:101-102,190-203 | `scale_with_z` exceeds the stated cap by up to 2x; fade not flipped on a through-band jump |
| C10-010 | P2 | DESIGN GAP | basis.hpp:124-137,281-311; calendar.hpp:96-101,130-191,271-297 | "Net of full cost" leaves out exit spread and impact, and SLB fee; option calendar has no cost and no staleness check |
| C10-011 | P3 | HYPOTHESIS | fundamentals.hpp:144 | A filing is visible all day on its filing date |
| C10-012 | P3 | CONFIRMED DEFECT | cointegration.hpp:178-193 | `32 + lags` wraps around, giving an out-of-bounds read |
| C10-013 | P2 | CONFIRMED DEFECT | momentum.hpp:125-128,151-152; meanrev.hpp:19-22; test_directional.cpp:128; test_directional_real.cpp:194,229 | A 5.5 bps cost labelled round-trip is charged on every unit of turnover, so directional cost is doubled |
| C10-014 | P3 | CONFIRMED DEFECT | momentum.hpp:71-76,148-150 | `round_trips` counts each entry and each exit, so one round trip counts as 2 |
| C10-015 | P3 | DESIGN GAP | basis.hpp:129-137; calendar.hpp:198-207 | Dividend is only a continuous gross yield; T day-count and lot-multiple qty are left to the caller |
| C10-016 | P2 | CONFIRMED DEFECT | test_directional_real.cpp:183-190,373-376,406-408; meanrev.hpp:151-160 | Overnight returns are dropped but positions are still carried across them |
| C10-017 | P3 | DESIGN GAP | test_directional_real.cpp:250-254,288,475-496; CMakeLists.txt:69-73 | Real-data test: verdict never asserted, a vacuous check, and a hard FAIL (not a skip) when the dataset is absent |

No P0 or P1: no function in this group has an engine or oms caller (section 5).

## 3 Finding details

### C10-001 — non-finite carry inputs reach an int64 cast (P2, CONFIRMED DEFECT)
**Evidence.** `measure_basis` checks `max_skew`, the prices, and `!(q.t.raw() > 0.0)` (basis.hpp:159-165). It never checks `rate`, `dividend_yield`, or that `t` is finite.
basis.hpp:183 `b.fair_basis = S * std::expm1((q.rate - q.dividend_yield) * T);`
With rate = NaN:
- Line 271 picks `ReverseCashAndCarry`.
- Line 307: `per_unit` is NaN.
- Line 310: `static_cast<std::int64_t>(value + 0.5)` is undefined behaviour.
- Line 311 then subtracts cost from that result, a possible signed overflow.

`Years{inf}` passes line 165. `scan_futures_calendar` has the same flow: calendar.hpp:269 gives a NaN carry, the comparison at 287 is false, and 295-296 casts NaN.

**Trigger.** A missing rate or yield parsed as NaN, or `t = inf`.
**Impact.** UB in a noexcept scanner. On x64, `net` becomes about INT64_MIN, so `actionable()` stays false, but direction and value are meaningless. Detection only.
**Tests.** No non-finite case.
**Regression.** Set `q.rate = std::nan("")`, then separately `dividend_yield = NaN` and `t = Years{inf}`. `scan_cash_futures` and `scan_futures_calendar` must refuse (a new `BadRate` or `BadTenor`), not return a value.

### C10-002 — cross-venue cost omits the closing legs (P2, CONFIRMED DEFECT)
**Evidence.** basis.hpp:29-31 says: "buy NSE, sell BSE, both squared off before the close". Lines 357-363 call the scan INTRADAY.
The cost has two legs only (lines 388-397: `o.cost = Notional{lb->total.raw() + ls->total.raw()}`). Squaring off on each venue takes four trades: buy NSE, sell NSE, sell BSE, buy BSE. The unwind also crosses both spreads again.
If the two opening legs are meant to net against each other across exchanges, neither is plainly an intraday trade, so the 0.025% sell-side intraday STT rate is doubtful. Either reading makes the cost too low.

**Trigger.** Any call. test_basis.cpp:365-383 prices a 60-paise cross on 500 shares at Rs 584 for the two legs.
**Impact.** Cost is about half of the four-leg intraday cost, so `actionable()` can be true for a cross that loses money. Detection only.
**Tests.** Test 5 asserts only `net < gross` and sell-side STT.
**Regression.** Price the same quote as four intraday legs and assert that `o.cost` equals their sum, or record the netting assumption and test it against the schedule.

### C10-003 — "Johansen" is a restricted single-root statistic (P2, CONFIRMED DEFECT)
**Evidence.**
- cointegration.hpp:336-337 apply the same weights `(c, s)` to the levels and to the differences. Johansen maximises the squared canonical correlation over separate vectors for Δz_t and z_{t-1}.
- Line 346, `trace_stat = -m*log(1 - best)`, uses only the largest root. The r=0 trace statistic is `-T Σ ln(1-λ_i)` over both roots.
- Line 299 compares against 15.4948, which is the trace critical value.
- The VAR has no lag augmentation.

**Consequence.** The restricted maximum is at most λ1, and `-T ln(1-λ1)` is at most the trace. The reported number is therefore always at or below the true trace, yet it is compared with the trace critical value, so the test under-rejects.
Structurally it is a Dickey-Fuller-type test on the best spread. That means "Johansen agrees with Engle-Granger" (header lines 27-29; test_cointegration.cpp:146-153) is not an independent second opinion. The 1-degree angle grid (line 329) is also coarse when the two price levels differ by about 10x.

**Impact.** `PairCandidate::tradeable` needs `joh.cointegrated` (line 397), so real pairs are thrown out. The label is misleading. Research path only.
**Tests.** Only agreement with EG is checked, never a reference value.
**Regression.** On a fixed seeded pair, compare with a reference VECM trace (statsmodels `coint_johansen`, det_order=0, k_ar_diff=0). Report both eigenvalues.

### C10-004 — ADF augmentation wrong for lags >= 2 (P2, CONFIRMED DEFECT)
**Evidence.** cointegration.hpp:196-209 regress `de` and `lev` on each lagged difference in turn. But `lag[i]` (line 200) is the raw L-th lagged difference; it is never residualised on the earlier lags.
Frisch–Waugh–Lovell needs a joint projection on all the lagged differences. Projecting on them one at a time only matches when they are mutually orthogonal, and they are not for an autocorrelated residual.
Two smaller issues:
- `ols` uses `dn - 2` degrees of freedom (line 128), though the regression has 2 + lags parameters.
- Line 282 looks up `eg_critical(n, ...)` at the full n, not the effective m.

**Trigger.** `engle_granger(..., lags >= 2)` or `adf(..., lags >= 2)`.
**Impact.** The t-statistic is wrong for every multi-lag call; which way it is biased depends on the data.
**Tests.** Every test and the desktop call use lags=1 (test_cointegration.cpp:110,111,188,189,231,285; quant_pages.hpp:2487,2539).
**Regression.** Build a seeded AR(2) residual. Assert that `adf(e,n,2).t_stat` equals the t-ratio from a joint OLS of de on [1, e_{t-1}, de_{t-1}, de_{t-2}].

### C10-005 — structural break blind at the start of the window (P2, CONFIRMED DEFECT, analytic)
**Evidence.** cointegration.hpp:461-466 set `denom` to the SSE over [burn_in, n). Line 476 is `cum = run/denom - expected`, so only a change in residual variance inside the test window registers.
If beta moves at `burn_in`, every out-of-sample residual is roughly `Δβ·x_i + u_i`. When the price level dominates its own variation (x ≈ 2,900 ± 50), `e_i²` is nearly constant. Then `run/denom` follows the linear path and `max_excursion` stays small: `broke = false` for the very break the header calls an EXIT.
Also, `t.at` (line 479) is chosen with a `denom` built from data after `at`, so treating it as a dated exit in a replay is look-ahead. `band` has no critical-value source.

**Trigger.** y = 1.0·x + ε before bar 100 and 1.3·x + ε after, with x a random walk around 2,900, burn_in=100, n=300.
**Tests.** The only break is at i=250 with burn_in=150 (test_cointegration.cpp:311,315), in the middle of the window, where the variance does change inside it.
**Regression.** The trigger series above; assert `broke`. Fix by normalising on the burn-in residual variance or by using recursive residuals.

### C10-006 — refusals name the wrong reason (P3, CONFIRMED DEFECT)
- cointegration.hpp:186, 269-270, 314-315 and 365-366 return `TooFewSamples` when n is too LARGE (more than 2048).
- `NoCriticalValue` (line 90) is never used.
- Line 455 returns `NoHoldingPeriod` for `band <= 0`.
- calendar.hpp:141-143 returns `BadSlice` for `CalendarAlignment::Unknown`; test_calendar.cpp:176-181 locks that in.
- fundamentals.hpp:120-122 returns `NoFilingDate` for a row that is missing its period end or shares.

Rule 11 requires a refusal to say which bound was hit. The desktop works around the 2048 cap on its own (quant_pages.hpp:2475).
**Regression.** n=2049 must return a distinct `WindowTooLarge`.

### C10-007 — fundamentals have no period type (P2, DESIGN GAP)
`Fundamental` (fundamentals.hpp:80-107) has period_end, filed and revision. It has no period length (quarter or full year) and no consolidated/standalone flag.
- `as_of` (138-156) keeps the latest period_end. A 31 March FY row and a Q4 row therefore tie; revision decides, and if revisions are equal the first row inserted wins (strict `>`, line 148). Rule 9 says ambiguity blocks.
- By design the multiples (188-247) use unannualised period figures. A sector median over names whose latest row is sometimes a quarter and sometimes a year mixes P/Es about 4x apart.
- `insert` accepts duplicate (instrument, period_end, revision) rows without complaint.

**Tests.** None of these cases.
**Regression.** Insert a Q4 row and an FY row with the same period_end. `as_of` must refuse or make the caller name the period type.

### C10-008 — `relative_to_sector` sign and NaN (P3, CONFIRMED DEFECT)
- fundamentals.hpp:290 refuses only a zero median, although the error is called `NonPositiveDenominator`. `net_debt_to_ebitda` (240-247) is legitimately negative for net-cash firms, so a net-cash sector gives a negative median and line 293 inverts the ranking.
- A NaN peer breaks the insertion sort (line 278) silently.
- The caller's array is sorted in place, which is not documented.

**Regression.** Peers {-2,-1,-0.5,-0.2} with value -0.1 must be refused.

### C10-009 — mean-reversion cap and through-band jumps (P3, CONFIRMED DEFECT)
- meanrev.hpp:101 calls `max_position` the "Cap on |position|", yet line 202, `held * spec.max_position * std::min(stretch, 2.0)`, allows up to 2x.
- Line 196: a short entered at z > entry_z stays short if z jumps straight below -entry_z, because |z| never drops under exit_z. With `scale_with_z` the short then GROWS (line 201 uses `fabs(zi)`).

The clamp does not move toward the safe side. Research path only, and `scale_with_z` is off by default.
**Regression.** z path {+2, -3}: the position must not be short, and |p| <= max_position.

### C10-010 — cost excludes spread, impact and borrow; option calendar unpriced (P2, DESIGN GAP)
- `BasisQuote` holds single `spot`/`future` prices, not touches (basis.hpp:124-128). The carry never crosses a spread.
- Exit legs are priced at the entry prices (285-297) and convergence is assumed to happen at fair, so no exit half-spread is ever charged.
- `scan_futures_calendar` crosses the touch on entry (calendar.hpp:271-297) but also assumes the exit is at fair.
- With `borrow_available`, the reverse carry pays no SLB fee.
- `scan_option_calendar` (130-191) returns violation counts in total-variance space with no cost at all, and no staleness check even though `DatedSlice::observed` exists (line 100).

CLAUDE.md lists impact as part of the cost that removes textbook arbitrage, and rule 5 requires a net-of-full-cost number before a signal exists.

### C10-011 — filing visible for the whole filing date (P3, HYPOTHESIS)
fundamentals.hpp:144, `if (r.filed > when) continue;`, includes a filing stamped exactly at `when`. If a loader stamps the date at 00:00 while results come out after the close, every intraday `as_of` that day sees them. The test uses whole days (test_fundamentals.cpp:37,95-97). Depends on a loader outside this group.

### C10-012 — ADF lag overflow (P3, CONFIRMED DEFECT)
cointegration.hpp:178, `if (n < 32 + lags)`, wraps when `lags > SIZE_MAX-32`. Line 185 (`m = n-1-lags`) then wraps as well. Line 191 reads `e[t]` with a wrapped `t = i+1+lags`, which lands before the start of the buffer.
**Regression.** `adf(e, 500, SIZE_MAX-10)` must refuse.

### C10-013 — directional cost counted twice (P2, CONFIRMED DEFECT)
**Evidence.**
- `evaluate` charges `cost_bps` for each unit of |Δposition| (momentum.hpp:125-128, 152: `net[i] = gross[i] - to * cost_bps;`), and a +1→-1 flip is 2 units. That makes `cost_bps` a ONE-WAY cost.
- test_directional.cpp:128 defines `kCostBps = 5.5; // the project-wide round-trip hurdle`.
- meanrev.hpp:21-22 says "at 5.5 bps round trip is a hurdle of 11 bps PER BAR" for turnover 2.0.
- test_directional_real.cpp:194 uses `kCost = 5.5` in the same accounting.

A buy-then-sell round trip is 2 turnover units, so it is charged 11 bps against a stated 5.5 bps round-trip cost. The cost is also a flat scalar, not priced through `risk/cost.hpp` (sell-side STT, futures vs cash), and the final exit is never charged.

**Impact.** The real-data verdict "nothing clears; the gross edges are turnover" (test_directional_real.cpp:483-486) and the desktop pages that use `kCost` (quant_pages.hpp:885-1054; value not verified) are computed at double the stated cost. The error is on the conservative side, but a headline research conclusion depends on it.
**Regression.** Define the convention explicitly (`one_way_cost_bps`). Assert that a hand-built +1 entry and exit costs exactly one round-trip figure.

### C10-014 — `round_trips` double count (P3, CONFIRMED DEFECT)
momentum.hpp:148, `if ((p == 0.0) != (prev == 0.0) || p * prev < 0.0) ++out.round_trips;`, fires on entry (flat→long) and again on exit (long→flat). One round trip counts as 2. The doc at 71-76 says "changed sign or returned to flat".

### C10-015 — carry conventions left to the caller (P3, DESIGN GAP)
- Dividends are only a continuous gross yield (basis.hpp:134-136; calendar.hpp:207). A discrete single-stock dividend going ex before expiry is not modelled, nor is the tax on dividends the cash-leg holder receives.
- `Years t` has no day-count check (basis.hpp:129-131). Tests and the desktop use literal 30/365 and 60/365 (quant_pages.hpp:3828,3831).
- `qty` is never checked against the lot size, so rule 1 depends entirely on the caller.

### C10-016 — overnight returns dropped, positions still carried (P2, CONFIRMED DEFECT)
**Evidence.** test_directional_real.cpp:183-190 drop every return that crosses a session boundary and join the remaining returns into one vector. `momentum_positions` / `meanrev_positions` (373, 406) then run on that joined vector:
- A position decided at the last bar of day D is applied to the first kept return of day D+1.
- The overnight move it would really have carried is gone from P&L.
- No flat-at-close exit is charged either.
- `level_from_returns` (meanrev.hpp:151-160) builds a price path with the gaps removed, a path that never traded, so z-scores at the open are measured against a level that ignores the gap.

The same vector feeds buy-and-hold (279-281), which is why "buy-and-hold is negative intraday" (298-300).
**Impact.** The intraday cells score neither an intraday-flat strategy (no forced exit cost) nor a hold-through strategy (no gap exposure). Research verdict only.
**Regression.** Force position = 0 on the last bar of each session and charge the exit, or keep the gap return. Assert that a synthetic series with a large known gap produces the expected P&L.

### C10-017 — real-data test semantics (P3, DESIGN GAP)
- test_directional_real.cpp:250-254 returns 1 (FAIL) when any frequency has fewer than 2000 returns. ctest `strategies_directional_real` therefore fails on any checkout without `dataset/`, instead of skipping.
- The verdicts (483-493) are printed only. PASS depends on the dataset being present and on `bh_net[3] > 0` (288), never on any strategy result.
- Line 480, `check(beat_bh <= corrected + n_cells, ...)`, is always true.
- The Bonferroni cut-off is two-sided but applied one-sided (452-454), which is conservative.
- In-sample status is disclosed (494-496), which is honest.

## 4 Lead revalidation

**basis.hpp**
- Arithmetic is correct: fair = S·expm1((r−q)T), mispricing = (F−S) − fair, implied repo = ln(F/S)/T + q.
- Cost uses four legs through `compute_cost`: cash legs as delivery (STT both sides), futures legs sell-side only.
- T comes from the caller with no day-count check.
- Dividend is a continuous yield only.
- Defects: non-finite inputs (C10-001), no spread or exit cost (C10-010), cross-venue legs (C10-002), conventions (C10-015).

**calendar.hpp**
- Option-calendar k-alignment is correct: w_long(k) − w_short(k) at the same k, with `Strike` mode kept only for comparison.
- Futures calendar: fair = near·expm1((r−q)Δt), touch-correct in both directions, four futures legs costed.
- The option calendar has no cost (C10-010). NaN handling as in C10-001.

**cointegration.hpp**
- EG critical values are the MacKinnon (1991) N=2, constant, no-trend response surface (−3.3377 − 5.967/n − 8.98/n² at 5%). That is the correct table, not a plain ADF table.
- The Johansen statistic is defective (C10-003).
- ADF with lags >= 2 is defective (C10-004).
- Half-life −ln2/ln(1+β) is correct and refuses β >= 0.
- The hedge ratio is fitted on the same window that the z-score's last point comes from. That is in-sample, and look-ahead only if the caller's window extends past the decision.
- Break test: C10-005.

**fundamentals.hpp**
- Keyed on the filing date. Undated rows and filings before the period end are refused, and restatements are kept as separate rows. The point-in-time design is sound.
- Gaps: C10-007 (period type), C10-011 (same-day visibility).

**meanrev.hpp / momentum.hpp / test_directional*.cpp**
- Rolling windows are causal: momentum.hpp:295-305 use r[..i-1], meanrev.hpp:128-144 use level[..i-1], and test_directional.cpp:148-196 proves it by perturbation.
- Returns are net of cost, but the cost is double-counted (C10-013) and is a flat scalar rather than the cost calculator.
- The real-data verdict is honest in structure: paired buy-and-hold benchmark, Bonferroni over the actual cell count, in-sample disclosed. It is weakened by C10-013 and C10-016, and nothing about it is asserted (C10-017).

## 5 Architecture facts

- `altair_strategies` is an INTERFACE library that does not link oms (CMakeLists.txt:11-22). Rule "only oms places orders" holds for this group.
- **Implemented vs connected.** A Grep across app, oms, server, backtest, models, risk, flagging, features, feed and book found no caller of any function in this group. The engine never invokes them. Their only callers are the tests and desktop/quant_pages.hpp research pages:
  - `measure_basis` at 1786, on a synthetic future priced exactly at carry;
  - `scan_option_calendar` at 3457-3518;
  - `scan_futures_calendar` at 3835 and 3871, on synthetic touches with literal tenors;
  - `engle_granger` at 2487 and 2539;
  - momentum, meanrev, `evaluate` and `excess_over` at 885-1054 and 2003-2005.
- quant_pages.hpp:3315-3316 describes these as "the same functions the engine calls". No engine caller was found for the G10 functions.
- `scan_cash_futures`, `scan_cross_venue`, `johansen_bivariate`, `assess_pair`, `structural_break`, `half_life` and all of fundamentals.hpp are called from tests only.
- **Cost integration.** basis and futures-calendar scanners price every leg through `risk/cost.hpp` `compute_cost` with an effective-dated `ChargeSchedule`. Option calendars, cointegration and the directional strategies do not use the cost calculator; directional strategies use a flat bps scalar.
- **Hot path.** None of these headers is marked `ALTAIR_HOT`. Directional code allocates `std::vector` and is research-only. Cointegration uses `static thread_local` fixed buffers and refuses windows over 2048.

## 6 Test-suite observations

- test_basis, test_calendar, test_cointegration, test_directional and test_fundamentals are synthetic and deterministic (seeded LCG) and read no dataset. Their PASS does not depend on market data.
- **test_directional_real.cpp reads real data:** `ALTAIR_DATASET_DIR/spot/nifty/{5m,15m,60m,1d}/all.csv`, or the `*.csv` files in name order, taking the date prefix from column 0 and the close from column 4 (not opened by this audit). Details:
  - It needs 2000 returns per frequency, otherwise it FAILS (not a skip).
  - bars/year are measured from the data.
  - Session-boundary returns are dropped for intraday series, with a count check that sessions−1 were dropped.
  - Verdicts are printed against zero and against buy-and-hold at a Bonferroni t_crit; none is asserted.
  - PASS does not depend on whether any strategy clears.
- test_directional.cpp:313-392, the pooled 20-path random-walk control, is sound and honestly reports how many single paths look profitable.
- **Gaps:** no non-finite inputs (C10-001); no four-leg cross-venue (C10-002); no Johansen reference (C10-003); no lags >= 2 (C10-004); no start-of-window break (C10-005); no Q/FY tie (C10-007); no negative median (C10-008); no `scale_with_z` (C10-009); no round-trip cost convention test (C10-013).
- Test literals such as lot 500 or 75 and 30/365 are fixtures, not rule-1 violations in production code.
- test_fundamentals.cpp:72 builds a ~393 KB `FundamentalStore` on the stack.

## 7 Open questions

1. Is an NSE buy plus a BSE sell of the same scrip in one session netted by clearing and the broker (and charged at intraday or delivery STT), or is it two independent obligations? This decides the correct fix for C10-002.
2. Is the "project-wide 5.5 bps" meant as one-way or round-trip, and which constant does desktop/quant_pages.hpp use for `kCost`? (C10-013)
3. Where will live `Years t` come from — expiry instant, and which day count — once a scanner is wired to the engine?
4. Is any engine wiring of these scanners planned? quant_pages.hpp:3315-3316 describes them as engine functions, but no engine caller exists at a34af5c.
