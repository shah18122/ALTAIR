# Status

Where ALTAIR stands, in one page. Updated 2026-10-27.

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
| **Auto strategies** | Arbitrage, OHL and option arbitrage can send real orders when their own switch is on and LIVE is armed. Every other model is paper-only. |
| **Workstation** | Ctrl+K palette for any page, model, view or layout (nothing that trades). Strategies, Overnight Gap, Pair Trading and Basis each carry Study trades, Paper & real trades and Saved runs tabs; double-click any trade for why it was entered and exited, the model's decisions around it, its fills and that day's forecasts. Daily findings (Models & research) puts one day's forecasts, trades and decisions on one screen and saves it to `data/logs/findings/<date>.md`. Work log (System & audit) lists status, research notes, runbooks, results, daily logs and `git log`. Layouts (Morning, Trading, Review, or your own) from the status bar. A green/red edge and a PAPER / LIVE / HALTED switch in the status bar, with the last halt reason; the switch asks before halting. |
| **Models engine** | Terminal → Models runs the live models on the stream and paper-trades them. Desk Trader (Terminal → Models) is a paper-only consensus of the models. Straddle IV>=RV NIFTY / BANKNIFTY (live/straddle.hpp) reads the ATM straddle's IV, realised vol and VIX on every tick and sells the nearest-expiry ATM straddle on paper once forecast IV (bid) >= forecast RV; no sale is logged with its reason. |
| **Recording and nightly retrain** | Scheduled tasks record the whole FYERS session (trades, OI, bid/ask, 5/50-level books; 3 expiries per index) whether or not the desktop is open, then store it, merge `dataset/` and FYERS into one snapshot at one cutoff, retrain with the leak check and push compact daily data to the private `altair-data` repo. One activity log: `data/logs/`. See [../ops/recorder.md](../ops/recorder.md). Not yet run on a live session. |
| **Research and backtests** | Forecast curriculum, data audit, demo-trading CLIs, NSE bhavcopy backtests 2015-2026. |

## What the research found

Honest results on real NSE data, after costs. Details in
[research/research-ideas.md](research/research-ideas.md) and
[../ops/demo-trading.md](../ops/demo-trading.md).

- **One edge:** selling the hedged NIFTY monthly at-the-money straddle
  (variance premium). About ₹5.12 lakh net over 134 months, t ≈ 3.5. It comes
  from the volatility premium, not from any forecast.
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
  only, paper; options get only the sell-only ask) loses on bars: the quoted
  half spread (~7 pts) is gone five minutes after a fill (markout about 0) and
  expenses (~7 pts a fill) take the rest. Smoke run on dataset/ only; rerun on
  FYERS 1-minute futures before reading anything into it.
- **HMM regime** (`altair_regimes`, models/hmm_regime.hpp): walk-forward
  P(stress), leak-checked. It now joins `altair_trader` as a daily input and
  scales Avellaneda-Stoikov's risk aversion; not yet tested as a straddle gate.

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
