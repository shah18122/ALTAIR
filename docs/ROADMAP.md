# ALTAIR roadmap

Written 2026-11-13 from the repo at main `11048c9` and the studies in
`docs/` and `research/`. It plans only. An implementer with no other context
should be able to follow it phase by phase. Every number below comes from a
file in this repo, named next to it.

---

## 1. What ALTAIR is today (summary)

ALTAIR is a C++/Qt trading workstation for NSE index derivatives, about
230,000 lines of C++ plus Python research, run on an EC2 c7i.2xlarge in
Mumbai with FYERS as the only broker.

| Part | What exists | State |
|---|---|---|
| Broker and data | FYERS login with daily TOTP auto-login, history, live socket (5-level and 50-level depth), order router with PAPER/LIVE switch, caps, day-loss stop, kill switch | Works |
| Recorder | Records every session (ticks, OI, bid/ask, books, 3 expiries per index, option chains at 15:45), stores it, pushes compact data to `altair-data` | Works, but sessions are often incomplete (see 2) |
| Nightly | 20:00 retrain of every model on a FYERS snapshot with a leak check (`ops/linux/nightly.py`, `ops/linux/train_all.py`) | Works |
| Models | About 33 forecasters x 20 tracks (curriculum), 108 rows in the Model Atlas, about 25 live paper models in `app/live_engine_main.cpp` | Runs, almost none has an edge |
| Desktop | 45 pages in 7 sidebar groups (`desktop/navigation_registry.hpp`) | Too many, mostly for models that failed |
| Low latency | AF_XDP feed, DPDK order line, kept-alive TLS, isolated cores, host tuning (`docs/LATENCY.md`, `net/`, `ops/linux/`) | Built; internal path is tens of µs |
| Research | Pre-registered studies with dev and sealed holdout (`research/strategy`, `research/equity`, `research/cnn`, `research/signals`) | Honest and complete; nearly all FAIL |

Real money traded so far: none. Validated strategies: none.

## 2. Where we are stuck

1. **No goal was ever written down**, so every idea was built: forecasting
   papers, quantum models, OBI, diffusion, market making, pairs, arbitrage,
   DPDK. The infrastructure is far ahead of the evidence.
2. **The one live lead cannot be tested yet.** The short straddle (sell when
   IV >= forecast RV) needs option history. FYERS serves no expired option
   contracts, and the NSE file is banned (`docs/DATA_POLICY.md`), so the only
   test is forward, on chains the recorder saves itself. That takes 60 full
   sessions (`research/strategy/README.md`, about mid-February 2027).
3. **The recorder loses sessions.** Of the first five recorded sessions
   (4-11 Nov) only 6 Nov and 11 Nov are complete; 10 Nov has no chain
   (`research/strategy/RESULTS.md`). Every incomplete session pushes the
   verdict further out.
4. **CI fails within seconds on every push since 2026-11-09**, so nothing
   checks main automatically.

## 3. Why there is no profit after so many models

Plainly: the models were aimed at something that cannot be predicted, and the
one thing that can be predicted has not been traded yet.

1. **Index price direction is not forecastable here.** No model beats "no
   change" on NIFTY or BANKNIFTY at 1m, 5m, 15m, 1h or daily
   (`docs/MODEL_AUDIT.md`: best RMSE skill 0.00 % to +0.13 %, direction
   51-54 %). The CNN, its 29-feature combination (90 tests), six
   pre-registered futures rules and seven stock rules all fail after costs in
   dev and in the 2023 holdout (`research/cnn/README.md`,
   `research/strategy/RESULTS.md`, `research/equity/RESULTS.md`).
2. **Costs are bigger than the edges.** The best gross edges found are 0-5 bp
   a trade; a futures round trip costs 6-10 bp. Real effects exist (trend
   since the open continues, basis jumps revert) but each is worth 1-5 bp.
3. **More models is not more edge.** Testing hundreds of model x track x rule
   combinations on two correlated indices guarantees some look good by luck.
   The pre-registered studies were the right fix, and they say "no".
4. **Speed cannot be the edge on a retail API.** The microstructure models
   (OBI A-D, OBI quoter, diffusion, Avellaneda-Stoikov quoting, cross-exchange
   arbitrage) need to act before others. Our orders go over the internet to
   FYERS and then to NSE: milliseconds, against colocated firms at
   microseconds. Avellaneda-Stoikov loses even at zero cost because its fills
   are adversely selected (`docs/STATUS.md`).
5. **The one positive stock lead decayed.** Bank pairs earned +39.5 bp a trade
   in 2015-2019 and **-35.5 bp in 2024-2026** (`research/equity/FLOWS_RESULTS.md`).
   The live "Pair trading" paper model trades exactly that losing rule.
6. **What does work has not been turned into a trade yet.** Volatility is
   forecastable: HAR is 16-35 % below a random walk from 1 minute to 1 hour,
   stable in every third of the data (`docs/MODEL_AUDIT.md`). The strategy
   that is paid for being right about volatility is selling options, and that
   one is waiting on data (2 above).
7. **Short volatility has a fat left tail, and nothing caps it yet.** On the
   recorded sessions, quiet days earn Rs 300-1,800 a lot and one trend day
   (6 Nov) cost Rs 16,000 a lot (`research/strategy/RESULTS.md`). With
   "never buy options", the tail can only be limited by size, stops, timing,
   skipping days and futures hedges. A 1-lot BANKNIFTY straddle cannot be
   delta-hedged at all (the futures lot equals the option lot).

## 4. Latency: what is physically possible

| Hop | Measured | Can code cut it? |
|---|---|---|
| Inside our server, tick in to decision | tens of µs (`docs/LATENCY.md`) | Yes, already done |
| EC2 Mumbai to FYERS edge, one round trip | 1.4-1.55 ms | No: it is the network path |
| FYERS OMS and risk checks, then NSE | unknown, several ms | No: it belongs to FYERS |
| FYERS exchange timestamps | 1 second resolution | No |

- **Nanoseconds end to end is not possible on this setup.** Being in Mumbai
  is not colocation: NSE's colo racks are inside NSE's data centre, and our
  path goes over the internet through FYERS, which offers no colo, no direct
  market access and no exchange multicast feed to retail clients.
- **Real colo means**: a trading member (or a broker that offers co-lo racks
  with DMA), NSE colocation space, the TBT multicast feed, an NNF/direct order
  gateway, exchange approval of the algo, and a budget of lakhs a month. That
  is a different business, not a code change (`docs/NSE_NNF_TBT.md`).
- **It does not pay now.** S1 (section 5) trades once a session. A
  millisecond, even a second, changes nothing for it. Latency only matters to
  a strategy whose edge disappears within milliseconds, and none of ours has
  an edge to protect.

**Decision in this roadmap: latency work is frozen.** Keep what is built, keep
the kernel path as the safe default, and add nothing until a validated
strategy shows, in its own fills, that its edge decays faster than our path.

## 5. Vision and goal

**Vision.** ALTAIR is a one-person volatility-selling desk for NIFTY weekly
and BANKNIFTY monthly options: it forecasts volatility, sells premium only
when the market pays more than the forecast, limits the tail, and logs why
every trade happened. Its edge is its own recorded option-chain history and a
strict go/kill process, not speed and not price prediction.

**Goal (measurable).**

1. **By the 60th complete recorded session** (target late February 2027):
   S1, the BANKNIFTY IV >= forecast-RV short straddle, gets a written PASS or
   FAIL under the rules already fixed in `research/strategy/README.md` (mean
   net P&L per lot > 0 with t >= 2, positive in each third, beats the
   sell-every-day shadow, beats 95 % of random day sets, worst session within
   the drawdown provision).
2. **If PASS:** LIVE with 1 lot and hard limits for 20 sessions; live fills
   within the paper model's slippage assumption; then scale by the sizing
   rule in Phase 6.
3. **12-month target from first live trade:** positive net P&L after all
   costs, max drawdown within the limit Smit sets, every trade with its
   reason logged.
4. **Kill rule:** if S1 FAILS and the pre-registered variants (Phase 3) also
   FAIL by session 120, stop strategy development. ALTAIR then stays a
   recording and analysis tool, and no money is traded. This is a real
   possible outcome and should be accepted as one.

One strategy at a time. A second strategy is studied only after S1 has a
verdict (Phase 6).

## 6. What is irrelevant and should go

"Archive" = move to `research/archive/` (or `docs/archive/`), drop from the
build's default targets, the live engine, the nightly and the desktop. Git
keeps everything, so nothing is lost. "Delete" only for code that is already
dead.

### Live paper models (`app/live_engine_main.cpp`, about lines 725-870)

| Model | Verdict | Action |
|---|---|---|
| Straddle IV>=RV BANKNIFTY | S1, the only lead | **Keep** |
| Straddle IV>=RV NIFTY (weekly) | Same rule, NIFTY weekly | **Keep** |
| "Sell every day" shadow of each | Needed by the S1 pass test | **Keep** (add if missing) |
| Straddle IV>=RV timed BANKNIFTY | Pre-registered variant only if written in Phase 3 | Keep if registered, else archive |
| Strangle 80 % (x4), books straddle rules | Not safer intraday (`research/strategy/RESULTS.md`) | Archive |
| Vol band (HAR) | Overlaps S1 | Archive |
| Direction 10:15-14:15, Desk trader, altair_trader (daily) | No direction edge | Archive |
| Pairs BANKNIFTY/NIFTY, Stat-arb NIFTY 50, Bank pairs / Pair trading | Pairs edge gone in 2024-2026 (-35.5 bp banks) | Archive |
| Basket alpha | ~1 bp vs 6 bp cost (`research/signals/`) | Archive |
| OBI engine, OBI A-D, OBI quoter, OBI stocks, Order flow, OBI punch | Speed-dependent, unvalidated, retail path too slow | Archive |
| Diffusion engine and traders | Speed-dependent, unvalidated | Archive |
| Cross-exchange arbitrage, OHL | Speed-dependent; can send real orders | Archive (removes a real-order path) |
| Option arbitrage | Buys options: breaks the sell-only rule | Delete from engine, archive code |

### Tools, models and code

| Area | Files | Action |
|---|---|---|
| Direction forecasting zoo | `models/` except the vol models: arima_hybrid, aro, attention, deep_rl, regime_rl, gbdt, mlp, recurrent, trainable_cnn, trainable_recurrent, meta_learners, meta_trader, combo_search, aggregator, sentiment, dcf, named_factors, markov, markov_eval, event_study, spot_forecast, band_curriculum | Archive. Keep `models/vol_forecast.hpp`, HAR (`analytics/har_rv.hpp`), EWMA, GARCH, VIX, tick_forecast's vol part, forecast metrics |
| Curriculum price tracks | `altair_forecast_curriculum` | Keep only vol, VIX and IV tracks |
| Nightly jobs | `ops/linux/train_all.py`: trader, combos, magnitude, as_mm, band_demo, tune, obi_select | Remove. Keep vol_forecast, vol_premium, regimes (as a candidate S1 input), paper_report |
| Market making | `strategies/as_market_maker.hpp`, `app/as_mm_main.cpp` | Archive |
| Other strategies | basis, calendar, band_option_fade, threshold (`threshold_strategy/`, `live/threshold.hpp`), overnight gap, cointegration | Archive |
| Vol of vol | `app/vol_of_vol_main.cpp` (buys straddles) | Archive |
| Quantum, paper replications | `research/models/`, `app/quantum_smile_main.cpp` | Already in research; drop from default build |
| Retired web client | `client/` | Delete |
| Legacy scripts | `scripts/legacy/` | Delete |
| macOS build | parked | Drop from CMake presets and docs |
| News ticker | `desktop/news/`, `altair_news` | Delete (not part of the goal) |
| Low latency | `net/`, `ops/linux/nic_swap.sh`, `dpdk_*`, `feed_bypass.sh` | **Freeze**, do not delete. Kernel path stays default |

### Keep (the core)

FYERS login, recorder and chain saver, `altair-data` push, nightly vol
retrain, data-policy locks, `live/straddle.hpp`, paper OMS, order router
(kernel line), risk gate, kill switch, trade-reason logging, terminal,
option chain, charges (`config/charges.toml`).

## 7. UI plan: from 45 pages to 11

Rule for every screen: one line of text per idea, numbers over sentences,
details behind the existing "? Help" (`desktop/help_text.hpp`).

### Final screen list

| Group | Screen | Shows | Built from |
|---|---|---|---|
| Today | **Today** | Token, recorder, engine, today's S1 decision (SELL / SKIP and why), IV vs forecast RV, P&L, limits used | Start here, Daily findings, status from Who Does What |
| Trade | **Terminal** | Market watch, F1/F2 order windows, order book, positions | Terminal, Live Grid |
| Trade | **Option Chain** | Chain with IV, greeks, IV vs RV, vol cone | Options, Volatility, Analytics |
| Trade | **Chart** | Price chart | Chart |
| Straddle | **Straddle Desk** | S1 and shadow live, decision log, forward-test scoreboard (sessions done / 60, P&L, t, each third, placebo) | Models (S1 rows), Strategies |
| Straddle | **Trades** | Every trade with entry and exit reason, fills, costs; filter by model and day | Strategies' trade tabs, Backtests, Audit Trail, Execution |
| Straddle | **Vol Forecasts** | HAR / VIX / IV forecast accuracy by day | Forecast (vol part only) |
| Risk | **Risk** | Margin, lots, day loss vs limit, worst-case gap loss, kill switch | Cost, Risk VaR, Portfolio, Sizing & Limits |
| System | **Data & Recorder** | Jobs and whether they ran, session completeness, data flow, server | Who Does What, Data Flow, Server |
| System | **Broker** | FYERS account, funds, link status, static IP | Broker Account, FYERS Primary, Brokers, Link Broker |
| System | **Research** | Model Atlas as a read-only verdict list, work log | Model Atlas, Work log |

### Removed pages

Microstructure, Order Book Imbalance, Value DCF, Aggregator, ML Trees,
Regimes, Forecast (price part), Memory, Neural, Features, Pair Trading,
Overnight Gap, Basis, Cointegration, Arbitrage, Demo Trading (already
retired), Threshold, Flagging, About (moves to the Ctrl+K palette), news bar.

## 8. Roadmap

Rules for the implementer, every phase:

- Commit straight to `main` and push at once. No pending PRs, no leftover
  branches.
- Never stop the recorder or the feed during market hours (09:00-15:47 IST
  weekdays). Server restarts after 15:47.
- Sell only. Never an opening option buy, in any model or tool.
- FYERS data only. Never the NSE bhavcopy. If FYERS lacks something, stop and
  report it.
- Every trade logs its reason.
- UI text short.
- After each phase: build, `ctest`, update `docs/STATUS.md`.

### Phase 0: Set the goal and fix CI (1-2 days)

| Task | Files | Done when |
|---|---|---|
| Put the vision, goal and kill rule (section 5) at the top of `docs/STATUS.md`; link this roadmap | `docs/STATUS.md` | Text on main |
| Find why CI fails in seconds since 2026-11-09 (a job that fails that fast is usually a workflow syntax error, a missing runner, or Actions billing/disabled) and fix it | `.github/workflows/ci.yml` | CI green on a main push |
| Freeze notice in `docs/LATENCY.md`: no new latency work; reopen condition from section 4 | `docs/LATENCY.md` | Text on main |

### Phase 1: Make every session count (start now, highest priority)

The S1 verdict is limited by complete sessions, not by code.

| Task | Files | Done when |
|---|---|---|
| Session completeness check: after 15:47, mark the day complete only if the BANKNIFTY monthly and NIFTY weekly ATM ±5 chains have quotes every minute 09:15-15:30, and spot, futures and VIX are present | new check in `ops/linux/record_session.py` or `ops/linux/nightly.py`; result in `altair-data` | A `sessions.csv` with one row per day: complete yes/no, gaps |
| Alert when the recorder or chain saver is down during market hours (desktop status bar + log) | `desktop/feed_status.hpp`, `ops/linux/` | A planted stop raises the alert within 2 minutes |
| Recorder starts by itself after a reboot and after a token refresh, before 09:15 | `ops/linux/`, systemd units | A reboot test at 08:45 still records from 09:15 |

### Phase 2: Cut (1-2 weeks, after Phase 1)

| Task | Files | Done when |
|---|---|---|
| Remove archived models from the live engine | `app/live_engine_main.cpp`, `live/models.hpp`, `live/strategy_keys.hpp` | Engine starts with only S1 models and shadows; option arbitrage, arbitrage and OHL gone from the router path |
| Move archived code to `research/archive/`, out of default build targets | `models/`, `strategies/`, `live/`, `app/`, the CMakeLists files, `desktop/atlas_data.hpp` paths, `cmake/AtlasAudit.cmake` | Default build and `ctest` pass; archived code still builds with `-DALTAIR_ARCHIVE=ON` |
| Trim the nightly to vol jobs | `ops/linux/train_all.py`, `ops/linux/nightly.py`, curriculum tracks | Nightly runs only vol_forecast, vol curriculum tracks, vol_premium, regimes, paper_report; runtime drops |
| Delete dead code | `client/`, `scripts/legacy/`, news, macOS preset | Removed; build passes |
| Update docs | `docs/STATUS.md`, `docs/DATA_FLOW.md`, `README.md` | They describe only what remains |

### Phase 3: Make the S1 forward test automatic (parallel with Phase 2)

| Task | Files | Done when |
|---|---|---|
| Pre-register any S1 variant **before** its data exists: candidates are 13:00 entry, skip when move-since-open and OI both trend (from `research/cnn/README.md`), skip on `config/scheduled_events.csv` days. Write rules and pass bar, commit, then run | `research/strategy/HYPOTHESES.md` (new section S1a, S1b ...) | Commit dated before the first session it is judged on |
| Add the "sell every day" shadow to the engine if missing | `live/straddle.hpp`, `app/live_engine_main.cpp` | Shadow trades every complete session |
| Nightly scorer: replays every complete session (S1, shadow, variants), writes the scoreboard | turn `research/strategy/option_sessions.py` into a nightly job; output in `altair-data/research/s1/` | Table grows by one row each complete session, unattended |
| Compare paper fills with the recorded bid/ask to measure slippage | scorer above | Slippage per lot reported |
| Straddle Desk screen reads the scoreboard | `desktop/` (new `straddle_desk.hpp`) | Shows sessions n/60, net P&L, t, thirds, placebo, shadow |
| At 60 complete sessions: write the verdict, no tuning before | `research/strategy/RESULTS.md` | PASS or FAIL committed |

### Phase 4: Tail control for short vol (parallel with Phase 3)

Without buying options, the tail is limited only by these. All paper first.

| Task | Files | Done when |
|---|---|---|
| Per-straddle stop as a broker-side stop order placed at entry (not only in our process) | `oms/`, `app/live_real_orders.hpp`, FYERS order types | Paper and a FYERS sandbox order confirm the stop rests at the broker |
| Day loss limit and max lots from margin and drawdown | `risk/`, `config/altair.toml` | Gate refuses a sale that breaches either |
| Event blackout | `config/scheduled_events.csv`, `live/straddle.hpp` | No sale on listed event days |
| Futures delta hedge only when straddle lots make it possible (BANKNIFTY needs several lots per futures lot) | `live/straddle.hpp` | Hedge rule documented and logged; off at 1 lot |
| Stress replay of 6 Nov and 11 Nov with every control on | scorer | Loss on those days reported with and without controls |

### Phase 5: Tidy UI (2 weeks, after Phase 2)

| Task | Files | Done when |
|---|---|---|
| New groups and pages per section 7 | `desktop/navigation_registry.hpp`, `workspace_pages.hpp`, `shell_pages.hpp`, `quant_pages.hpp`, `main_window.hpp` | Sidebar shows 5 groups, 11 screens |
| Old page ids redirect to their new home so saved layouts still open | `navigation_registry.hpp` (alias table, `kRetiredNavigationGroups`), `saved_layouts.hpp` | Every old id resolves; test covers it |
| Today screen | `start_page.hpp`, `daily_findings_page.hpp` | One screen answers "did S1 sell today, why, and how are we doing" |
| Text audit | `help_text.hpp`, every kept page | No paragraph visible by default |
| Screenshots before and after | `/mnt/project-files/ui-tidy/` | Saved |

### Phase 6: Go live, or stop (after the S1 verdict)

**If PASS:**

| Step | Done when |
|---|---|
| Prerequisites: static IP registered with FYERS (SEBI rule), real brokerage in `config/charges.toml` from a contract note, Smit's per-trade and per-day loss limits in config | All three in place |
| LIVE 1 lot BANKNIFTY, S1 only, all Phase 4 controls on, for 20 sessions | Live P&L and slippage within the paper model's range |
| Scale: add 1 lot per 20 sessions while live drawdown stays under half the limit; never above the margin cap | Sizing rule in config, logged |
| NIFTY weekly S1 judged on its own 60 sessions before it goes live | Separate verdict |

**If FAIL:** run the pre-registered variants to session 120. If they fail too,
apply the kill rule (section 5): stop strategy work, keep recording.

**A second strategy** is considered only after S1 has a verdict, one at a
time, pre-registered, and only from what the data supports (volatility, sell
side). Not before.

## 9. Decisions only Smit can make

These change results more than any code. The roadmap works without them,
but slower or with more risk.

1. **Option history.** FYERS has no expired options and the NSE file is
   banned, so S1 waits months for its own data. A licensed historical option
   data vendor would allow a backtest now. Your rule stands unless you change
   it.
2. **Sell-only and the tail.** Never buying options means a short straddle's
   loss has no hard cap. Buying far wings (an iron fly) would cap it at a
   known cost. Your rule stands unless you change it; Phase 4 is the best
   that can be done within it.
3. **Capital and limits.** How much capital S1 may use, the per-trade and
   per-day loss limits, and the max drawdown that ends the live test.
4. **Colocation.** Only worth discussing after a strategy has a measured
   edge that decays in milliseconds. Today: no.
