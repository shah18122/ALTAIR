# G07_analytics_b -- CX-01 findings

Reader: claude-subagent:G07_analytics_b · Date 2026-09-14 · Baseline a34af5c5c9fa8c7498e8cc4df00907548a41e1a5

## 1 Scope & coverage

Assigned 15 files, 5,657 lines: eight headers (`analytics/{hurst,iv,kalman,rolling,sabr,svi,svi_fit,vix}.hpp`) and seven tests (`analytics/tests/test_{hurst,iv,kalman,rolling,svi,svi_fit,vix}.cpp`). All 15 are **fully read**, in Read chunks of 300 lines or fewer. The sha256 before and after matched the inventory for every file.

- No build, test or executable was run.
- Three numeric claims (C07-001, C07-003, C07-008) were reproduced in a scratch Python mirror of the C++ double arithmetic, same operation order. MSVC/clang FP contraction could change the last ulp, but not the qualitative result.
- Not read, and outside this group: `analytics/greeks.hpp` (grep only, for signatures) and `analytics/tests/test_surface_hmm.cpp`, which holds the only SABR/Dupire tests.

## 2 Findings table

| ID | Sev | Class | Location | Title |
|---|---|---|---|---|
| C07-001 | P3 | CONFIRMED DEFECT | sabr.hpp:72,106-113 | `valid()` accepts nu=0; `sabr_vol` then refuses every off-ATM strike, and is 7% wrong at nu=1e-15 |
| C07-002 | P3 | CONFIRMED DEFECT | kalman.hpp:85-126 | Non-finite y, h, q, r or x0 are accepted and permanently poison the filter state |
| C07-003 | P3 | CONFIRMED DEFECT | hurst.hpp:145-167 | A constant series at a non-representable value returns H=0.950±0.031, "significant", instead of Degenerate |
| C07-004 | P3 | HYPOTHESIS | hurst.hpp:260-278 | The 1/sqrt(n) SE floor may understate the true sampling SD of R/S-AL; validated on one seed only |
| C07-005 | P3 | CONFIRMED DEFECT | vix.hpp:108-111,236-259 | `truncated_low/high` counts absent quotes, not the quoted strikes dropped past the cutoff |
| C07-006 | P3 | DESIGN GAP | vix.hpp:83-93,114-119,218-259 | CBOE zero-bid rules can't be applied (no bid); zero or negative mids not refused; NSE deviations unmodelled |
| C07-007 | P3 | DESIGN GAP | sabr.hpp:212-228 | Dupire dw/dT is taken at fixed strike with one forward; the formula needs fixed log-moneyness |
| C07-008 | P3 | CONFIRMED DEFECT | sabr.hpp:150-161 | `sabr_min_density` prices at int64-truncated strikes but divides by the untruncated h^2, giving false arbitrage |
| C07-009 | P3 | CONFIRMED DEFECT | svi_fit.hpp:65-66,104-107,337-398 | `NoConvergence` is never returned; `projections` is 0/1, not the documented count |
| C07-010 | P3 | CONFIRMED DEFECT | iv.hpp:119-125 | `iv_uncertainty` passes a NaN or negative price uncertainty through as the band |
| C07-011 | P3 | DESIGN GAP | hurst.hpp:98-100,193 | The `kHurstMaxScales` loop cap stops silently; its unreachability proof is not stated at the bound (rule 11) |
| C07-012 | P3 | DESIGN GAP (doc drift) | ROADMAP.md:306,678,1045 vs iv.hpp:6-24 | ROADMAP still promises Jäckel at ~2 ns with no Newton loop; the code is safeguarded Newton at ~1.6 µs |

No P0, P1 or P2. None of the confirmed defects sits on an order-placing path in the code I traced (see §5).

## 3 Finding details

### C07-001 -- SABR at nu -> 0
- **Evidence:**
  - sabr.hpp:72 has `rho > -1.0 && rho < 1.0 && nu >= 0.0`, so nu=0 is valid.
  - sabr.hpp:106 has `const double z = p.nu / p.alpha * fk * log_fk;`
  - sabr.hpp:111-112 refuses when `|xz| < 1e-300`.
  - The ATM guard at line 100 tests `log_fk` only. It never tests z, and there is no small-z series for z/x(z).
- **Trigger** (F=100, K=110, t=1, alpha=0.2, beta=1, rho=0). The Hagan limit is exactly 0.2.

  | nu | Result |
  |---|---|
  | 0, 1e-18, 1e-16 | `BadCalibration` |
  | 1e-15 | 0.21462 (+7.3%) |
  | 1e-14 | -0.18% |
  | 1e-13 | +5.6e-4 |
  | 1e-12 | +9.1e-5 |

  At paise scale (F=2,408,000, K=2,500,000, beta=0.5, alpha=232.8, rho=-0.5, T=30/365): nu=1e-14 gives 0.14072, against 0.14862 at nu=1e-9 (-5.3%).
- **Impact:** a calibrator that drives nu toward its lower bound on a flat smile gets refusals or percent-level vol errors. No production caller was found, so impact today is limited to tests and future use.
- **Tests:** test_surface_hmm.cpp is outside this group and was not read. None of my files cover it.
- **Regression:** `sabr_vol(100,110,1,{0.2,1.0,0.0,0.0})` must return 0.2 to 1e-12. The same inputs with nu=1e-15 must also return 0.2 to 1e-9. Fix with a series for |z| < ~1e-6: z/x(z) ≈ 1 − ρz/2 + O(z²).

### C07-002 -- Kalman1D accepts non-finite input
- **Evidence:**
  - kalman.hpp:86 has `if (!(p0 > 0.0) || !(q > 0.0) || !(r > 0.0))`. Infinity passes, and x0 is never checked.
  - kalman.hpp:110-115: `update()` computes `v = y - h*x_` and `x_ = x_ + k*v` with no finiteness test. The only guard is `!(s > 0.0)` at 112.
- **Triggers:**
  - `make(0,1,1e-4,1e-2)` then `update(NaN,1.0)` returns success. `state()` is NaN from then on: a later `update(1.0,1.0)` computes `x_ = NaN + k*v`. `nis()` is NaN for good.
  - `update(1.0, inf)`: s=inf passes, k = inf/inf = NaN.
  - `make(0,1,inf,1)` constructs, and the first update gives k=NaN.
- **Expected:** refuse with an error and leave the state unchanged, as `RunningMoments::add` / `RollingWindow::push` do (counted rejection).
- **Impact:** none live today. The only non-test include is desktop/quant_pages.hpp, which has no call site.
- **Tests:** test_kalman.cpp section 4 checks only zero/negative variance refusal.
- **Regression:**
  1. After `update(NaN,1)`, expect `!ok` and `state()==0.0`, with `observations()` unchanged.
  2. `make(0,1,inf,1)` must be refused.

### C07-003 -- Hurst: a "constant" series yields strong persistence
- **Evidence:**
  - hurst.hpp:162-163: `const double s = std::sqrt(ss / m); if (!(s > 0.0) ...) return false;` The zero-spread test is absolute.
  - For a constant non-representable value, the summed mean rounds away from x. Every deviation d is then the same ~1e-17, cum is linear, and R/S = |m·d|/|d| = m exactly.
- **Trigger:** `hurst_rs` on 1024 copies of 0.1 (also 0.05 or 0.3) returns:
  - h = 0.9500617, std_error = 0.03125, 6 scales
  - block R/S = 16, 32, …, 512, i.e. exactly m
  - so `hurst_departs_from_random_walk` → **true**.

  Control: 2,408,000.0 and 0.0 both return `Degenerate`, as test 5 expects.
- **Impact:** a flat-but-nonzero increment series gives "significant persistence" instead of a refusal, in the one direction the header calls most dangerous. Examples are a constant log-return from a synthetic or replayed drift, or a constant spread in rupees. Reachable through strategies/regime.hpp:170 (→ score.hpp → models/aggregator.hpp; flagging/scorecard.hpp) and features/kinematics.hpp:253. A real stale feed yields exact zeros, which are refused correctly, so live reach is narrow.
- **Tests:** test_hurst.cpp:246-252 uses only the exactly representable 2,408,000.0.
- **Regression:** 1024 × 0.1 must return `HurstError::Degenerate`. A relative test fixes it: s <= 64·eps·max|x| treated as zero spread.

### C07-004 -- Hurst SE floor (hypothesis)
- **Evidence:** hurst.hpp:276 has `const double sampling_floor = 1.0 / std::sqrt(static_cast<double>(n));`. The rationale at 270-275 is a single run.
- **Concern:** 1/sqrt(n) is Peters' asymptotic variance for the uncorrected estimator. Published Monte Carlo work (Weron 2002, from memory and not verifiable offline) gives R/S-AL confidence bands several times wider at n ≤ 1024. With dyadic non-overlapping blocks from m=16, the true SD of `h` at n=256..1024 may exceed the floor, so `hurst_departs_from_random_walk(e, 2.0)` could flag iid noise far more often than 5%.
- **Tests:** test_hurst.cpp tests 1 and 4 use one seed (0xC0FFEE), so a single pass says nothing about the false-positive rate.
- **Reproducer:** draw 2,000 iid N(0,1) series at n ∈ {256, 512, 1024}. Report the fraction with `departs == true` and the empirical SD of `h`. Expect ≤ 5% and SD ≤ `std_error`. Otherwise recalibrate the floor from that table.

### C07-005 -- VIX truncation counters count the wrong thing
- **Evidence:**
  - vix.hpp:110-111 documents "Strikes dropped by the truncation rule, below and above K0".
  - Lines 238-241 (and 251-254) do `++out.truncated_low` only for each absent quote seen before the break. Quoted strikes beyond the break are never counted.
- **Trigger:** test_vix.cpp test 5's own setup, with two adjacent holes near 0.97F. `ended->strikes_used` falls by the entire lower wing, hundreds of fully quoted strikes, while `truncated_low` is 2.

  Minimal case: strikes 100..1000 step 100, both sides quoted except no put at 400 and 300, parity forward ~850.
  - K0 = 800. Puts 700, 600, 500 are used. 400 and 300 are the gaps.
  - The quoted puts at 200 and 100 are dropped.
  - Result: `truncated_low` = 2. It should be 2 dropped quoted strikes plus 2 absent, reported separately.
- **Impact:** the rule-11 "count what was dropped" signal understates the dropped strip. It is the one number that would expose a truncated, downward-biased index. No non-test caller exists.
- **Tests:** test_vix.cpp:252-254 asserts only `>= kVixTruncateAfter`.
- **Regression:** on the minimal case, assert `truncated_low` reports the dropped quoted strikes (2). Add a separate `absent_low` = 2 if both are wanted.

### C07-006 -- VIX quote model vs CBOE/NSE rules (design gap)
- **Evidence:**
  - vix.hpp:87-93: `VixQuote` has mid prices and has_call/has_put, but no bid.
  - vix.hpp:114-119 substitutes "two consecutive absent quotes" for CBOE's two consecutive zero bids.
  - vix.hpp:244-245 counts a present zero-priced quote as a real quote, which resets the gap.
  - Nothing refuses a negative `call`/`put` raw value.
- **Consequences:**
  1. An ask-only wing (bid 0, stale ask) gives a positive mid and enters the strip. CBOE excludes every zero-bid option.
  2. A long run of zero mids never truncates. That one is harmless, since they contribute 0.
  3. A negative mid subtracts variance silently.
  4. NSE's published India VIX deviations cannot be checked offline and are not modelled. From memory these may include a forward from NIFTY futures, a MIBOR-tenor rate, the roll timing (`vix_should_roll` uses CBOE's legacy 7 days) and the missing-quote treatment.
- **Impact:** not connected to any executable today. The strip correctness depends entirely on the caller setting has_put/has_call from bid > 0.
- **Regression:** a quote with `put = Price{-5}` must be refused (`NotFinite` or a new `BadQuote`). Document the caller contract that has_* means bid > 0.

### C07-007 -- Dupire time derivative at fixed strike (design gap)
- **Evidence:**
  - sabr.hpp:220-221 computes `w(strike, t + dt)` and `w(strike, t - dt)`, with one `forward` argument for all maturities.
  - The implied-vol-form formula at 191-193 needs ∂w/∂T at fixed y = ln(K/F_T).
- **Impact:** the difference is −(r−q)·∂w/∂y. For a 30-day NIFTY smile (r−q ≈ 0.055, ∂w/∂y ≈ −0.0074) that is ≈ 4e-4/yr against ∂w/∂T ≈ 0.0225, about 1.8% in local variance. It is exact only when the IV functor is keyed on forward moneyness with a T-independent forward, or when r = q. This is undocumented. There are no non-test callers.
- **Regression:** give the surface σ(y) = 0.2 − 0.3·y with y = ln(K/F_T) and F_T = 100·e^{0.05T}. `dupire_local_vol` at K = F_T, T = 0.5 must match a fixed-y finite difference to 1e-4.

### C07-008 -- sabr_min_density strike truncation
- **Evidence:**
  - sabr.hpp:151 evaluates `sabr_vol(forward, K, t, p)` at the real K.
  - sabr.hpp:154-155 prices at `Price{static_cast<std::int64_t>(forward)}` and `Price{static_cast<std::int64_t>(K)}`.
  - sabr.hpp:160 divides by the untruncated `h * h`.
- **Trigger** (alpha=0.2, beta=1, rho=0, nu=0.3, F=100, t=1, k_lo=80, k_hi=120, n=81, so h=0.5):
  - With truncation, min density = **−3.322** at K=81, so `sabr_density_positive` → false.
  - The same grid priced at the true strikes gives min +0.0100, so the smile is arbitrage-free.
  - At paise scale (F=2,408,000, T=30/365, alpha=0.15, beta=1, rho=−0.3, nu=0.5, grid 2,000,000..2,800,000, n=1000, h≈800.8 paise): truncated −1.557e-6 against exact +2.458e-9, also a false arbitrage flag.
- **Impact:** false "arbitrage" (and potentially a masked real negative density) whenever the grid is not integer-valued in the caller's units. No non-test caller.
- **Tests:** only test_surface_hmm.cpp (not read), which calls with `F * 0.85 .. F * 1.15, 40`.
- **Regression:** the first input set above must return a positive min density (~0.0100 ± 1e-3). Fix by pricing through `detail::black76_unchecked` in double.

### C07-009 -- svi_fit convergence and projection reporting
- **Evidence:**
  - svi_fit.hpp:65-66 declares `NoConvergence`, "Nelder-Mead exhausted its budget", but no return statement produces it.
  - The loop at 338 simply exits at `cfg.max_iterations`, and line 395 stores `iter`.
  - Lines 104-107 document `projections` as "How many inner solves had to be projected". Line 397 sets `out.projections = in.projected ? 1 : 0` from the final solve only.
  - The rho clamp to ±0.999999 at 272-273 is not counted anywhere.
- **Trigger:** `SviFitConfig cfg; cfg.max_iterations = 0;` then `svi_fit(g_noisy_weighted, 21, cfg)` returns success from the initial simplex, with `iterations == 0`. `cfg.max_iterations = 3` returns a fit that is not converged with no error.
- **Impact:** desktop/quant_pages.hpp:1527 displays such a fit with no signal. It is not a trading path.
- **Tests:** test_svi_fit.cpp:457 checks `iterations < 400` only on a converging case.
- **Regression:** `max_iterations = 3` must return `SviFitError::NoConvergence`, or at least set a `converged == false` field. Also count projections across all objective evaluations.

### C07-010 -- iv_uncertainty input validation
- **Evidence:** iv.hpp:121-124 checks only vega and then returns `price_uncertainty / vega`.
- **Trigger:**
  - `iv_uncertainty(272607.0, NaN)` returns NaN.
  - `iv_uncertainty(272607.0, -5.0)` returns −1.83e-5, a negative error bar.
- **Impact:** features/options_calendar.hpp:289 writes the result into a feature slot. `svi_weight_from_iv_error` happens to map both cases to weight 0, so the fit path is safe.
- **Tests:** test_iv.cpp test 5c covers only vega ≤ 0.
- **Regression:** a NaN or negative price_uncertainty must return +inf, matching the "no information" convention, or be refused.

### C07-011 -- kHurstMaxScales proof not at the bound
- **Evidence:** hurst.hpp:98-100 says the ceiling exists "so the regression workspace is a fixed array". The loop at 193 has `m <= n / 2 && k < kHurstMaxScales`.
- **Analysis:** the 25th scale needs n ≥ 16·2^24·2 = 536,870,912 samples, over 4 GB of doubles. That is a valid proof the bound is unreachable in practice, but rule 11 requires the proof stated next to the bound. `scales` is reported, but dropped scales would not be.
- **Regression:** documentation only. Alternatively, return `TooFewScales` / a new error if the cap is hit with m ≤ n/2 still true.

### C07-012 -- ROADMAP still promises Jäckel
- **Evidence:**
  - ROADMAP.md:306: "IV solve: Jäckel's *Let's Be Rational* — no Newton loop that fails on wings".
  - ROADMAP.md:678: "~2 ns/solve".
  - iv.hpp:6-13 says it is NOT Jäckel; it is safeguarded Newton at about 1.6 µs with 3-14 iterations.
  - prompts/P3-04_iv.md:57 records that decision (D4).
- **Impact:** a latency budget or a design read from ROADMAP §11 would be off by ~800×. The code is honest; the design document is stale.
- **Regression:** doc fix. The 20 µs assert in test_iv.cpp:498 is the real budget and is not latency-gated.

## 4 Lead revalidation

**iv.hpp -- mostly holds.**
- The bracket starts at lo = 1e-9 and hi = 1.0 and doubles hi to at most 1024; one more step past that gives NoConvergence (line 199). That is at most 11 pricer calls, not counted in `iterations`.
- The Newton/bisection loop is capped at 100 (`kIvMaxIterations`). The `bisections` uint8 saturation at 255 is unreachable because 100 < 255.
- No-arbitrage refusal covers the open interval (df·max(F−K,0), df·F) for calls and (df·max(K−F,0), df·K) for puts. Both endpoints are refused, and this is tested.
- In the wings where vega underflows, the vega test is skipped and bisection runs until (hi−lo) ≤ 1e-12(1+σ). That takes at most ~50 steps, inside the budget.
- Once `resolution > 1e-13`, the stopping rule reduces to |price residual| ≤ 4·eps·target, a relative price criterion. That is the correct arithmetic floor, but not literally "converge on vol".
- Residual risk, a hypothesis only:
  - f(lo) < 0 at 1e-9 is assumed, never evaluated.
  - The loop reports success on bracket collapse without checking the residual.

  I found no concrete trigger with integer-paise public inputs.
- `iv_uncertainty` propagation dσ = dP/vega is correct and tested (5b, 5d), apart from C07-010.
- Jäckel claim: the header disclaims it. ROADMAP has drifted (C07-012).

**svi.hpp / svi_fit.hpp -- correct, with reporting defects.**
- g(k) matches Gatheral-Jacquier.
- The calendar scan uses total variance at fixed forward log-moneyness, which is correct.
- The domain check matches a + bσ√(1−ρ²) ≥ 0, |ρ| < 1, σ > 0, b ≥ 0.
- The Lee check exists as a separate function but is not invoked by `svi_check_params`. `svi_fit` enforces Lee in its projection.
- The fit is 2-D Nelder-Mead, default 400 iterations; the budget is not reported as failure (C07-009). The inner problem is a ridged Cholesky solve.
- Projection is sequential clamping (d≥0, |c|≤d, Lee scaling, a re-solve, then an a-floor), not a constrained least squares. It is a legal slice but not the constrained optimum at (m, σ).
- The scan covers the fitted range padded by 25%.
- Edge case: `svi_check_params` admits a minimum w of exactly 0, which contradicts the `svi_g` precondition comment at 195. g is then NaN at the argmin and the scan's `<` skips it. Not raised as a finding: it needs an exact floating-point hit.

**sabr.hpp -- formula correct; numerical edges fail.**
- Every Hagan (2002) term was checked: t1, t2, t3, z, x(z), the denominator, and the ATM limit α/F^{1−β}·corr.
- β ∈ [0,1] is validated.
- There is no refusal or warning for ν²T outside the expansion's domain (documented in prose only).
- Near-ATM: the ATM branch keys on |ln F/K| < 1e-9 instead of |z|. Cancellation for 1e-9 < |ln| < 1e-6 is ~1e-7 relative, which is harmless.
- Near ν = 0 the formula fails outright (C07-001). The density check has the truncation bug (C07-008). Dupire has the fixed-K dT (C07-007).
- The butterfly check is on prices, which is correct in principle.

**hurst.hpp -- holds, with caveats.**
- The Anis-Lloyd expectation is exact (lgamma form) and tested against Python.
- Bias correction regresses log(R/S) − log E[R/S].
- The error bar is max(regression SE, 1/√n). The floor's adequacy is unverified (C07-004).
- Degenerate detection is absolute (C07-003).

**kalman.hpp -- holds apart from input validation.**
- The scalar Joseph form p = (1−kh)²p_pred + k²r is ≥ 0 by construction for r > 0, and nothing drives it negative except non-finite inputs (C07-002).
- NIS = v²/S is correct and predicts before updating (no look-ahead).

**rolling.hpp -- holds.**
- Pebay/Welford updates match the reference RunningStats formulation term by term, and the update order is correct.
- Window capacity N is compile-time, and eviction is the definition of the window, not a rule-11 truncation. Non-finite input is rejected and counted.
- There is no Kahan/Neumaier in the shipped accumulators. This is a documented, measured decision; Neumaier exists only as the test referee.
- **Two-scale realised variance:** not implemented in any file in this group. Grep across analytics/desktop/features/strategies/app found no TSRV. CLAUDE.md's hygiene claim is not realised here.

**vix.hpp -- CBOE core correct; quote model thin.**
- Forward: parity K* + e^{rT}(C−P) at the strike with min |C−P|. Correct.
- K0: the largest listed strike ≤ F. Correct.
- ΔK: half the neighbour spacing, full spacing at the array ends. Correct, but computed on the full listed ladder, not on the strikes actually included.
- Contribution: ΔK/K²·e^{rT}·Q, with σ² = 2/T·Σ − (1/T)(F/K0−1)². Correct.
- Time is caller-supplied `Years`. There is no minutes-to-expiry logic, so accuracy depends on the caller. The 30/365 target is consistent with CBOE's 43,200/525,600.
- Near/next interpolation is in total variance with the ×365/30 annualisation. It matches CBOE and is verified from the definition in test 6.
- Missing quotes: two consecutive absent quotes truncate. The zero-bid rule can't be applied (C07-006), and the counters are wrong (C07-005).
- Roll at ≤ 7 days follows CBOE legacy; NSE's rule is unverified.

## 5 Architecture facts

- **Hot:**
  - `RunningMoments::add` and `RollingWindow::push` are `ALTAIR_HOT`, allocation-free and noexcept.
  - `svi_local` and `svi_total_variance` are `ALTAIR_HOT`.
  - RollingWindow queries are O(N) and not annotated.
- **Warm or cold, not annotated:** `solve_iv` (bounded loop), `hurst_rs` (O(n log n), stack workspace), `Kalman1D::update`, `svi_fit` (≤ 400 NM iterations × O(n)), `vix_term_variance` (O(n)), `sabr_*`, `dupire_local_vol`.
- Dupire is a template taking a callable and is not noexcept.
- None of the eight headers allocates or throws. None places orders, and none includes oms/.
- **Implemented vs connected:**
  - rolling: live-path consumers in strategies (regime, cointegration, vix_forecast), features/kinematics, and desktop.
  - hurst: features/kinematics, strategies/regime → strategies/score.hpp → models/aggregator.hpp, flagging/scorecard.hpp, desktop.
  - iv: features/options_calendar, desktop.
  - svi: strategies/calendar (params check, total variance), features/options_calendar (w, g), desktop.
  - svi_fit: desktop UI only.
  - kalman, sabr: `#include`d by desktop/quant_pages.hpp with no call site, so tests only.
  - vix: tests only.
  - I did not trace whether app/ links models/aggregator.hpp or flagging/scorecard.hpp.

## 6 Test-suite observations

- **Independent references:**
  - test_iv prices, test_hurst's Anis-Lloyd values and test_svi's w/g values are all stated as computed in Python.
  - test_rolling uses a two-pass Neumaier referee plus an exact int64 turnover referee.
  - test_vix uses a flat-surface truth and a continuous SVI integral.
  - test_svi ties the sign of g to butterfly prices.

  All strong.
- **test_hurst:** single PRNG seed for the null and for the error-bar tests (C07-004). The constant-series refusal uses an exactly representable value (misses C07-003).
- **test_kalman:**
  - Section 3 reads `ALTAIR_DATASET_DIR` (gitignored `dataset/`) and prints SKIP with exit 0 when absent, so the only real-data check can silently not run.
  - The check at line 173 sits inside the `else` of `s.size() < 500` and cannot fail.
  - Section 2 never runs the textbook update, so "the regime that breaks the textbook form" is asserted, not demonstrated.
  - There is no non-finite input test.
- **test_iv:** the 20 µs timing assert (line 498) is not behind `latency_gate_active()`, unlike test_rolling. It could spuriously fail under asan/debug. The public integer-paise entry points get only light coverage.
- **test_svi_fit:** no test for budget exhaustion or `NoConvergence`. `projections == 0` is asserted only on the final solve.
- **test_vix:** the truncation counters are asserted with `>=` only (C07-005). There is no test for negative or zero mids, nor for ask-only wings.
- **SABR/Dupire:** the only tests are in test_surface_hmm.cpp, outside this group and not read.

## 7 Open questions

1. What exactly does NSE's India VIX methodology use: a futures-based or parity-based forward, which roll day, and how are missing or zero-bid quotes handled (spline fill or truncation)? This can't be verified offline, and the header claims "CBOE applied to NIFTY".
2. Is models/aggregator.hpp or flagging/scorecard.hpp linked into `app/altair` and fed live returns? That decides whether C07-003 can reach a live regime call.
3. Should `hurst_departs_from_random_walk` be recalibrated from a Monte Carlo SE table, per the C07-004 reproducer?
4. Is the kalman/sabr include in desktop/quant_pages.hpp dead, or is a page planned? If dead, it widens the UI target's compile surface for nothing.
