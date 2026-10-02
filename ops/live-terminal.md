# Live terminal: streaming prices and live models

The Terminal opens on a live market watch that ticks. The live models run on the
same stream and paper-trade their signals. Nothing in this path can place an order.

```
FYERS data socket ──► altair_price_service ──► 127.0.0.1:7421 ──► desktop Terminal (Watch, chain, depth, T&S)
   (or --sim)            trades / quotes / book                 └► altair_live_engine ──► data/live/ ──► Terminal → Models
```

## Running it (Windows, market hours)

1. Log in to FYERS for the day: `altair_fyers_login`. This writes `data/fyers_session.json`; the token lasts one trading day.
2. Desktop → **Terminal → Watch → Start FYERS feed**. This runs
   `altair_price_service --fyers --go`, and the Terminal connects to it by itself.
3. **Terminal → Models → Start models**. This runs `altair_live_engine`.
   - Tick **Price UNVERIFIED expenses** to have expenses charged while
     `config/charges.toml` is unverified. Every figure is then marked UNVERIFIED.
   - Unticked, expenses are refused, as in every demo, and P&L is gross.
4. When the market is shut, **Start SIM feed** runs the same universe
   simulated. Every price is marked **SIM** on every screen, and nothing learned
   from it is evidence about the market.

From a shell, the same steps:

```
altair_price_service --fyers            # dry run: prints the universe it would stream
altair_price_service --fyers --go       # live, until 15:35 IST (--until HH:MM)
altair_price_service --sim --speed 30 --from 09:15   # a simulated session, 30x
altair_live_engine [--unverified-costs]  # the models; stops when the feed's clock passes 15:35
```

On a headless host, set `ALTAIR_FYERS_CLIENT_ID` and `ALTAIR_FYERS_ACCESS_TOKEN`
instead of `data/fyers_session.json`.

## What streams

The universe is built at feed start from `data/instruments.csv` (the Kite master)
and `config/universe_nifty50.csv`. It is written to `data/live/universe.csv`.

| Group | What | Depth |
|---|---|---|
| Indices | NIFTY 50, NIFTY BANK, INDIA VIX | — |
| Futures | the nearest two NIFTY and BANKNIFTY futures | 5 levels |
| NIFTY / BANKNIFTY options | nearest expiry, ATM ± 20 strikes, CE and PE | ATM ± 5 |
| NIFTY 50 | the 50 stocks | 5 levels |
| Stock futures | each stock's near and next future (stat-arb trades and rolls these) | — |

That is about 320 symbols, well under FYERS' 5,000 per socket.

- **Identity:** everything is published under the Kite instrument token the desktop
  already uses. FYERS tickers are the same trading symbol prefixed `NSE:`, and any
  ticker FYERS does not know is printed at start.
- **ATM:** fixed when the feed starts, from a FYERS quote (falling back to the last
  close in `dataset/`). ±20 strikes leaves room for a 2-3 % day. Beyond that,
  restart the feed.
- **Late joiners:** a terminal that connects mid-session gets the whole board
  replayed at once, so no illiquid strike sits blank until it next trades.

Each instrument carries three frame types:
- trade: LTP, last quantity, volume and OI;
- quote (`server/quote_payload.hpp`): OHLC, previous close, best bid/ask with
  sizes, ATP, total buy/sell, circuits and last-trade time;
- book: five levels a side.

## The Watch

- **Market watch:**
  - One row per instrument: LTP (with ▲/▼), change, % change, bid qty, bid, ask,
    ask qty, LTQ, volume, ATP, OI, open, high, low, previous close, LTT and the
    day's trade count.
  - A price that moves flashes green or red. Every frame updates its row as it
    arrives; painting is coalesced to 10 a second.
  - Grouped by the table above, searchable and sortable.
- **Depth & trades:**
  - Five levels a side with order counts and totals.
  - Time and sales: every trade of the selected instrument, newest first, stamped
    in IST to the millisecond.
- **Option chain:**
  - OI, volume, LTP, bid and ask per side.
  - IV and Δ inverted from the market mid with Black-76. The forward is the near
    future's mid, carried back to the options' expiry.
  - The ATM strike is marked and in-the-money halves are shaded.
- **Status line:** LIVE, REPLAY or SIM from the frames' own flags, plus frames and
  sequence gaps. The feed's own state (`data/live/feed_status.json`) sits alongside,
  so "why is nothing ticking" has an answer.

## The models (Terminal → Models)

Each is the research rule run forward (see `live/models.hpp` and
`app/live_direction.hpp`). Where live differs from the backtest, the model's row
says so.

| Model | What it does | Held |
|---|---|---|
| Vol band (HAR) | HAR forecast of today's σ, realised so far, the 80 % close band and the BANKNIFTY/NIFTY ratio z, recomputed every minute | trades nothing |
| Strangle 80% NIFTY / BANKNIFTY | at 09:20, sell the 80 % band's two edges (nearest streamed expiry, one lot each, at the bid) | intraday, out at 15:20 |
| … stop2x | the same, buying a leg back at the first 5-minute close where its premium doubled | intraday |
| Direction 10:15 AR(2), ARMA(1,1), Logistic, Ridge, GBDT, Vote | at 10:15, forecast 10:15-to-close; trade one NIFTY future lot only if (2q−1)·E\|r\| beats the round-trip cost | intraday |
| Pairs BANKNIFTY/NIFTY | 250-day spread; at 15:15, in at \|z\| ≥ 2, out at ≤ 0.5, stop at 4 | carried, rolled on expiry |
| Stat-arb NIFTY 50 | Avellaneda-Lee s-scores with today's return as the last day; at 15:15, open at ±1.25; one lot of each stock future | carried, rolled, out after 60 sessions |

**Direction models in more detail.** They run only the 10:15-to-close track. At one
and five minutes the curriculum measured break-even accuracy at 80-99 %, and no
model came close. The 10:15-to-close track is the one the research shortlisted.
- q is not the model's own stated probability. It comes from a walk-forward
  calibration of the model's past calls (three years of 5-minute history,
  re-walked at every start).
- E|r| is the history's sd of 10:15-to-close returns, scaled by today's HAR
  volatility, times √(2/π).
- When the gate stays shut, the row says by how many basis points.

**Paper fills.**
- A buy fills at the ask and a sell at the bid, from the quote in force. With no
  quote it fills at the last trade, and the fill says so.
- Every fill is charged through `risk/cost.hpp`.
- Every round trip records the model, gross, expenses, net, why it was opened and
  why it was closed.
- Open positions are marked tick by tick: a long at the bid, a short at the ask.

**Clock and schedule.**
- Everything runs on the feed's own time stamps, never the wall clock. That is why a
  simulated session at 30× decides exactly as a live one would.
- 15:15 rolls carried positions whose contract expires that day; 15:20 squares off
  intraday ones.
- No new position opens while the feed is stale (30 s without a frame).

## Files (`data/live/`, git-ignored)

| File | Written by | What |
|---|---|---|
| `universe.csv` | price service | what streams, and under which token |
| `feed_status.json` | price service | source, state, counts, last error |
| `engine_state.json` | engine, every second | each model's state, signal and reason, and the open positions |
| `paper/trades.csv` | engine | every round trip: date, model, symbol, side, qty, entry and exit, gross, expenses, net, why in, why out, LIVE/SIM, VERIFIED/UNVERIFIED/UNPRICED |
| `paper/fills.csv` | engine | every fill, at the bid or ask it dealt at |
| `paper/open_positions.csv` | engine | what is held; carried positions resume on the next run |

## Limits, stated

- **Stat-arb needs daily closes in `data/pairs/<symbol>/1d`.** Run
  `ops/fetch_universe.ps1 -Go`. Until then it abstains and says why.
- **History through yesterday matters.** The vol band, strangles, pairs and
  direction models read `dataset/` for HAR, the spread and the walk-forward. Keep
  it current (`altair_kite_update` or `altair_fyers_history`).
- **The engine must be running by 09:15** to catch the 09:20 strangles and the
  10:15 direction decisions. A late start is reported, not back-filled.
- **The strangle sells the nearest streamed expiry at the real bid.** The backtest
  sold a monthly contract at a modelled price, so the two are not the same trade.
- **SIM proves nothing about the market.** SIM P&L is the simulator's random walk;
  it shows the plumbing, not an edge.
