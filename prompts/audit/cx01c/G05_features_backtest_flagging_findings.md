# CX-01C G05 — features / backtest / flagging review

Reader: `claude-subagent:G05_features_backtest_flagging` · Date 2026-09-14 · Baseline HEAD `a34af5c5c9fa8c7498e8cc4df00907548a41e1a5` (re-verified at end).
Read-only audit. Nothing was built or run. Numbers marked "derived" were worked out by hand, not measured.

## 1 Scope & coverage

- **24 of 24 assigned files fully read, 7,335 of 7,335 lines.** Every Read used chunks of 300 lines or fewer, except one early single 352-line view of `features/registry.hpp`. That view was complete (not truncated), and the file was then re-read in compliant chunks [1,289] + [290,352].
- For all 24 files, sha256 before reading = after = the partition inventory hash. No file changed since the inventory.
- Outside excerpts read only to check integration (these files are not reviewed): `analytics/derivatives.hpp` 290-379, `models/serving.hpp` 130-189, `models/horizon_eval.hpp` 215-284, `models/dataset.hpp` 195-274, `risk/cost.hpp` 134-153, `desktop/analytics_panel.hpp` 150-199, `desktop/quant_pages.hpp` 1915-1989. Plus grep hits in `book/l2_book.hpp`, `strategies/regime.hpp`, `analytics/greeks.hpp`, `analytics/rolling.hpp` and the Codex CX01 notes (Q04/Q05 text).

## 2 Findings table

| ID | Sev | Class | File | One line |
|---|---|---|---|---|
| C05-001 | P2 | CONFIRMED DEFECT | features/vector.hpp | Constructor does not cap `n` to `kMaxFeatures`; `set()` then writes out of bounds (Codex Q04) |
| C05-002 | P2 | CONFIRMED DEFECT (on reuse) | features/book_flow.hpp | Builders never clear or age-gate slots; a reused vector keeps a normal book's values after a crossed/empty book and still reports `complete()` (Codex Q05) |
| C05-003 | P2 | DESIGN GAP | features/kinematics.hpp | Distribution/Hurst builders have no time gate: history minutes old is emitted as present; warmup is a tick count, not the registered lookback |
| C05-004 | P3 | DESIGN GAP | features/registry.hpp | Band/lookback/warmup checks are opt-in and have no non-test caller; nothing ties a FeatureVector's `n`/version to a sealed registry |
| C05-005 | P3 | CONFIRMED DEFECT | features/kinematics.hpp | `trend_deviation` uses the newest print even when `now` is earlier (look-ahead); out-of-order push returns `NotFinite` |
| C05-006 | P2 | CONFIRMED DEFECT | backtest/engine.hpp | `run()` reports P&L exactly 0 when the last quote is not live, even with fills, costs and an open position |
| C05-007 | P2 | DESIGN GAP | backtest/engine.hpp | Market fills are zero-latency at the decision quote, ignore displayed size, use constant impact; `Quote` has no timestamp; refused intents are not counted |
| C05-008 | P3 | CONFIRMED DEFECT | backtest/engine.hpp | Ledger "conservation" identity is tautological; `worst_residual` can only be 0; negative fill price and negative cost accepted |
| C05-009 | P2 | DESIGN GAP | backtest/engine.hpp | `run()` never sets `Trade.delivery`: every Cash backtest is costed as intraday; unverified charge schedule not refused by `run()` |
| C05-010 | P3 | CONFIRMED DEFECT | backtest/validation.hpp | Walk-forward refuses an exactly-fitting fold; `gap=0` accepted; embargo not tied to lookback; `survives_purge` reads out of bounds for an out-of-range block |
| C05-011 | P2 | CONFIRMED DEFECT (numerical) | flagging/drift.hpp | ADWIN cut threshold depends on input scale (Hoeffding for [0,1] with no range check) and has no multiple-testing correction; its test masks false cuts and has a vacuous check |
| C05-012 | P2 | CONFIRMED DEFECT | flagging/deploy.hpp | Canary loss limit cannot fire before `min_observations`; `open()` can be re-called mid-canary or after rollback, resetting the loss accumulator |
| C05-013 | P3 | CONFIRMED DEFECT | flagging/scorecard.hpp | `at`/`aggregate`/`ic_spread` unchecked indices; `regime_cell` has no upper bound; a constant-forecast model is never de-weighted |
| C05-014 | P3 | DESIGN GAP | flagging/deploy.hpp | Scheduler proposes a "Scheduled" retrain on every first poll (every restart); `RollbackTrigger::armed()` does not validate `min_ic` (NaN disables the IC floor) |
| C05-015 | P3 | DESIGN GAP | flagging/drift.hpp, backtest/montecarlo.hpp | NaN inputs not refused by PSI/KS; O(n^2) insertion sorts; metric sentinels (`sortino=0` with no downside, `summarise().worst` is always the max) |

No P0 or P1: no audited component in this group is reachable from the order path. Engine, deploy and mc_pricing are test-only; see §5.

## 3 Finding details

### C05-001 — FeatureVector count not capped (P2, CONFIRMED DEFECT)
- **Evidence:** `features/vector.hpp:63-65` — `FeatureVector(std::uint64_t feature_version, std::size_t n, Timestamp ts) noexcept : version_(feature_version), count_(n), ts_(ts) {}`. `:71` guards only `if (i >= count_)`. `:75-76` then write `value_[i] = v;` and `present_[i / 64] |= ...`. Storage is `value_[kMaxFeatures]` (512) and `present_[8]` (`:132-133`).
- **Trigger:** `FeatureVector v{1, 600, Timestamp{0}}; (void)v.set(550, 1.0);` writes `value_[550]` and `present_[8]`, both past the end: out-of-bounds write into adjacent members/stack. `present_count()` and `complete()` (`:94-109`) also read out of bounds for any `count_ > 512`. With `n > 65535`, `static_cast<FeatureIndex>(i)` wraps, so `complete()` can never be true. `kSkip = 0xFFFF` becomes a writable slot once `n > 65535`.
- **Impact:** memory corruption in a noexcept value type consumed by `models/serving.hpp:146` (`ALTAIR_HOT infer`). No non-test constructor call exists today (grep), so it is latent.
- **Existing tests:** none construct an oversized vector (test_kinematics, test_book_flow use n ≤ 9).
- **Regression test:** construct with `kMaxFeatures + 1` and require refusal (factory returning `std::expected`, or clamp `count_` to 0 and flag), and `set(512, x)` → `OutOfRange` for any `n`.

### C05-002 — book/flow builders keep stale-but-present slots (P2, CONFIRMED on reuse)
- **Evidence:**
  - `features/book_flow.hpp:88-92`: NoLiquidity returns before touching any slot.
  - `:102-106`: Crossed writes only `s.crossed` and returns.
  - `:108-141`: each level slot is written only if its estimator succeeds.
  - `:175-183`: same for flow.
  - No call to `out.clear()`; `out.timestamp()` is never compared with the book.
  - `BookState` carries `exchange_ts`/`recv_ts` (`book/l2_book.hpp:47-48`), and `build_book` ignores both.
- **Trigger:** `FeatureVector v{1,8,Timestamp{0}}; build_book(normal_book(), all_slots(), v);` fills 8 slots. Then with the same `v`, `build_book(crossed_book, all_slots(), v)` returns `Crossed`, yet `v.has(0)` is true, still holding the normal book's imbalance, `v.value(7)==1.0`, and `v.complete()==true`. With an empty `BookState{}` in place of the crossed book: `NoLiquidity`, and all 8 old values remain present and complete.
- **Impact:** a caller that reuses a vector per tick without `clear()` feeds a model a complete vector mixing a stale book with a fresh crossed flag. `models/serving.hpp:154-166` checks only `feature_version` and presence, not age.
- **Existing tests:** `test_book_flow.cpp:77,109,154,196,206,220` always use fresh vectors. The "stay absent rather than taking a stale value" claim (`:224`) is only true for a fresh vector.
- **Regression test:** normal→crossed and normal→empty on one vector; assert level slots are absent and `complete()` is false. Better: the builder refuses when `out.timestamp()` ≠ `b.exchange_ts`, or clears its own slots before writing.

### C05-003 — distribution/Hurst features not time-gated (P2, DESIGN GAP)
- **Evidence:**
  - `features/kinematics.hpp:209-214`: `build_distribution(const PriceHistory<N>& h, const DistributionSlots& s, FeatureVector& out)` takes no `now`; its only warmup is `if (w.size() < 4)`.
  - `:242-247`: `build_hurst` is count-only.
  - The header promise at `features/vector.hpp:11-13` is to be absent "on an illiquid symbol that has not printed in ten minutes".
  - The registry's `FeatureSpec.lookback` (`registry.hpp:131-134`) is hashed but never consulted by any builder. `RollingWindow<N>` is a tick-count window, not a duration.
- **Trigger:** push 200 ticks at t = 0..2 s; call `build_distribution(h, s, v)` with `v` stamped 600 s later. mean, stddev, skew, kurtosis and zscore are all present.
- **Impact:** stale statistics admitted as fresh. A feature registered with a 5-minute lookback is computed over whatever N ticks the buffer holds (for `PriceHistory<1024>` at 10 ms that is about 10 s), so the hashed lookback does not describe the computation (rule 10 tuple). Builders are test-only today.
- **Tests:** `test_kinematics.cpp:208-247` only checks absence for degenerate data.
- **Regression test:** stale-`now` case → WarmingUp/absent; a lookback-mismatch case.

### C05-004 — registry enforcement is opt-in and unwired (P3, DESIGN GAP)
- **Evidence:**
  - `check_band` (`registry.hpp:270-283`), `features_for` (`:291-305`) and `warmup()` (`:325-331`) have no caller outside `features/tests` (grep of all first-party dirs).
  - `find`/`check_band`/`features_for` work on an unsealed registry.
  - `FeatureVector` accepts any version/`n` (`vector.hpp:63-65`), and `set()` knows nothing of bands.
- **Impact:** the header claims "HORIZON BANDS ARE ENFORCED, NOT ADVISORY" (`:25`), but enforcement exists only if a future pipeline calls it.
- **Test:** `test_registry.cpp` covers the functions in isolation. Sealing, order-hashing and refusal behave as documented.

### C05-005 — trend_deviation reads a print after `now` (P3, CONFIRMED DEFECT)
- **Evidence:**
  - `kinematics.hpp:140-145` fits only points with `0 <= age <= window` (`analytics/derivatives.hpp:312-313`).
  - `kinematics.hpp:177-181` then uses `h.window().last()`, the newest buffered print, whatever its timestamp.
  - `kinematics.hpp:78-80`: an out-of-order push increments `out_of_order_` but returns `BuilderError::NotFinite`.
- **Trigger:** push linear prices at 0..600 ms; `KinematicSlots s{}; s.trend_deviation=0;` `build_kinematics(h, at(300), duration::millis(200), s, v)`. The fit covers 100-300 ms, but the deviation uses the price at 600 ms (future relative to `now`).
- **Impact:** a rule-7 leak whenever a caller evaluates at a timestamp earlier than the newest tick (e.g. a feature stamped at a bar close while the buffer has later ticks). Low today (test-only).
- **Regression test:** the trigger above, asserting the slot is absent or equals the value computed with the future ticks removed; assert the error code for out-of-order is not `NotFinite`.

### C05-006 — P&L silently 0 when the final quote is not live (P2, CONFIRMED DEFECT)
- **Evidence:** `backtest/engine.hpp:336` `BacktestResult out{};` (`pnl{}` = 0) and `:377-380`:
  ```
  if (n > 0 && quotes[n - 1].live()) {
      const Price mark{(quotes[n - 1].bid.raw() + quotes[n - 1].ask.raw()) / 2};
      out.pnl = ledger.pnl(mark);
  ```
  There is no else branch, no error, and no "unmarked" flag.
- **Trigger:** the `test_engine.cpp` test-1 tape with `quotes[kN-1] = Quote{}` (a zeroed post-close or one-sided quote). Result: 400 fills, costs > 0, `pnl == 0`.
- **Impact:** a backtest reports break-even for any strategy whose tape ends on a missing or one-sided quote. That is the kind of plausible number rule 9 forbids.
- **Tests:** `test_engine.cpp:429` asserts `pnl == 0` only for a do-nothing strategy.
- **Regression test:** the trigger; expect an error (e.g. `NoTouch`) or a mark from the last live quote with a visible flag.

### C05-007 — optimistic market-fill model (P2, DESIGN GAP)
- **Evidence:**
  - `engine.hpp:343-349`: the strategy decides on `quotes[i]` and the order fills at `quotes[i]`, with zero latency.
  - `:102-111`: `Quote` has no timestamp, so alignment of the parallel `quotes[]` with `tape[]` cannot be checked (a quotes array shifted by +1 is look-ahead the engine cannot detect).
  - `bid_qty`/`ask_qty` are never read: a 10,000-unit buy against `ask_qty 900` fills fully at the ask.
  - `:223-228`: impact is a size-independent constant; `std::fabs(impact_paise)+0.5` cast to int64 is undefined for NaN.
  - `:350`: `if (!px) { continue; }` refused intents are not counted in `BacktestResult`.
- **Impact:** CLAUDE.md's reality check puts retail round trips at 10-50 ms. Fills here are faster and deeper than achievable, which flatters high-turnover strategies. The limit-order queue model (`:241-294`) is not used by `run()`.
- **Tests:** none for size, latency or refused-intent counts.
- **Regression test:** oversized qty → partial fill or refusal; `ticks_refused` counter; a quote-timestamp check (`quote.ts <= tick.exchange_ts`).

### C05-008 — ledger identity cannot detect what it claims (P3, CONFIRMED DEFECT)
- **Evidence:**
  - `engine.hpp:141-153` updates `cash_` and the gross/cost accumulators from the same `notional` and `f.cost` in the same statement block, and `:162-165` recombines them. The residual is identically 0 barring int64 overflow.
  - The comment at `:59-62` claims "a sign error, a double count, or a cost applied to the wrong side breaks the identity"; none can, because both routes consume the same `f.cost`.
  - `:155-157` already refuses a nonzero residual, so `:370-372` `worst_residual` can only be 0.
  - Only qty is validated (`:138`). A negative `f.cost` and a non-positive price (`:226-227`, `Price{q.bid.raw() - imp}` with bid < impact) apply cleanly.
- **Trigger:** `Fill{.side=Sell,.qty=75,.price=Price{-5},.cost=Notional{-1000}}` → `apply()` succeeds, residual 0.
- **Also:** this is a second conservation ledger alongside `core/invariant/conservation.hpp` `ConservationLedger`, which `app/main.cpp:125,383` uses.
- **Regression test:** refuse `price <= 0` and `cost < 0`; replace the tautology with an independent check (e.g. against `ConservationLedger`, or recompute cost independently).

### C05-009 — cost inputs in `run()` (P2, DESIGN GAP)
- **Evidence:**
  - `engine.hpp:352-358` builds `Trade` without `delivery`.
  - `risk/cost.hpp:144-145` gives `bool delivery = false`, and `:255` selects `t.delivery ? &s.equity_delivery : &s.equity_intraday`. So every `Segment::Cash` backtest is costed on the intraday schedule.
  - Delivery's both-side STT (`cost.hpp:316`) and DP charge (`:353-354`) can never apply.
  - `run()` has no product parameter.
  - `run()` does not inspect `CostBreakdown.schedule_verified` (`cost.hpp:164,296`). Whether `compute_cost` itself refuses an unverified schedule was not read (see §7).
- **Impact:** delivery-style strategies are backtested with understated costs (gate 7).
- **Test:** `test_engine.cpp:61-75` builds only an intraday schedule.
- **Regression test:** a delivery flag or product in `run()`; a delivery round trip must charge STT on both sides plus DP.

### C05-010 — validation boundary defects (P3, CONFIRMED DEFECT)
- **Evidence:**
  - `validation.hpp:166-169` `if (n <= first_test + s.test_len) → TooFewSamples`, but the loop at `:171` yields k=1 when `n == first_test + test_len`. An exactly-fitting fold is refused (safe side).
  - `:141-146` says the gap is "NOT optional", yet `gap=0` is accepted (contrast `NoEmbargo` at `:276-278`).
  - The embargo is only required to be nonzero, not ≥ lookback or label horizon.
  - `:229` reads `windows[test.start]` before any `test.start < n` check. `survives_purge(0, Block{10,11}, w, 10, 0)` reads `w[10]` out of bounds.
  - `forward_horizon_windows(..., 0)` produces empty windows; `LabelWindow::valid()` and `BadLabelWindow` are never used.
- **Callers:** they set gap 1 or 20 (`models/horizon_eval.hpp:251` one-bar label, `desktop/data/fits.hpp:225`, `desktop/quant_pages.hpp:2571`), so no live leak was found.
- **Tests:** `test_validation.cpp:436-438` covers only a clearly short n.
- **Regression tests:** exact-fit n → 1 fold; gap 0 → refused unless explicitly deliberate; out-of-range block → false without a read.

### C05-011 — ADWIN threshold is scale-dependent and uncorrected (P2, CONFIRMED DEFECT, numerical)
- **Evidence:** `flagging/drift.hpp:357-359`:
  ```
  const double m = 1.0 / (1.0 / n0 + 1.0 / n1);
  const double eps = std::sqrt(std::log(2.0 / delta_) / (2.0 * m));
  if (std::fabs(m0 - m1) > eps) {
  ```
  This is Hoeffding's bound for variables in [0,1], but `push` accepts any double and `configure` (`:322-330`) takes no range. δ is not divided by the number of split points tested (`:349` tests up to n-2 cuts on every push); Bifet & Gavaldà use δ' = δ/n.
- **Derived:** for iid N(0,σ²), sd(m0−m1) = σ/√m, so a cut fires at z = √(ln(2/δ)/2)/σ = 1.86/σ at δ=0.002.
  - σ=1: 1.86σ, about 6% per split test; at n=16 there are 7 eligible splits, so pure noise is expected to cut within tens of pushes.
  - σ=0.3 (the test): 6.2σ, so no false cuts.
  - σ=0.01: 186σ, effectively never fires. `desktop/quant_pages.hpp:1942-1946` feeds `std::fabs(r[i])`.
- **Tests:**
  - `test_flagging.cpp:287` records a cut only `if (... i > 800)`, so pre-shift false cuts are ignored.
  - `:297` `ad.window() < 1500` is vacuous for `Adwin<1024>`.
  - Also `drift.hpp:335-341`: at capacity the window silently drops the oldest sample, and `window()` (THE output) is capped at `Cap` with no count (rule 11, visible truncation).
- **Impact:** a detector that is simultaneously a false-alarm generator and blind, depending on units. A drift alarm unconditionally rolls back a canary (`deploy.hpp:294`). Flagging is not wired to trading today.
- **Regression test:** `Adwin<1024>`, `configure(0.002)`, 1,000 iid N(0,1) pushes → assert `cuts()==0` (fails today by derivation); the same stream ×0.01 with a +3σ shift → assert a cut. Fix: take and enforce a value range R (eps·R), use δ/n.

### C05-012 — canary loss limit gated by sample size; re-open resets it (P2, CONFIRMED DEFECT)
- **Evidence:** `flagging/deploy.hpp:294-301`:
  ```
  if (n_ < trigger_.min_observations) { return outcome_; }
  ...
  if (loss_ > trigger_.max_loss_paise) {
  ```
  `open()` (`:243-267`) has no `open_`/rolled-back guard and resets `loss_ = 0`, `n_ = 0` under the same armed trigger.
- **Trigger:** `arm({min_ic 0.1, max_loss 50'000, max_div 2.0, min_obs 30})`; open; 29× `observe(..., realised_paise=-10'000'000, false)`. Outcome `None`, `running()` true, `capital_fraction()` still 0.10. The canary lost Rs 29 lakh against a Rs 500 limit before the rule could fire. Separately: after a rollback, `open(shadow, fallback, 0.10)` succeeds again with a fresh loss counter.
- **Impact:** the hard money stop is subordinated to an IC-noise guard (rule 9 inverted), and "pre-registered" limits can be reset by re-opening. `deploy.hpp` is included only by its test.
- **Existing tests:** `test_flagging.cpp:430-433,448-496`: poisoning starts at observation 60, after `min_observations` 30, so the gate is never exercised.
- **Regression test:** the trigger → `LossExceeded` on observation 1; `open()` while running or after rollback → refused.
- **Minor:** `loss_ += -realised_paise` (`:279`) is undefined for INT64_MIN.

### C05-013 — scorecard index hygiene and a constant-forecast blind spot (P3, CONFIRMED DEFECT)
- **Evidence:**
  - `scorecard.hpp:189-196` `at()`, `:209-223` `aggregate()` and `:232-246` `ic_spread()` index `cells_`/`agg_` with no bounds check (`observe` checks at `:176-178`). `sc.aggregate(32, 0)` reads past `agg_`.
  - `regime_cell` (`:159-166`) has no upper bound. Valid enums give at most 3·4+3 = 15 (`strategies/regime.hpp:80-92`), but a corrupted enum value writes out of bounds in `observe` (`:181`).
  - `ScoreCell::ic()` refuses a constant forecast as `TooFewObservations` (`:139-141`), so `update_weight` returns `current` (`:292`) and `RetrainScheduler`'s ScoreDecay (`deploy.hpp:425`) never fires for a model whose output collapsed to a constant.
  - A NaN `current` weight passes through the clamp (`:296-299`).
- **Regression test:** out-of-range model in `aggregate` → refused; constant-forecast cell → an explicit Degenerate error that de-weights toward the floor.

### C05-014 — deploy parameter hygiene (P3, DESIGN GAP)
- **Evidence:**
  - `deploy.hpp:432-433`: `last_.ns_since_epoch() == 0` means the first `poll()` after any construction or restart proposes `Scheduled`, and `last_` is not persisted.
  - `:204-207`: `armed()` ignores `min_ic`, so `min_ic = NaN` makes `*v < trigger_.min_ic` always false and silently disables the IC floor.
  - `configure(period, ic_floor)` accepts NaN `ic_floor`.
- **Regression test:** NaN `min_ic` → `arm()` refused; first poll with a persisted last-fired time.

### C05-015 — input and sentinel hygiene in drift/metrics (P3, DESIGN GAP)
- **Evidence:**
  - `drift.hpp:111-146,156-171`: NaN in the reference corrupts the insertion sort and edges, and `bin_of(NaN)` returns bin 0 (`:181-185`). Nothing is refused.
  - `ks_statistic` (`:201-229`) likewise.
  - Insertion sorts are O(n²) (`:123-128`, `:206-215`, `montecarlo.hpp:441-446`): cold path, but a 100k reference is about 5e9 moves.
  - `montecarlo.hpp:375-377` returns `sortino = 0.0` when there is no downside (a sentinel indistinguishable from "no edge").
  - `:455` `d.worst = v[n-1]` is always the maximum, which is the best, not the worst, for a Sharpe distribution.
- **Regression test:** NaN input → refused; summarise on Sharpe values documents or fixes "worst".

## 4 Lead revalidation

- **Codex Q04 — CONFIRMED.** `vector.hpp:63-65` stores `n` uncapped; `:71` guards against `count_` only; `:75-76` write `value_[i]`/`present_[i/64]`. Concrete input: `FeatureVector{1, 600, ts}` then `set(550, 1.0)`. See C05-001.
- **Codex Q05 — CONFIRMED and extended.** Normal-then-stale/crossed reuse leaves old slots present and `complete()` true. Neither builder reads `BookState.exchange_ts`, and serving does not age-check. See C05-002.
- **registry.hpp bands / lookback / sealing — PARTIAL.**
  - Sealing is enforced (`add` refuses after seal; empty seal refused; order-sensitive FNV-1a hash including index, version, bands and lookback).
  - `band_of` boundaries are consistent.
  - Band and warmup enforcement exist only as opt-in functions with no non-test caller.
  - Lookback is hashed but not honoured by builders (C05-003/004).
- **kinematics causality — PARTIAL.** The polynomial fit excludes future points, and out-of-order ticks are refused and counted. But `trend_deviation` can read a future print (C05-005), and distribution/Hurst have no time gate (C05-003).
- **options_calendar causality — NOT CONFIRMED as a defect.** No clock is read; out-of-session is refused, not clamped; cycle positions are caller-supplied from the calendar; T ≤ 0 fills no greeks.
- **engine.hpp:**
  - Fill model: Mid/Touch/TouchPlusImpact; Unspecified refused. Costs are applied per fill, net, via `compute_cost` (rule 5 respected), but the delivery flag is never set (C05-009).
  - Timing is zero-latency at the decision quote: not a future read by itself, but unverifiable (C05-007).
  - **Stop ordering (rule 8): NOT APPLICABLE.** No stop logic exists anywhere in `backtest/` (case-insensitive grep "stop": no matches).
  - **Same-bar entry/exit ambiguity: NOT APPLICABLE.** The engine is tick-level with one intent per tick and no bars.
  - Final mark defect: C05-006.
- **validation.hpp — PARTIAL.** Purge uses per-sample label windows and is correct for contiguous blocks. The embargo must be nonzero or explicitly zero, but is not checked against lookback/horizon. The walk-forward gap is unvalidated, plus boundary defects (C05-010). There is no random K-fold generator: `survives_purge_set` exists to measure the shuffled case only. Walk-forward is what the models/desktop callers use.
- **montecarlo.hpp jump bound — CONFIRMED (proof holds analytically), with a caveat.**
  - λ > 12 is refused (`:258-260`).
  - For Poisson(12), ln P(N=65) = −12 + 65·ln12 − ln65! ≈ −12 + 161.52 − 209.34 = −59.8, so P(N≥65) ≈ 1.3e-26 < 1e-25 (derived).
  - Caveat: the inversion's floating-point cumulative sum can settle a few ulps below the largest `uniform()` (1−2^-53), so the cap is reachable with probability of order 1e-16 per step, not 1e-25, and a cap hit is uncounted. Negligible in practice; recorded in §7, not raised as a finding.
- **mc_pricing.hpp — reviewed, no defect raised.** The control variate is consistent with discounted Black-76 (`greeks.hpp:244` df; `exact_euro / df`). The antithetic pair counts as one observation (correct SE). `std::vector` is used in `mc_asian` on a cold path only. Minor: the control is priced on F and K truncated to integer paise (`:191-192`).
- **flagging:**
  - deploy: C05-012, C05-014.
  - drift PSI/KS: bins frozen on the reference, floors finite, KS tie-handling correct; Page-Hinkley standard one-sided form; ADWIN defective (C05-011).
  - scorecard: per-regime cells and an aggregate that carries cell counts are CONFIRMED as described; hygiene issues in C05-013.
  - **Wiring: NOT wired into any trading runtime.** `deploy.hpp` has test-only use; `drift.hpp`/`scorecard.hpp` feed only desktop research pages (`desktop/quant_pages.hpp`).

## 5 Architecture facts

- **Ownership:**
  - features/: P5-01..04, header-only `altair_features` INTERFACE.
  - backtest/: P6-05..07 and P17-01, `altair_backtest` INTERFACE. It links strategies and risk but no oms (CMake line 15-22).
  - flagging/: P9-01..07, `altair_flagging` INTERFACE. It links models and strategies but no oms.
  - None of the three can place an order.
- **Implemented vs connected:**
  - Feature builders (`build_book`, `build_flow`, `build_kinematics`, `build_distribution`, `build_hurst`, `build_calendar`, `build_options`, `build_surface`, `build_cross_asset`): tests only. `research/feature_card.hpp` emits code templates that reference them.
  - `FeatureVector`: consumed by `models/serving.hpp` (ALTAIR_HOT `infer`), `models/dataset.hpp` and `models/registry.hpp`; no non-test code constructs one.
  - `FeatureRegistry`: included by `desktop/quant_pages.hpp` and `research/feature_card.hpp`.
  - `backtest/engine.hpp`: tests only. No strategy implements its `on_tick(Tick, Quote, Ledger)` concept (grep across strategies/app/oms/feed/models/desktop/risk: no matches). The "backtest and live share one path" property is therefore not yet exercised by real strategies. `app/main.cpp` uses `core/invariant` `ConservationLedger`, not this `Ledger`.
  - `validation.hpp`: `models/horizon_eval.hpp`, `models/markov_eval.hpp`, `models/labels.hpp` (LabelWindow), `desktop/data/fits.hpp`, `desktop/quant_pages.hpp`.
  - `montecarlo.hpp`: `desktop/analytics_panel.hpp` (bootstrap + metrics).
  - `mc_pricing.hpp`: tests only.
  - `flagging/deploy.hpp`: tests only.
  - `flagging/drift.hpp`, `flagging/scorecard.hpp`: `desktop/quant_pages.hpp` only.
- **Hot vs cold:**
  - The only ALTAIR_HOT consumer touching this group is `models/serving.hpp::infer`, which calls `FeatureVector::value`: noexcept, no allocation.
  - Builders are noexcept and heap-free (`static thread_local` scratch in `build_hurst`), but unmarked.
  - Allocation: `mc_asian` (`std::vector`) and the desktop callers. Validation, MC, drift and scorecard are cold/research paths.
  - ADWIN `push` is O(Cap) per call.
- **Threads/shared state:** all types are plain single-owner values with no internal synchronisation. `thread_local` scratch appears in `build_hurst` and `derivatives_at`.
- **Time convention:** builders and the engine read tick time (`Tick.exchange_ts`, caller-supplied `now`); no wall clock found in this group. Money in the engine ledger is int64 paise; feature values are doubles by design (analytics).
- **Backtest granularity:** **tick-level**, market orders only in `run()`; a queue-aware limit model exists as standalone helpers not driven by `run()`. No bars, no stops, no latency.

## 6 Test-suite observations

- `test_engine.cpp`:
  - Covers fill-model gift, queue model helpers, ledger arithmetic and in-order delivery.
  - Missing: non-live final quote (C05-006), displayed size or latency, negative price or cost, refused-intent counts, `worst_residual`, limit orders through `run()`, delivery costs.
- `test_validation.cpp`: strong demonstration of contamination and embargo. Missing: exact-fit fold, gap=0 refusal, out-of-range block.
- `test_montecarlo.cpp`: moment and Ito/compensator checks; λ-refusal test checks only accept/refuse, not the arrival distribution near the cap; no NaN parameter tests.
- `test_mc_pricing.cpp`: good closed-form agreement, 1/√n and control-variate checks; no put, no non-integer F/K.
- `test_registry.cpp`: complete for registry semantics; nothing ties builders to bands.
- `test_book_flow.cpp`: fresh vectors only (Q05 gap); flow tested with fakes.
- `test_kinematics.cpp`: no stale-`now` case, no oversized vector (Q04), out-of-order error code not asserted (`:256-257` checks only `!back`).
- `test_options_calendar.cpp`: good. The circular/linear measurement is sound; negative `price_uncertainty` not tested.
- `test_flagging.cpp`:
  - ADWIN false cuts before the shift are ignored (`:287`), and `window() < 1500` is vacuous (`:297`).
  - Canary loss-before-min-observations and re-open are untested.
  - Scheduler first-poll and NaN `min_ic` are untested.
  - Its IC/PSI assertions are meaningful.
- Tests are executable programs that print, with a `check()` counter; each is registered with `add_test` in its CMakeLists. Not run in this audit.

## 7 Open questions

1. Does `risk/cost.hpp::compute_cost` refuse an unverified `ChargeSchedule` (only `out.schedule_verified = sch.verified;` at `:296` was seen)? If not, C05-009 should include an unverified-schedule reproducer.
2. What units does `desktop/quant_pages.hpp` feed ADWIN/Page-Hinkley (`std::fabs(r[i])` with PH delta 5.0, lambda 500)? That determines whether the page is showing a blind or an over-firing detector (C05-011).
3. Is the intended per-tick feature pipeline "one vector per tick, cleared each time" or "one persistent vector"? The answer decides whether C05-002 is a live defect or an API contract that must be written into the builder signatures.
4. Should the jump cap's rule-11 proof also account for floating-point accumulation (a counter on cap hits would make the question moot)?
5. Is `backtest/Ledger` meant to be replaced by `core/invariant/ConservationLedger` so there is one conservation implementation (C05-008)?
