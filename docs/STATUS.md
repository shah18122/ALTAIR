# Status

Where ALTAIR stands, in one page. Updated 2026-11-02.

The detailed history lives in [archive/SMIT.txt](archive/SMIT.txt) (plan and
status to 2026-09-28) and [archive/change_by_codex.txt](archive/change_by_codex.txt)
(change ledger to the same date). Later changes are in the pull request
descriptions and `git log`.

## What works

| Area | State |
|---|---|
| **Market data** | FYERS is the primary feed: OAuth login, history, live socket (HSM, 5-level depth, 50-level TBT depth for the scrip on screen). Kite code is kept as a fallback. A FYERS token lasts one trading day. |
| **Terminal** | GETS-style market watch, option chain with IV/Δ and vanna-volga, F1/F2 order windows, order/trade books, net position (Alt+F6), Simple / Advanced view. One theme for every page (desktop/theme.hpp), a collapsible sidebar (Ctrl+B), quick-start tiles on Start here, and a news bar of free RSS headlines across the top (fetched by `altair_news`, Ctrl+Shift+N to hide). |
| **Paper trading** | Default everywhere. Fills at the live bid/ask with expenses, durable ledger, replayable sessions. |
| **Real orders** | Off by default. Only through the Terminal's PAPER / LIVE switch (typed `LIVE`) and `altair_order_router`, with per-strategy switches, caps, day-loss stop and a kill switch. NSE/BSE only: MCX and currency are paper-only. See [../ops/live-terminal.md](../ops/live-terminal.md) and [../ops/go-live.md](../ops/go-live.md). |
| **Auto strategies** | Arbitrage and OHL can send real orders when their own switch is on and LIVE is armed. Every other model is paper-only, option arbitrage included (it buys options; removed from the router 2026-11-02). |
| **Workstation** | Ctrl+K palette for any page, model, view or layout (nothing that trades). Strategies, Overnight Gap, Pair Trading and Basis each carry Study trades, Paper & real trades and Saved runs tabs; double-click any trade for why it was entered and exited, the model's decisions around it, its fills and that day's forecasts. Daily findings (Models & research) puts one day's forecasts, trades and decisions on one screen and saves it to `data/logs/findings/<date>.md`. Work log (System & audit) lists status, research notes, runbooks, results, daily logs and `git log`. Layouts (Morning, Trading, Review, or your own) from the status bar. A green/red edge and a PAPER / LIVE / HALTED switch in the status bar, with the last halt reason; the switch asks before halting. |
| **Models engine** | Terminal → Models runs the live models on the stream and paper-trades them. Desk Trader (Terminal → Models) is a paper-only consensus of the models. Straddle IV>=RV NIFTY / BANKNIFTY (live/straddle.hpp) reads the ATM straddle's IV, realised vol and VIX on every tick and sells the nearest-expiry ATM straddle on paper once forecast IV (bid) >= forecast RV; no sale is logged with its reason. Since 2026-11-02 both sides are a per-session rate on trading time (IV restated over the sessions left, holidays from config/nse_holidays.csv; RV = HAR for the rest of the session plus today's intraday realised, no overnight gap); on expiry day it sells the NEXT expiry, with a "0DTE" control that still sells today's; it reads FYERS history (newest snapshot, else data/store/fyers) and does not sell on history that is stale or not FYERS; every entry logs its 4 % gap loss (`--straddle-max-gap-loss` caps it). Judged by [research/paper-evaluation.md](research/paper-evaluation.md). OBI A–D futures / option-sell BANKNIFTY and NYKAA (live/obi_trader.hpp, signals live/obi_signals.hpp, engine book/flow.hpp, one shared engine per contract in the "OBI engine" hub) read every book (5 or 50 levels) and trade print and trade four types: A bid pressure, B ask pressure (OBI5 held past ±0.30 with the prints agreeing), C absorption (reversal), D withdrawal (breakout); each trades the near future long or short, or sells a put (long view) or call (short view) one strike out of the money, paper only, the only filter being that the target pays for costs, spread and a safety margin; every entry and exit with its numbers. Stocks come from config/obi_underlyings.csv. The **Order Book Imbalance** page (sidebar, under Terminal; desktop/obi_page.hpp) has BANKNIFTY / NYKAA buttons, the ladder (up to 50 levels), the four trade types live on the future, the 11 strikes (ATM ±5, call and put, each with its own engine and OBI), a paper PUNCH panel (the active type's trade armed, BUY/SELL at the touch with F2/F3, options sold only, fills logged to data/live/punch/obi_punch.csv with the readings), and tabs for "How a trade happens", the formulas worked live, and the order-book tape. No charts. Its "50-level book" box, or Market watch's right-click "Watch for OBI", puts a symbol on the 50-level slots (`data/live/obi_watch.csv`); FYERS gives 5 symbols a connection and 3 connections on Prime, so `--depth50 15` covers BANKNIFTY's future and the strikes nearest the money first. The thresholds are a starting framework, not validated. |
| **Recording and nightly retrain** | Scheduled tasks record the whole FYERS session (trades, OI, bid/ask, 5/50-level books; 3 expiries per index) whether or not the desktop is open, then store it, merge `dataset/` and FYERS into one snapshot at one cutoff, retrain with the leak check and push compact daily data to the private `altair-data` repo. One activity log: `data/logs/`. See [../ops/recorder.md](../ops/recorder.md). Not yet run on a live session. |
| **Research and backtests** | Forecast curriculum, data audit, demo-trading CLIs. The NSE bhavcopy backtests 2015-2026 still run (opt-in) but are not evidence: FYERS data only since 2026-10-26. |

## What the research found

Honest results on real NSE data, after costs. Details in
[research/research-ideas.md](research/research-ideas.md) and
[../ops/demo-trading.md](../ops/demo-trading.md).

- **One candidate edge, not yet validated on FYERS:** selling the hedged
  NIFTY monthly at-the-money straddle (variance premium). The ₹5.12 lakh /
  t ≈ 3.5 figure came from NSE bhavcopy, which is no longer used: FYERS data
  only (2026-10-26). FYERS does not serve expired option contracts, so option
  results can only be rebuilt from chains ALTAIR records itself from 2026-10
  on. Paper only until then. `altair_fyers_chain` saves the chains daily at
  15:45 (`ops/fyers_chain.ps1`, registered by `ops/install_tasks.ps1`) into
  `data/store/fyers_chain/`, and the option research tools read them by
  default; the bhavcopy is opt-in only.
  Validation checklist and model limits:
  [research/vol-premium-validation.md](research/vol-premium-validation.md).
- **No forecaster beats "no change" after costs**, on any track or timeframe
  (last retrain on data to 2026-09-24).
- `altair_trader` (one model over every model) loses to buy and hold on NIFTY.
- `altair_trader combos` is the meta-trader: every model's output is a
  variable, 12 sell-only strategies (futures, straddles, strangles, futures
  delta hedges, stops, hold to expiry) at 8 times of day, rules of up to two
  conditions searched walk-forward with a placebo, a deflated Sharpe, a holdout
  and a leak check. FYERS data only (`ops/fetch_fyers_meta.ps1`). Option legs
  are model-priced until recorded chains exist, and reported apart. Paper only.
- The paper replications in [`research/models/`](../research/models) and
  [`research/execution/`](../research/execution) showed no tradable edge. They
  are kept as research, outside the production folders.
- **Avellaneda-Stoikov quoting** (`altair_as_mm`, NIFTY/BANKNIFTY futures
  only, paper; options get only the sell-only ask) loses on every series
  tried, 2015-2026 1-minute NIFTY and BANKNIFTY included, even with zero
  costs: fills are adversely selected (five-minute markout below zero). Local
  dataset/ only so far; the FYERS 1-minute futures rerun is pending
  ([research/avellaneda-stoikov-hmm-results.md](research/avellaneda-stoikov-hmm-results.md)).
- **HMM regime** (`altair_regimes`, models/hmm_regime.hpp): walk-forward
  P(stress), leak-checked. It now joins `altair_trader` as a daily input and
  scales Avellaneda-Stoikov's risk aversion; not yet tested as a straddle gate.

## NSE review fixes (2026-11-02)

From the read-only review of 2026-10-29. Done on main:

- Live straddles: FYERS-only, fresh history (they had been reading `dataset/`,
  which ended 2026-09-25); IV and RV on one clock and one window (calendar-time
  IV read ~1.5x rich on Mondays and ~2.4x on expiry day); the expiry-day rule;
  the gap-loss reading and cap; the price service streams the next expiry on
  expiry day.
- Sell-only rule: option arbitrage removed from the real-order router (it buys
  options); `altair_vol_of_vol` marked research only.
- `config/scheduled_events.csv` filled to April 2027 (RBI MPC, US FOMC, Union
  Budget); `config/nse_holidays.csv` added (the rest of 2026).
- One risk-free rate, `analytics/inr_rate.hpp` (91-day T-bill, 5.57 %, was a
  typed 6.5 %); the desktop chain's forward now nets the dividend yield.
- `altair_vol_premium --weeklies` reads the recorded NIFTY weekly chains.
- `altair_trader combos`: BANKNIFTY's monthly expiry was the last Wednesday
  from March to December 2024.
- How the paper models are judged, written down first:
  [research/paper-evaluation.md](research/paper-evaluation.md).

Still needed from Smit (not code): run the recorder and the 15:45 chain task
every session so `altair-data` fills; a FYERS contract note for the brokerage
in `config/charges.toml` (the STT rates there match Budget 2026); the per-trade
loss limit for real money; broker-side stop orders before any LIVE straddle.

## What is open

- Real intraday option prices, to test the 09:20 band strangles properly.
- Real fills to refit the execution models (VRC weights, impact).
- The macOS build (parked until its compile errors are fixed).
- Phase 8 items that need entitled broker accounts or licensed data:
  [../ops/phase8-acceptance.md](../ops/phase8-acceptance.md).

## Where things are

| Folder | What |
|---|---|
| `core/ instruments/ feed/ book/ broker/ oms/ risk/` | Engine: units, contract specs, feeds, order book, broker I/O, order handling, costs |
| `analytics/ features/ models/ strategies/ backtest/ flagging/` | Pricing, features, the model zoo, strategies, backtests, scorecards |
| `live/ server/ desktop/ client/` | Live terminal engine, price service, Qt desktop |
| `app/` | Every executable (`altair_*`) |
| `research/` | Paper pipeline, and the paper replications that showed no edge |
| `threshold_strategy/` | Your threshold strategy workbooks and TradingView exports (input data) |
| `dataset/` `data/` `config/` | Market data (tracked), runtime data (mostly git-ignored), settings |
| `ops/` | Runbooks |
| `docs/` | This page, research notes and references, the archive |
| `scripts/legacy/` | Old GUI-only build scripts, kept for reference (use `build.bat net`) |
