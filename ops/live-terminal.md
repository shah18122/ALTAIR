# Live terminal: streaming prices, paper orders and live models

The Terminal opens on a live market watch that ticks, and it starts the live feed
by itself. Orders are **paper**: + and − open a GETS-style order window, and fills
come from the live bid and ask. The live models run on the same stream and
paper-trade their own signals. Nothing in this path can place a real order.

```
FYERS socket ─┐
              ├─► altair_price_service --live ──► 127.0.0.1:7421 ──► Terminal (watch, chain, depth, T&S, paper orders)
Kite ticker ──┘    (or --sim [--date D])                          └► altair_live_engine ──► data/live/ ──► Terminal → Models
```

## Running it (Windows, market hours)

1. **Log in for the day** on **Brokers**, to either broker:
   - **Log in to FYERS**, or
   - **Log in to Kite**.

   If a login can't start, the reason appears under the button. For example,
   "app credentials are not saved yet" puts the cursor in the form that fixes
   it, and the login link is shown with a **Copy** button.
2. **Open Terminal.** If nothing is streaming after a few seconds, the Terminal
   runs `altair_price_service --live --go` itself:
   - FYERS is used when its session is good today, otherwise Kite.
   - If FYERS refuses the socket, the feed carries on from Kite.

   **Start live feed** does the same by hand; **Live on open** turns the
   automatic start off.
3. **Terminal → Models → Start models** runs `altair_live_engine`.
   - Tick **Price UNVERIFIED expenses** to have expenses charged while
     `config/charges.toml` is unverified. Every figure is then marked UNVERIFIED.
     The paper orders follow the same tick box.
   - Unticked, expenses are refused, as in every demo, and P&L is gross.
4. **When the market is shut,** pick a day, a start time and a speed next to
   **Start SIM**. The previous closes are the session before that day. When
   `dataset/` holds that day's 1-minute bars, NIFTY, BANKNIFTY and INDIA VIX
   follow them minute by minute: a Brownian bridge lands on every real close.
   Every price is marked **SIM**, and nothing learned from it is evidence about
   the market.

From a shell, the same steps:

```
altair_price_service --live            # dry run: says which broker it would use, writes the universe
altair_price_service --live --go       # live, FYERS else Kite, until 15:35 IST (--until HH:MM)
altair_price_service --fyers --go      # FYERS only        (--kite --go: Kite only)
altair_price_service --sim --date 2026-09-24 --from 10:00 --speed 30   # that day, simulated, 30x
altair_live_engine [--unverified-costs] [--date 2026-09-24]   # stops when the feed's clock passes 15:35
altair_live_engine --latency-ms 250 --quote-age-s 10 --entry-timeout-s 60 \
                   --max-positions 80 --max-gross 1e8 --max-daily-loss 2e5   # the defaults, spelt out
```

Credentials:
- **FYERS:** `data/fyers_session.json`, or on a headless host
  `ALTAIR_FYERS_CLIENT_ID` and `ALTAIR_FYERS_ACCESS_TOKEN`.
- **Kite:** the API key from the OS vault (Brokers → Log in · Kite → App
  credentials) or `ALTAIR_KITE_API_KEY`, plus today's `data/kite_session.json`.

## The Terminal

- **Views:** **Market Watch** (F4) or **Option Chain** (Ctrl+O) in the main area,
  with depth and time & sales of the selected scrip beside it. **Models** is
  Ctrl+M.
- **Adding a scrip:** **Insert**, or type in **＋ Add scrip**. It searches NSE
  equities and NSE F&O in `data/instruments.csv` and adds the scrip to
  `data/live/watchlist.csv`.
  - The feed picks it up within a few seconds: SIM adds it in place, and FYERS
    and Kite reconnect with it.
  - **Delete** removes the selected scrip; right-click → *Restore removed
    scrips* brings removed ones back.
- **Positions are only in Alt+F6.** The net position window holds:
  - the paper book;
  - *Broker account · GETS*: positions and funds, Greek watch, portfolio
    Greeks, simulation, expenses, trade history, RMS, movers and indices.

  These are priced live from the stream. The account snapshot is fetched by
  itself every minute while the window is open; there is no Refresh button.
- **Halt controls** (toolbar) opens the Operations window: halt, the intent
  queue and the gated ticket.

### Keys (GETS / ODIN)

| Key | What |
|---|---|
| **+** or **F1** | Buy order window for the selected scrip (blue) |
| **−** or **F2** | Sell order window (red); F1/F2 inside it switch side |
| F3 | Order book. Shift+F1 cancels, Shift+F2 modifies, Shift+F3 cancels every pending order |
| F8 | Trade book |
| Alt+F6 | Net position |
| F5 / F6 / Shift+F9 | Market picture (best five) and snap quote |
| Shift+F7 | Security information |
| F10 | Message log |
| Insert / Delete | Add / remove a scrip |
| Ctrl+F | Find in the watch |
| F12 or Ctrl+/ | Every key |

### Paper orders

- **Fills follow the market, not the last trade.** An order meets the market
  250 ms after it is placed, on the feed's clock, and fills only against a
  **fresh** quote: one stamped within 10 s of the feed's clock, from a feed that
  is still updating. It is never filled at the last trade.
- **MKT** takes the ask (buy) or the bid (sell). It walks the five-level book
  when that is fresh, and fills no more than the size shown. The rest keeps
  working, and the order book shows filled/qty and the average price. With no
  fresh bid or ask, a market order is **rejected**, and the reason gives the
  quote's age. An index is refused.
- **LMT** rests until the book crosses it, then takes only the levels inside
  the limit. It never fills at a price the book did not show.
- Quantity is whole lots and a limit is on the tick; anything else is refused,
  and the refusal is in the order book.
- **Positions** net per scrip and product. Realised P&L is taken on what
  closes. The open part is marked at a fresh bid (long) or ask (short). With
  no fresh quote it is **unmarked**, and its net is shown as unavailable.
  **Square off** sends a paper market order for the net quantity.
- **Expenses** are charged per fill through `risk/cost.hpp` (brokerage as in
  the demos). Unpriced is shown as unpriced, not zero. A net that would need an
  unpriced expense is shown as **unavailable**, not as gross P&L.
- **Persistence:** `data/live/paper/manual_orders.csv` (an event log) and
  `manual_trades.csv`, appended and flushed.
  - A restart rebuilds the book.
  - A day order left open from an earlier day shows as expired.
  - Only one Altair window writes the book (`manual.lock`). A second window
    refuses orders and says why.
  - If a write fails, new orders are refused and the message log says which
    file.

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
- **Late joiners:** a terminal that connects mid-session gets the whole board at
  once, so no illiquid strike sits blank until it next trades.
  - The board is sent as **Snapshot** frames to that terminal alone, stamped
    with the stream's current sequence number.
  - Terminals already connected never see them. To a running terminal, a
    replayed trade would look like a fresh print.
- **One thread owns the bus.** Sources (FYERS, Kite, SIM) push each update
  into a ring. The bus's thread publishes, accepts subscribers and flushes;
  there is no lock on the path.
  - A full ring makes the source wait. It never drops.
  - A slow *subscriber* is coalesced by the bus (oldest frames first) and sees
    a sequence gap.
- **Kite reads never block a stop.** The ticker reads asynchronously in short
  slices (`broker/ws_pump.hpp`).
  - A stop, the session's end, or 30 s with no frame at all (not even a
    heartbeat) ends a read at once.
  - The feed reconnects if the session isn't over. It used to stop after 60 s.

Each instrument carries three frame types:
- trade: LTP, last quantity, volume and OI;
- quote (`server/quote_payload.hpp`): OHLC, previous close, best bid/ask with
  sizes, ATP, total buy/sell, circuits and last-trade time;
- book: five levels a side.

## The watch in detail

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
| Direction 10:15 AR(2), ARMA(1,1), Logistic, Ridge, GBDT, Vote | at 10:15, forecast 10:15 to the 15:20 square-off; trade one NIFTY future lot only if (2q−1)·E\|r\| beats the round-trip cost | intraday |
| Pairs BANKNIFTY/NIFTY | 250-day spread; at 15:15, in at \|z\| ≥ 2, out at ≤ 0.5, stop at 4 | carried, rolled on expiry |
| Stat-arb NIFTY 50 | Avellaneda-Lee s-scores with today's return as the last day; at 15:15, open at ±1.25; one lot of each stock future | carried, rolled, out after 60 sessions |

**Direction models in more detail.** They run only the 10:15-to-close track. At one
and five minutes the curriculum measured break-even accuracy at 80-99 %, and no
model came close. The 10:15-to-close track is the one the research shortlisted.
- q is not the model's own stated probability. It comes from a walk-forward
  calibration of the model's past calls (three years of 5-minute history,
  re-walked at every start).
- E|r| is the history's sd of 10:15-to-15:20 returns, scaled by today's HAR
  volatility, times √(2/π).
- When the gate stays shut, the row says by how many basis points.

**Paper fills** (`live/paper.hpp`). A decision becomes a working order.
- It meets the market `--latency-ms` (250) after the decision.
- It fills only against a quote no older than `--quote-age-s` (10), at the
  touch: a buy at the ask, a sell at the bid. It walks the five-level book when
  that is fresh.
- It fills no more than the size shown. The rest keeps working, as a partial
  fill.
- It is **never filled at the last trade**. With no executable quote, an
  entry is refused or keeps working.
- **Entries time out.** An entry not filled within `--entry-timeout-s` (60) is
  cancelled. If it was one leg of a decision (a strangle or a pair), the legs
  that did fill are unwound, so no leg is left on alone.
- **Exits never expire.** An exit works until it fills, however long the
  market stays unquoted. The page shows the position as *closing*, since when
  and why.
- Every fill is charged through `risk/cost.hpp`. A round trip with an unpriced
  fill has **no net**: the trade row leaves it blank, and the summary says
  "net unavailable".
- Open positions are marked at a fresh bid (long) or ask (short). With no fresh
  quote they are not marked.

**One risk check before every entry.** An entry is refused when any of these
holds:
- the feed is stale;
- decisions are paused after a trade gap;
- the ledger can't be written;
- `data/kill_request.json` exists (the desktop's Kill Switch);
- it is an intraday entry after 15:20;
- the position count (`--max-positions`, also 60 per model) would be exceeded;
- the gross notional (`--max-gross`) would be exceeded;
- today's loss is past `--max-daily-loss`.

**Exits are never refused.**

**Clock and schedule.**
- **The feed's clock.** Everything runs on the feed's own time stamps, never
  the wall clock. That is why a simulated session at 30× decides exactly as a
  live one would.
- **Roll and square-off.** 15:15 rolls carried positions whose contract expires
  that day; 15:20 squares off intraday ones.
  - These are **obligations**, not moments. They run on the first minute at or
    after their time, so a feed that jumps from 14:40 to 15:25 still rolls and
    squares off (the reason says "overdue").
  - The square-off is re-checked every minute after 15:20.
  - If the feed stops, its clock stops too. A **watchdog** on the machine's
    clock (live feeds only) submits the overdue exits, and they fill as soon as
    quotes return.
- **The clock moves before a tick is used.** The clock advances to a trade's
  time first: the previous minute closes and its models decide, and a new
  session resets. Only then does the trade enter its bar.
  - A late print for a minute already closed is refused and counted. It is
    never written into history.
- **Trade gaps pause decisions.** A lost trade frame means today's bars are
  incomplete: a sequence jump, or trades that went by while the engine was
  reconnecting.
  - No model decides until one whole clean minute has passed. Obligations still
    run.
  - Snapshot frames (a late joiner's baseline) set prices but never enter a bar.
- **The engine's socket is read on its own thread** (`app/live_feed_reader.hpp`),
  so a slow minute in a model never makes the bus drop frames for it.
- **Staleness.** No new position opens while the feed is stale (30 s without a
  frame).
- **Direction models trade the horizon they were trained on.** They are trained
  and calibrated on 10:15 to the **15:20** close (`SessionInputs::exit_minute`),
  the square-off they are held to, not 15:30.

## Files (`data/live/`, git-ignored)

| File | Written by | What |
|---|---|---|
| `universe.csv` | price service | what streams, and under which token |
| `feed_status.json` | price service | source (fyers, kite or sim), state, counts, last error |
| `watchlist.csv` | Terminal (Insert / ＋ Add scrip) | scrips added to the watch, by Kite token; the price service streams them |
| `watch_removed.csv` | Terminal (Delete) | scrips removed from the watch |
| `paper/manual_orders.csv` | Terminal | every paper order and each change of state (+ / − orders) |
| `paper/manual_trades.csv` | Terminal | every paper fill, at the bid or ask it dealt at, with expenses |
| `engine_state.json` | engine, every second | each model's state, signal and reason, and the open positions |
| `paper/journal.csv` | engine | **the record**: every fill (open or close, quantity, price, expenses, decision time), appended and flushed. A restart rebuilds the book from it. A tree from before the journal resumes once from `open_positions.csv`, and those positions become the journal's first rows |
| `paper/trades.csv` | engine | every round trip: date, model, symbol, side, qty, entry and exit, gross, expenses, net (blank when unpriced), why in, why out, LIVE/SIM, VERIFIED/UNVERIFIED/UNPRICED |
| `paper/fills.csv` | engine | every fill, at the bid or ask it dealt at |
| `paper/open_positions.csv` | engine | what is held, for people; replaced in one step, never deleted first |
| `paper/engine.lock` | engine | held while it runs: a second engine on the same tree refuses to start |
| `paper/manual.lock` | Terminal | held by the window that writes the paper book |

**Writes are acknowledged.** A row counts as written when the file took it
(appended and flushed). If a write fails:
- the rows are kept and retried every second;
- the engine is **halted** (no new entries) until they land;
- at exit, unwritten rows make the engine exit with status 4.

Files are replaced with one rename. The old file is never deleted first.

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
- **SIM proves nothing about the market.** SIM P&L is the simulator's random walk
  (between real minute closes, on a past day); it shows the plumbing, not an edge.
- **A past SIM day uses today's contracts.** The instrument master lists live
  contracts only, so the chain on a simulated past day is today's expiries,
  valued at the simulated clock.
- **Paper fills take what is shown.** A paper order takes the displayed size at
  the touch (or the five levels). It never queues behind other size, and never
  moves the market. A real order of size would do both.
