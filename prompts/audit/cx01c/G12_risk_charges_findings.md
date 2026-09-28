# CX-01 G12_risk_charges -- findings (FINAL)

Reader: claude-subagent:G12_risk_charges · Date 2026-09-14 · Baseline a34af5c5c9fa8c7498e8cc4df00907548a41e1a5

## 1 Scope & coverage

Assigned: 26 files (risk/ + config/charges.toml + config/lot_size_history.csv), 7,540 lines per partition counts.

**All 26 files fully read**, every line, in chunks of 300 lines or fewer. All sha256 values before and after equal the inventory. Unread or partial: none.

No build, test or executable was run. Integration statements come from Grep across the first-party dirs (reference trees, build/, dataset/, data/, prompts/ and client/ excluded). Files outside the group were grepped, not read, except for context lines of core/types/units.hpp:225-334, which this group does not claim.

## 2 Findings table

| ID | Sev | Class | Title |
|---|---|---|---|
| C12-001 | P1 | DESIGN GAP | `check_order` (pre-trade limits) has no non-test caller |
| C12-002 | P1 | DESIGN GAP | Conservation check and daily-loss halt never trip the kill switch outside tests |
| C12-003 | P1 | CONFIRMED DEFECT | `[safety]`, `[broker.*]`, `[implicit]` in charges.toml are never parsed; "block on unverified" is inert |
| C12-004 | P2 | CONFIRMED DEFECT | A rate key missing from a present segment table silently becomes 0 |
| C12-005 | P2 | DESIGN GAP | Currency options are unreachable; they get priced on the currency-futures line |
| C12-006 | P2 | CONFIRMED DEFECT | Sizing accepts `edge_sigmas <= 0`, negative std error, and `kelly_divisor < 1` |
| C12-007 | P2 | CONFIRMED DEFECT | Slippage model accepts zero arrival/fill/final prices, which biases eta low |
| C12-008 | P2 | DESIGN GAP | Zero-valued `RiskLimits` disables every cap, including daily loss and staleness |
| C12-009 | P3 | CONFIRMED DEFECT | `uncovered_after_days` is days-since-epoch, not the documented gap |
| C12-010 | P3 | CONFIRMED DEFECT | `verified` flips true for any string other than exactly "UNVERIFIED" |
| C12-011 | P3 | CONFIRMED DEFECT | `to_paise` rounds halves toward zero while its doc claims away from zero |
| C12-012 | P3 | DESIGN GAP | Optimisers: no convergence diagnostic; `IllConditioned` never returned; doc/check mismatch on sample count |
| C12-013 | P3 | DESIGN GAP | Unchecked int64 sums/products (cost total, GST base, tail hedge, `Leg::notional`, Kelly budget cast) |
| C12-014 | P3 | DESIGN GAP | Limits refuse risk-reducing orders at gross-cap or daily-loss breach (no reduce-only path) |
| C12-015 | P3 | CONFIRMED DEFECT | Loader parses schedule dates as UTC midnight; test_cost builds IST-midnight schedules (no in-session trigger) |
| C12-020 | P3 | CONFIRMED DEFECT (test) | test_charges_toml claims a file-vs-mirror comparison that does not exist; no exact rate or paise assertions |
| C12-021 | P3 | CONFIRMED DEFECT (test) | Stress/BL tests pass vacuously without dataset/; tautological hedge check; optimiser verdict unasserted |
| C12-016 | P3 | CONFIRMED DEFECT | lot_size_history.csv: nothing reads it, and its boundary rows overlap on 2026-09-29 |
| C12-017 | P2 | CONFIRMED DEFECT | Black-Litterman multi-view posterior is wrong for correlated assets (sequential scalar updates) |
| C12-018 | P3 | CONFIRMED DEFECT | StressWindow documented inclusive but used half-open; correlation failure reported as 0.0 |
| C12-019 | P3 | DESIGN GAP | MC VaR promises a quantile error bar it does not return; iid bootstrap ignores vol clustering; no NaN guard |

## 3 Finding details

### C12-001 · P1 · DESIGN GAP -- pre-trade limits are not connected
- Evidence: a grep of core feed book instruments analytics features models strategies risk oms broker flagging backtest server desktop app research (excluding the reference trees) finds `check_order(` only in `risk/limits.hpp:246` (definition) and `risk/tests/test_limits.cpp`. `risk/limits.hpp` is included by `oms/reconcile.hpp:39` (it uses `KillSwitch`) and by `desktop/quant_pages.hpp:58`. No `Violation::` use exists outside risk/.
- Trigger: any order-placement path in oms/ or app/ places an order without the freeze-qty, band, tick, whole-lot, position/gross cap, stale-quote or daily-loss checks.
- Impact: the "last thing between a signal and the exchange" (limits.hpp:3) is not in the path in any executable.
- Test coverage: unit tests exercise the function in isolation only.
- Regression test: an oms-level test that submits an order with qty 66 (lot 65), which must be refused with `NotWholeLots` before any adapter `place` is invoked. Also assert that a tripped `KillSwitch` blocks the adapter call.

### C12-002 · P1 · DESIGN GAP -- conservation and daily-loss trips are unwired
- Evidence: `enforce_conservation` appears outside risk/ only in strings and comments in `desktop/kill_switch.hpp:11,271`. `KillReason::DailyLoss` is used only in `risk/tests/test_limits.cpp:168`. `check_order` takes `const KillSwitch&` (limits.hpp:247) and only sets the `DailyLossLimit` bit (313-316), even though `RiskLimits::max_daily_loss` says "Breaching it is a kill-switch condition, not merely a rejected order" (limits.hpp:201-202).
- Trigger: a ledger residual that is not zero, or `day_pnl <= -max_daily_loss`, never trips the switch in a running engine.
- Impact: CLAUDE.md's "checked every tick; a breach trips the kill switch" invariant is not enforced at runtime.
- Test coverage: unit tests only.
- Regression test: engine loop test with fills −1,565,200, costs −1,234 and cash_delta +1,566,433 paise (residual −1). The switch must be tripped with `ConservationBreach`. A second case with day_pnl −500,001 against a limit of 500,000 must trip `DailyLoss`.

### C12-003 · P1 · CONFIRMED DEFECT -- safety keys in charges.toml are never read
- Evidence: `charges.toml:223-229` has `block_on_unverified_schedule = true`, `edge_over_cost_factor = 2.5` and `block_on_unset_broker_slab = true`. `charges.toml:189-190` says of XTS: "the cost calculator REFUSES to price an XTS order until they are set". `charges_toml.cpp:169-302` reads only `last_verified`, `[common]` and `[[schedule]]`. A grep finds no C++ reader of `block_on_unverified`, `edge_over_cost_factor`, `[broker.*]` or `[implicit]`. `compute_cost` (cost.hpp:271-376) never refuses on `sch.verified == false` and only copies the flag (296). A default `BrokerageRule{}` means zero brokerage (cost.hpp:127-135). `ops/go-live.md:38` lists 1.6 as done because the flag "is what makes 1.2 fail loudly".
- Trigger: today's file, where `last_verified = "UNVERIFIED"`. Every `compute_cost` succeeds, with `schedule_verified=false`.
- Impact: the go-live checklist relies on an enforcement that does not exist. Broker slabs are not sourced from config, so callers build them in code.
- Regression test: load the real charges.toml and price a NIFTY fut sell of 65 units at 2,408,000 paise on 2026-04-02. With `block_on_unverified_schedule=true` the result must be an error. Today it returns a breakdown with STT 78,260 paise (156,520,000 × 0.0005).

### C12-004 · P2 · CONFIRMED DEFECT -- missing rate key inside a present table becomes 0
- Evidence: charges_toml.cpp:89-92, `if (n == nullptr) { return false; // absent: caller keeps the default }`. The `SegmentCharges` defaults are all 0 (cost.hpp:83-89), and `out.present = true` is set regardless (charges_toml.cpp:165). This contradicts charges_toml.hpp:34 "A MISSING RATE BLOCKS. IT DOES NOT DEFAULT TO ZERO". Unknown keys are ignored, so a typo such as `stt_rte` is also silent.
- Trigger: a `[schedule.equity_options]` table without `stt_rate`, or with a misspelled key.
- Impact: options are priced with no STT, which understates cost and flatters strategies.
- Regression test: load a one-schedule TOML whose equity_options has every key except `stt_rate`, then price a sell of 65 units at 10,000 paise. Expected: a load error, or `UnknownSegment`. Today STT = 0 where 975 paise is due (650,000 × 0.0015).

### C12-005 · P2 · DESIGN GAP -- currency options unrepresentable
- Evidence: `Segment` has no currency-option value (`instruments/contract_spec.hpp:51`). cost.hpp:258 is `case Segment::Currency:  return &s.currency_futures;`. `currency_options` is loaded (charges_toml.cpp:257) but unreachable.
- Trigger: a currency option trade.
- Impact: exchange charge 0.0000035 is used instead of 0.0003503, a 100× understatement, on a futures (notional) basis label.
- Regression test: a currency option sell of 1,000 units at 5,000 paise premium. Turnover is 5,000,000 paise, so exchange txn must be 1,752 paise. Today it is 18.

### C12-006 · P2 · CONFIRMED DEFECT -- sizing policy inputs not refused on the unsafe side
- Evidence: sizing.hpp:124-130 claims "The five policy numbers below are ZERO, and every one of them is refused as zero". `edge_sigmas` (147) is never validated. kelly_fraction (227-229) computes `edge_bps - edge_sigmas * edge_std_error_bps` with no check on sign or zero. `kelly_divisor` only needs `> 0` (223), so 0.25 (a "quarter" typed as a fraction) gives 4× full Kelly. The Kelly budget cast at 304-306 has no 9e18 guard, unlike vol-target at 271.
- Trigger: `edge_sigmas = 0` sizes on the point estimate. `edge_sigmas = -2` sizes on the UPPER bound. `kelly_divisor = 0.25`.
- Impact: this violates the "size on lower confidence bound" rule. Kelly still competes with fixed-fractional and vol-target in a min(), so the damage is bounded by those legs.
- Regression test: capital 1e9 paise, price 2,408,000, lot 65, edge 20 bps, se 15 bps, sigmas 0, vol 0.01, divisor 4, target_vol 0.01, leverage 1, risk_fraction 0.01, stop 10,000. `size_position` must return `BadParameter`. Today Kelly f = 0.002/1e-4/4 = 5.0, which is then capped by leverage.

### C12-007 · P2 · CONFIRMED DEFECT -- slippage observations with zero prices accepted
- Evidence: slippage.hpp:140-142 validates only `dp > 0`, the quantities, and finiteness (always true for int64-derived doubles). `arrival_price`, `fill_price` (when filled>0) and `final_price` (when unfilled>0) can be 0. `add` regresses `y = impact_bps + opportunity_bps` (205).
- Trigger: a buy with ordered=100, filled=0, decision=arrival=10,000, fill=0 and final=0 (final_price not populated). This gives opportunity_bps = (0−10,000)×1×1 = −10,000 bps.
- Impact: a huge favourable "cost" pulls eta down, the unsafe direction, which undermines `predict_upper`.
- Regression test: that outcome must return `BadObservation`.

### C12-008 · P2 · DESIGN GAP -- zero limits mean unlimited
- Evidence: every cap is gated on `> 0` (limits.hpp:276, 294, 298, 304, 313, 317, 320), including `max_daily_loss` and `max_quote_age`. The doc at 189-190 says "none defaulted to something permissive". `RiskLimits{}` approves an hour-old quote on a book down ₹1 crore.
- Regression test: `check_order(good, AccountState{day_pnl=-1e9}, RiskLimits{}, k, now)` with quote_ts = now−3,600s. The expected result is a refusal (StaleQuote|DailyLossLimit), or the struct must be made unconstructible without explicit values.

### C12-009 · P3 · CONFIRMED DEFECT -- `uncovered_after_days` wrong quantity
- Evidence: charges_toml.cpp:298-299 is `rep.uncovered_after_days = last / kNsPerDay;`. The doc (charges_toml.hpp:92-95) says "Zero means the file covers the future".
- Trigger: the real file gives 47,481 (days from 1970-01-01 to 2099-12-31), a positive number although coverage is open-ended. No consumer was found by grep.
- Regression test: loading the real file, expect 0 (or rename the field and expect 47,481).

### C12-010 · P3 · CONFIRMED DEFECT -- verification flag too permissive
- Evidence: charges_toml.cpp:178 is `rep.verified = v && !v->empty() && *v != "UNVERIFIED";`. "unverified", "TODO" and "NO" all read as verified. The doc says "True when last_verified is a date" (hpp:89-90).
- Regression test: `last_verified = "unverified"` must give `verified == false`.

### C12-011 · P3 · CONFIRMED DEFECT -- portfolio rounding direction
- Evidence: portfolio.hpp:143-144 is `v >= 0.0 ? std::ceil(v - 0.5) : std::floor(v + 0.5)`. For 2.5 this gives ceil(2.0)=2, and for −2.5 it gives −2, so halves go toward zero. The doc (132-136) says away from zero. The impact is at most half a paisa per aggregate.
- Regression test: `detail::to_paise(2.5) == 3` and `to_paise(-2.5) == -3`.

### C12-012 · P3 · DESIGN GAP -- optimiser/covariance diagnostics
- Evidence: covariance.hpp:51-53 documents TooFewSamples as "Fewer observations than assets + 2", but the check is `n < 3` (118), so p ≥ n (singular) is accepted. `OptError::IllConditioned` (optimise.hpp:55) is never returned. `min_variance` (130) and `risk_parity` (153) run fixed iteration counts with no residual or convergence flag. The projected-gradient step `0.5/avg_var` is not guaranteed below 1/λmax of the tangent-space Hessian (HYPOTHESIS for non-convergence). `condition_number` starts power iteration at the all-ones vector, which is an exact eigenvector for equicorrelated equal-variance matrices. The shifted pass can then return 0 and report cond≈1 (HYPOTHESIS; roundoff may rescue it).
- Regression test: p=10, σ²=1, ρ=0.9 equicorrelation. Expected condition number 91 (9.1/0.1), within 10%.

### C12-013 · P3 · DESIGN GAP -- unchecked integer arithmetic
- Evidence: cost.hpp:363-374 (gst_base and total are plain int64 sums; rates have no upper bound at load, and `flat_per_order` is unbounded). hedge.hpp:214-217 (`gap + protection - 1`, `units * premium`). neutralise.hpp:89-91 (`qty.raw() * price.raw()` bypasses `notional_of`), plus 201 and 215. sizing.hpp:304-306. limits.hpp:264 (negating INT64_MIN). All of these need absurd magnitudes; no realistic trigger.

### C12-014 · P3 · DESIGN GAP -- no reduce-only exemption
- Evidence: limits.hpp:304-310 adds `|qty|` notional to gross even for a closing order. The daily-loss bit (313-316) refuses every order. A book at its gross cap, or past its daily loss, cannot be flattened through this gate. Whether oms/exit_ladder bypasses it is an open question (the function is unwired anyway; C12-001).

### C12-015 · P3 · CONFIRMED DEFECT (convention mismatch, no in-session trigger) -- UTC vs IST schedule boundary
- Additional evidence: risk/tests/test_cost.cpp:46-50 builds schedule bounds as `days*86400e9 - kIstOffset` (IST midnight), and 56-57/95-96 use them. The loader uses UTC midnight. The codebase therefore holds two conventions for the same dates. test_cost's hand-built `valid_to = ist_date(2026,3,31)` is the START of 31 March IST, so a 09:15 IST trade on 31 March would find no schedule with that fixture. test line 249 checks only the exact boundary instant.
- Evidence: charges_toml.cpp:45-66 maps "YYYY-MM-DD" to UTC midnight. Circular dates are IST. A trade stamped 2026-04-01 00:00–05:29 IST is 2026-03-31 UTC and gets schedule 1. No exchange session falls in that window, so there is no live trigger today. It matters only for non-session timestamps (exercise, settlement).

### C12-016 · P3 · CONFIRMED DEFECT (data) -- lot_size_history.csv
- Evidence: the grep finds no C++ reader; the only mentions are `ops/go-live.md:57` and the file itself. Rows 75-76 are `NIFTY,UNKNOWN,2026-09-29,...` and `NIFTY,2026-09-29,OPEN,65,...`. Both claim the 2026-09-29 expiry under the inclusive semantics the header defines (lines 45-47, 67-68). BANKNIFTY rows 77-78 have the same problem. A future reader would hit an UNKNOWN row for that expiry, which blocks, so the failure is on the safe side.

### C12-017 · P2 · CONFIRMED DEFECT -- BL posterior double-counts correlated views
- Evidence: stress.hpp:168-170 claims "With absolute views P is a selection matrix and the inverse is diagonal". But P·τΣ·P' is the τΣ sub-matrix, which is diagonal only for uncorrelated assets. Lines 204-219 apply each view as an independent scalar update, `resid = v.value - out.equilibrium[v.on]`, and accumulate into `mu` with no cross-view covariance.
- Trigger: Σ=[[0.04,0.03],[0.03,0.04]], w=(0.5,0.5), λ=2.5, τ=0.05, views q=(0.10,0.10) with confidence (std dev) 0.05 each. π=(0.0875,0.0875). The correct posterior is π+τΣ(τΣ+Ω)⁻¹(q−π) = 0.0947917 each. The code gives 0.0875 + 2×(0.4444×0.0125×{1, 0.75}) summed = 0.0972222 each, a 33% over-tilt. As ρ→1 and Ω→0 the code overshoots both views (→0.1125).
- Impact: wrong expected returns wherever more than one view touches correlated assets. Currently used only in the desktop quant pages; not in the order path.
- Regression test: the numbers above, `mu[0] == 0.0947917 ± 1e-6`.

### C12-018 · P3 · CONFIRMED DEFECT -- stress window semantics
- Evidence: stress.hpp:66 says "Inclusive index range", but 114, 119 and 122 use `[from, to)` (`i < w.to`, `days = to - from`). At 132-133, `r.correlation = c ? *c : 0.0;` turns a BadShape or zero-variance failure into a reported correlation of 0.0 (rule 9).
- Trigger: a caller following the doc passes `to` = the last crisis day's index, and that day's return is dropped. An `other` series shorter than `port` reports correlation 0.0.
- Regression test: port = 10 returns of −0.01 plus one of −0.10 at index 10, window {0,10} documented inclusive. Expected cumulative exp(−0.20)−1 = −0.18127; the code gives exp(−0.10)−1 = −0.09516. A mismatched `other` must yield an error, not 0.0.

### C12-019 · P3 · DESIGN GAP -- VaR details
- Evidence: var.hpp:194-195 says MC "adds ... an error bar on the quantile", but `monte_carlo_var` returns the plain `VarResult` (196-223) with no error field. The iid bootstrap (217-219) destroys volatility clustering, so multi-day horizons understate the tail. There is no finiteness check on inputs before `std::sort` (135); a NaN breaks strict weak ordering. `k = floor(alpha*n)` (139) gives a slightly more extreme quantile, which is the safe side. Parametric ES `sd*phi/alpha - mu` and VaR `-(mu+z sd)` verified correct. No look-ahead is intrinsic; the window is chosen by the caller.

### C12-020 · P3 · CONFIRMED DEFECT (test) -- loader test does not pin values
- Evidence: test_charges_toml.cpp:10-13 says "one of these tests compares the two" (the file against test_cost.cpp's mirror). No such comparison exists in lines 1-253. The rates loaded from the file are checked only relationally: `apr01->equity_futures.stt > mar31->equity_futures.stt` (106-109). The priced option sell pins only turnover (375,000) and sum-of-parts (170-180).
- Trigger: a wrong but still-increasing rate in charges.toml passes every check. For example, post-April futures `stt_rate = 0.00025` (instead of 0.0005) is still > 0.0002.
- Regression test: assert nano rates futures 200,000 → 500,000 and options 1,000,000 → 1,500,000. Assert the 75 × 5,000-paise NIFTY option SELL on 2026-06-01 with ₹20 flat brokerage itemises to brokerage 2,000, STT 563 (562.5), exchange 131 (131.3625), SEBI 0 (0.375), IPFT 2 (1.875), stamp 0, GST 384 (2,133 × 0.18 = 383.94), total **3,080 paise**.

### C12-021 · P3 · CONFIRMED DEFECT (test) -- vacuous or unasserted checks
- Evidence: test_stress.cpp:80-83 is `if (nifty.ret.size() < 3000) { ... SKIP ...; return 0; }` and sits BEFORE both the stress windows and the whole Black-Litterman section (153-221). dataset/ is gitignored, so in a clean checkout ctest reports `risk_stress` green with zero BL checks run. test_var.cpp:115-119 skips the real-series section the same way, but the subadditivity counterexample runs first. test_hedge_book.cpp:169 is `check(worst_sector(*exposure).share != 0.0 || true, ...)`, which is always true. test_optimise.cpp:209-234 prints the out-of-sample verdict but asserts only `rows.size() >= 3`. The BL tests use a single view only, so C12-017 is invisible to them.
- Regression test: move the BL section ahead of the dataset guard and add the two-view case from C12-017. Make the SKIP path exit with ctest's skip code (e.g. `SKIP_RETURN_CODE`) rather than 0.

## 4 Lead revalidation (cost / charges)

- STT futures 0.0002 → 0.0005 sell, options 0.001 → 0.0015 sell on premium, from 2026-04-01: **present and correct in the file** (charges.toml:58,69 → 130,140). Side applied through `side_applies` (cost.hpp:318).
- Options turnover = premium: **by construction** (traded price × qty, cost.hpp:287). The `basis` field is informational only.
- Stamp duty buy-side only: **correct** via `stamp_side="buy"` (default Buy, cost.hpp:88).
- GST 18% on brokerage + exchange + SEBI + IPFT, not on STT or stamp: **correct** (cost.hpp:363-368).
- SEBI ₹10/crore → 1e-6: **correct** (charges_toml.cpp:195).
- IPFT: equity 0.000001 (₹10/cr) and options 0.000005 (₹50/cr premium) look right. Futures `ipft_nse = 0.0000005` (₹5/cr) could not be verified offline (open question). IPFT uses the NSE rate even for BSE trades.
- Effective-date boundary: inclusive both ends. valid_to is extended to 23:59:59.999999999 of the day (charges_toml.cpp:241). Overlap is refused and gaps become `NoSchedule` (safe). See C12-015 for the UTC/IST question.
- apply_bps (E002): **cost.hpp does not use it.** It uses 128-bit `mul_rate` (cost.hpp:176-219). The MSVC `_umul128/_udiv128` path checks `hi >= divisor` before dividing, which is correct. No file in risk/ calls `apply_bps` (grep), so E002 is not reachable from this group.
- DP charge 15.34 = 13 × 1.18, so GST is already included, and compute_cost correctly adds no GST on dp.
- Exercise/assignment STT is not modelled (acknowledged at cost.hpp:17-19).

Sizing lead: lots come from the spec `LotSize` (never a literal); rounding is floored (sizing.hpp:182, 210); a zero or negative stop is refused (198). "Quarter-Kelly ceiling" does not exist: the divisor is free (C12-006). Vol target is capped by max_leverage.

Limits lead: bitmask, all checks run, kill switch first (limits.hpp:251). There is no net-greeks check. Freeze qty, price band, tick, daily loss and staleness are present. It is **not connected before any send** (C12-001), and a daily-loss breach does not halt (C12-002).

Portfolio: cash-greek conversions verified dimensionally (delta ×S×0.01; gamma ×S²×1e-4; vega ×0.01; vanna ×S×1e-4; volga ×1e-4; charm per day ×S×0.01), and predicted_pnl has the ½ on gamma. The units of the incoming greeks (rupee- vs paise-priced) are the caller's responsibility. The test prices them in paise, which is consistent, but nothing enforces it.

Slippage: square-root law fitted through the origin, n−1 dof, correct. Kyle λ is not here (it is in book/flow.hpp). The `[implicit]` seeds in charges.toml are unread (C12-003).

lot_size_history.csv: read by nothing (C12-016).

## 5 Architecture facts
- risk/ is header-only (INTERFACE lib) plus `altair_charges_toml`, which is built only when toml++ is found (risk/CMakeLists.txt:24-35).
- Callers found by grep: `compute_cost` in strategies/basis.hpp, parity.hpp, calendar.hpp, backtest/engine.hpp:359, desktop/cost_panel.hpp and app tests. `load_charges_file` only in desktop/cost_panel.hpp:140 and desktop/quant_pages.hpp:3577. `size_position` only in desktop/quant_pages.hpp (display) and tests. `check_order` and `enforce_conservation` only in tests. `oms/kite_adapter.hpp:38` includes cost.hpp.
- Implemented but not connected: pre-trade limits, conservation, daily-loss halt, charges.toml safety keys.
- The order build path exists at `oms/kite_adapter.hpp:196` (`build_place_order`). That file includes risk/cost.hpp (38) and contains no `check_order` reference (grep only; oms/ is not in this group). The only `place(` grep hit under desktop/ (kite_link.hpp:76) is a string `replace`, a false positive.
- Where schedules come from: every hand-built `ChargeSchedule`/`BrokerageRule` found by grep is in tests (backtest/tests, app/tests, strategies/tests, risk/tests) or the desktop panels (`br.pct = rate_from(0.0003L)` at desktop/cost_panel.hpp:218 and quant_pages.hpp:3807). The Zerodha brokerage slab is therefore a literal in UI code, not read from `[broker.kite]` (C12-003). How the live/backtest executables obtain schedules was not traced beyond grep.
- `apply_bps` has no caller in risk/; cost.hpp uses its own 128-bit `mul_rate`. So Codex E002 (long double == double on MSVC) is not reachable from risk/.
- Conventions: money is int64 paise (`Notional`) at the cost and limits boundaries. Portfolio/hedge/neutralise/VaR carry money-like doubles internally; portfolio rounds to paise on exit, while hedge.hpp `VegaBucket::vega` and neutralise `BookExposure` stay double. Rates are int64 nano-fractions. Timestamps are UTC ns (tests subtract `kIstOffset` for IST dates). The loader assumes UTC dates (C12-015).
- Hot vs cold: cost.hpp, limits.hpp and `SlippageModel::add` are marked ALTAIR_HOT and are allocation- and exception-free. covariance/optimise/var/stress allocate (std::vector) and are cold analysis. charges_toml.cpp is cold and catches toml++ exceptions at its boundary. `load_document` is noexcept but calls `push_back`, so OOM there means terminate.

## 6 Test-suite observations
- test_charges_toml.cpp runs against the real config. Refusals tested: bad type, bad enum, overlap, missing dates, absent segment. NOT tested: absent key in a present table (C12-004), verified-string variants (C12-010), `uncovered_after_days` (C12-009), exact values (C12-020). It is built only when toml++ is found (risk/CMakeLists.txt:62-72), so the `default` preset's ctest never exercises the real charges file.
- test_cost.cpp: expectations are mostly relational (>0, ratios). Exact, independently derived values: mul_rate rounding (15/5/−15 × 0.1), 1e12 × 0.0000297 = 29,700,000, brokerage 30 and 2,000, futures STT 31,304 → 78,260 (a 5/2 check). The GST expectation (272-274) re-uses `detail::mul_rate`, so it is not independent. No BSE, currency, commodity, or exact delivery totals. The boundary test uses the exact IST-midnight instant.
- test_limits.cpp: good per-function coverage, and the integer vs double conservation demonstration is exact. Lines 362-367 assert that zero caps approve (C12-008 is intended behaviour). There is no oms or engine integration test (C12-001/002).
- test_sizing.cpp: property tests (floor to lots, min binds and is named, NoEdge when the 2σ lower bound is <0, leverage cap at vol 1e-9). The Kelly table is computed from `log_growth`. Degenerate refusals cover capital, price, lot, vol, risk_fraction, stop, divisor=0 and leverage=0, but NOT edge_sigmas ≤ 0, negative std error, or divisor in (0,1) (C12-006).
- test_slippage.cpp: known-eta synthetic data, a survivorship-bias experiment, and a decomposition with exact 3/3/4.8 bps. The degenerate test covers participation, vol, ordered, over-fill and decision price, but not zero arrival, fill or final price (C12-007). Misses in test 3 set `fill_price = arrival_price`, so the zero-price path is never exercised.
- test_portfolio.cpp: an independent check. The book is repriced with `black76` at S×1.01 and compared to cash greeks within 2%. Greeks are priced with S and K in paise, so vega and theta come out in paise, consistent with `line_exposure`. Nothing enforces that callers price in paise. `to_paise` half-rounding is untested (C12-011).
- test_hedge_book.cpp: relational checks (per-pair notional > 3× portfolio; |beta| < 0.1 after the hedge; rounding toward zero; tail-hedge rounding up). One tautological check (C12-021). Test 3 exercises strategies/vix_forecast.hpp, which is outside this group.
- test_optimise.cpp: data with a known true covariance. Checks that shrinkage lowers the condition number and Frobenius error. It feeds p=40, n=30 and relies on sample_covariance accepting p ≥ n (C12-012 doc mismatch). No convergence assertion, and the out-of-sample verdict is unasserted.
- test_stress.cpp / test_var.cpp: depend on dataset/ and skip with exit 0 (C12-021). The VaR subadditivity example is exact: k=25 of 1,000, VaR(A)=VaR(B)=−1, VaR(A+B)=49.5. Stress windows are built half-open (`to = index_of(next date)`), consistent with the code rather than the doc (C12-018). No NaN-input test.

## 7 Open questions
1. How do the running executables (app/, backtest engine) obtain `ChargeSchedule`/`BrokerageRule` at runtime? Grep shows `load_charges_file` called only from desktop/, with hand-built schedules everywhere else being in tests. The owners of app/backtest need to confirm.
2. Futures IPFT ₹5/cr vs ₹10/cr; contract-note STT rounding (rupee per scrip vs paisa per trade). Needs a circular or contract note; not verifiable offline.
3. Does NSE freeze qty mean "max allowed" or "first refused" quantity? limits.hpp:273 treats it as max allowed.
4. Does the oms exit ladder flatten through a path that bypasses limits?
