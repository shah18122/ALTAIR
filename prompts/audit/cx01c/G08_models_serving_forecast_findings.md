# CX-01 G08 models_serving_forecast: findings

Reader: claude-subagent:G08_models_serving_forecast. Date 2026-09-14. Baseline HEAD a34af5c5c9fa8c7498e8cc4df00907548a41e1a5.
Nothing was built or run. Every claim below comes from reading the files.

## 1 Scope & coverage

- **Assigned:** 23 files, 8,109 lines.
- **Read:** all 23, in full, in chunks of at most 300 lines.
- **Hashes:** the SHA-256 before and after reading matched the inventory for every file.
- **Unread or partial:** none.
- **Outside the group, read only for integration evidence:** desktop/live_forecast.hpp:360-434, grep hits in desktop/quant_pages.hpp:4007-4194, strategies/score.hpp:130-145, and features/vector.hpp (`set`, `value`, `has`).

## 2 Findings table

| ID | Sev | Class | Title |
|---|---|---|---|
| C08-001 | P2 | CONFIRMED DEFECT | ModelServer reuses a slot while a reader that loaded it is still inferring (slot ABA). This is a data race and can tear the model. |
| C08-002 | P2 | CONFIRMED DEFECT | `generation_` is a plain uint32 shared across threads, and the Forecast record's fields are not coherent. |
| C08-003 | P2 | DESIGN GAP | Served forecasts carry no std_error and no stage. `aggregate` accepts a std_error of 0, so the lower bound collapses to the point estimate. |
| C08-004 | P2 | DESIGN GAP | The weight-estimation term `wse^2 * value^2` vanishes exactly when members disagree. |
| C08-005 | P3 | DESIGN GAP | The horizon check counts bars with no bar width. Member staleness and digests are not checked. |
| C08-006 | P3 | CONFIRMED DEFECT | `net_edge_lower_bound` casts double to int64 without a range check (UB when out of range), and a negative cost is not refused. |
| C08-007 | P2 | CONFIRMED (h>1) / HYPOTHESIS (h=1) | The paired-t verdict uses an iid standard error. Overlapping horizons and clustered volatility overstate t. |
| C08-008 | P3 | CONFIRMED DEFECT (latent) | `calibrate_band` has look-ahead when horizon > 1. |
| C08-009 | P3 | CONFIRMED DEFECT | The band width is measured on the same period it is then scored on (desktop page and real-data tests). |
| C08-010 | P2 | CONFIRMED DEFECT | `horizon_eval` writes past the end of `double x[16]` when `lags >= 16` (rule 11). |
| C08-011 | P3 | CONFIRMED DEFECT (latent) | The `forecast_spot` cost hurdle uses vol-scaled labels when `vol_scaled_label` is on. |
| C08-012 | P3 | CONFIRMED DEFECT | The spot_forecast sigma uses a coin-flip SE and ignores overlap and multiple testing. |
| C08-013 | P3 | CONFIRMED DEFECT | MaskAndImpute: `Scaler::fit` includes the zero placeholders. |
| C08-014 | P2 | CONFIRMED DEFECT | `average_uniqueness` silently caps at 65,536 bars and gives later labels uniqueness 1.0 (rule 11, unsafe side). |
| C08-015 | P3 | CONFIRMED DEFECT | Model names of 40 or more characters are silently truncated (rule 11). |
| C08-016 | P3 | DESIGN GAP | Registry: several Live versions are allowed and `live()` returns the oldest. There is no rollback path and nothing is persisted. |
| C08-017 | P3 | DESIGN GAP | Trainer splits have no purge or embargo. The one-shot test is per Trainer object. A zero-weight block scores 0. |
| C08-018 | P3 | DESIGN GAP | The Scaler range guard cannot detect a fit on everything. |
| C08-019 | P3 | CONFIRMED DEFECT | Conformal: the finite-sample level is clamped to 1.0, and a NaN score breaks `std::sort`. |
| C08-020 | P3 | CONFIRMED DEFECT | `calibrate_band` substitutes 0 bps volatility for warm-up bars. |
| C08-021 | P3 | CONFIRMED DEFECT | Non-positive closes become NaN/inf returns in two functions and a fabricated 0 return in a third. |
| C08-022 | P3 | DESIGN GAP | `horizon_eval.profitable_after_cost` has no error bar, and the oracle units are inconsistent. |
| C08-023 | P3 | TEST GAP | `test_serving` cannot see the ABA tear, and its SKIP branches still end in PASS. |
| C08-024 | P3 | DESIGN GAP | Conformal scores are kept in absolute price units across price levels. |

No P0 or P1. No file in this group is on a path that places or sizes a live order today (see section 5).

## 3 Finding details

### C08-001 P2 CONFIRMED DEFECT: slot reuse while a reader holds it (Codex Q01)

**Evidence**
- serving.hpp:104-108: `const std::size_t idx = 1 - active_.load(std::memory_order_acquire);` then `slot_[idx].predict = p;`.
- serving.hpp:151-152: the reader does `const std::size_t s = active_.load(...)` and `const Slot& m = slot_[s];`, and uses `m` through :169-173.
- serving.hpp:137-140: `publish` only flips the index.

**Interleaving**
1. Reader R loads active_=0, binds `m = slot_[0]` and enters `m.predict(x, Features)`.
2. The writer runs stage/warm/publish: it fills slot 1 and stores active_=1.
3. The writer runs stage again: idx = 1-1 = 0, so it writes `slot_[0].predict/.key/.stage/.warmed` (:105-108), and warm() then calls predict on it (:123).
4. R is still reading those same fields.

The header's promise (:28-31) covers the moment of publication, not the reader's lifetime. There is no RCU, epoch, hazard pointer or reader count.

**Trigger:** two promotions (for example promote then rollback) within one inference. The test loop at test_serving.cpp:223-229 does this 2,000 times per burst.

**Impact:** a data race (UB). A multi-weight model returns a forecast mixing two models, and the rule-10 digest may name either one. ModelServer is used only by models/tests/test_serving.cpp, so this is latent. It escalates to P1 once it is wired into a live path.

**Existing coverage:** none. See C08-023.

**Reproducer:** a Predict over a 64-coefficient array whose operator() sets latch A, waits on latch B, then sums. After A, the writer does stage/warm/publish twice with different coefficients and then releases B. Assert the value equals old*sum or new*sum, and that `model_digest` matches the key of the coefficients used. Also run under the tsan preset.

### C08-002 P2 CONFIRMED DEFECT: non-atomic generation, incoherent record (Codex Q02)

**Evidence**
- serving.hpp:194: `std::uint32_t generation_ = 0;` is written at :137 (`slot_[idx].generation = ++generation_;`) and read by readers at :147 and :178 with no synchronisation.
- The Forecast is assembled from separate reads: `m.predict` (:169), `m.key.digest()` (:170), `m.key.feature_version` (:171), `m.generation` (:173), plus the version check at :154.
- Under C08-001 these reads can straddle two writes, so {value, model_digest, feature_version, generation} is not one record.
- There is no writer mutex: two concurrent `stage()` calls compute the same idx.

**Impact:** TSan-visible UB in the existing test, and a rule-10 record that can name the wrong model.

**Reproducer:** tsan preset on models_serving, plus the latch test from C08-001 asserting that the digest matches the value.

### C08-003 P2 DESIGN GAP: served forecasts carry no uncertainty or stage

**Evidence**
- serving.hpp:168-174 never sets `out.std_error`, although :76-78 documents it as the input to the P8-12 bound. `Predict` returns only `double` (:90-91).
- aggregator.hpp:166-167 (`const double c = w * m[i].forecast.std_error;`) accepts 0. With `weight_std_error` also 0, :185-187 gives std_error 0 and `lower_bound` (:98-101) equals `|value|`.
- `stage()` accepts Shadow, Canary and Live alike (:100-103), and Forecast has no stage field, so a shadow model's output cannot be told apart downstream.
- Calling `publish()` twice without `stage()` silently flips back to the older warmed slot (:133-141).

**Impact:** latent. Nothing feeds ModelServer output into `aggregate`. "Size on the lower bound" is not enforceable end to end.

**Test coverage:** test_serving sets `std_error = 0.10` by hand on Members.

### C08-004 P2 DESIGN GAP: weight-error term

**Evidence:** aggregator.hpp:185-187 adds `weight_std_error * weight_std_error * out.value * out.value`.

**Why it is wrong:** the sensitivity of a normalised weighted mean to weight error scales with member dispersion (v_i - value), not with value.
- Identical members: the true term is 0, but the code adds wse·|v|. Too wide, which is safe.
- Members +1.0 and -0.8, equal weights, wse 0.15: value 0.1. The code adds 0.015 against roughly 0.095 from dispersion, so the interval is about 6x too tight. This is the unsafe case.

The units of `weight_std_error` (absolute or relative) are undocumented.

**Test coverage:** test_serving.cpp:357-406 uses five identical members, the only configuration where the error is conservative. desktop/quant_pages.hpp:4133-4194 uses a fixed 0.05.

**Reproducer:** two members (+1, -0.8), wse 0.15; compare against a Monte Carlo over perturbed weights.

### C08-005 P3 DESIGN GAP: horizon without bar width, no staleness check

**Evidence:** aggregator.hpp:78 (`std::size_t horizon = 0;`) counts bars, and :156 compares only the counts, so a 10×1m and a 10×5m member aggregate together. `forecast.asof` and `model_digest` are never compared, so a stale forecast from an earlier tick combines with a fresh one.

### C08-006 P3 CONFIRMED DEFECT: unchecked cast, cost sign (Codex Q03)

**Evidence**
- aggregator.hpp:205-206 is `static_cast<std::int64_t>(net >= 0.0 ? net + 0.5 : net - 0.5)` with no range check. It is UB above 9.22e18 paise, for example `paise_per_unit` 1e16 with a lower bound of 1e3.
- :200-202 validates ppu and k but not `round_trip.raw() < 0`, and a negative cost would inflate net edge.

**Nonfinite inputs:** a NaN or inf value or std_error propagates into `out.value`/`std_error`, but `lower_bound` returns 0 (`mag > 0.0` is false for NaN) and `direction()` returns 0, so the outcome lands on the safe side. A NaN weight is refused (:171).

**Reproducer:** `net_edge_lower_bound(a, 1e17, 1.0, Notional{0})` with `a.value=1e3`, run under UBSan.

### C08-007 P2: paired t with an iid standard error

**Evidence**
- forecast_scorecard.hpp:236: `diff.push_back(ev * ev - em * em);`.
- :266: `s.paired_se = s.n > 1 ? std::sqrt(dv / (n - 1.0)) / std::sqrt(n) : 0.0;`.
- :204: `if (s.t_stat > k) { return ForecastVerdict::BetterThanNaive; }`.

**Horizon > 1 (confirmed):** `backtest_forecasts` emits one point per bar with `actual = closes[i + spec.horizon - 1]` (:422). For h>1, consecutive errors share h-1 returns, so the loss differential is MA(h-1) and the iid SE understates it. Diebold-Mariano requires a HAC (Newey-West, at least h-1 lags) variance.

**Horizon 1 (hypothesis):** squared-error differentials on returns inherit volatility clustering, which would also overstate t. `min_n = 200` counts dependent points as independent.

**Caller:** desktop/live_forecast.hpp:368 sets `spec.horizon = 1` and :579 calls `judge_forecast`, so the operator-visible verdict is exposed only to the h=1 part.

**Test coverage:** test_forecast_scorecard uses AR and random-walk controls without heteroskedasticity, at h=1.

**Reproducer:** 1,000 seeds of GARCH(1,1) returns with no predictability; count verdicts other than NoBetterThanNaive at k=2. The expected rate is about 4.6%. Repeat with horizon=5 on a random walk.

### C08-008 P3 CONFIRMED DEFECT (latent): observe order with horizon > 1

**Evidence:** calibration.hpp:338-340 observes `pts[i].actual - pts[i].predicted` right after point i, and :318-319 then calibrates point i+1.

**Why it leaks for h>1:** `actual = closes[i+h-1]` (forecast_scorecard.hpp:422) is still unknown when point i+1 (anchor `closes[i]`) is forecast. The buffer therefore holds h-1 future outcomes. The comment at :338 ("Hard rule 7 lives in this ordering") holds only for h=1.

**Related:** refit blocks skipped at forecast_scorecard.hpp:361 and :372 leave `pts` non-contiguous, so `causal_vol_bps` (calibration.hpp:161-164) mixes multi-bar returns.

**Caller:** h=1 (live_forecast.hpp:368), so the defect is latent. test_calibration's planted test covers only `causal_vol_bps`.

**Reproducer:** points with h=5; detonate `actual` of point k. Calibrated bands for points k+1..k+4 must not change, but they do.

### C08-009 P3 CONFIRMED DEFECT: band width measured on the scored period

**Evidence:** desktop/live_forecast.hpp:384-396 does `backtest_forecasts(..., bt, 1.0, false)`, then `const double band = probe_score->rmse_model_bps;`, then `backtest_forecasts(..., bt, band, false)`. The same scored points (the last `max_points=1500`) set the band width and then have their coverage measured at :397 and :422. The same pattern appears in test_calibration_real.cpp:130-151 and test_rich_features.cpp:247-256.

**Impact:** the "before" coverage and quintile table for the RMSE band are in-sample. The forward band at :401 legitimately uses past data, so only the diagnostic comparison is flattered.

**Fix:** derive each point's band from RMSE over points strictly before it (expanding).

### C08-010 P2 CONFIRMED DEFECT: stack overflow at lags >= 16 (rule 11)

**Evidence:** horizon_eval.hpp:276 (`double x[16];`) and :278-280 (`for (std::size_t l = 0; l < p; ++l) { x[l + 1] = r[j + l]; }`). The loops at :282-286 read x[0..p]. Validation at :177-180 only rejects `lags == 0`.

**Trigger:** `HorizonSpec.lags = 16` writes `x[16]`. The bound neither refuses nor is proved unreachable.

**Callers:** test_horizon_eval only, with lags 3.

**Reproducer:** `evaluate_horizon` with lags=16 under asan. Fix: refuse `lags > 15`.

### C08-011 P3 CONFIRMED DEFECT (latent): hurdle computed in scaled units

**Evidence:** spot_forecast.hpp:527 (`for (const double v : y) {`) compares against `spec.cost_bps` at :529, where `y` holds `fwd / sc` (:514), which is scaled when `vol_scaled_label` is true. `y_bps` (:515) is available unused, and the file's own comment at :380-383 says scaled units make the net figure fictional.

**Effect:** `frac_exceeding_cost` comes out near 0 and the oracle near -cost, which is pessimistic.

**Callers:** no current caller turns the flag on for `forecast_spot`. desktop/live_forecast.hpp:679-681 copies the default spec, and test_rich_features passes the flag only to `backtest_forecasts`.

### C08-012 P3 CONFIRMED DEFECT: sigma statistic

**Evidence:** spot_forecast.hpp:249-253 uses `se = std::sqrt(0.25 / scored)` with `(d - dir_constant)/se`. This treats the constant's hit rate as known without error and the rows as independent, although rows overlap for h>1. Four horizons × two growth modes are printed with no multiplicity adjustment (test_spot_forecast.cpp:123-125, 168). test_horizon_eval.cpp:260-273 acknowledges its sigma is optimistic.

### C08-013 P3 CONFIRMED DEFECT: mask placeholders enter the fit

**Evidence:** dataset.hpp:255 writes `x_store[out * k + j] = present ? *v : 0.0;`. `Scaler::fit` (:116-127) averages every row without looking at the mask, and `impute_with_fitted_mean` (:294-296) writes that mean.

**Example:** a feature with mean 100 and 50% absent fits a mean of about 50 with an inflated sd, and every row is standardised in the wrong units.

**Test coverage:** test_dataset.cpp:220-228 fits on rows 0..400, which include the 60 placeholders, but the data has mean 0, so the bias cannot show. It asserts only the fill count.

**Callers:** tests only.

**Reproducer:** the same test with feature values shifted by +100; assert the fitted mean of feature 2 is about 100.

### C08-014 P2 CONFIRMED DEFECT: uniqueness cap (rule 11, unsafe side)

**Evidence**
- labels.hpp:330: `static thread_local std::uint32_t concurrency[1 << 16];`.
- :331: `const std::size_t cap = bars < (1u << 16) ? bars : (1u << 16);`.
- The loops at :334-336 and :342-343 stop at `t < cap`.
- :349: `out[j] = m > 0 ? acc / static_cast<double>(m) : 1.0;`.

**Trigger:** `bars > 65,536`. CLAUDE.md cites 211,000 bars.

**Effect:** labels starting at or after bar 65,536 get uniqueness 1.0, and straddling labels are averaged over a prefix. `effective_observations` then overstates the evidence, which is the very overstatement the function exists to remove. Nothing is refused or counted.

**Test coverage:** test_labels uses 20,000 bars, so the cap is never reached. Callers: tests only.

**Reproducer:** 70,000 consecutive 30-bar labels; assert the mean uniqueness of the last 1,000 is well below 1.

### C08-015 P3 CONFIRMED DEFECT: name truncation (rule 11)

**Evidence:** registry.hpp:165 (`for (; i + 1 < kModelNameLen && name[i] != '\0'; ++i)`) truncates silently. `same_name` (:255-259) later compares the stored `'\0'` at index 39 with the caller's character and returns false.

**Trigger:** any name of 40 or more characters.
- `versions_of()` and `live()` never find it.
- Re-registering the identical key creates a new version-1 record each time, until the registry is Full.
- Two long names sharing 39 characters become indistinguishable through `at()`.

This is the same class as the kite_dump entry in rule 11.

### C08-016 P3 DESIGN GAP: registry lifecycle

**Evidence**
- registry.hpp:205-207 allows Canary→Live while another version is Live, and `live()` (:233-240) returns the first match, which is the oldest.
- Retired is terminal. Re-registering a retired key returns the retired record (:158), so a rollback target can never serve again.
- Storage is an in-memory array (:269-270) with no serialisation. "Never forgets" and rule 10 hold for one process lifetime only.
- `operator==` compares 64-bit digests (:119-121). The same key under two names makes `find()` ambiguous (:262-266).

**Test coverage:** test_training.cpp:366-376 covers the happy path only.

### C08-017 P3 DESIGN GAP: training splits

**Evidence**
- training.hpp:160-164 requires only `train.end <= validation.start && validation.end <= test.start`. There is no purge or embargo, even though Dataset carries LabelWindow (dataset.hpp:205-206). test_training.cpp:80 and 131-133 give row 699 the window [699,700], which crosses into validation unpurged.
- The one-shot test flag is per Trainer object (:318-319). A new Trainer can run and re-consume the same test split, so the "touched once" guarantee does not survive model selection across retrains.
- `weighted_mse` returns 0.0 when the total weight is 0 (:182), so an empty block reads as a perfect loss.

### C08-018 P3 DESIGN GAP: Scaler guard claim

**Evidence:** dataset.hpp:143-146 claims a fit across everything is unreachable by accident. But `fit(x, 0, rows)` followed by `transform(x, a, b, false)` passes the `inside` test (:154-157). test_dataset.cpp:95 and 115 do exactly this and succeed, while :155-157 repeats the claim.

**Related:** a NaN column gets sd NaN, is marked degenerate (:132) and passes through unscaled instead of being refused. `DatasetError::Degenerate` is never returned.

### C08-019 P3 CONFIRMED DEFECT: conformal edge cases

**Evidence**
- conformal.hpp:446-448: `level = std::min(1.0, level * (1.0 + 1.0 / total));`. When the inflated level exceeds 1, a conformal bound cannot be certified, but the clamp returns the buffer maximum, which is narrower and on the unsafe side. With alpha 0.01 and lambda 0.02 the total weight is at most about 49.5, and 0.99×(1+1/49.5) ≈ 1.010, so the clamp always fires. It is visible only through `step.level`. `finite_sample` defaults to false.
- `observe` (:459-471) accepts NaN scores, and `weighted_quantile` sorts with `values[a] < values[b]` (:263-264), which is not a strict weak ordering with NaN (UB).

**Test coverage:** test_conformal has no NaN or finite_sample case.

### C08-020 P3 CONFIRMED DEFECT: zero volatility substituted

**Evidence:** calibration.hpp:314 (`const double raw = vol[i] >= 0.0 ? vol[i] : 0.0;`, repeated at :351) feeds 0 bps into the regime embedding for the first `vol_window` bars. This contradicts :150-152, which says absence is not a volatility of zero. It affects RegimeWeighted mode only and decays under recency weights.

### C08-021 P3 CONFIRMED DEFECT: non-positive closes

**Evidence:** spot_forecast.hpp:368 and :491 compute `std::log(closes[i] / closes[i - 1])` without a positivity or finiteness check, so NaN or inf returns reach `fit_gbdt`. forecast_scorecard.hpp:316-317 pushes `0.0` instead, a fabricated "no move" where rule 9 asks for refusal. The desktop cleans history first (live_forecast.hpp:360-364), and test loaders drop closes <= 0.

### C08-022 P3 DESIGN GAP: horizon_eval verdict without an error bar

**Evidence**
- horizon_eval.hpp:346 (`rep.profitable_after_cost = rep.trades > 0 && rep.net_bps_total > 0.0;`) is a point-estimate boolean with no SE.
- :237-238 divides `oracle_selective` by `sel` (per trade), while spot_forecast.hpp:538 divides by `n_all` (per bar) under the same field name.

The test's printed verdict does combine RMSE ratio, sigma and trade count (test_horizon_eval.cpp:465-466).

### C08-023 P3 TEST GAP: serving test

**Evidence**
- test_serving.cpp:63-71: `Uniform` has one double, so a torn weight vector cannot be observed.
- :224: the key is identical on every swap.
- :176-179 and :247-250 print SKIP and fall through.
- :454: prints PASS whenever failures==0, so a fully skipped concurrency test exits 0.

### C08-024 P3 DESIGN GAP: conformal scores in price units

**Evidence:** calibration.hpp:321-326 (`p.hi = p.predicted + std::max(0.0, a->buffer);`) keeps offsets in absolute price, and desktop/live_forecast.hpp:432-433 applies `next_lo`/`next_up` to today's price. Scores from different price levels are pooled. On NIFTY over 1,500 daily bars the level roughly doubles, so older scores are too small. Recency weighting mitigates this. Log or bps scores would be scale-invariant.

## 4 Lead revalidation

**Codex leads**
- **Q01 (serving slot reuse): CONFIRMED.** See C08-001: reader loads index 0, writer publishes slot 1, writer stages again into slot 0 while the reader is still in `predict`.
- **Q02 (non-atomic generation, incoherent record): CONFIRMED.** See C08-002: generation_ is a plain uint32 read at :147 and written at :137, and value, digest, feature_version and generation are separate reads.
- **Q03 (aggregator): PARTIAL.** The cast is unchecked and a negative cost is unrefused (C08-006). Nonfinite member values do not yield a positive edge, because `lower_bound` goes to 0. Horizon mismatch is enforced among ready members (aggregator.hpp:147-158), but horizon carries no bar-width unit (C08-005).

**conformal.hpp / calibration.hpp**
- Calibration look-ahead: NOT CONFIRMED for h=1, where calibrate-before-observe is correct. CONFIRMED latent for h>1 (C08-008).
- Regime scaler fitted on training statistics only: CONFIRMED (calibration.hpp:291-299 fits on warm-up; the test does the same, test_conformal.cpp:211-216).
- kMaxRegimeDim refusal: CONFIRMED (conformal.hpp:286-288, 368-370, 462-464).
- Weighted quantile: correct definition for finite inputs; unsafe with NaN (C08-019).
- n_eff fallback: CONFIRMED counted (conformal.hpp:414-418), carried on the step, and reported (calibration.hpp:372). Buffer evictions counted (conformal.hpp:476-484).

**spot_forecast / forecast_scorecard / horizon_eval**
- Rows read strictly past data: CONFIRMED correct.
  - `append_row` reads r[i-lags..i-1] and rich features anchor at i-1 (spot_forecast.hpp:182-203).
  - `label_scale` reads up to r[i-1] (:214).
  - Backtest predictions use label index i-1 with anchor `closes[i-1]` (forecast_scorecard.hpp:407-419).
  - Training labels end at r[block-2] (:352-358).
  - The forecast_spot gap of h rows is sufficient (spot_forecast.hpp:552).
- first_row_index warm-up: CONFIRMED correct, including the 20-bar vol-label window.
- Verdict statistics: DEFECTIVE (C08-007, C08-012, C08-022).
- Label scaling and unscaling: correct in predictions (forecast_scorecard.hpp:414-415, spot_forecast.hpp:429-430, 595-598), wrong in the hurdle (C08-011).

**dataset.hpp / labels.hpp / training.hpp / registry.hpp**
- dataset: fitting on train only is supported but not enforced (C08-018), and MaskAndImpute is biased (C08-013).
- labels: the first-touch walk is CONFIRMED correct (labels.hpp:196-229), with the stop assumed first within a bar and truncation not labelled neutral. Uniqueness cap defect: C08-014.
- training: walk-forward ordering is checked but there is no purge, and the third split is one-shot only per object (C08-017).
- registry: hash-addressed by a 64-bit digest over six fields and immutable within one process only; not persisted (C08-015, C08-016).

## 5 Architecture facts

- **Model publication and lifetime.** ModelServer holds two in-object slots and an atomic index, with no reader-lifetime protection and no writer lock. `infer` is `ALTAIR_HOT`, noexcept and allocation-free. ModelRegistry is an in-process array of 128 records with no persistence. Nothing connects the registry to ModelServer: neither `stage()` nor `publish()` consults a ModelRecord stage.
- **Threads.** The only multi-threaded use in the group is test_serving (one reader thread, main thread as writer). Everything else is single-threaded and cold-path.
- **Hot vs cold.**
  - Hot (declared): `ModelServer::infer`.
  - Warm: aggregator (pure, noexcept).
  - Cold, allocating: everything else (conformal, calibration, forecast_scorecard, spot_forecast, horizon_eval, labels, dataset, training).
- **Implemented vs connected.**
  - Connected to an executable (desktop, in the same process as the engine): backtest_forecasts, score_forecasts, judge_forecast, forecast_next, score_calibration, calibrate_band (desktop/live_forecast.hpp); forecast_spot (live_forecast.hpp:681, quant_pages.hpp:4011); aggregate and net_edge_lower_bound (quant_pages.hpp:4099-4194, demonstration).
  - Tests only: ModelServer, ModelRegistry, Trainer, build_dataset, Scaler (models and flagging tests), label_event, average_uniqueness, evaluate_horizon.
  - No caller in app/, strategies/, oms/ or server/.
- **Money path.** No function in this group feeds an order or a size. `net_edge_lower_bound` is the only paise-producing function, and it is used only on a desktop explanatory page.

## 6 TRAINING STATUS

| Model / estimator | Where trained | Data | Parameters persisted / loaded | Out-of-sample verdict vs naive baseline | Net of cost | Missing before it could inform a live decision |
|---|---|---|---|---|---|---|
| GBDT spot evaluation (`forecast_spot`, level- and leaf-wise) | test_spot_forecast; desktop Live Forecast "trade" section (live_forecast.hpp:679-681) and quant_pages.hpp:4011, refit on each run | Real NIFTY spot 5m/15m/60m (YYYY-MM.csv) and 1d/all.csv under dataset/spot/nifty/; desktop uses loaded history | No. In-memory fit every call; no ModelKey; not registered | Printed only, never asserted. The file header says RMSE is worse than a constant at every horizon, and leaf-wise loses to level-wise out of sample. Sigma statistic flawed (C08-012) | Yes, 5.5 bps round trip (hurdle wrong only in the unused vol-scaled mode, C08-011) | A positive, significant net edge; persistence and registry key; HAC/overlap-aware statistics; per-regime report; costs from risk/cost rather than a literal 5.5 |
| GBDT price forecaster (`backtest_forecasts` / `forecast_next`) | Desktop Live Forecast page at runtime; test_forecast_scorecard (synthetic); test_calibration_real and test_rich_features --experiment (real) | Real closes (desktop); NIFTY daily (tests); synthetic AR(-0.45) and random walk (scorecard test) | No. Refit per block and per page run; not persisted or registered | `judge_forecast` against a random walk: the synthetic AR control must say BETTER and the random walk must not. Real-data verdicts are printed, never asserted. iid SE (C08-007); band width in-sample (C08-009) | No. cost_bps=0 by design ("a forecast is not a trade") | Cost-aware evaluation; HAC t; expanding out-of-sample band; persistence; stamps; rule-10 key |
| Rich-feature / vol-scaled variants (P41) | test_rich_features --experiment only | Real NIFTY daily | No | Printed paired t and verdict per configuration; asserted only that runs complete and are finite | No (cost 0) | Same as the row above; no recorded result in code |
| Conformal band (`calibrate_band`, TWC/SWC/RWC) | Desktop at runtime (TimeWeighted); test_conformal and test_calibration (synthetic GARCH); test_calibration_real (NIFTY daily) | Real (desktop, real test); synthetic (unit tests) | No | Synthetic: exceedance pulled back to target within ±0.02-0.025, with a control that must stay calibrated. Real: printed only | n/a (a band, not a trade) | Causality for h>1 (C08-008); log-unit scores (C08-024); persistence |
| AR(p) horizon model (`evaluate_horizon`) | test_horizon_eval only | Real NIFTY spot 60m; 1m downsampled to 5m/15m; NIFTY futures 1m and continuous 1d | No (refit per fold) | Compared against persistence, training mean and constant direction. The test's printed conclusion is "NO FORECAST AT THIS HORIZON" unless RMSE ratio < 1, sigma > 2, trades > 100 and profitable; only the protocol is asserted | Yes, 5.5 bps literal round trip; impact excluded (the test says net is an upper bound) | Error bar on P&L (C08-022); lags bound (C08-010); not connected to any executable |
| Trainer harness (early stopping, best checkpoint) | test_training only | Synthetic independent noise, NoisyModel that does not learn | param_hash computed; best params held in a caller buffer; nothing persisted | Demonstrates validation optimism and a one-shot test; no real model is trained through it in this group | n/a | A real model; purge/embargo (C08-017); persistence; registry integration |
| Triple-barrier labels + uniqueness; Dataset/Scaler | test_labels, test_dataset (synthetic); flagging tests use Scaler | Synthetic | No | n/a (preprocessing) | n/a | A dataset/ → label → dataset pipeline exists nowhere in these files; C08-013, C08-014, C08-018 |
| ModelRegistry / ModelServer | Tests only (toy keys, single-weight Uniform model) | Synthetic | Registry in-memory only; server holds a Predict by value | n/a | n/a | Persistence, reader-safe swap (C08-001/002), std_error and stage in Forecast (C08-003) |
| Aggregator weights / correlation | Not trained anywhere. Desktop demonstration uses equal weights, a swept rho and fixed wse 0.05 over members built from `forecast_spot` runs (quant_pages.hpp:4037-4046) | n/a | n/a | n/a | Yes (round_trip subtracted at the lower bound) | Fitted weights with an estimated SE; measured rho; correct weight-error term (C08-004); horizon units (C08-005) |

**Answer to "is training of all models perfectly done":** no.
- Nothing here is trained once and reused. Every fitted model is refit in memory on each test or desktop run, and nothing is persisted, registered or loaded back.
- The generic training stack (Trainer, Dataset, labels, registry, serving) runs only on synthetic data and toy models.
- The real-data results (GBDT spot, AR horizon, conformal) are printed reports whose own comments conclude there is no tradeable edge. Their verdicts are not asserted.
- The price-forecast scorecard is not net of cost, and its significance test is not autocorrelation-robust.

## 7 Test-suite observations

- **test_serving:** both concurrency blocks can print SKIP and still exit PASS. The double-buffer block cannot detect the slot-reuse tear (C08-001, C08-023). The aggregation blocks use identical members.
- **test_spot_forecast:** each horizon SKIPs when its data is absent, and the test fails only if all are absent (`check(any, ...)`). All four cases use horizon 1. The P30-01 block is nested inside the REFUSALS block (:203-263); it still executes.
- **Loud-failing data tests:** test_calibration_real and test_rich_features --experiment return 1 when NIFTY daily is absent. test_horizon_eval returns 1 when 60m is absent, and its `check(h1 > 100000)` fails if 1m is absent. These are loud rather than vacuous.
- **test_calibration:** the planted look-ahead test covers `causal_vol_bps` only, not the observe/calibrate ordering or horizon > 1.
- **test_dataset:** imputation is verified by count on mean-0 data (it hides C08-013). Lines 95 and 115 transform after a fit on everything and succeed (C08-018).
- **test_labels:** 20,000 bars, so the 65,536 uniqueness cap is untested.
- **test_training:** row label windows cross the split boundaries with no purge; the registry test is happy-path only.
- **test_conformal:** good controls (a miscalibrated band plus an already-calibrated one), but no NaN, finite_sample or level > 1 case.
- **test_forecast_scorecard:** the look-ahead test compares forecasts from the first `size - 250` of 300 points after tripling the last 200 bars. It is a valid causal check but only over a short window.

## 8 Open questions

1. Is ModelServer intended to be single-writer? If so, where is that enforced once it is wired to canary/rollback (flagging/deploy.hpp)?
2. What are the intended units of `weight_std_error` (absolute weight SE or relative)? The correct propagation formula depends on it (C08-004).
3. Should `judge_forecast` adopt a Diebold-Mariano HAC variance before any horizon > 1 path is exposed on the desktop?
4. Is there a plan to persist ModelRegistry records? Rule 10 reproducibility across restarts currently has no storage.
5. `flagging/deploy.hpp` includes models/registry.hpp. Its use of ModelStage/ModelKey belongs to the flagging group and was not reviewed here.
