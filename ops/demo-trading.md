# Demo trading: band-fade short options and futures pairs

Paper trades only. Nothing here reaches a broker, and live order submission
stays disabled. Two research CLIs write CSVs under `data/verified/`, and the
desktop's **Strategies → Demo Trading** page runs them and shows the results.

| What | Code | CLI | Output |
|---|---|---|---|
| Band-fade short option, 1 lot | `strategies/band_option_fade.hpp` | `altair_band_option_demo` | `data/verified/band_option_demo/{trades,summary,meta}.csv` |
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

## Band-fade short option

**Rule.** Each band model forecasts the day's range at 09:20 from the 5-minute
bars:
- If a bar's high first reaches the upper edge, sell one lot of the call at
  the first strike at or above it.
- If a bar's low first reaches the lower edge, sell one lot of the put at the
  first strike at or below it.
- Each side trades at most once a day.
- Fill at the close of the touching bar; buy back at the 15:20 close.
- Slippage is 0.5 point each way.
- Every trade records the model that placed it.

**Premiums are synthetic.** The dataset has no option-chain history, so:
- Both legs use Black-76 on the forward at INDIA VIX (`analytics/greeks.hpp`).
- BANKNIFTY uses VIX scaled by its 20-day realised-vol ratio to NIFTY.
- Volatility is flat across strikes, with no skew.
- The contract is the current monthly, rolled on expiry day.

**Sizes.**
- Lot sizes are today's, from `config/lot_size_history.csv` (NIFTY 65,
  BANKNIFTY 30), applied to every historical day.
- Strike steps are from `data/instruments.csv` (50 and 100).

**Result** (2015-2026, 14 band models × NIFTY and BANKNIFTY, 27,095 trades):

| | NIFTY | BANKNIFTY |
|---|---|---|
| Touches that close back inside the edge by 15:20 | 42.7 % | 44.6 % |
| Win rate, by model | 39-43 % | 43-47 % |
| Gross per trade, by model | −₹491 to −₹633 | −₹483 to −₹779 |
| Net per trade (UNVERIFIED expenses, about ₹76) | −₹562 to −₹702 | −₹564 to −₹862 |

**Every band model loses before expenses.**
- A touch is a breakout more often than a reversal. That is consistent with
  intraday momentum (Gao, Han, Li & Zhou 2018; see `research/papers/index.md`).
- Big trend days dominate the losses: the median trade loses about ₹196 gross,
  the mean about ₹588.
- The least-bad bands are the time-of-day seasonal one on NIFTY (−₹562 a
  trade net) and the random forest on |r| on BANKNIFTY (−₹564). GARCH(1,1)
  loses −₹693 on NIFTY and −₹656 on BANKNIFTY.

The rule is rejected. Selling at the touch sells into the move.

What this does **not** test:
- The volatility risk premium (Carr & Wu 2009) needs real option prices.
- Delta-hedged selling.
- Fading on a different trigger.

```powershell
build\net\app\altair_band_option_demo.exe --instrument both                    # gross only
build\net\app\altair_band_option_demo.exe --instrument both --unverified-costs  # + expenses, stamped
```

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
