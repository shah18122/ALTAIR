# ALTAIR roadmap

Version 2, 2026-11-13. It replaces version 1 (b358f3d) after Smit's answers:
keep every model (no archiving), trade frequently with purely model-based
rules, sell options only, use HAR volatility to drive the other models, make
OBI BANKNIFTY and pair trading work and prove them with backtests, get real
FYERS orders working, capital Rs 1 crore with 1 lot compulsory, colocation
later.

This document is a plan. An implementer with no other context should be able
to follow it phase by phase. Every number comes from a file in this repo,
named next to it.

---

## 1. What ALTAIR is today

ALTAIR is a C++/Qt trading workstation for NSE index derivatives: about
230,000 lines of C++ plus Python research. It runs on an EC2 c7i.2xlarge in
Mumbai, and FYERS is the only broker.

| Part | What exists | State |
|---|---|---|
| Broker and data | FYERS login with daily TOTP auto-login, history, live socket (5- and 50-level depth), order router, per-model PAPER/LIVE switches, caps, day-loss stop, kill switch | Paper works. **Real orders have never reached FYERS** (section 2) |
| Recorder | Every session: ticks, OI, bid/ask, books, 3 expiries per index, chains at 15:45. Data is pushed to `altair-data` | Works, but sessions are often incomplete |
| Nightly | 20:00 retrain of the models on a FYERS snapshot with a leak check (`ops/linux/nightly.py`, `ops/linux/train_all.py`) | Works |
| Models | About 33 forecasters x 20 tracks, 108 Model Atlas rows, about 25 live paper models (`app/live_engine_main.cpp` lines ~725-870) | Run. None is proven profitable yet |
| Desktop | 45 pages in 7 sidebar groups (`desktop/navigation_registry.hpp`) | Cluttered, too much text |
| Low latency | AF_XDP feed, DPDK order line, kept-alive TLS, isolated cores (`docs/LATENCY.md`) | Internal path in the tens of µs |
| Research | Pre-registered studies with dev data and a sealed holdout (`research/strategy`, `research/equity`, `research/cnn`, `research/signals`) | Honest; nearly every test FAILs after costs |

So far ALTAIR has traded no real money and has no validated strategy.

## 2. Where we are stuck

1. **Real orders do not work.** The server check on 2026-11-12
   (`/mnt/project-files/terminal-orders/diag-2026-11-12.txt`) found these causes:
   - LIVE was never armed (`data/live_trading.json` missing).
   - `altair_order_router` was not running.
   - The desktop running on the server was the 9 Nov build, so it lacked the newer switches.
   - The webhook tunnel was down: cloudflared is not installed on EC2, and the public callback returns HTTP 530.
   - FYERS still has to whitelist the server's static IP (a SEBI rule since 2026-04-01).
   - The order must come from an app of the new type.

   The PAPER/LIVE switch exists in code (`desktop/live_trading.hpp`, Terminal top strip and the Models table "Mode" button). It is not visible enough, and it was not in the build Smit was running.
2. **OBI BANKNIFTY takes no trades live.** The cause is not yet diagnosed
   (Phase 0).
3. **No model has passed a test after costs.** Section 3 explains why.
4. **The recorder loses sessions.** Only 2 of the first 5 recorded sessions
   are complete (`research/strategy/RESULTS.md`). Every model that learns
   from the recorder (OBI, straddle, depth features) is limited by this.
5. **CI has failed within seconds on every push since 2026-11-09.**

## 3. Why there is no profit yet, and what changes now

| Cause | Evidence | What this roadmap does about it |
|---|---|---|
| Index price direction did not beat "no change" at any timeframe, on spot or futures | `docs/MODEL_AUDIT.md`: best RMSE skill 0.00-0.13 %, direction 51-54 % | Keep forecasting futures and spot for both indices, but trade the parts that do forecast (size of move, range, volatility) and score direction only through option sales (3D) |
| Gross edges of 0-5 bp a trade against 6-10 bp of futures costs | `research/cnn/README.md` (90 combination tests), `research/strategy/RESULTS.md` | Express signals by **selling options**. Costs are charged on premium, not notional, so they are far smaller in rupees (below). Each signal is re-scored this way |
| Many models tested on two correlated indices find winners by luck | hundreds of model x track x rule tests | Keep every model training, but one scoreboard with the same costs, holdout and placebo decides what may go LIVE (Phase 2) |
| Speed strategies cannot win on a retail API | Avellaneda-Stoikov loses even at zero cost (`docs/STATUS.md`) | Aim for MFT (seconds to minutes), not HFT (section 4) |
| Bank pairs decayed: +39.5 bp a trade 2015-2019, **-35.5 bp 2024-2026** | `research/equity/FLOWS_RESULTS.md` | Rebuild pairs as intraday, with an index link and a vol gate, and backtest it before any paper or live run (3C) |
| The one forecastable thing, volatility, is not traded yet | HAR 16-35 % below a random walk from 1 min to 1 hour (`docs/MODEL_AUDIT.md`) | HAR becomes the gate and sizing input for **every** model (3E) and the core of S1 (3A) |
| The short-vol tail is uncapped | One trend day (6 Nov) cost Rs 16,000 a lot, about ten quiet days of profit (`research/strategy/RESULTS.md`) | Tail controls within sell-only (Phase 4) |

**On point 2 (option selling to cut costs).** It does cut costs. Option STT
and exchange fees are charged on the premium (`config/charges.toml`, schedule
for options), so one lot of an ATM BANKNIFTY option costs tens of rupees plus
the spread, while a futures round trip is about 6 bp of notional (around
Rs 1,000 a lot). The catch is that a sold option earns at most its premium,
and the spread (Rs 1-2 a unit on BANKNIFTY ATM) is a large share of a small
intraday move. Cheaper costs help a real signal. They do not create one. So
every signal is backtested as an option sale with these costs before it is
believed (Phase 2).

**On point 3 (the CNN combination).** It was built and run. All 90 tests lost
after costs on FYERS merged futures 2019-2026, with the best gross edge at
2-5 bp (`research/cnn/README.md`). It did find stable effects: the move since
the open continues to the close, NIFTY trend plus rising OI continues,
BANKNIFTY follows NIFTY, 5-15 minute moves revert, and basis jumps revert.
These are now inputs for the option-sale and gate tests (3D, 3E), not traded
alone.

## 4. Latency: MFT from our end, HFT later with colocation

| Hop | Measured | Can code cut it? |
|---|---|---|
| Inside our server, tick to decision | tens of µs (`docs/LATENCY.md`) | Already done |
| EC2 Mumbai to the FYERS edge, one round trip | 1.4-1.55 ms | No: it is the network path |
| FYERS OMS and RMS, then NSE | several ms, unknown | No |
| FYERS order rate limit | **10 requests a second** for place, modify and cancel (`/mnt/project-files/terminal-orders/fyers-docs/`) | No |
| FYERS exchange timestamps | 1 second resolution | No |

- **HFT from our end is not possible on FYERS.** HFT needs microsecond
  tick-to-trade and hundreds of orders a second. FYERS allows 10 a second
  over a millisecond internet path. Being in Mumbai is not colocation.
- **MFT is the right target, and we are already fast enough for it.** For
  holds of seconds to minutes, the only latency work still useful is
  measurement: per-hop numbers in `feed_status.json` and
  `data/live_orders/latency.json`, so slippage is known.
- **Colocation later** (Smit's plan) means a trading member or broker with
  co-lo racks and DMA, NSE colocation, the TBT multicast feed and an NNF
  gateway (`docs/NSE_NNF_TBT.md`). The trigger to start it is in Phase 7.
- **New latency work stays frozen** until then.

## 5. Vision and goal

**Vision.** ALTAIR is a model-driven MFT desk for NIFTY and BANKNIFTY. Every
model trains every night. Every signal is expressed by selling options (or by
futures for pairs) and gated and sized by the HAR volatility forecast. No
model trades real money until it passes one common scoreboard, and then it
trades 1 lot with hard limits.

**Goals (measurable).**

1. **Real orders work this week:** a 1-lot LIVE sell order placed from ALTAIR
   shows in FYERS and comes back through the order socket or webhook
   (Phase 0).
2. **Every model is on one scoreboard by end of December 2026:** the same
   costs, walk-forward, holdout and placebo, expressed as an option sale, with
   a status per model: FAILED, TESTING, PAPER-PASS or LIVE (Phase 2).
3. **First LIVE model:** the first one that passes paper (section 8) goes LIVE
   at 1 lot. The likeliest candidates are S1 (judged at 60 complete sessions,
   about late February 2027) and OBI BANKNIFTY option-sell (judged at 40
   complete recorded sessions).
4. **Frequency:** at least one passing model that trades several times a
   session (OBI, pairs or an intraday option-sale signal), so the result is
   judged on hundreds of trades, not dozens.
5. **Capital:** Rs 1 crore allotted, 1 lot per model compulsory. A model may
   add a lot only under the scaling rule in Phase 6.
6. **Kill rule per model:** a model that FAILS its pre-registered test stays
   training and visible as FAILED, but it never goes LIVE until a new,
   pre-registered version passes.

## 6. Every model stays (no archiving)

Smit's choice: nothing is archived. Instead every model gets a **status**
shown on the scoreboard and in the UI:

| Status | Meaning | Can trade |
|---|---|---|
| FAILED | Failed its pre-registered test after costs | Paper only |
| TESTING | Test running or not enough data yet | Paper only |
| PAPER-PASS | Passed backtest and forward paper test | Paper; LIVE on Smit's switch |
| LIVE | Switched LIVE by Smit, 1 lot | Yes |

Starting statuses, from the studies:

| Model | Start status | Source |
|---|---|---|
| Straddle IV>=RV BANKNIFTY / NIFTY (S1) | TESTING (forward, 60 sessions) | `research/strategy/README.md` |
| OBI A-D BANKNIFTY futures and option-sell, OBI quoter | TESTING (no tick-level test yet) | `docs/MODEL_AUDIT.md` |
| Pair trading / Bank pairs | FAILED in 2024-2026 | `research/equity/FLOWS_RESULTS.md` |
| Pairs BANKNIFTY/NIFTY, Stat-arb NIFTY 50, Basket alpha | FAILED (gross ~1-2 bp vs 6-10 bp) | `research/equity/RESULTS.md`, `research/signals/` |
| Direction 10:15-14:15, Desk trader, altair_trader, curriculum price tracks, CNN | FAILED as futures trades; re-test as option sales (3D) | `docs/MODEL_AUDIT.md`, `research/cnn/` |
| Strangle 80 %, vol band | TESTING | `research/strategy/RESULTS.md` |
| Diffusion, order flow | TESTING | `docs/STATUS.md` |
| Avellaneda-Stoikov | FAILED (adverse selection) | `docs/STATUS.md` |
| Cross-exchange arbitrage, OHL | TESTING (1 share a leg) | `live/arbitrage.hpp`, `live/threshold.hpp` |
| Option arbitrage, vol of vol | Never LIVE (they buy options) | `docs/MODEL_AUDIT.md` |
| Quantum and paper replications | FAILED (research only) | `research/README.md` |

Only real dead code is removed: `client/` (retired, unbuilt),
`scripts/legacy/`, and the parked macOS preset. Ask Smit before removing
even these.

## 7. UI plan: organised, short text, nothing lost

Rules for every screen: no paragraph visible by default (details behind
"? Help", `desktop/help_text.hpp`); numbers and status chips instead of
sentences; at most one table and one action strip per screen.

**Main sidebar: 5 groups, 11 screens.** Everything else moves into one
collapsed **Lab** group, so nothing is deleted.

| Group | Screen | Shows | Built from |
|---|---|---|---|
| Today | **Today** | Token, recorder, engine, router, PAPER/LIVE, today's trades and skips with reasons, P&L, limits used | Start here, Daily findings, Who Does What status |
| Trade | **Terminal** | Market watch, order windows, order book, positions, **one big PAPER / LIVE switch** | Terminal, Live Grid |
| Trade | **Option Chain** | Chain, IV, greeks, IV vs HAR RV, vol cone | Options, Volatility, Analytics |
| Trade | **Order Book** | BANKNIFTY ladder, OBI live, the 4 trade types, strikes | Order Book Imbalance, Microstructure |
| Models | **Scoreboard** | Every model: status, trades, net P&L, t, holdout, placebo, PAPER/LIVE switch | Models, Model Atlas, Strategies, Backtests |
| Models | **Trades** | Every trade with entry and exit reason, fills, costs; filter by model and day | Strategies' trade tabs, Audit Trail, Execution |
| Models | **Vol Forecasts** | HAR, VIX and IV forecasts and their accuracy, the gate state per model | Forecast (vol part) |
| Risk | **Risk** | Margin used of Rs 1 crore, lots per model, day loss vs limit, worst-case loss, kill switch | Cost, VaR, Portfolio, Sizing & Limits |
| System | **Data & Recorder** | Jobs, session completeness, data flow, server | Who Does What, Data Flow, Server |
| System | **Broker** | FYERS account, funds, link, static IP, webhook status | Broker Account, FYERS Primary, Brokers, Link Broker |
| System | **Work log** | Status, results, git log | Work log |

**Lab (collapsed by default):** Pair Trading, Value DCF, Aggregator, ML
Trees, Regimes, Forecast (price), Memory, Neural, Features, Overnight Gap,
Basis, Cointegration, Arbitrage, Threshold, Flagging, About. They are reached
from Lab or the Ctrl+K palette.

## 8. Roadmap

Rules for the implementer, every phase:

- Commit straight to `main` and push at once. Leave no pending PRs and no leftover branches.
- Never stop the recorder or the feed in market hours (09:00-15:47 IST weekdays).
- Sell options only; never an opening option buy, in any model or tool. Futures are allowed for pairs and hedges.
- Use FYERS data only, never the NSE bhavcopy. If FYERS lacks something, stop and report it. Drive ticks are a separate source, labelled `drive-ticks`.
- Log the reason for every trade and every skip.
- Keep UI text short.
- Ship 1 lot per model.
- After each phase: build, `ctest`, update `docs/STATUS.md`.

### Phase 0: Make real orders work and find why OBI does not trade (this week)

| Task | Files | Done when |
|---|---|---|
| Rebuild and restart the server desktop on current main after 15:47 | `ops/build.sh`, DCV session | Desktop build date is today |
| Run `altair_order_router` as a systemd service that starts with the session | `ops/linux/`, unit file | Router is running at 09:00 every day; Today screen shows it |
| One always-visible PAPER / LIVE switch in the status bar and the Terminal (typed `LIVE` confirm, expires 15:30) | `desktop/live_trading.hpp`, `desktop/main_window.hpp`, `desktop/terminal.hpp` | Smit sees it without opening any dialog |
| Smit: register the server's static IP (Elastic IP) in the FYERS API dashboard and use a new-type app | FYERS dashboard (Smit) | FYERS shows the IP |
| Smit: `sudo ops/linux/fyers_tunnel.sh`, then add the webhook URL from `ops/linux/fyers_webhook_url.sh` in the FYERS dashboard | server, FYERS dashboard | Public callback returns 200, `data/fyers_webhook_events.jsonl` gets a post |
| First live test: sell 1 lot of a far-OTM NIFTY weekly option LIMIT at a price that will not fill, then cancel | router, `data/live_orders/journal.jsonl` | Order appears in FYERS order book, cancel confirmed by socket or webhook |
| OBI BANKNIFTY no-trade diagnosis: read the engine's decision logs for a full session, count skips by reason (OBI5 not held, target below costs, no 50-level slot, no fresh quote) | `live/obi_trader.hpp`, `live/obi_signals.hpp`, `data/live/` | A table of skip reasons for one session in `docs/research/obi_no_trade_<date>.md` and the fix for the top reason |
| Fix CI | `.github/workflows/ci.yml` | CI green on main |

### Phase 1: Make every recorded session count (start now)

| Task | Files | Done when |
|---|---|---|
| Completeness check after 15:47: BANKNIFTY monthly and NIFTY weekly ATM ±5 chains quoted every minute 09:15-15:30; BANKNIFTY future depth and prints; spot, futures, VIX | `ops/linux/record_session.py` or `nightly.py`; `altair-data/sessions.csv` | One row per day: complete yes/no, gaps |
| Alert in the status bar when the recorder or chain saver stops in market hours | `desktop/feed_status.hpp`, `ops/linux/` | A planted stop alerts within 2 minutes |
| Recorder survives reboot and token refresh | systemd units | Reboot at 08:45 still records from 09:15 |

### Phase 2: One scoreboard for every model (by end of December)

This replaces archiving: every model keeps training and is judged the same way.

| Task | Files | Done when |
|---|---|---|
| Common backtest harness: walk-forward, signal strictly before entry, next-tick or next-bar fills at bid/ask, costs from `config/charges.toml`, random-sign placebo, one-bar delay check, planted leak refused, dev then sealed holdout | reuse `research/strategy/study.py` and `backtest/validation.hpp`; new `research/scoreboard/` | Every model in section 6 has a row |
| **Option-sale expression** of any direction signal: bullish = sell a put, bearish = sell a call, at fixed strikes (ATM, 1 and 2 out), held to the signal's horizon; priced from recorded FYERS chains where they exist, else from a model price marked as such | `research/scoreboard/`, `app/option_chain.hpp` | Each direction model shows futures net and option-sale net side by side |
| Nightly trains every model (Smit's point 7) and writes the scoreboard | `ops/linux/train_all.py` | Scoreboard in `altair-data/scoreboard/<day>.csv` each night |
| Status chips per model from the scoreboard | `desktop/live_models.hpp`, Scoreboard screen | Status matches the CSV |
| Promotion rule (pre-registered in `research/scoreboard/RULES.md` before any run): net P&L per trade > 0 with t >= 2 on the holdout, positive in each third, above the placebo's 95th percentile, then a forward paper test of at least 100 trades or 40 sessions with the same sign | `research/scoreboard/RULES.md` | Committed before the first scoreboard run |

### Phase 3: Strategy tracks (parallel, each pre-registered)

Each track writes its hypothesis and pass bar into its own `HYPOTHESES.md`
**before** it runs. Each shows Smit a backtest report: a trade list, an
equity curve, net P&L after costs, t, results by year or third, placebo, and
holdout.

**3A. S1 short straddle (BANKNIFTY monthly, NIFTY weekly).** It keeps its
fixed forward test (`research/strategy/README.md`). Add the "sell every
day" shadow and a nightly scorer from `research/strategy/option_sessions.py`.
Pre-register the variants before their data exists: 13:00 entry, skip trend
days (move since open plus rising OI), skip event days.
Done when the 60-session verdict is committed.

**3B. OBI BANKNIFTY, tick by tick, tested on recorded option sessions.**
- Data, two layers:
  1. **Drive ticks, about 820 sessions of NIFTY and BANKNIFTY futures trades (2020, 2022, 2023; date, time, price, qty, OI).** These are enough to pre-train the trade-flow part now: signed volume (tick rule), trade arrival rate, trade size, OI change, HAR vol, on 1-60 s horizons. They have no bid/ask, no book and no options, so they cannot train the order-book imbalance itself or price an option fill. The earlier tick study found the next-trade "edge" is bid-ask bounce (`docs/MODEL_AUDIT.md` §3), so the pre-registered target is the mid-free move over 5-60 s, never the next trade.
  2. **Recorder FYERS tapes** (BANKNIFTY future 5/50-level depth, prints, ATM ±5 option quotes). These fine-tune the model with the book features and are the only test set for the option-sell leg.
- Model: label every book update with the future's mid move over 1, 5, 30 and 60 s. Train on the OBI5/OFI/microprice/queue features (`models/depth_features.hpp`, `book/flow.hpp`) with ridge and gradient-boosted trees, walk-forward by session. The CNN can be added as one more input.
- Test: replay each held-out recorded session through the engine (`altair_live_engine --replay`). Trade the option-sell leg on the recorded option bid/ask with the queue fill model (`book/queue_fill.hpp`) and option costs.
- Data limit: the trade-flow model can be trained and tested on Drive ticks now (dev 2020 + 2022, holdout 2023, as in `research/strategy/`). The book features and the option-sell verdict need at least 40 complete recorded sessions (train on the first 25, test on the next 15, then roll). Until then, report the recorded-session results as "too few to judge".
- Done when the 40-session report is committed and the live model uses the trained thresholds instead of the hand-set ones.

**3C. Pair trading, rebuilt for frequent trades.**
- Why the old version failed: daily bank pairs decayed to -35.5 bp in 2024-2026, and 5-minute basket lead was 1-2 bp gross.
- New design to test, each piece from the literature and Smit's papers in `research/papers/`:
  - Intraday pairs on 1-minute FYERS stock and index futures bars.
  - Hedge ratio by Kalman filter, refit daily (`analytics/kalman.hpp`).
  - Entry on a z-score of the spread, with the half-life cut to under 60 minutes.
  - Index-constituent link: the bank basket against the BANKNIFTY future, and NIFTY against BANKNIFTY. This uses the Hayashi-Yoshida lead-lag that the diffusion engine already measures.
  - HAR gate: trade only when predicted spread vol covers costs by 3x.
  - Cointegration re-test each morning on the past 20 sessions only.
- Execution: 1 lot of each leg's future. Costs are 10 bp a round trip per leg (stock futures).
- Data: FYERS 1-minute history for stocks and indices, and the recorder for stock futures. If FYERS lacks a series, stop and report it.
- Pass bar as in Phase 2, and at least 5 trades a session on average.
- Done when the backtest report is in `research/pairs_intraday/RESULTS.md` and the live Pair trading model shows its new status.

**3D. Index forecasting for futures and spot, NIFTY and BANKNIFTY.**
- Keep every forecaster training on both futures and spot at 1m, 5m, 15m, 60m and 1d (Smit's point 1).
- Report three targets separately: direction, size of the move, and the day's range.
- Trade only what passes Phase 2. Size and range forecasts go to the strike choice of option sales (sell beyond the forecast range). Direction goes to the put/call choice.
- Done when every forecaster shows option-sale net on the scoreboard.

**3E. HAR volatility drives every model (Smit's point 6).**
- One HAR service per index (`models/vol_forecast.hpp`) publishes, on every bar, the forecast RV for the next 1, 5, 30 and 60 minutes and the rest of the session.
- Every model reads it as: (a) a gate, skipping when forecast vol cannot cover costs, or when it is too high for option selling; (b) sizing for the future, while live stays at 1 lot now; (c) strike distance for option sales.
- Each model's scoreboard row shows gated against ungated, to prove the gate helps.
- Done when every live model logs the HAR value it used for each decision.

### Phase 4: Tail control for option selling

| Task | Files | Done when |
|---|---|---|
| Stop order at the broker placed with every sale (SL-L/SL-M per FYERS order types) | `oms/`, `app/live_real_orders.hpp` | Stop visible in FYERS right after entry |
| Day loss limit, per-model loss limit, max margin of the Rs 1 crore | `risk/`, `config/altair.toml` | Gate refuses a breach; Risk screen shows use |
| Event blackout from `config/scheduled_events.csv` | `live/straddle.hpp`, other sellers | No sale on listed event days |
| Futures delta hedge only where lots allow | `live/straddle.hpp` | Logged; off at 1 lot BANKNIFTY |
| Stress replay of 6 Nov and 11 Nov with every control on | scorer | Loss with and without controls reported |

### Phase 5: UI organise (after Phase 2 so the Scoreboard has data)

| Task | Files | Done when |
|---|---|---|
| 5 groups, 11 screens, collapsed Lab group | `desktop/navigation_registry.hpp`, `workspace_pages.hpp`, `shell_pages.hpp`, `quant_pages.hpp`, `main_window.hpp` | Sidebar matches section 7 |
| Old page ids redirect, saved layouts still open | `navigation_registry.hpp` aliases, `saved_layouts.hpp` | Test covers every old id |
| Text audit: no visible paragraph | `help_text.hpp`, every page | Screens show numbers and chips only |
| Before/after screenshots | `/mnt/project-files/ui-tidy/` | Saved |

### Phase 6: Go LIVE, model by model

| Step | Done when |
|---|---|
| Prerequisites: Phase 0 done, real brokerage from a FYERS contract note in `config/charges.toml`, Smit's per-trade and per-day loss limits in config | All in place |
| A PAPER-PASS model goes LIVE at 1 lot on Smit's switch, for 20 sessions | Live slippage and P&L inside the paper model's range |
| Scaling: +1 lot per 20 sessions while live drawdown stays under half its limit, never above the margin cap | Rule in config, logged |
| A LIVE model that breaks its loss limit or falls outside its paper range goes back to PAPER automatically | Tested by a planted breach |

### Phase 7: Colocation (later, Smit's plan)

Start only when a LIVE model's own fills show its edge disappears faster
than our path, measured by markouts at 10 ms, 100 ms and 1 s. Then cost a
broker with co-lo racks and DMA, the TBT feed and an NNF gateway
(`docs/NSE_NNF_TBT.md`). Until then, no new latency work.

## 9. Compute: when a GPU helps

| Work | Data | Compute | When |
|---|---|---|---|
| Ridge, gradient-boosted trees, HAR, the curriculum CNN (`TrainableTcn<4,8,3,3,16>`) | Drive ticks and bars, FYERS bars | CPU. The full Drive tick run (39 M ticks, walk-forward) took about 20 minutes on 4 cores | Now, in the cloud or on the server nightly |
| Deep sequence models (LSTM, TCN, transformer) on raw trade sequences, walk-forward with a monthly refit and many epochs | Drive ticks, about 820 sessions (Smit, 2026-11-13: equal to well over 100 recorded sessions for trade flow) | **One rented cloud GPU** (A10 or L4 class, by the hour). On CPU each walk-forward run would take days | **Now**, as part of 3B layer 1 |
| Deep models on the 50-level order book | Recorder tapes only (Drive has no book) | GPU | After about 100 complete recorded sessions |

Smit's PC (`S:\tick_data`) holds the raw Drive tick zips. Stage them to the
cloud GPU run, or run there if the PC has a GPU. Deep models get the same
walk-forward, costs, placebo and holdout as everything else. The earlier CNN
and tick studies found no direction edge after costs, so a bigger model has
to beat them on the 2023 holdout before it counts.

## 10. Decisions only Smit can make

1. **Option history.** FYERS has no expired options and the NSE file is
   banned, so option strategies wait months for recorded data. A licensed
   historical option data vendor would allow backtests now. Your rule stands
   unless you change it.
2. **Sell-only and the tail.** Without long wings, a short option's loss has
   no hard cap. Phase 4 is the best possible within your rule.
3. **Loss limits.** Per trade, per model, per day, and the drawdown that ends
   a live test (Phase 6).
4. **Phase 0 broker steps:** the static IP whitelist, the new-type app, and
   running the tunnel script. These need your FYERS login and sudo on the
   server.
