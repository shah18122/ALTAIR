# Demo trading: band-fade short options and futures pairs

Paper trades only. Nothing here reaches a broker, and live order submission
stays disabled. Two research CLIs write CSVs under `data/verified/`, and the
desktop's **Strategies → Demo Trading** page runs them and shows the results.

| What | Code | CLI | Output |
|---|---|---|---|
| Band models selling options, 1 lot | `strategies/band_option_fade.hpp` | `altair_band_option_demo` | `data/verified/band_option_demo/{trades_<rule>,summary,meta}.csv` |
| Pairs hedged with futures | `strategies/pairs_futures.hpp` | `altair_pairs_futures` | `data/verified/pairs_futures/{trades,windows,summary,ratio}.csv` |
| Stock legs for the pairs | `ops/fetch_pairs.ps1` | `altair_fyers_history` | `data/pairs/<name>/1d/fyers.csv` (git-ignored: broker data) |

Both CLIs need tomlplusplus, which provides the charges schedule. Without it
they are not built.

## Expenses are refused until the charges are verified

Expenses come only from `config/charges.toml`, through `risk/cost.hpp`. That
file is UNVERIFIED, so by default both CLIs price gross P&L and leave the
expense and net columns empty, marked `refused`.

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
| `touch` | Wait for the first touch of an edge. Sell the option at the first strike past it. |
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
