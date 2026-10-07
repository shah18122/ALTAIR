# Demo trading: band-fade short options and futures pairs

Paper trades only. Nothing here reaches a broker (real orders go only through
the Terminal's LIVE switch: ops/live-terminal.md). Two research CLIs write CSVs under `data/verified/`, and the
desktop's **Strategies → Demo Trading** page runs them and shows the results.

| What | Code | CLI | Output |
|---|---|---|---|
| Band models selling options, 1 lot | `strategies/band_option_fade.hpp` | `altair_band_option_demo` | `data/verified/band_option_demo/{trades_<rule>,summary,meta}.csv` |
| Pairs hedged with futures | `strategies/pairs_futures.hpp` | `altair_pairs_futures` | `data/verified/pairs_futures/{trades,windows,summary,ratio}.csv` |
| Stock legs for the pairs | `ops/fetch_pairs.ps1` | `altair_fyers_history` | `data/pairs/<name>/1d/fyers.csv` (git-ignored: broker data) |
| Volatility premium, delta hedged | `strategies/vol_premium.hpp`, `analytics/har_rv.hpp` | `altair_vol_premium` | `data/verified/vol_premium/{trades_<variant>,summary,signals,forecast_eval,meta}.csv` |
| Real option prices | `app/bhavcopy.hpp` | `ops/fetch_bhavcopy.ps1` | `data/bhavcopy/<YYYY>/*.csv` (git-ignored) |
| Cross-sectional stat-arb | `strategies/residual_reversion.hpp` | `altair_resid_reversion` | `data/verified/resid_reversion/{trades,daily}_<variant>.csv`, `summary.csv`, `meta.csv` |
| Stock universe | `config/universe_nifty50.csv` | `ops/fetch_universe.ps1` | `data/pairs/<symbol>/1d/fyers.csv` (git-ignored, shared with the pairs) |
| Order-book study | `book/depth_study.hpp` | `altair_depth_study` (record with `ops/record_depth.ps1`) | `data/verified/depth_study/summary.csv`; recordings in `data/ticks/` (git-ignored) |
| One model over every model's output | `models/meta_trader.hpp` | `altair_trader` (reads the curriculum's `data/verified/forecast_log/`) | `data/verified/altair_trader/{trades,days,weights}.csv`, `summary.txt` |

Every CLI here needs tomlplusplus, which provides the charges schedule.
Without it they are not built. On the desktop page, pick one in the Run
selector and press **Run**.

## Expenses are refused until the charges are verified

Expenses come only from `config/charges.toml`, through `risk/cost.hpp`. The
file is marked verified (2026-10-07, on the owner's instruction; the rates
were not re-checked against the circulars then). While a schedule is
UNVERIFIED, the CLIs price gross P&L only and leave the expense and net
columns empty, marked `refused`.

`--unverified-costs` (or the page's **Price UNVERIFIED expenses** box) prices
them anyway. Every expense and net figure is then stamped `UNVERIFIED`, and the
page shows the stamp in red. To make the figures real, check the rates against
the exchange and broker circulars and set `last_verified`.

Brokerage is ₹20 an order for options. For futures it is ₹20 or 0.03 %,
whichever is lower. A roll costs four orders.

## Band models selling options

Each band model forecasts at 09:20 an 80 % band for the session's close,
using the 5-minute bars. Each **rule** sells options on that band, one lot,
bought back by 15:20, and every trade records the model that placed it:

| Rule | What it does |
|---|---|
| `touch` (dropped) | Waited for the first touch of an edge and sold the option past it: it sold after the move, and was the one rule that lost. It is no longer run, and the page hides an older output's touch trades. |
| `strangle` | At 09:20, before any touch: sell the call at the first strike ≥ the upper edge and the put at the first ≤ the lower edge. |
| `strangle-stop2x` | The same, but a leg is bought back at the first 5-minute close where its premium has doubled. |
| `strangle-hedged` | The same, with a futures delta hedge in whole lots, reset every 5 minutes. |
| `expiry-day` | The strangle on monthly expiry days only, in the contract expiring that day (0DTE). |
| `expiry-day-stop2x` | The same, with the 2× stop. |

Pricing:
- **Premiums are synthetic.** Black-76 on the forward at INDIA VIX, flat
  across strikes with no skew. BANKNIFTY uses VIX scaled by its 20-day
  realised-vol ratio to NIFTY. The dataset has no option-chain history.
- **Sizes.** Lot sizes are today's (NIFTY 65, BANKNIFTY 30), applied to every
  historical day. Strike steps are 50 and 100.
- **Slippage** is 0.5 point each way. Futures hedge orders pay futures charges.
- **Decomposition.** Every trade's gross is split into time decay, the
  underlying's move, the IV change and slippage. The four add up to the gross.

### Why the touch rule loses

The band's 80 % is a statement made at 09:20 about the close. Once the price
has reached an edge, whether it ends back inside is close to a coin flip: for
a random walk exactly half, by the reflection principle. Measured, only
42.7 % (NIFTY) and 44.6 % (BANKNIFTY) of touches close back inside, so touches
lean towards breakouts (intraday momentum; see Gao et al. 2018 in
`research/papers/index.md`).

The option sold at the touch is near the money, with |delta| 0.47. One lot
moves like half a lot of futures, against the move. A few hours of a
16-day option earns little decay:

| Per trade, calendar clock | NIFTY | BANKNIFTY |
|---|---|---|
| Time decay | +₹62 | +₹93 |
| Underlying kept moving | −₹615 | −₹727 |
| IV change | +₹44 | +₹62 |
| Slippage | −₹65 | −₹30 |
| Gross | −₹574 | −₹602 |

The worst 10 % of trades (trend days) account for 136 % (NIFTY) and 173 %
(BANKNIFTY) of the total loss. Costs, about ₹75 a trade, are not the reason.
**The touch rule loses on every clock below.**

### The clock decides synthetic intraday results

How much an option decays between 09:20 and 15:20 depends on how time to
expiry is counted. Synthetic prices cannot settle it, so the CLI runs three
clocks (`--clock`):

| Clock | One session's share of a day's decay | Note |
|---|---|---|
| `calendar` | about ¼ | INDIA VIX's convention. Credits an intraday seller a quarter of a day's decay for most of the day's risk. |
| `trading` | 1 | Credits a whole day's decay, including the overnight gap risk, to someone who is flat overnight. |
| `variance` (default) | the intraday share of the day's variance | That share is measured over the trailing 250 sessions: NIFTY 0.44 to 0.80 by year, mean 0.63. |

Net per trade, with UNVERIFIED expenses of about ₹72–159 a trade:

| Rule | Calendar NIFTY | Calendar BANKNIFTY | **Variance NIFTY** | **Variance BANKNIFTY** | Trading NIFTY | Trading BANKNIFTY |
|---|---|---|---|---|---|---|
| touch | −₹646 | −₹682 | **−₹496** | **−₹435** | −₹382 | −₹287 |
| strangle | −₹514 | −₹617 | **+₹59** | **+₹224** | +₹495 | +₹754 |
| strangle-stop2x | −₹436 | −₹498 | **+₹98** | **+₹283** | +₹522 | +₹790 |
| strangle-hedged | −₹522 | −₹599 | **+₹61** | **+₹226** | +₹497 | +₹759 |
| expiry-day | −₹1,504 | −₹1,580 | **+₹155** | **+₹242** | +₹1,014 | +₹1,357 |
| expiry-day-stop2x | −₹342 | −₹324 | **+₹107** | **+₹507** | +₹840 | +₹1,337 |

On the calendar clock, expiry-day trades are mostly refused: the 0DTE
premiums are priced so cheap that they round to nothing.

### What the variance-clock result is, and is not

**Significance.** For `strangle-stop2x`, take each day's average across the
14 band models:
- NIFTY earns +₹96 a day, t = 3.4 over 2,784 days.
- BANKNIFTY earns +₹280, t = 7.1 over 2,797 days.

**Mostly the volatility premium, not the forecast.**
- A constant-width band (GBM) earns +₹68 on NIFTY (t = 2.3) and +₹256 on
  BANKNIFTY (t = 6.6).
- The 14 band models add only about ₹25–40 a day.
- The money comes from VIX pricing more intraday variance than the session
  delivers.

**Recent, not permanent.** Every year from 2021 to 2026 is positive: NIFTY
+₹185 to +₹461 a trade, BANKNIFTY +₹367 to +₹637. Before that it lost:
2015–2020 on NIFTY, and 2016–2018 on BANKNIFTY.

**The hedge changes almost nothing.** A one-lot strangle rarely reaches half
a lot of delta, so whole-lot hedging seldom trades and mostly adds charges.

**Unproven until real prices agree.** There are three open assumptions:
- the clock;
- no skew (real out-of-the-money puts are dearer than VIX, and calls cheaper);
- the bid-ask spread, assumed to be 0.5 point.

Daily NSE bhavcopy files give end-of-day prices only, so they test multi-day
selling, not intraday decay. Checking the clock needs intraday option
candles. FYERS serves these for contracts that are still listed, so a few
months of current-month strikes would calibrate it.

```powershell
build\net\app\altair_band_option_demo.exe                                   # all rules, variance clock, gross only
build\net\app\altair_band_option_demo.exe --unverified-costs                # + expenses, stamped
build\net\app\altair_band_option_demo.exe --clock calendar --unverified-costs
build\net\app\altair_band_option_demo.exe --rules strangle,strangle-stop2x --clock trading
```

Output in `data/verified/band_option_demo/`:
- `trades_<rule>.csv`: one row per trade (touch) or per strangle, with the
  decomposition columns.
- `summary.csv`: every rule × instrument × model.
- `meta.csv`: every assumption, including the clock.

## Volatility premium, delta hedged

This is how an options desk sells volatility: on its size, not on direction.
`strategies/vol_premium.hpp` works through each close:

1. Price the at-the-money straddle of the first monthly expiry at least 10
   sessions away.
2. Solve its implied vol.
3. Compare that with a forecast of realised vol over the same sessions:
   HAR (Corsi 2009, `analytics/har_rv.hpp`), fitted in logs on 5-minute
   realised variance plus the overnight gap, and refitted every day on only
   what was known that day.
4. When implied vol is rich, sell one lot, hedge its delta with whole futures
   lots reset at every close, and buy it back one session before expiry.

The variants run side by side, so the forecast's worth can be read against a
control:

| Variant | What it does |
|---|---|
| `always` | Sell every month, whatever the forecast says (the control). |
| `har-0`, `har-2`, `har-4` | Sell when implied vol beats the HAR forecast by 0, 2 or 4 vol points. |
| `har-2-stop2x` | `har-2`, bought back once the straddle costs 2× the credit. |
| `har-2-unhedged` | `har-2` without the futures hedge. |
| `har-2-vixveto` | `har-2`, skipping days the `Hedge` model calls INDIA VIX up. Needs `--vix-log`. |

**Synthetic prices, 2015–2026.** Black-76 at INDIA VIX, flat across strikes.
BANKNIFTY uses VIX × its 20-day realised-vol ratio, on NIFTY's expiry
calendar. One lot, slippage 1 point a fill, UNVERIFIED expenses:

| Variant | NIFTY net/trade | t | BANKNIFTY net/trade | t |
|---|---|---|---|---|
| always | +₹6,295 | 5.6 | +₹10,366 | 5.3 |
| har-0 | +₹6,597 | 6.1 | +₹10,534 | 5.4 |
| har-2 | +₹5,828 | 4.5 | +₹11,022 | 5.2 |
| har-4 | +₹7,539 | 3.7 | +₹11,879 | 4.5 |
| har-2-stop2x | +₹5,770 | 3.5 | +₹9,761 | 4.7 |
| har-2-unhedged | +₹6,945 | 2.6 | +₹7,333 | 1.8 |
| har-2-vixveto | +₹6,716 | 4.7 | +₹10,926 | 5.1 |

What the numbers say:

- **The premium is there.** INDIA VIX was above NIFTY's next-21-session
  realised vol on 85.7 % of days, by 2.4 vol points on average.
- **The forecast adds nothing.** Selling every month does as well as waiting
  for HAR's signal.
  - HAR does forecast realised vol better than its trailing value. Over 21
    sessions its RMSE is 7.2 vol points against 8.1 (NIFTY), and 9.0 against
    9.8 (BANKNIFTY).
  - But the premium is present almost every month, so there is little to
    filter.
  - The VIX-direction veto and the stop change little.
- **The hedge is the risk control.** Unhedged, the same trade's t-statistic
  roughly halves.
  - In March 2020, the straddle sold on 2020-02-27 at 17.6 % IV lost ₹191,655
    on the options as realised vol reached 83 %.
  - The futures hedge earned back ₹176,686, leaving −₹14,969 for the month.
- **The years.** Every year from 2015 to 2025 is positive on NIFTY (`always`).
  2026 so far is −₹23,000.

**Why this is not yet a result.** Synthetic at-the-money options are priced
at VIX. Real at-the-money IV usually sits below VIX, because VIX carries the
put skew. Synthetic pricing therefore overstates the premium a straddle
seller collects, possibly by most of the 2.4 points. The real test is the
same engine on NSE's own closes:

```powershell
powershell -ExecutionPolicy Bypass -File ops\fetch_bhavcopy.ps1 -From 2015-01-01           # dry run: lists the files
powershell -ExecutionPolicy Bypass -File ops\fetch_bhavcopy.ps1 -From 2015-01-01 -Go       # fetch (paced; ~2,900 days)
build\net\app\altair_vol_premium.exe --source bhavcopy --unverified-costs `
    --vix-log data\verified\forecast_log\india_vix_daily.csv
```

`app/bhavcopy.hpp` reads both NSE formats: the legacy `fo…bhav.csv` through
2024-07-05, and the UDiFF `BhavCopy_NSE_FO_…` files from 2024-07-08. It finds
columns by name, keeps monthly expiries within ±20 % of the future, and
prices only strikes that traded.

## Cross-sectional statistical arbitrage

This is the quant-fund version of pairs trading (Avellaneda & Lee 2010). It
trades every stock in a universe, each against the factors that move it, with
many small hedged bets at once. `strategies/residual_reversion.hpp` works
through each close, for every stock:

1. Regress its last 60 daily returns on the market's (NIFTY), and on the
   leave-one-out mean of its sector peers.
2. Model the cumulative residual as an Ornstein-Uhlenbeck process. Stocks
   whose residual reverts slower than about 30 days are not traded.
3. Score the s-score, centred on the day's cross-section.
4. Open a long below −1.25 and a short above +1.25. Close a long once s rises
   above −0.5, and a short once it falls below +0.75.

Each position is ₹10 lakh of stock futures against β × that in NIFTY futures
and the sector, with the betas frozen on the day it opens. A position decided
at close t earns from close t+1. The test changes every price after a day and
checks that nothing up to that day moves.

Two variants run: `market+sector` and `market-only`. Expenses on all three
legs, at entry and exit, go through risk/cost.hpp.

**Universe.** `config/universe_nifty50.csv` holds today's NIFTY 50 (2026-09-30)
in 13 sectors, rebuilt from the index changes since early 2025. Check it
against NSE's own constituent file. Backtesting today's members over a decade
is **survivorship bias**: they are the stocks that made it, and that flatters
the result. Historical membership is not transcribed.

**Status.** The engine is tested on synthetic data:
- Reverting residuals: 866 trades, 83 % won.
- Random-walk residuals: no edge, −1.2 against 80.5 of absolute P&L.
- A shared sector shock: the sector factor cuts the daily P&L swing from
  0.130 to 0.035.

**Real results need the FYERS data**, which this environment cannot reach:

```powershell
.\build\net\app\altair_fyers_login.exe
powershell -ExecutionPolicy Bypass -File ops\fetch_universe.ps1 -Go     # 50 stocks, daily, from 2015
build\net\app\altair_resid_reversion.exe --unverified-costs
```

## Order-book research (the HFT question)

Market makers and HFT desks earn from the spread, and from signals that look
seconds ahead in the order book. The best documented of these is **order flow
imbalance** (Cont, Kukanov & Stoikov 2014): bids added or lifted, against asks
added or hit. Over short intervals the mid moves linearly with it, with a
slope of about 1 / depth.

That is a *contemporaneous* fit. Whether the last few seconds' flow predicts
the *next* few is a much weaker claim, and it is the one a trader needs.

`book/depth_study.hpp` answers both from recorded FYERS depth, per symbol, on
a 1-second grid:
- the paper's contemporaneous regression, as a check on the data;
- a predictive regression using OFI (level 1 and 5 levels), level-1 imbalance
  and the microprice offset over the last 5 seconds. It is fitted on the
  first half of the recording and scored on the second: out-of-sample R², how
  often the sign is right, and how often the predicted move even exceeds half
  the spread.

The tests check it both ways. A synthetic market where past flow moves the
mid scores R² 0.28 out of sample, with the sign right 67 % of the time. A
market where it doesn't scores R² 0.002, with the sign right 50 % of the time.

**There is no depth history in the dataset, so it has to be recorded.**
`altair_fyers_ticker` already subscribes to 5-level depth. `--stamp` (new)
adds the receive time that depth messages lack:

```powershell
.\build\net\app\altair_fyers_login.exe
powershell -ExecutionPolicy Bypass -File ops\record_depth.ps1 -Go     # before 09:15; listens to 15:30
build\net\app\altair_depth_study.exe --in data\ticks                # after a week of sessions
```

**What to expect, honestly.**
- In the literature, the contemporaneous R² is high (about 65 % for US
  stocks), and predictive power at seconds ahead is small.
- A predicted move smaller than half the spread cannot be traded by crossing
  the spread. It can only inform passive quoting, which needs queue position
  and colocated latency that a FYERS retail connection does not have.
- The study reports this as its verdict. It is research into what the book
  says, not a strategy.

## Pairs hedged with futures

**Method.** `strategies/pairs_futures.hpp` follows the cards in
`research/papers/index.md`. It walks each pair forward:
- **Windows.** Estimate on a 250-day formation window, then trade the next 60
  days with everything frozen. Nothing is estimated on a day it trades.
- **Test.** Engle-Granger on log prices, at MacKinnon's 5 % critical value.
  A window is traded only if it is cointegrated, the hedge β is positive, and
  the Ornstein-Uhlenbeck half-life is 30 days or less.
- **Entry and exit.** Enter at |z| ≥ 2: long the cheap leg's future, short the
  rich one's. Exit at |z| ≤ 0.5, stop at |z| ≥ 4, and close after two
  half-lives or at the window end.
- **Sizing.** Whole lots. Leg A is one lot. Leg B is the nearest whole number
  of lots to β × A's notional ÷ B's lot notional, at least one. The hedge
  actually held is reported next to β.
- **Pricing.** Futures are priced as the underlying, so the basis is ignored.
  A position held across a monthly expiry pays a roll.

Pairs are listed in `config/pairs.csv`:
- NIFTY-BANKNIFTY
- CIPLA-SUNPHARMA
- DRREDDY-LUPIN
- HDFCBANK-ICICIBANK
- AXISBANK-KOTAKBANK
- SBIN-BANKBARODA
- TCS-INFY
- HCLTECH-WIPRO
- TATASTEEL-JSWSTEEL
- HINDALCO-VEDL
- BAJFINANCE-BAJAJFINSV
- ULTRACEMCO-GRASIM
- MARUTI-M&M

The stock legs need FYERS data:

```powershell
.\build\net\app\altair_fyers_login.exe
powershell -ExecutionPolicy Bypass -File ops\fetch_pairs.ps1          # dry run: prints the requests
powershell -ExecutionPolicy Bypass -File ops\fetch_pairs.ps1 -Go      # fetch daily candles from 2015
build\net\app\altair_pairs_futures.exe --unverified-costs
```

**NIFTY-BANKNIFTY result** (2000-2026):
- 8 of 107 windows were cointegrated.
- 15 trades, 60 % won.
- Gross ₹54,789; net ₹46,193 with UNVERIFIED expenses.

That is too few trades to call.

### The 2.6 ratio hypothesis

The hypothesis was that BANKNIFTY/NIFTY never exceeds 2.6. It is **rejected**.

The ratio:
- reached 2.661 on 2019-07-05;
- closed above 2.6 on 91 days, from 2019-03-26 to 2020-02-28;
- has not exceeded 2.44 since 2021;
- was last 2.404 (2026-09-24).

It drifted from 0.61 in 2000 to its 2019 high, so it is not stationary, and a
fixed cap on a drifting level is not a signal. The pairs model trades the
cointegration residual instead, re-estimated every window
(`ratio.csv` has the numbers).

## Every trade, its expenses, and the ratios

Every trades file carries gross, each expense head (brokerage, STT, exchange
transaction charge, SEBI fee, stamp duty, IPFT, GST), their total and net, so
net = gross − expenses can be checked row by row. Under the table the page
shows, for the trades on show: trips, win rate, profit factor, expectancy,
average win and loss, Sharpe and Sortino (daily net, annualised by √252) and
the maximum drawdown of cumulative daily net. Double-click a row: every
column, gross walked from its parts, expenses head by head, and net.

## altair_trader: one model over every model

`altair_trader` (`app/trader_main.cpp`, `models/meta_trader.hpp`) takes
every base model's out-of-sample call as its inputs and nothing else. That
covers 161 inputs across 7 daily tracks: NIFTY, BANKNIFTY, NIFTY futures and
INDIA VIX, each also with the VIX forecast fed in. Each input is the model's
conviction, 2·p_up − 1, or ±1 for a bare call.

**The model.** It is ridge regression of the next session's move, in bp,
with older days fading (half-life 250 sessions). Four penalties run side by
side, each scored on the calls it made before the outcome; the one with the
least recent error makes the call. It needs 250 outcomes before its first
call.

**Day by day, to the last close in the data:**
- yesterday's outcome is learned and the model refitted;
- today's call comes from weights that never saw today;
- it trades one lot of the index future:
  - it enters only when the expected move beats the round trip's expenses
    (both fills priced from `config/charges.toml`);
  - it holds while the call points the held way, and exits when the call
    turns;
  - a month end inside a holding pays a roll.

No threshold is tuned.

Walk-forward, 2016-02-08 to 2026-09-24, one lot, expenses priced. Net is in
₹ lakh:

| | Trips | Net | Longs | Shorts | Sharpe | Direction right | Buy and hold, net |
|---|---|---|---|---|---|---|---|
| NIFTY | 675 | 5.65 | +6.98 | −1.33 | 0.73 | 52.5 % | 9.74 |
| BANKNIFTY | 468 | 3.48 | +6.08 | −2.60 | 0.38 | 51.8 % | 11.65 |

It calls direction better than a coin and makes money after expenses, but
less than holding the future over a decade that rose. Its short calls lose.

Ways to improve it, each a run of the same walk-forward, not a tuning on
the result:
1. Predict the move over the trailing drift, so a short needs the model to
   expect less than the trend.
2. Add the vol-band and regime models' outputs as inputs, to learn *when*
   the calls work.
3. Use the hourly tracks for intraday entries.
4. Size by conviction instead of one lot.
5. Refit the base models daily; the curriculum refits them in doubling
   stages.

Run it after `altair_forecast_curriculum` has brought the logs up to the
latest close. It replays the whole walk-forward, which is deterministic and
takes about 10 s, so each new session is one more day traded. Limits:
- futures are priced on the index close, without the basis;
- a trip open at the end of the data is closed at the last close.
