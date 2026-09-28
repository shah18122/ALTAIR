# CX-01 Phase 1 — G09_models_families — findings

Reader: claude-subagent:G09_models_families · Date 2026-09-14 · Baseline a34af5c5c9fa8c7498e8cc4df00907548a41e1a5
Status: **final**. All 17 assigned files were fully read (6,338 lines). The before-read hashes match the inventory; after-read hashes are in the coverage JSON. Nothing was built or run. Test outcomes come from reading test bodies plus `baseline_logs/ctest_default.log`.

## 1 Scope & coverage

| File | Lines | Ranges read |
|---|---|---|
| models/tensor.hpp | 223 | 1-223 |
| models/mlp.hpp | 253 | 1-253 |
| models/recurrent.hpp | 447 | 1-300, 301-447 |
| models/attention.hpp | 282 | 1-282 |
| models/gbdt.hpp | 502 | 1-300, 301-502 |
| models/markov.hpp | 481 | 1-300, 301-481 |
| models/markov_eval.hpp | 402 | 1-300, 301-402 |
| models/dcf.hpp | 389 | 1-300, 301-389 |
| models/regime_rl.hpp | 336 | 1-300, 301-336 |
| models/tests/test_attention.cpp | 340 | 1-300, 301-340 |
| models/tests/test_dcf_sensitivity.cpp | 255 | 1-255 |
| models/tests/test_gbdt.cpp | 468 | 1-300, 301-468 |
| models/tests/test_markov.cpp | 379 | 1-300, 301-379 |
| models/tests/test_markov_eval.cpp | 277 | 1-277 |
| models/tests/test_neural_5m.cpp | 524 | 1-270, 271-524 |
| models/tests/test_recurrent.cpp | 405 | 1-300, 301-405 |
| models/tests/test_regime_rl.cpp | 375 | 1-300, 301-375 |

Context read outside the group, not counted as coverage: models/training.hpp 180-339, models/dataset.hpp 150-314, desktop/quant_pages.hpp 668-717, 795-844 and 2785-2814. Grep only: core/types/units.hpp:232/251 (`Notional` = int64 paise) and ctest_default.log.

## 2 Findings table

| ID | Sev | Class | File:line | One line |
|---|---|---|---|---|
| C09-001 | P2 | CONFIRMED DEFECT | mlp.hpp:171, recurrent.hpp:364 | A NaN training SSE is returned as 0.0, a perfect fit |
| C09-002 | P2 | CONFIRMED DEFECT | gbdt.hpp:84-85, 102, 424-461 | No finiteness checks in GBDT: sorting NaN is UB, NaN goes to the top bin, a NaN label returns a "successful" NaN model |
| C09-003 | P3 | CONFIRMED DEFECT | gbdt.hpp:66-72, 92, 405, 470-487 | `Frame` shape (`x.size()==rows*p`) is never checked, so a short `x` reads out of bounds |
| C09-004 | P3 | CONFIRMED DEFECT (contract) | gbdt.hpp:37-41 vs 198-204 | Header says predict does not allocate and is ALTAIR_HOT-safe; `predict_row` allocates a vector on every call |
| C09-005 | P3 | CONFIRMED DEFECT | gbdt.hpp:170-173 vs 348 | Leaf-wise growth is still capped by `max_depth` (default 3, so at most 8 leaves of the 31 budget) with no report |
| C09-006 | P3 | CONFIRMED DEFECT | gbdt.hpp:59-60, 444, 460 | `Degenerate` ("every label identical") is never detected. Skipped trees are not counted |
| C09-007 | P3 | CONFIRMED DEFECT | attention.hpp:127 | Any `Causality` other than `Masked`, including an out-of-range value, runs UNMASKED |
| C09-008 | P3 | CONFIRMED DEFECT | mlp.hpp:69-87 | `cross_sectional_rank`: a NaN ranks below -1, `RaggedUniverse` is unreachable, and there is no production caller |
| C09-009 | P3 | CONFIRMED DEFECT | tensor.hpp:172-181 | `softmax_row` lets NaN/inf through, contrary to its comment |
| C09-010 | P3 | DESIGN GAP | tensor.hpp:48-57, 77-86, 108-128 | Unchecked accessors, error codes never produced, `matmul` aliasing undetected |
| C09-011 | P3 | CONFIRMED DEFECT | markov_eval.hpp:208-209, 303-306 | Wrong error code for alpha/gap, NaN alpha accepted, a comment contradicts the code |
| C09-012 | P3 | CONFIRMED DEFECT | markov.hpp:454-458, 203-205 | Uncounted drops: non-positive prices splice returns across a gap, out-of-range states are skipped |
| C09-013 | P3 | CONFIRMED DEFECT | markov.hpp:422-437, 462-479; test_markov.cpp:204 | χ² df ignores empty rows/columns (conservative). The `stationary()` non-convergence claim is false, and the test's "ergodic" check cannot fail |
| C09-014 | P3 | CONFIRMED DEFECT | regime_rl.hpp:240-246, 287-299, 312-330 | Unchecked Q-table indices, `heuristic_action(NaN)` returns NaN, `state_of(NaN)` is a UB cast |
| C09-015 | P3 | DESIGN GAP | dcf.hpp:144-209; regime_rl.hpp:229 | Money is a bare double. The same DCF API is fed rupees in one caller and paise in another |
| C09-016 | P2 | DESIGN GAP | whole group | No persisted/versioned model, no cost netting, no path to a decision. Training is not "done" |
| C09-017 | P3 | HYPOTHESIS | recurrent.hpp:276-293, 280, 399 | Encode windows ignore session/symbol boundaries. Missing `<type_traits>`. Stale comment |
| C09-018 | P3 | CONFIRMED DEFECT (test) | test_regime_rl.cpp:139-142 vs 371; SKIP pattern in 4 tests | The P33-05 clamp regression is unreachable without dataset/, and four real-data tests pass on SKIP |
| C09-019 | P3 | CONFIRMED DEFECT (test protocol) | test_regime_rl.cpp:204-213, 251-254, 230-243 vs 85-91 | QLearner "out-of-sample" episodes overlap training in time, and the training reward omits the completion cost evaluation charges. The recorded "RL closed" verdict rests on this |
| C09-020 | P3 | DESIGN GAP (tests) | test_neural_5m.cpp:245-250, 496-507; test_markov_eval.cpp:186-190; test_attention.cpp (whole) | Verdicts are printed, not asserted; one verdict is unconditional; NaN would print the "negative" verdict; the Mlp row-cap fix and Mlp training have no unit test |

## 3 Finding details

### C09-001 — a NaN training error is reported as a perfect fit (P2)
Evidence: mlp.hpp:171 and recurrent.hpp:364 `return sse > 0.0 ? sse / static_cast<double>(n) : 0.0;`. A NaN in `d.y`/`d.x`/`d.weight` inside the block poisons the Gram matrix. `solve_normal` divides anyway, because `fabs(NaN) < 1e-300` is false. `sse` becomes NaN, `NaN > 0.0` is false, and **0.0** comes back. The clamp meant for round-off lands on the unsafe side.
Trigger: a 200-row Dataset with `y[10] = NaN`. `build_dataset` (dataset.hpp:248-260) does no finiteness check.
Mitigation: inside `Trainer::run`, 0.0 passes `isfinite` (training.hpp:263), but the NaN validation loss then returns `Diverged` (273). Direct callers get no protection. test_neural_5m discards the return (245, 250), and a NaN model there turns the RMSE into NaN, so `best < 1.0` is false and the **negative verdict is printed** (496-507).
Coverage: none. Reproducer: assert `!(Mlp<8,4>::train_epoch(d, {0,200}, 0) == 0.0)` with a NaN label; the same for `RecurrentReadout<GruCell>`.

### C09-002 — GBDT does no finiteness checks (P2)
gbdt.hpp:102 `std::sort(col.begin(), col.end());` over raw features: NaN breaks strict weak ordering, which is UB (MSVC debug asserts). gbdt.hpp:84 `upper_bound`: NaN is never `<` an edge, so it goes to the top bin. gbdt.hpp:424-425 takes the mean of y unchecked, so a NaN label makes every residual and gain NaN, no split occurs, every leaf is NaN, `m.tree` is non-empty, and **success is returned**. The header (11) claims the method "handles ... missing values ... without being told".
Callers are research and UI only: spot_forecast.hpp, forecast_scorecard.hpp, and a synthetic desktop demo. Coverage: none (test_gbdt refusals cover only rows < 64 and lr = 0). Reproducer: `fit_gbdt` with `y[5] = NaN` must fail, and `fit_binner` with one NaN cell must refuse or report.

### C09-003 — Frame shape is never validated (P3)
`Frame::at` (70-72) is unchecked. fit_binner (92) and fit_gbdt (405) never compare `x.size()` with `rows*p`, even though `BadShape` is documented as "Ragged feature matrix" (55). permutation_importance (470-487) does not check `y.size()`. Reproducer: `Frame{std::vector<double>(100), 64, 2}` should be refused with BadShape; today it reads x[100..127].

### C09-004 — predict allocates despite the header (P3)
gbdt.hpp:37-41 claims a served model "can sit inside ALTAIR_HOT". gbdt.hpp:199 allocates `std::vector<std::uint8_t> b(...)` in `predict_row`, which every caller uses (spot_forecast.hpp:429, 597-598; forecast_scorecard.hpp:414; quant_pages.hpp:711; test_gbdt). `predict_binned` does not allocate. There is no hot-path caller today. Fix direction: a caller-supplied scratch span.

### C09-005 — leaf-wise budget silently capped by depth (P3)
gbdt.hpp:171-172 says max_depth bounds the tree "instead" only for level-wise, but 348 `if (front[k].depth >= prm.max_depth) { continue; }` also applies it in leaf-wise mode. With default params, a leaf-wise tree cannot exceed 8 leaves against `max_leaves = 31`, and nothing reports it. test_gbdt.cpp:133 sets `max_depth = 12`, so the default is never exercised. Reproducer: `GbdtParams p; p.leaf_wise = true;` on 4,000 signal rows, then assert the leaf count exceeds 8 or that the cap is reported.

### C09-006 — `Degenerate` never detected; skipped trees uncounted (P3)
Constant y with valid params returns success with zero-valued stumps. `Degenerate` fires only when every tree was skipped (460). Line 444 drops trees whose subsample is below 2·min_leaf without counting them (rule 11, visible truncation).

### C09-007 — invalid Causality runs unmasked (P3)
attention.hpp:127 `c == Causality::Masked ? i + 1 : T`. Only `Unspecified` is refused (108), so `static_cast<Causality>(3)` reads the future. `dilated_conv` falls the safe way instead (182-184). No test covers it (test_attention.cpp:151 checks only Unspecified).

### C09-008 — cross_sectional_rank (P3)
mlp.hpp:74-84: a NaN value gets `out = -1 - 1/(n-1)`. `RaggedUniverse` (56) can never be returned. The only caller is test_attention.cpp:278-323, and `Mlp` never ranks its inputs, so the cross-sectional premise in the header is enforced nowhere.

### C09-009 — softmax_row NaN handling (P3)
tensor.hpp:174-180: NaN in x[0] makes everything NaN, +inf gives `inf-inf = NaN`, and a NaN `s` leaves the row unnormalised. The comment (166-171) promises that a NaN will not reach a position size.

### C09-010 — tensor.hpp checks (P3, DESIGN GAP)
`at/row` are unchecked (77-86). `TooLarge`/`NotFinite` (53-56) are never produced. `matmul` with `&out == &a` zeroes the row before reading it (119). The header (21-27) says "SHAPES ARE CHECKED, NOT ASSUMED".

### C09-011 — markov_walk_forward parameters (P3)
208-209 reports `alpha < 0` / `gap < 1` as `BadStateCount`. NaN alpha passes, producing `log(NaN)` and a NaN edge. The comment at 303-306 says scoring uses te.start-1, but the loop at 307-309 does not. The code is correct; the comment is wrong.

### C09-012 — uncounted data drops in Markov prep (P3)
markov.hpp:455 skips both returns around a non-positive price with no count, so the next transition spans the gap. markov.hpp:203-205 skips out-of-range states with no count.

### C09-013 — χ² df, stationary, and a check that cannot fail (P3)
markov.hpp:425-436 skips `expected == 0` cells but keeps df = (k-1)², which is conservative. markov.hpp:462-464 says non-convergence shows as a sum ≠ 1, but power iteration over populated stochastic rows always sums to 1, even when a periodic chain oscillates. test_markov.cpp:204 "sums to one, so the chain is ergodic" therefore cannot fail.
**Lead:** kTable holds 88 entries (df 0..87). df 64 = 83.675 (asserted at test_markov.cpp:328), and df 1/16/63 = 3.841/26.296/82.529 are correct. The P33-03 fix holds, and the nine-state sticky chain rejects (test_markov.cpp:332-357).

### C09-014 — QLearner and heuristic bounds (P3)
regime_rl.hpp:316, 326, 328: `s`, `s2`, `a` are unchecked, so s ≥ 25 or a ≥ 5 reads or writes the vector out of bounds. regime_rl.hpp:243-245: a NaN price passes both clamp tests and NaN is returned. regime_rl.hpp:288: `static_cast<std::size_t>(NaN)` is UB, because NaN passes the range checks at 300-301 and 305. **`aggression()` (280-283) maps an out-of-range action to m[0] = 0.2, the least aggressive. The lead is fixed and tested (test_regime_rl.cpp:104-125, but see C09-018).**

### C09-015 — money as double (P3, DESIGN GAP)
dcf.hpp:184/192 take and return untyped `double` money, and 204-209 subtract `Notional::raw()` (int64 **paise**, units.hpp:232/251). desktop/quant_pages.hpp:2793-2813 is consistent in paise (`per_share = equity / shares / 100.0; // paise -> rupees`), while test_dcf_sensitivity.cpp:52 feeds `kFcff` "in rupees" to the same API. No live mis-valuation was found. The unit lives only in caller comments (rules 2/3). regime_rl.hpp:229 `std::vector<double> price; ///< paise`.

### C09-016 — nothing is trained into a usable artefact (P2, DESIGN GAP)
None of Gbdt, the Mlp/Recurrent readout, TransitionMatrix, Clustering or QLearner can be serialised or loaded. `params()` covers only the H+1 readout, so rebuilding a model needs the same seed and template sizes. There is no LibTorch/ONNX (tensor.hpp:7-13). No result is net of cost, and no strategy/oms path consumes these models with the rule-10 tuple. See §6.

### C09-017 — recurrent encode windows (P3, HYPOTHESIS)
recurrent.hpp:278 windows over the previous Seq rows and ignores `Dataset::bar` (dataset.hpp:204), so a window can span a session or symbol boundary. test_neural_5m's lag rows do exactly that (168-175) even though overnight returns are removed. There is no look-ahead. Line 280 uses `std::is_same_v` without `<type_traits>`. Line 399 is a stale comment.

### C09-018 — a regression that does not run, and SKIP counted as a pass (P3)
test_regime_rl.cpp:139-142 `return 0;` when there are < 3,000 closes, but the P33-05 rule-11 regression `an_unknown_action_clamps_to_the_least_aggressive()` is called at 371, after that return. On a checkout without dataset/ (which is gitignored), the only test of the clamp direction never runs and ctest reports Passed. test_neural_5m (144-147), test_gbdt (344-345, section only), test_markov_eval (67-70) and test_regime_rl also pass on SKIP; test_markov (364-367) returns 1. Fix: call synthetic tests before loading data; return ctest's skip code (e.g. 77 with `SKIP_RETURN_CODE`).

### C09-019 — the QLearner verdict comes from a flawed protocol (P3)
(a) Training episodes (204-213) and "out-of-sample" episodes (251-254) are both drawn uniformly from `[first, last)` of the same daily series, so they are out of sample by RNG only, not by time. That violates walk-forward-only. (b) The training loop charges only filled slices (230-243, reward `-qty*(p-p0)/p0`). Evaluation (`run_episode` 85-91) buys any remainder at the last price. On an up-drifting series the training objective rewards under-filling. (c) The only assertions are finiteness, |heuristic−TWAP| < 1000 bps, and `bps_t > 0.0` (350), which asserts that the market rose. The "RL IS CLOSED" / "neither beats TWAP" text is printed conditionally, and desktop/quant_pages.hpp:834-841 hard-codes that conclusion as a string. No live risk (tests/UI text only); the risk is a closed research question resting on this protocol. Reproducer: split episodes by date (train < T ≤ test), add a terminal completion cost to the training reward, and rerun.

### C09-020 — assertion gaps in the suite (P3, DESIGN GAP)
test_neural_5m asserts none of the model-vs-baseline comparisons (389-395), and its verdict selection maps NaN to "NO NONLINEAR EDGE" (496). test_markov_eval.cpp:186-190 prints "That is a real dependence" whatever `folds_positive`/`mean_edge` are, and line 164 dereferences `train_b` unchecked. test_gbdt meta-labeling asserts only `n_kept > 50` and `isfinite(z)` (419-444). **No unit test trains `Mlp` at all, and there is no regression test for the P33-05 8,192-row fix.** test_attention includes mlp.hpp only for `cross_sectional_rank`, and test_recurrent's all-rows regression (318-393) covers the GRU readout only.

## 4 Lead revalidation

| Lead | Verdict | Evidence |
|---|---|---|
| gbdt binner fitted on training rows only | **Holds in gbdt.hpp.** Edges are fitted on the Frame passed to `fit_gbdt` and stored (410-421, 476). test_gbdt meta-labeling fits on the first-half `tr` and scores the second half through the train binner ✔ (379-396). spot_forecast.hpp:556-598 uses a separate `tr` Frame (grep only). The desktop GBDT page is a synthetic demo (quant_pages.hpp:669-717) | gbdt.hpp:23-26 |
| gbdt predict path allocation-free | **Partly false**: `predict_binned` yes, `predict_row` no (C09-004) | 193-204 |
| leaf-wise vs level-wise | Both exist; level-wise is the default. Leaf-wise is tested only with max_depth 12. Depth still caps it (C09-005) | 170, 264-396; test_gbdt 77-186 |
| subsampling seeds | Deterministic xorshift64 (default 0xB0057, 0 replaced), one stream, Bernoulli per row. Bit-identical refit asserted (test_gbdt 166-178) | 429-443 |
| mlp/recurrent frozen hidden + ridge readout | **Yes.** Seeded Xavier weights are never updated; only the H+1 readout is solved in closed form. There is no backpropagation, and `lr` is ignored. The code and test_neural_5m (15-28) both say so | mlp.hpp:89-107; recurrent.hpp:11-17, 295-365 |
| 8,192 / 4,096 row caps | **Removed in code**: both stream into an (H+1)² Gram matrix. Regression test exists for the GRU (test_recurrent 318-393) but **not for Mlp** (C09-020) | mlp.hpp:118-172; recurrent.hpp:299-365 |
| attention: single-head causal only? | **Yes, and less.** One scaled dot-product function over caller-supplied Q/K/V. No learned projections, multi-head, positional encoding, layer norm, stacked blocks, parameters or training. Unspecified causality is refused (tested); an out-of-range value runs unmasked (C09-007). T > 128 is refused | attention.hpp:98-142 |
| markov χ² table vs kMaxStates | **Fixed** (df 0..87, df 64 = 83.675, tested) | markov.hpp:386-403; test_markov 307-358 |
| markov expanding past-only boundaries | **Holds**: edges computed before `returns[i]` is inserted. The full-sample vs expanding disagreement > 0 and < 50% is asserted | markov.hpp:291-318; test_markov 113-163 |
| markov_eval per-fold boundaries | **Holds**: boundaries, counts, baseline and mag_ref come from the train block only. Train-vs-global edges differ (asserted) | markov_eval.hpp:227-297; test_markov_eval 150-176 |
| QLearner clamp direction | **Fixed**: out of range maps to 0.2, the least aggressive. Tested, but the test is skipped without dataset/ (C09-018) | regime_rl.hpp:280-283 |
| QLearner reward look-ahead | **No look-ahead in action or reward.** The action uses the state at i, the reward the price at i, and s2 uses i+1 as the next state. The **evaluation is not time-out-of-sample and the reward is misspecified** (C09-019) | test_regime_rl 216-243 |
| dcf FCFF@WACC vs FCFE@Ke | Separate functions, named params and distinct return types. Type-level protection is naming only (both double) | dcf.hpp:183-196 |
| dcf g ≥ r refusal | **Holds**: `!(g < rate)` refuses g ≥ r and NaN. The grid marks and counts refusals (tested, straddle case). The reverse DCF bisects below r and refuses unreachable targets (tested) | dcf.hpp:147-153, 303-387; test_dcf 145-243 |
| tensor.hpp bounds | Dimension caps are `static_assert`ed in the recurrent cells; accessors unchecked (C09-010) | tensor.hpp:44-46; recurrent.hpp:98, 174 |
| test_neural_5m data and verdict | **Real** `dataset/spot/nifty/1m` downsampled to 5m (overnight excluded, > 150,000 returns asserted). Expanding walk-forward with train-fold scaler ✔. **The verdict is printed, not asserted.** Baseline ctest: `models_neural_5m Passed 36.95 sec` (the duration suggests it did not SKIP; inference). I did not see the numeric verdict. The in-file comment at 365-367 implies a GRU RMSE ratio above 1.0 vs the constant, i.e. no edge (unverified) | test_neural_5m 141-161, 178-266, 495-520 |

## 5 Architecture facts
- Everything is header-only and cold/research. None of it is `ALTAIR_HOT`, and none of it includes oms/ or broker/. There is no shared mutable state (QLearner owns its table).
- The Model interface (training.hpp:230-237) is met by `Mlp` and `RecurrentReadout`. The Trainer keeps best-epoch params in the caller's buffer only.
- With H = 64, `train_epoch` uses about 68 KB of stack plus a 34 KB copy in `solve_normal`. `LstmCell<64,32>` holds about 197 KB of weights by value.
- **Non-test callers**: `fit_gbdt`/`predict_row` in models/spot_forecast.hpp and models/forecast_scorecard.hpp (not read in full), and a synthetic demo in desktop/quant_pages.hpp:669-717. Markov: desktop/data/fits.hpp:83-234 via desktop/panels.hpp:559. `kmeans`/`cluster_quality` run on the real series at desktop runtime (quant_pages.hpp:804-826). DCF: quant_pages.hpp:2753-2905 → main_window.hpp:1013-1019. `QLearner`, `Mlp`, `RecurrentReadout`, LSTM/GRU cells, `attention`, `dilated_conv` and `variable_selection` are **test-only**. app/ uses none of them.
- Baseline ctest (ctest_default.log): models_recurrent, models_attention, models_markov, models_markov_eval, models_dcf_sensitivity, models_gbdt, models_regime_rl and models_neural_5m all **Passed**.

## 6 TRAINING STATUS

| Model | Trained where | Data | Params persisted/loaded | OOS verdict vs naive baseline | Net of cost | Missing before a live decision could use it |
|---|---|---|---|---|---|---|
| Markov regime chain | desktop runtime (fits.hpp ← panels.hpp:559); test_markov, test_markov_eval; desktop/tests/test_fits.cpp | **real** daily NIFTY `dataset/spot/nifty/1d/all.csv` | **No**, refit on every call | Walk-forward log-score edge vs unconditional distribution, plus sign/magnitude vs constants. **Printed, not asserted.** Source comments claim in-sample χ² 298.07 vs a non-rejecting shuffle and test prose claims "no directional content" (unverified by me) | **No.** "No strategy may read these numbers as a signal" (markov_eval.hpp:47-52) | cost model, a tradeable signal definition, persistence/versioning, strategy wiring, rule-10 record, count of data drops (C09-012) |
| GBDT | tests; models/spot_forecast.hpp and forecast_scorecard.hpp (callers, not read); desktop demo (synthetic) | tests: synthetic, plus **real** NIFTY 1d meta-labeling (first half train / second half test) | **No** serialisation; refit per call | Meta-label lift vs primary momentum hit rate **printed, not asserted**. Synthetic tests are in-sample only | Not in tests; callers unknown | persistence, NaN/shape refusal (C09-002/003), allocation-free predict (C09-004), cost-aware target, time-embargoed validation, strategy wiring |
| MLP (random features + ridge readout) | **tests only** (test_neural_5m; no unit training test) | **real** 5m NIFTY | **No** | RMSE and direction vs train-mean constant, **printed, not asserted** | **No** (test says so) | real training (backprop/LibTorch), a unit test incl. row-cap regression (C09-020), NaN handling (C09-001), persistence, any caller |
| GRU reservoir + ridge readout | **tests only** (test_recurrent synthetic; test_neural_5m real) | synthetic AR driver; **real** 5m NIFTY | **No** | synthetic: beats mean on validation (asserted); real: **printed, not asserted**. The comment suggests RMSE ratio > 1 (no edge), unverified | **No** | same as MLP, plus session-aware windows (C09-017) |
| LSTM reservoir + ridge readout | **tests only** (test_recurrent) | synthetic only | **No** | beats mean on synthetic validation (asserted) | No | never evaluated on real data |
| Attention / TFT gates / dilated conv | **Not trainable**: no parameters or fit | synthetic; fixed random logits on real 5m rows | n/a | n/a (the real-data check is a tautology, test_neural_5m 472-479) | n/a | the entire model: projections, heads, training |
| k-means regimes | test_regime_rl; desktop runtime (quant_pages.hpp:804-826) | **real** NIFTY 1d | **No** | return-blind separation × persistence only; no forward evaluation | n/a | OOS stability, persistence, a consumer |
| QLearner (execution timing) | **tests only** | **real** NIFTY daily closes, 12-day episodes | **No** | vs TWAP and heuristic, **printed**. Protocol is flawed (C09-019). Desktop prints a hard-coded "neither beats TWAP" | Shortfall vs arrival only; no fees/STT | time-split evaluation, fixed reward, intraday data, costs, bounds (C09-014) |
| DCF | Not a trained model | desktop: hard-coded illustrative rates (quant_pages.hpp:2795); test: illustrative FCFF | n/a | n/a | n/a | point-in-time fundamentals (test_dcf 245-251: "still blocked"), typed money (C09-015) |

**Answer to "is training of all models perfectly done?" — No.** Nothing in this group is trained into a persisted, versioned artefact. Nothing is evaluated net of cost, and no live decision path consumes any of these models. The "neural" models (MLP, GRU, LSTM) fix random hidden weights and fit only a linear readout. Attention has no trainable parameters at all. Only the Markov chain and k-means are fitted at desktop runtime on real data, refit on every press. Every real-data out-of-sample verdict is printed rather than asserted, and the RL verdict rests on a time-overlapping evaluation.

## 7 Test-suite observations
- **test_neural_5m** (real 1m→5m): expanding walk-forward 40,000/10,000/10,000, gap 1. Scaler fitted on the train fold ✔. `train_epoch` returns are discarded. Only volume, fold count, scored count, price arithmetic and a near-tautological "band straddles last close" are asserted. The forecast refits on all rows (legitimate). SKIP passes. Not net of cost (stated).
- **test_recurrent** (synthetic): bias half-lives, order sensitivity, LSTM/GRU readout beats mean on validation via Trainer (`consume_test` unused), and a sound GRU all-rows regression.
- **test_gbdt**: leaf-wise at max_depth 12 only. In-sample synthetic R² and importance checks. Real 1d meta-labeling with a train-only binner ✔, strictly forward labels ✔, lift not asserted, squared loss on 0/1 labels. Missing tests: NaN, ragged Frame, constant labels.
- **test_markov** (real 1d): look-ahead disagreement asserted. Min populated count > 0 asserted. χ² df asserted, but the real-vs-shuffle outcome is `check(true)` (263). A tautological ergodicity check (204). Good P33-03 regression. Returns 1 when the dataset is missing, which is the opposite of the other tests.
- **test_markov_eval** (real 1d): protocol assertions only; unconditional verdict prose; unchecked `train_b` dereference (164).
- **test_regime_rl** (real 1d): see C09-018 and C09-019. k-means determinism asserted ✔. Refusals for tiny and ragged input ✔.
- **test_attention** (synthetic): the causal-mask leak (unmasked < 0.5, masked > 4× leak), mask before softmax, Unspecified refused, causal vs same-padding receptive field, variable-selection magnitude, and cross-sectional rank under a market-wide shock are all well built. There is no out-of-range Causality test and no Mlp test.
- **test_dcf_sensitivity** (illustrative): terminal share, 5×5 grid spread > 1.5, zero refusals on a centred grid and some on a straddling grid, reverse DCF round-trip < 1e-6, unreachable target refused. The FCFE/Ke mix-up is covered in strategies/tests/test_fundamentals.cpp (not in group).

## 8 Open questions
1. Do models/spot_forecast.hpp:374-429 and models/forecast_scorecard.hpp:349-414 train on rows whose labels are not yet realised at forecast time, and do they net costs? (Other group's files; I saw only grep lines.)
2. What numeric verdicts did models_neural_5m, models_gbdt (meta-label lift), models_markov_eval and models_regime_rl print on the baseline run? ctest_default.log records only Passed and duration.
3. Is `WalkForwardSpec`/`walk_forward_fold` (backtest/validation.hpp) guaranteed to keep `test.end <= n`? markov_eval and test_neural_5m index `returns[i+1]` and `xs` on that assumption.
