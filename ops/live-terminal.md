# Live terminal: streaming prices, paper orders and live models

The Terminal opens on a live market watch that ticks, and it starts the live feed
by itself. Orders are **paper** by default: + and − open a GETS-style order
window, and fills come from the live bid and ask. Switch **LIVE** on in the
Terminal and the same window sends **real orders to FYERS**, within limits you
set, through the order router (see *LIVE orders* below). The live models run on
the same stream and paper-trade (demo-trade) their own signals by themselves.

```
FYERS socket ─┐  (+ the 50-level book)
              ├─► altair_price_service --live ──► 127.0.0.1:7421 ──► Terminal (watch, chain, 50-level depth, T&S, orders)
Kite ticker ──┘    (or --sim [--date D])                          └► altair_live_engine ──► data/live/ ──► Models, Arbitrage
Terminal (LIVE on) ──► data/order_intents.jsonl ──► altair_order_router ──► FYERS orders API ──► data/live_orders/
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
3. **The models start by themselves** once the feed streams (*Demo trade
   automatically*, on by default): `altair_live_engine` paper-trades every
   model, the cross-exchange arbitrage included. **Stop models** turns that off
   until **Start models**.
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
altair_live_engine [--unverified-costs] [--date 2026-09-24]   # stops at 15:35 on the feed's clock, or a quiet minute after 15:30
altair_live_engine --latency-ms 250 --quote-age-s 10 --entry-timeout-s 60 --max-positions 80 \
                   --max-gross 1e8 --max-daily-loss 2e5 --max-margin 1e7 --gate-z 1   # the defaults, spelt out
altair_live_engine --record                      # also write the session tape (data/live/tapes/; GBs a day)
altair_live_engine --replay data/live/tapes/2026-09-24-091012.tape   # run it again; must come out identical
altair_paper_report                              # daily mark-to-market, intervals, multiple testing
altair_exec_study --tape data/live/tapes/2026-09-24-091012.tape   # shortfall, markouts, passive fills
altair_charges_check --note note.csv             # config/charges.toml against a real contract note
altair_readiness                                 # the gates before any money (enables nothing)
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
- **Adding a scrip:** **Insert** (or **＋ Add scrip**) opens the GETS Add Scrip
  window. It narrows the way GETS does: **Exchange** (NSE, BSE, NFO) →
  **Instrument** (EQ; FUTIDX, FUTSTK, OPTIDX, OPTSTK) → **Symbol** → **Expiry**
  → **Option type** (CE/PE) → **Strike**. Each list holds only what
  `data/instruments.csv` has under the choices before it, and the window shows
  the one contract they name (trading symbol, lot, tick, token) before **Add**.
  The quick-add box beside it still takes a typed symbol. The scrip goes into
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
| F5 / F6 / Shift+F9 | Market picture (every level the feed sends: fifty from FYERS) and snap quote |
| Shift+F7 | Security information |
| F10 | Message log |
| Insert / Delete | Add Scrip window (Exchange → Instrument → Symbol → Expiry → Option → Strike) / remove a scrip |
| Ctrl+F | Find in the watch |

### Paper orders

- **Fills follow the market, not the last trade.** An order meets the market
  250 ms after it is placed, on the feed's clock, and fills only against a
  **fresh** quote: one stamped within 10 s of the feed's clock, from a feed that
  is still updating. It is never filled at the last trade.
- **MKT** takes the ask (buy) or the bid (sell). It walks the book (five or
  fifty levels) when that is fresh, and fills no more than the size shown. The rest keeps
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

### LIVE orders (FYERS, real money)

**Off by default.** The **PAPER** button on the Terminal's top strip switches
LIVE on:
1. It asks for the limits, and for **LIVE** typed out: the most lots per order
   (default **1**), the largest order value (price × quantity, default
   Rs 25 lakh: one index-future lot), the most orders today (20), the most open
   at once (5), a loss today at which new orders stop (Rs 5,000), and how far
   a limit may be from the last price (3 %).
2. It writes `data/live_trading.json` (armed, until 15:30 IST today, the limits)
   and starts `altair_order_router`. The button turns red: **● LIVE**.
3. Now **F1/F2** (and + / −, and a click in the chain) open the same order
   window, marked **LIVE · FYERS**. **Every order asks for a confirmation** that
   shows it in full (side, lots and quantity, symbol, exchange, MARKET or the
   limit, product, validity, value). Only then is a request appended to
   `data/order_intents.jsonl`.
4. The router sends it only if **all** of these hold, and otherwise refuses it
   and says why (Live orders, the message log F10, `journal.jsonl`):
   - LIVE is on and not expired; the kill switch is off; a FYERS session exists;
   - market hours (09:15–15:30 IST, weekdays);
   - the token is in today's universe and its symbol and exchange match;
   - product: CNC or MIS for equity (NSE or BSE), NRML or MIS for F&O;
   - lots ≤ the per-order limit; a limit on the tick and within the band of
     FYERS's last price (a fresh `/data/quotes` read);
   - value, orders today and open orders within their limits;
   - today's P&L from FYERS positions read, and above the loss limit.

   Then the dispatch gate (`oms/broker_dispatch_gate.hpp`) issues a permit and
   rechecks it immediately before the POST: a changed arm file, a kill or a new
   login voids it.
5. **Nothing is retried.** A send with no reply or no order id is looked for in
   the FYERS order book by its tag (`AL` + 16 hex digits); if it is not there
   after three reads it is marked **NOT PLACED** and never sent again.
6. **Live orders** (button on the strip) lists every request today and what
   became of it: REFUSED, PENDING, OPEN, FILLED (with the average), CANCELLED,
   REJECTED (with FYERS's message), UNCERTAIN, NOT PLACED. **Cancel selected**
   and **Cancel all open** go through the router.
7. **Switching LIVE off** (click the red button) writes `"armed": false`:
   orders are paper again at once. Orders already at FYERS stay there; cancel
   them in Live orders.
8. **The kill switch** (Operations → Kill switch, `data/kill_request.json`)
   refuses every new request and cancels every open order the router placed.

**Test with one lot first**, and watch it in Live orders and in the FYERS app.
`altair_order_router --dry-run` checks every request and shows the exact order
body it would send, sending nothing. Orders placed by the router appear in the
FYERS order book with the tag `AL…`.

## What streams

The universe is built at feed start from `data/instruments.csv` (the Kite master)
and `config/universe_nifty50.csv`. It is written to `data/live/universe.csv`.

| Group | What | Depth |
|---|---|---|
| Indices | NIFTY 50, NIFTY BANK, INDIA VIX | — |
| Futures | the nearest two NIFTY and BANKNIFTY futures | 50 levels |
| NIFTY / BANKNIFTY options | nearest expiry, ATM ± 20 strikes, CE and PE | ATM ± 5: 50 levels |
| NIFTY 50 | the 50 stocks | 50 levels |
| NIFTY 50 · BSE | the same stocks' BSE listings (`BSE:<SYM>-A`): the arbitrage's other leg | best bid/ask |
| Stock futures | each stock's near and next future (stat-arb trades and rolls these) | — |

That is about 370 symbols, well under FYERS' 5,000 per socket.

**The 50-level book.** FYERS sends it on a second socket (`feed/fyers_tbt.hpp`:
protobuf, decoded without a library and pinned by messages the official SDK
encoded). The price service subscribes the instruments marked for depth,
futures first, five to a channel (`--depth50 N` caps how many; `--depth50 off`
keeps the 5-level book only). While an instrument's 50-level book is fresh it
replaces the HSM's five levels; if FYERS refuses a subscription the reason is
printed and that instrument keeps five. Kite and the HSM socket send five
levels; SIM sends fifty.

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
- book: up to fifty levels a side (five from the HSM socket and Kite).

## The watch in detail

- **Market watch:**
  - One row per instrument: LTP (with ▲/▼), change, % change, bid qty, bid, ask,
    ask qty, LTQ, volume, ATP, OI, open, high, low, previous close, LTT and the
    day's trade count.
  - A price that moves flashes green or red. Every frame updates its row as it
    arrives; painting is coalesced to 10 a second.
  - Grouped by the table above, searchable and sortable.
- **Depth & trades:**
  - Every level the feed sends (fifty from the FYERS 50-level book, else
    five), with order counts and the totals under them.
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
| Direction 10:15 AR(2), ARMA(1,1), Logistic, Ridge, GBDT, Vote | at 10:15, forecast 10:15 to the 15:20 square-off; trade one NIFTY future lot only if q·gain − (1−q)·loss − cost clears zero by `--gate-z` (1) standard errors | intraday |
| Pairs BANKNIFTY/NIFTY | 250-day spread; at 15:15, in at \|z\| ≥ 2, out at ≤ 0.5, stop at 4 | carried, rolled on expiry |
| Stat-arb NIFTY 50 | Avellaneda-Lee s-scores with today's return as the last day; at 15:15, open at ±1.25; one lot of each stock future | carried, rolled, out after 60 sessions |
| Cross-exchange arbitrage | on every quote: when one exchange's bid beats the other's ask (NSE vs BSE, same stock) by the four fills' expenses, both spreads and 2 bp, buy the cheap listing and sell the dear one, Rs 2 lakh a leg (no more than both touches show); out when the mids meet, after 30 minutes, or at 15:15 | intraday |

**The Arbitrage page** (left bar) is cross-exchange only: the manual pair must
be the same stock on NSE and BSE, and the page shows the arbitrage model's
reading of every pair (best edge first), what it holds, and whether demo
trading is on.

**Direction models in more detail.** They run only the 10:15-to-close track. At one
and five minutes the curriculum measured break-even accuracy at 80-99 %, and no
model came close. The 10:15-to-close track is the one the research shortlisted.
- q is not the model's own stated probability. It comes from a walk-forward
  calibration of the model's past calls (three years of 5-minute history,
  re-walked at every start), with its standard error.
- gain and loss are what the model's right and wrong calls actually moved,
  in units of the day's volatility, learned from the same walk-forward
  (shrunk toward a normal's √(2/π) while few), each with its standard error.
  A model that is right on small moves and wrong on big ones is caught here;
  the old (2q − 1)·E|r| could not see it.
- The gate takes the call when value − z·se > 0, se by the delta method over
  q, gain and loss. When it stays shut, the row and `decisions.csv` say by how
  many basis points, with every input.
- **Nothing is fitted at 10:15.** The models are fitted on the history before
  the session starts (and fingerprinted into the bundle); at 10:15 they only
  predict, in a few milliseconds.
- Hyperparameters are tuned on an inner fold of the training rows, with the
  scaler fitted on those rows only: the outer test rows never shape a choice.

**Paper fills** (`live/paper.hpp`). A decision becomes a working order.
- It meets the market `--latency-ms` (250) after the decision.
- It fills only against a quote no older than `--quote-age-s` (10), at the
  touch: a buy at the ask, a sell at the bid. It walks the book (five or fifty
  levels) when that is fresh.
- It fills no more than the size shown. The rest keeps working, as a partial
  fill.
- It is **never filled at the last trade**. With no executable quote, an
  entry is refused or keeps working.
- **Entries time out.** An entry not filled within `--entry-timeout-s` (60) is
  cancelled — never filled late, even by a quote that arrives after the
  deadline before any trade has moved the clock. An exit decided while an
  entry is part-filled withdraws the rest and sells only what filled. If it was one leg of a decision (a strangle or a pair), the legs
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
- the **margin estimate** (`--max-margin`) would be exceeded, or a leg held or
  new cannot be priced (`live/margin.hpp`: conservative, **not SPAN** — futures
  at scan + exposure, short options NSE-shaped with no credit for premium or
  hedges, long options at premium);
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
- **The day ends itself.** At `--until` (15:35) on the feed's clock, or — since
  the market stops printing at the close and the feed's clock with it — after a
  quiet minute past 15:30.
- **A restart resumes.** The book is rebuilt from the journal; a strangle
  restarted after 09:20 takes up the legs it sold (its stop still watches
  them), and a late start is reported as such, never back-filled.
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
| `paper/decisions.csv` | engine | every model decision and refusal, at the feed time it was taken, with the gate's numbers |
| `paper/margin.csv` | engine | the margin estimate at every minute's close, for the book (`ALL`) and per model |
| `paper/marks.csv` | engine | each held position's mark at 15:29 (the mid): what the paper report marks carried positions to |
| `sessions.csv` | engine, at exit | one row per session: frames, trade frames lost, gaps, halts, latency p50/p99/p99.9, tape, bundle, exit code |
| `bundles/<day>/bundle-<id>.json` | engine | what the session was built from (see below), and `oos-<id>.csv`: every walk-forward call behind the calibration |
| `tapes/<day>-<time>.tape` | engine `--record` | the session tape (see below) |
| `replay/<tape>/` | engine `--replay` | a replay's own files; never the live ones |
| `replay_checks/<tape>.json` | engine `--replay` | did the replay reproduce the recorded session's rows? |
| `report/` | `altair_paper_report`, `altair_exec_study`, `altair_readiness` | the report, the execution labels, the readiness verdict |
| `paper/engine.lock` | engine | held while it runs: a second engine on the same tree refuses to start |
| `paper/manual.lock` | Terminal | held by the window that writes the paper book |

LIVE trading's files are in `data/` (git-ignored too):

| File | Written by | What |
|---|---|---|
| `live_trading.json` | Terminal (LIVE switch) | armed or not, until when, the limits |
| `order_intents.jsonl` | Terminal (a confirmed LIVE order), Operations ticket | requests for the router, one JSON line each |
| `live_orders/orders.json` | order router, every 2 s | the router's heartbeat and every request today with its FYERS id and status |
| `live_orders/journal.jsonl` | order router | every request, refusal, body sent (no credentials), reply, status change and cancel |
| `live_orders/cancels.jsonl` | Terminal (Live orders) | cancel requests, by FYERS id or all |
| `live_orders/intent_cursor.txt` | order router | how far it has read, and the request ids it has seen (never sent twice) |
| `live_orders/router.lock` | order router | one router at a time |

**Writes are acknowledged** (`live/ledger.hpp`). A row counts as written when
the file took it (appended and flushed). If a write fails:
- the rows are kept and retried every second;
- a write that failed part-way is cut back to the last whole row first, so the
  retry writes each row exactly once;
- the views (trades, fills, decisions, margin, marks) wait while the journal
  cannot be written: none shows a fill the record lacks;
- the engine is **halted** (no new entries) until they land;
- at exit, unwritten rows make the engine exit with status 4.

Files are replaced with one rename. The old file is never deleted first.

## Reproducing a session: bundles, tapes and replay

**The bundle.** Every session writes `bundles/<day>/bundle-<id>.json`:
- a digest of every history series and file it read: the 5-minute and daily
  bars, the stat-arb history, the universe, the charges, and the ledger it
  resumed from;
- the options it ran with;
- for the direction models: the features, the scaler, what each fit chose,
  its parameters where they print, a fingerprint of the fitted function (its
  call on every training row), the calibration and the learned gain and loss.

The id is the digest of all of it, so two sessions with one id were built from
the same inputs and fitted the same models. `oos-<id>.csv` beside it keeps every
out-of-sample walk-forward call.

**The tape** (`--record`, `live/tape.hpp`). Every byte off the bus, in order,
plus the controls the machine set, where they took effect: stale, kill
request, halt, watchdog, connect and disconnect. It starts with the options,
the universe, the charges, the starting ledger and the bundle. A live day is
gigabytes, so recording is opt-in.

**The replay** (`--replay TAPE`). The same engine, fed the tape, into
`replay/<tape>/`:
- it refuses when today's inputs no longer make the recorded bundle (exit 5;
  `--force-replay` runs it anyway and names what changed);
- the journal, fills, decisions, margin and marks come out **byte for byte**
  as the session wrote them;
- it checks this itself against the recorded ledger (`--verify-against`) and
  writes the verdict to `replay_checks/`; a replay that differs exits 6.

`app/tests/live_replay.sh` proves it in CI: a simulated morning, stopped and
restarted mid-way, both sessions replayed twice.

## Measuring it

- **`altair_paper_report`** (`live/report.hpp`). Daily **mark-to-market** net
  per model and for the book: cash moved plus the change in what is held,
  valued at the 15:29 marks. A day whose holdings have no mark is reported
  unmarked, and its P&L carried into the next marked day.
  - Drawdown, turnover, capital (the peak margin estimate), and return on it.
  - A split by India VIX regime (terciles of the previous close).
  - 95 % intervals from a **moving-block bootstrap over days**, drawn jointly
    for all models.
  - p-values adjusted for having tested every model: **Romano-Wolf**
    (step-down, from the same joint draws) and Holm.
  - A **stress** case: expenses ×1.5 plus 2 bp of every rupee traded.
- **`altair_exec_study`** (`live/exec_study.hpp`). For every fill, from the tape
  that saw it:
  - implementation shortfall against the decision's mid;
  - the spread then;
  - markouts at 1, 5, 30, 60 and 300 s;
  - whether a passive order at the touch would have filled within 1, 5, 30 or
    60 s. This counts the queue ahead and the prints at or through the price,
    so it is a lower bound: cancellations ahead are not seen.

  Horizons past the tape's end are unknown, never zero. These are the labels
  for execution research.
- **`altair_charges_check --note NOTE.csv`** (`app/charges_check.hpp`). A real
  contract note, normalised to one CSV shape, priced order by order exactly as
  the paper engine prices, and reconciled **head by head** (brokerage, STT,
  exchange, SEBI, stamp, IPFT, GST; signed, engine minus note). The finding
  goes to `data/verified/charges_check.json`.
- **`altair_readiness`** (`live/readiness.hpp`). The gates before any money,
  each with its evidence. Only LIVE sessions count: SIM proves the plumbing.
  - **Operational:**
    - enough sessions, with clean exits;
    - feed loss negligible;
    - worst-session p99 latency inside bounds;
    - no recent halts;
    - tapes recorded;
    - recorded sessions replayed identically;
    - every bundle on disk.
  - **Economic, per model:**
    - enough marked days, with every fill priced;
    - an edge that survives Romano-Wolf;
    - the edge still there under the stress case;
    - a bounded drawdown.
  - **And for everything:**
    - a contract note that agrees with the charges;
    - capital measured with **SPAN**. This build cannot do that, so the gate
      stays shut.

  It enables nothing by itself: LIVE orders depend only on the Terminal's
  LIVE switch and the limits you set (see *LIVE orders*). It is the evidence
  to read before switching it on for anything bigger than a one-lot test.

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
  the touch (or the levels of the book). It never queues behind other size, and never
  moves the market. A real order of size would do both.
- **The arbitrage is a demo.** Real NSE/BSE gaps last milliseconds and are
  taken by co-located firms; this one acts 250 ms after a quote over a home
  connection. BSE legs are charged at the NSE schedule (`app/demo_costs.hpp`
  has no BSE rates), and FYERS' BSE symbols are assumed group A (every NIFTY
  50 stock is).
- **FYERS' limits on the 50-level book are not documented here.** The feed
  asks for every depth instrument and prints any refusal; check the log the
  first day.
- **LIVE orders are the router's limits, not FYERS' RMS.** The router refuses
  what breaks the limits you set; FYERS can still reject an order (margin,
  circuit, a holiday), and its reason is shown.
- **Capital is an estimate.** `live/margin.hpp` errs high on purpose (no hedge
  or premium credit). The exchange's SPAN files are not loaded, so readiness
  keeps its capital gate shut.
- **Marks are mids at 15:29, not settlement prices.** A carried future's daily
  P&L is against the 15:29 mid, not the exchange's settlement price (the last
  half-hour VWAP).
- **Contract-note history, historical lot sizes and bid/ask history** are not
  in this tree. They need a broker's or the exchange's files: `altair_charges_check`
  reads a note once one is copied in.
