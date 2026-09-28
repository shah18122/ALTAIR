# CX-01 Phase 1 -- G06_analytics_a findings

Reader: claude-subagent:G06_analytics_a · Date: 2026-09-14 · Baseline HEAD a34af5c5c9fa8c7498e8cc4df00907548a41e1a5

## 1 Scope & coverage

18 of 18 assigned files fully read, 5,553 lines, every line received through Read in chunks of 300 lines or fewer:
analytics/CMakeLists.txt, greeks.hpp, greeks2.hpp, american.hpp, chain_soa.hpp, bench/bench_chain.cpp,
derivatives.hpp, ewma.hpp, garch.hpp, hmm.hpp, and tests/test_{american,chain_soa,derivatives,ewma,garch,greeks,
greeks2,surface_hmm}.cpp. The sha256 before and after reading equals the partition inventory for every file. Nothing is
partial or unread.

Method limits: callers were found with Grep over first-party directories only. Files outside the group were not opened,
and ROADMAP.md was only grep-searched for section 11. I built, ran and benchmarked nothing, and executed no project code.
I derived every numeric trigger below by hand.

## 2 Findings table

| ID | Sev | Class | File:lines | One line |
|---|---|---|---|---|
| C06-001 | P2 | CONFIRMED DEFECT | hmm.hpp:112,127,242,264-266,285-298 | The -1e300 failure value is finite, so when every restart fails the code indexes empty vectors (UB or a crash) and never returns DidNotConverge |
| C06-002 | P2 | CONFIRMED DEFECT | derivatives.hpp:128-147 | velocity_std_error understates the SE about 6.9x (order 2, at the window edge). A 2-sigma velocity_is_significant flags about 77% of pure-noise windows |
| C06-003 | P2 | CONFIRMED DEFECT | derivatives.hpp:304-318 | A 4,096-point buffer silently truncates and keeps the FIRST 4,096 points in array order, which are the oldest for a chronological ring. Breaks rule 11, and the code comment claims the reverse |
| C06-004 | P3 | CONFIRMED DEFECT | american.hpp:117,164-165 | pow(S, beta) in paise overflows and 0*inf = NaN, so NotFinite is returned for a put at vol below about 5.3% with carry = r |
| C06-005 | P3 | CONFIRMED DEFECT | garch.hpp:224-235 | gamma_se uses a one-sided difference when gamma < 0.01, and reports 0.0 when it cannot be computed |
| C06-006 | P3 | CONFIRMED DEFECT | garch.hpp:277-284 | qlike silently drops rows with forecast <= 1e-300. A zero-variance forecast is left out of the average instead of scoring +inf |
| C06-007 | P3 | CONFIRMED DEFECT | chain_soa.hpp:133-137,171-173 | A +inf strike passes validation and writes NaN. There is no output-finiteness refusal like black76's |
| C06-008 | P3 | DESIGN GAP | bench/bench_chain.cpp:43-61,92-97; chain_soa.hpp:104-122 | Mean only, one run per cell, no percentiles, so it cannot test the section 11 p99 budgets. AoS padding makes AoS use 1.77x the memory, which biases SWEEP toward SoA |
| C06-009 | P3 | CONFIRMED DEFECT | greeks2.hpp:145 | Put charm uses N(d1) - 1, which cancels in the tail. This contradicts the erfc approach greeks.hpp uses; absolute impact is negligible |
| C06-010 | P3 | HYPOTHESIS | ewma.hpp:298-302 (also 117-118, 237-238) | effective_n forms W^2 - D, which cancels in steady state at small alpha. The comment "D is the small quantity" is wrong in that regime |
| C06-011 | P3 | DESIGN GAP | derivatives.hpp:382-402 | derivatives_multiscale compacts its results, so out[i] no longer matches windows[i], and per-scale errors are swallowed |
| C06-012 | P3 | CONFIRMED DEFECT | greeks.hpp:289-306 | forward_from_spot casts a finite F above INT64_MAX (UB) and does not validate T, so a negative T is accepted |
| C06-013 | P3 | DESIGN GAP | chain_soa.hpp:82-94,127-138 | The public ChainInputs / ChainSoA carry paise and years as raw double at a public API (rule 2). Nothing outside tests/bench calls it |

No P0 or P1. The only file in this group that production code reaches is greeks.hpp (desktop/, risk/ and others), and I
found no correctness defect in its formulas. Every P2 is in a header whose only callers are tests.

## 3 Finding details

### C06-001 (P2, CONFIRMED DEFECT) fit_hmm: the failure value passes isfinite, then empty vectors are indexed
Evidence: `if (!(s > 0.0)) { return -1e300; }` (hmm.hpp:112, 127); `if (!std::isfinite(ll)) { continue; }` (264);
`if (ll > best_ll) { best_ll = ll; best = h; }` with `double best_ll = -1e300;` (242, 266).
Trigger: the forward pass returns -1e300 when gauss() underflows to 0 for every state at some t (|z| > about 38.6).
Initial state sigma is at most 2*sd and mu is within +/-sd of the mean. One outlier more than about 77 sample sd from
the mean therefore fails every restart at iteration 0, deterministically. Example: x = 9,999 values alternating +1/-1,
plus one value of 1000.0. That gives n = 10,000, mean about 0.1, sd about 10.05, z >= (1000-10.15)/20.1 = 49, and
exp(-1210) = 0. A realistic version is one 10x price-scale glitch in a 9,000-row daily-return series.
Result: lls holds 8 copies of -1e300. The list is not empty, so the DidNotConverge check at line 268 is skipped. best is
still a default Hmm (k = 0, empty vectors). Lines 285-298 sort on `best.sigma[p]` and write `out.mu[i]` into empty
vectors, which is out of bounds: an assertion abort in an MSVC debug build, a null dereference or heap corruption in
release. Partial failures also push -1e300 into lls.
Impact: one bad data row causes a crash or memory corruption. The only caller is analytics/tests/test_surface_hmm.cpp.
Test coverage: only 20 samples and a flat 500-sample series (test_surface_hmm.cpp:298-304). No outlier case.
Regression test: the 10,000-point series above with k = 2 and seed 1 must return `HmmError::DidNotConverge` and must
not crash.
Fix direction: return -inf/NaN and test `ll > -1e299`, or refuse when `best.k != k` before relabelling.

### C06-002 (P2, CONFIRMED DEFECT) the velocity significance test is about 7x too permissive
Evidence: `const double h = 0.5 * static_cast<double>(d.scale.raw()) / 1e9;` and
`return d.residual_sd / (std::sqrt(static_cast<double>(d.points)) * h);` (derivatives.hpp:132-134).
Derivation (independent of the code): the fit runs in u over [-1,1]. The reported velocity is the derivative at t_eval,
which is the newest edge of the window, u_e = +1 (`unpack`, line 339). For N uniformly spaced points at order 2:
- Var(c1) = 3 sigma^2/N and Var(c2) = 45 sigma^2/(4N), with zero covariance.
- So Var(p1(1)) = 48 sigma^2/N, and se(v) = sqrt(48) sigma/(sqrt(N) h).

The code leaves out the factor sqrt(48) = 6.93 (at order 1 it leaves out sqrt(3) = 1.73). It also uses the half-width of
the REQUESTED window instead of the span of the points actually present. A partly filled window (the open, after a halt,
an illiquid counter) understates the SE further.
Trigger: 1,001 evenly spaced points over a 10 s window, a constant price plus iid N(0,1) paise noise, order 2. The code
gives se = 1/(sqrt(1001)*5) = 0.00632 paise/s; the correct se is about 0.0438 paise/s. The 2-sigma test therefore
rejects whenever |z_true| > 0.289, so P(flagged significant | zero velocity) is about 77% instead of about 5% (about 25%
at order 1).
Impact: the header calls this "the only sanctioned way to ask" whether a velocity is a signal (ROADMAP section 3). It is
called only from test_derivatives.cpp (lines 169-171, 194), so there is no live effect today.
Test coverage: test 2 uses a deterministic bounce whose fitted velocity is about 0 by symmetry. Test 3 compares SEs only
against each other (`rows[0].se > 4.0 * rows[3].se`), so calibration is never tested.
Regression test: Monte Carlo over 2,000 seeds of the case above. The fraction flagged significant must fall in
[0.03, 0.08].

### C06-003 (P2, CONFIRMED DEFECT) silent 4,096-point truncation keeps the oldest points
Evidence: `constexpr std::size_t kCap = 4096;` and `for (std::size_t i = 0; i < n && m < kCap; ++i) {` (307, 311).
The comment at 305-306 says "anything past it is dropped oldest-first by construction".
Trigger: n = 5,000 points at 1 ms spacing in chronological order, t_eval = ts[4999], a 10 s window, value 0 for i < 4096
and 100,000 for i >= 4096. All 5,000 points are in the window, but the fit uses only pts[0..4095]. It returns value about
0, velocity about 0 and points = 4096, with no flag. Expected: refuse, or count the drop visibly, because the newest
904 ms decide the answer.
residual_sd is computed over the same truncated slice (the rule-11 warning sign), and evaluation extrapolates beyond the
kept span (u_e is about 1.44).
Callers: tests only. Test coverage: the test buffer is `kMaxPts = 4000` (test_derivatives.cpp:73), so the bound is never
reached.
Fix direction: add DerivativeError::TooManyPoints and refuse, or gather newest-first and return a dropped count.

### C06-004 (P3, CONFIRMED DEFECT) Bjerksund-Stensland overflows in paise units
Evidence: `std::exp(lambda) * std::pow(S, gamma)` (117); `alpha = (X - K) * std::pow(X, -beta)` and
`alpha * std::pow(S, beta)` (164-165).
Trigger: put, S = K = Price{2'400'000}, T = 0.25, vol = 0.05, rate = 0.065, carry = 0.065. Working through it:
- The auxiliary call has r' = 0 and b' = -0.065, so beta = 2*(0.5 + 0.065/0.0025) = 53.
- pow(2.4e6, 53) = 10^338, which overflows to inf.
- pow(2.438e6, -53) = 10^-338.5, which underflows to 0.
- 0*inf = NaN, so american_value returns GreekError::NotFinite.

For this S the refusal starts below vol of about 0.0526. The formula is degree-1 homogeneous in (S, K), so the same
contract quoted in rupees would price. The refusal is an artefact of the units.
Impact: a refusal, not a wrong price; the fail-loud path holds. The header says nothing on an NSE path uses this, and the
only caller is a test. Test coverage: the test inputs are at most 20,000 paise, except P26-03 at vol 0.15.
`the_put_boundary_is_below_the_money` does `if (!p) { continue; }` (test_american.cpp:345), so a refusal there would pass
silently.
Fix direction: compute alpha*S^beta as (X-K)*(S/X)^beta, or normalise S and K by K before pricing.

### C06-005 (P3, CONFIRMED DEFECT) gamma_se is wrong near gamma = 0 and reads 0 when unknown
Evidence: `dn.gamma = best.gamma - h > 0.0 ? best.gamma - h : 0.0;` then
`const double curv = (f_up - 2.0 * best_nll + f_dn) / (h * h);` (227, 233). `out.gamma_se = curv > 0.0 ? ... : 0.0;`,
and gamma_se stays 0.0 when `up.stationary()` is false (228, 234).
Trigger: best.gamma = 0.0 is the grid's lower edge and the likely answer for a symmetric series. Then dn == best, so
f_dn = best_nll and curv = (f_up - best_nll)/h^2, which is about slope/h + curv_true/2. That is a first difference, not a
curvature. For 0 < best.gamma < 0.01 the step is asymmetric and the central formula does not apply.
Impact: the header says gamma is read through its SE. test_garch.cpp:131-132 turns se == 0 into t = 0, which is the safe
direction, but the printed verdict still rests on the invalid stencil. No assertion checks gamma_se.
Fix direction: use a one-sided second-difference stencil at the boundary, and return NaN or inf for "not computed".

### C06-006 (P3, CONFIRMED DEFECT) qlike drops zero forecasts instead of scoring them
Evidence: `if (!(v > 1e-300)) { continue; }` and `return acc / static_cast<double>(used);` (279, 284).
Trigger: forecast = {0.0, 1.0} with realised = {1.0, 1.0} returns 1.0, exactly what forecast = {1.0, 1.0} returns. True
QLIKE at v = 0 with r != 0 is +inf. In a horse race, two models can be averaged over different subsets of rows, and the
dropped count is never returned. Rule 11 ("truncate visibly") is not met. In today's test every forecast is > 0, so no
rows are dropped.
Regression test: `qlike({0,1},{1,1},2)` must not equal `qlike({1,1},{1,1},2)`. It should refuse, or report the count.

### C06-007 (P3, CONFIRMED DEFECT) price_chain accepts a +inf strike and never checks its outputs
Evidence: `if (!(c.strike[i] > 0.0))` (172) is the only strike check, and there is no isfinite check after the loop.
black76 has one (greeks.hpp:215-219). The header (47-49) says inputs are validated so that the loop cannot fail.
Trigger: strike[0] = +inf gives log(F/inf) = -inf and nd2 = 0, so `K * nd2` = inf*0 = NaN and call_px[0] = NaN, yet
success is returned. The degenerate inputs vol = 1e-300 and years = 1e-300 make vsqrtT underflow to 0, so d1 = 0/0 = NaN
at the money. black76 refuses those same inputs with NotFinite; the chain reports success. The "copy of black76" does
not copy its refusals.
Test coverage: only a zero strike and T = 0 (test_chain_soa.cpp:175-191).

### C06-008 (P3, DESIGN GAP) bench_chain cannot support a p99 budget, and AoS starts at a disadvantage
**What is timed** (bench_chain.cpp:43-61):
- One untimed warm-up call of body(), then body() repeated until at least 250 ms of steady_clock has passed.
- Clock::now() runs inside the timed loop after every call, so its cost is included (negligible per strike here).
- SWEEP body: price_chain over every chain, including its per-call strike-validation loop, plus one volatile write.
- LOOKUP body: 65,536 random indices, generated before timing starts (lines 143-148).

**Samples and statistic:** each cell reports exactly ONE number, total ns / (calls * units). That is an arithmetic MEAN
over a single run. There are no per-call samples, no p50/p99/max, no repetitions, no variance, and no pinning or
priority. The warm-up is a single call, which for the L1 row is roughly tens of microseconds.

**Comparability with ROADMAP section 11:** the section is titled "Latency budget (target, p99)" (ROADMAP.md:956). Its
rows are "Greeks, single option | 0.3 µs" (965) and "Greeks, full chain (~200 strikes) | 50 µs" (966), and line 683
adds "greeks + IV in < 50 µs". The bench instead prints mean ns per strike for 256-strike chains, prices greeks only with
no IV, and has no tail statistic. It can neither show nor refute either p99 budget.

**Layout fairness:** ChainRowAoS is padded to 128 B (chain_soa.hpp:121), against 72.25 B per strike for SoA.
- Unpadded AoS (72 B) has exactly SoA's density: 9 lines per 8 strikes. Padded AoS needs 16.
- The header says an unpadded record "would make AoS look worse than it is" (107-110). For SWEEP the reverse holds: the
  padding makes AoS touch 1.78x the memory.
- Level sizes are set in SoA bytes (bench 92-97), so AoS's working set is 1.77x larger at every level. On the "L2" row,
  SoA uses 0.5 of L2 and AoS about 0.89.
- On a 32 KB L1d, l1d/2 = 16 KB is smaller than sizeof(ChainSoA) = 18,496 B. The "L1d" row then holds one chain that
  misses its half-L1d target, and AoS (32.8 KB) exceeds L1d entirely.

Fix direction: store per-call durations in a pre-sized array and report p50/p99/max over at least 5 repetitions. Add an
unpadded 72-byte AoS variant, and size each level on the larger layout.

### C06-009 (P3, CONFIRMED DEFECT) put charm computed through N(d1) - 1
Evidence: `const double delta_cdf = (right == OptionRight::Call) ? nd1 : (nd1 - 1.0);` (greeks2.hpp:145).
Trigger: a deep OTM put with d1 in roughly [8.3, 37]. There 0.5*erfc(-d1/sqrt2) rounds to exactly 1.0, so the carry term
comes out 0 instead of -r*df*N(-d1).
Impact: absolute error is at most r*df*1.1e-16 per year (about 7e-18), which is negligible. It is inconsistent with the
erfc approach that greeks.hpp and chain_soa.hpp enforce. The test's r*df identity (test_greeks2.cpp:195) passes by
construction, and no test covers put charm in the tail. Fix: `-norm_cdf(-d1)`.

### C06-010 (P3, HYPOTHESIS) effective_n cancels in steady state
Evidence: `// V = W^2 - D. Safe as a subtraction in this direction: D is the small quantity, so nothing cancels.` and
`const double v = w_ * w_ - d_;` (ewma.hpp:298-300).
In steady state with a constant small alpha, W = 1 and D = 1 - alpha/(2 - alpha), so D is the LARGE term. With om held
constant, `om * om` carries a fixed rounding error of up to about 5.5e-17. That shifts the fixed point of D by about
5.5e-17/(2 alpha).
Worked case: dt = 1 ms and tau = 1 day give alpha = 1.157e-8 and a true sum(w^2) of 5.8e-9. The possible offset in D is
about 2.4e-9, so effective_n can be off by up to about 40%, with a data-dependent sign. Below alpha of about 1e-9, v can
reach <= 0 and NotFinite is returned. variance() is unaffected because D is about 1 there.
Separately, alpha = 1 - exp(-x) (117-118, 237-238) instead of -expm1(-x). The relative error in alpha is about
1.1e-16/x, negligible at realistic dt/tau.
Fix: carry V = sum(w^2) recursively as well (V <- om^2 V + alpha^2).
Test coverage: effective_n is checked only for two points (test_ewma.cpp:252).
Reproducer: run EwmaVariance with dt = 1 ms and tau = 86,400 s for 3*tau of updates, then compare effective_n with
(2 - alpha)/alpha = 1.73e8.

### C06-011 (P3, DESIGN GAP) derivatives_multiscale result indexing and docs
Evidence: `if (!d) { continue; }` and `out[produced++] = *d;` (398-399), against the doc's "one per window, in the order
given" (385).
- A skipped scale shifts later results down, so the caller has to read `scale` to know which window a result belongs to.
  The test passes only because all 4 scales succeed (test_derivatives.cpp:297-316).
- Singular and TooFewPoints cannot be told apart.
- Header line 22 says "weighted least squares", but poly_fit is unweighted, so bursts of ticks dominate an irregularly
  sampled window.

### C06-012 (P3, CONFIRMED DEFECT) forward_from_spot cast and T validation
Evidence: `return Price{static_cast<std::int64_t>(F + 0.5)};` (306). The only checks are finite and F > 0 (302).
Trigger: spot = Price{1'000'000'000'000'000'000}, t = Years{10}, rate = 1.0, q = 0 gives F = 2.2e22, which is finite, and
the cast is UB. Years{-1} is accepted silently. The magnitudes are unrealistic; noted for completeness. No test exercises
forward_from_spot in this group's files.

### C06-013 (P3, DESIGN GAP) chain_soa's public API carries money and time as double
Evidence: `double forward = 0.0;   ///< paise` and `double years = 0.0;` (128-129), plus a public
`double strike[kChainCap]` (85). greeks.hpp keeps its double form inside `detail::` and exposes `Price`/`Years` instead
(133-135), as rule 2 requires. chain_soa has no typed entry point. Nothing outside tests and bench calls it today.

## 4 Lead revalidation

- **greeks.hpp tail cancellation:** confirmed correct. Put delta and price use norm_cdf(-d1)/norm_cdf(-d2) on erfc
  (110-112, 165-168). The deep-OTM price is still a difference of two tiny terms (F N(d1) - K N(d2)) and loses roughly
  log10(|d1|/(sigma sqrtT)), i.e. 2-5 digits, but the absolute error is negligible. Not a finding.
- **greeks.hpp T -> 0 and sigma -> 0:** refused at T <= 0 and sigma <= 0 (202-207). There is no floor above 0: at T = 1 s,
  gamma is huge but finite and is returned. Test 8 walks T down to 1e-8. This is design choice D4, not a defect. If
  s*sqrt(T) underflows to 0 the result is NaN, and the output check refuses it.
- **Formulas re-derived:** Black-76 price, delta, gamma, vega, theta (per year, calendar t, including the +rV term) and
  rho = -T V are all correct. Black-Scholes-on-spot theta and rho are correct.
- **GreekBasis discipline:** black76 stamps Forward, black_scholes stamps Spot, black76_higher stamps Forward, and the
  default is Unspecified (ordinal 0). There is no converter for spot gamma (gamma_spot = gamma_fwd * e^(2(r-q)T)); only
  delta has one (394-398). Minor gap, not raised as a finding.
- **greeks2.hpp:** re-derived all eight: vanna -df phi d2/s, volga, speed, zomma, ultima, veta, colour, and charm
  (dd1/dT = -ln(F/K)/(2 s T^1.5) + s/(4 sqrtT)). All are correct; the only issue is C06-009.
- **american.hpp:** phi, beta, B_inf, B0 = max(K, rK/(r-b)), h(T), X, alpha and the six-term price all match BS-1993.
  The put transform to (K, S, r-b, -b) is exact, and the boundary back-transform S*K/X is correct because X is
  proportional to the auxiliary strike. Boundary cases:
  - beta is real and > 1 whenever b < r (the discriminant is >= (b/v^2 + 0.5)^2 >= 0).
  - For r <= 0 the put becomes European-exact through the b >= r branch.
  - The price is floored at the European value (177, 244), so the floor test passes by construction.

  The only issue is C06-004.
- **chain_soa.hpp:**
  - kChainCap refusal: confirmed (169, 209) and tested.
  - Math against black76_unchecked: the SoA and AoS sweeps use the same expressions except the association in theta
    (`theta_head*pdf1 + r*call`). That differs at the ulp level, so the results are not bit-identical, but they sit well
    inside the test's 1e-9 relative tolerance. SoA and AoS are bit-identical to each other on the 4 fields tested.
  - Alignment: enforced by static_assert (97-102, 123). sizeof(ChainSoA) = 64 + 9*2048 = 18,496 B, one column per line
    boundary. The "fits in L1D" and "line is 64" checks are runtime and depend on the host.

  Issues: C06-007, C06-013.
- **bench_chain.cpp:** mean only, one run, no p50/p99, a single-call warm-up, steady_clock with now() inside the timed
  loop. It cannot be compared with the section 11 p99 budgets. See C06-008.
- **ewma.hpp:**
  - The bias correction W^2/D and the D recursion are correct (D' = om^2 D + 2 alpha om W; two points give diff^2/2,
    which the test checks exactly).
  - Seeded by the first observation, so there is no look-ahead.
  - Design note: after one far-future timestamp, every later tick is refused as TimeWentBackwards, while value() keeps
    returning the stale estimate with no error and no staleness indicator.

  The only issue is C06-010.
- **garch.hpp:**
  - Stationarity alpha + beta + gamma/2 < 1 is enforced by never evaluating outside it (187); variance targeting keeps
    omega > 0. The alpha+beta<1 constraint and omega positivity are sound.
  - The likelihood starts from v0 = in-sample mean(r^2).
  - garch_filter seeds with the unconditional variance of the fitted parameters. In test_garch that means training-half
    variance scored on the held-out half, so there is no look-ahead in the test. Fitting and filtering the same sample
    would be look-ahead, and nothing in the API stops it.
  - A grid optimum sitting on the edge of the search box is not flagged.

  Issues: C06-005, C06-006.
- **hmm.hpp:**
  - Scaled forward/backward is correct: the backward pass reuses the forward scale factors, and ll = sum of log c_t.
  - The reported ll belongs to the parameters BEFORE the final M-step (off by one iteration).
  - Allocates a k*k std::vector per time step per iteration (161). Cold-path cost only.
  - The sigma floor of 1e-9 is absolute.

  Issue: C06-001.
- **derivatives.hpp:** irregular timestamps are handled by a centred and normalised polynomial least squares. Units are
  per second (int64 ns / 1e9). Future points are excluded (age < 0). Issues: C06-002, C06-003, C06-011.

## 5 Architecture facts

- **Hot vs cold:** ALTAIR_HOT marks black76, black_scholes, black76_higher, price_chain, price_chain_aos,
  Ewma::update and EwmaVariance::update. None of them allocate or throw; errors come back through std::expected.
  - derivatives_at is not marked hot. It uses a static thread_local 64 KB buffer, so it is per-thread and non-reentrant.
  - american_value and garch.hpp use no heap but are cold.
  - hmm.hpp allocates heavily (std::vector, and a tmp vector per step) and throws on allocation failure, so it is
    cold-only.
- **Implemented vs connected:**
  - I grepped app/, desktop/, strategies/, risk/, features/, models/, backtest/, book/, flagging/, server/, oms/, core/,
    feed/, instruments/ and broker/ for fit_hmm, fit_garch, garch_filter, qlike, proxy_mse, derivatives_at,
    derivatives_multiscale, velocity_is_significant, american_value, black76_higher, price_chain, chain_soa.hpp,
    EwmaVariance and ewma.hpp. There are no callers outside analytics/tests and analytics/bench.
  - greeks.hpp IS connected. `black76(`, `black_scholes(`, `forward_from_spot` or the header name appear in
    desktop/quant_pages.hpp, desktop/atlas_data.hpp, risk/portfolio.hpp, risk/hedge.hpp, backtest/mc_pricing.hpp,
    models/tensor.hpp and features/options_calendar.hpp, plus tests (files_with_matches only; I did not open these).
  - chain_soa.hpp, although it is the P39-00 commit, is included by nothing but its test and the bench.
- **Benchmark methodology:** bench_chain is a manual executable, not in ctest (CMakeLists 98-104). It reports a mean from
  one run per cell (details in C06-008). The only other latency number in this group comes from test_ewma.cpp test 8: a
  mean-ns gate inside ctest, active only when `latency_gate_active()` (410-422). The EWMA budgets there (100 ns and
  150 ns) do not appear in the section 11 rows I grepped.

## 6 Test-suite observations

- **Independence of expected values:**
  - test_greeks and test_greeks2: Python reference values plus central finite differences, which are independent of the
    implementation's algebra. Put-side higher greeks are checked only through identities.
  - test_american: an in-file CRR binomial oracle (1500 steps), independent.
  - test_ewma: analytic step-response values and a known variance from a pinned LCG, independent.
  - test_derivatives: analytic polynomial, drift and sine series, independent.
  - test_chain_soa: uses black76 as its oracle, which is appropriate because black76 is tested separately.
- **Checks that pass by construction or can pass vacuously:**
  - The American >= European floor (test_american test 3) is enforced by clamps at american.hpp:177/244.
  - The put charm r*df identity follows directly from greeks2.hpp:145.
  - `the_put_boundary_is_below_the_money` skips refused or zero-boundary results (345-346, 358, 369), so it can pass
    with zero checks executed.
- **Dataset-dependent tests pass without data:** test_garch.cpp:83-87 prints SKIP and returns 0 when
  dataset/spot/nifty/1d/all.csv has fewer than 2000 closes. dataset/ is gitignored, so on a clean checkout the whole
  GARCH/GJR horse race reports green without running. test_surface_hmm section 5 skips the same way (254-256), though
  its other sections still run.
- **Bounds never exercised:** derivatives kCap = 4096 (the test buffer holds 4000); kChainCap is exercised; the HMM
  all-restarts-fail path is not; the American overflow region is not.
- **Possible flake (HYPOTHESIS):** test_garch checks `gjr->log_likelihood >= g->log_likelihood - 1e-9`
  (145-147). The GJR refinement pass centres on the GJR pass-1 winner, whose alpha/beta can differ from GARCH's. The GJR
  grid is therefore not guaranteed to contain GARCH's refined optimum, and nesting on the grid is not assured.
- **Host dependence:** test_chain_soa asserts `topo.line_matches()` and `sizeof(ChainSoA) <= topo.l1d` (219-223), so it
  fails on a host whose L1d is below about 18 KB or whose line size is not 64.

## 7 Open questions

1. Is analytics/chain_soa.hpp meant to be the desktop chain path from commit P39-00? At baseline nothing outside its test
   and the bench includes it.
2. Does any gate-5 tooling treat derivatives_at's `static thread_local` buffer as allowed on a hot path, if it is ever
   called there?
3. Should a zero-variance forecast in qlike be refused or scored +inf? The horse-race protocol needs one rule for all
   models.
