# OBI option simulator and hybrid model: plan

Written 2026-11-19 for Smit. **Plan only, nothing built yet.** It answers four
questions: are paper trades logged, how much tape we have, how to run a real
simulation on the recorded tape in the Qt desktop, and how to find the best
model mix (the hybrid / CNN question).

## 1. Are all paper trades logged?

**Yes, for every model the live engine runs, with the reason.** The gaps are about
where the logs go, not whether they are written.

| Model (PAPER) | Trades + reason | Decisions incl. "no trade" | Where |
|---|---|---|---|
| Straddle IV>=RV NIFTY / BANKNIFTY, timed + book-rule variants | yes | yes (one row per index per day) | `data/live/paper/trades.csv` (`why_in`, `why_out`), `decisions.csv` |
| Strangle 80% / stop2x, Vol band (trades nothing) | yes | yes | same |
| Direction 10:15 (ARMA, logistic, ridge, GBDT, vote) | yes | yes | same |
| Pairs BANKNIFTY/NIFTY, Stat-arb NIFTY 50 | yes | yes | same |
| OBI A-D (futures and option-sell), OBI quoter | yes | yes, including "signal but blocked because ..." | same |
| Bank pairs (10 pairs) | yes | yes | ledger + `data/live/pair_trading/decisions.csv` |
| Diffusion, Basket alpha | yes | yes | ledger + own `trades.csv` |
| Desk trader | yes | yes | ledger |
| Arbitrage, OHL, Option arbitrage (off: it buys options) | yes | yes | ledger |
| OBI page PUNCH (your manual paper clicks) | yes | n/a | `data/live/punch/obi_punch.csv` (`reason` column) |

Every fill must carry a reason (the paper broker refuses an order without one), and
every trade line also goes to the activity log, e.g. 12 Nov: 152 paper round trips,
net -Rs 11,988, each with its full reasoning.

**Gaps to fix (small, part of phase 1 below):**
1. The paper ledger (`data/live/paper/`) is **not pushed** to altair-data
   (`ops/linux/push_data.py` excludes `data/live/`). Only the log lines leave the server.
2. **altair-data has had no push since 12 Nov.** The last commit is 2026-11-12, so
   13-19 Nov (tapes decoded, paper trades, retrain) can't be seen from outside the
   server. This needs checking on the server first.
3. No single scoreboard: P&L per model per day is in files, not on one screen.

## 2. How much tape do we have?

From the decode reports in altair-data (server recorder, from 4 Nov):

| Day | State |
|---|---|
| 4 Nov | full day, 4 disconnects |
| 5 Nov | partial (stops 12:46) |
| 6, 9, 11, 12 Nov | full, clean (0 drops, 0 missed books) |
| 10 Nov | **broken** (6 books all day): unusable |
| 13-19 Nov | probably recorded, but not pushed (see gap 2) |

So: **4 clean full sessions confirmed, about 9-10 if 13-19 Nov are clean.** That's about
5.5 GB a day and 27 GB in total on 12 Nov.

What one day holds: about 30M frames, 5M prints, 19M quotes and 5.6M book updates over 1,464 instruments.
- **NIFTY**: weeklies (e.g. 17 Nov, 23 Nov) plus the monthly. **BANKNIFTY**: three monthlies. There are 81 strikes per expiry.
- **Quotes** (top of book) cover every strike. The **5-level book** covers ATM ±10 strikes.
- **50-level book**: one slot only (`--depth50 1`).

**Is it enough to replay weekly options?** To *build and debug* the simulator, yes,
right now. To *judge* a strategy, no:
- **Weekly cycles:** at most 1-2 complete Wed-to-Tue cycles, and the 10 Nov expiry day is broken.
- **Sessions:** intraday OBI trades within one day are correlated, so the sample size is sessions, not trades. The roadmap's 40-session verdict needs about **40 clean sessions, around early January 2027** if every day records cleanly.
- **Weekly expiries:** about 8 weekly expiries by then, which is still thin for expiry-day behaviour.

Old PC tapes (19-23 Oct) hold only partial sessions with no weeklies, so leave them out. FYERS can't
serve expired options later, so every day the recorder misses is gone for good.

## 3. The simulator: run the recorded tape in the Qt desktop

**Most of the engine already exists.** What's missing is mainly the desktop screen and
option-specific fill realism.

Already built:
- `altair_live_engine --replay`: re-runs a session byte for byte.
- `altair_tune`: walk-forward over all tapes, recorder tapes included, for every OBI A-D model.
- `live/paper.hpp`: latency, touch fills, and a passive queue model (`book/queue_fill.hpp`).
- `live/exec_study.hpp`: markouts and passive-fill odds.
- `config/charges.toml`: costs.

### What to build

**Phase 1: tape library and session check (small)**
- List every tape with its quality: full or partial, disconnects, missed books,
  weekly expiry day or not. Only clean sessions go into the sim and the study.
- Push the paper ledger and the tape index to altair-data nightly (fixes gaps 1 and 2).
- Copy tapes to `S:\tick_data\tapes` on your PC for training (your rule: train on
  the PC). The sim screen itself can run wherever the desktop runs.

**Phase 2: replay player (core)**
- Feed a recorder tape into the existing bus, so the **current** pages show it unchanged
  with a REPLAY badge (Terminal, OBI page, Option Chain, ladder).
- Controls: day picker, play / pause / step one event / step one minute, speed
  (1x, 10x, 100x, max) and jump to time.
- Feed time drives every clock, so nothing reads the wall clock.

**Phase 3: Simulator screen (the dashboard)**
- Pick sessions, a strategy (OBI A-D option-sell, straddle, your own PUNCH clicks) and its settings.
- During playback it shows the blotter with entry, exit, why in, why out and costs.
- It also shows P&L by session, markouts at 1/5/30/60 s, fill rate and slippage against mid.
- **Batch mode** runs many sessions × settings at max speed and gives a scoreboard
  (per session, not per trade), sharing code with `altair_tune`.
- You can also trade by hand on the replay (PUNCH on tape) to practise reading OBI.

**Phase 4: fill realism for options**
- **Aggressive** (sell at bid): fills at the bid, for the size shown at the bid, after
  latency (default 250 ms; also run at 50 and 1000 ms). If the bid size is short, the rest goes down the 5 levels.
- **Passive** (offer at ask or inside): queue position comes from the 5-level quantity
  when the order rests. It fills only when prints trade through the price, or prints at the price clear the queue
  ahead. FYERS depth is aggregated, so cancellations ahead are unseen and this is a
  **conservative** estimate.
- **Strikes outside ATM ±10** have quotes only, no depth. Allow aggressive fills only there, with
  a configurable haircut.
- **Costs**: brokerage, STT on the sell side, exchange charges, SEBI fee, GST and stamp duty from
  `charges.toml`, per fill.
- **Sell-only rule**: the sim's risk gate refuses any opening option buy, the same check as live.
  Buying back to close a short is allowed.
- **Margin**: SPAN-like estimate per position, capped at the Rs 1 crore capital, 1 lot.

### How OBI turns into an option trade (sell-only)

| OBI reading on the BANKNIFTY (or NIFTY) future | Option trade |
|---|---|
| Bid pressure (A) | Sell ATM / 1-OTM **put** |
| Ask pressure (B) | Sell ATM / 1-OTM **call** |
| Balanced, low vol forecast (HAR) | Sell straddle (existing S1 logic) |
| Absorption / withdrawal (C / D) | Exit or stand aside, per type |

The option's own book must not oppose the trade (already in the OBI trader). Exits are
target, stop, signal flip, time stop, and 15:20 square-off.

## 4. The hybrid model: which mix of models works

### Be honest about the starting point
- **Direction (11 Nov audit):** no model beats "no change" on price at any timeframe on FYERS data.
- **CNN on Drive ticks:** it had no direction edge after costs. It only forecasts move size, where it ties HAR at 1m/5m.
- **combo.py (29 features incl. CNN, ridge, GBM):** all 90 tests lose after costs.
- **What is real:** volatility forecasts (HAR, VIX), plus a few stable small effects. A move since the open tends to continue to the close, the futures lag the spot by about 5 minutes, and 5-15 minute moves revert.

So a hybrid must not be another direction predictor. Use the models to
**choose when OBI is allowed to trade and which threshold to use**, then judge the result in rupees.

### Design (pre-registered and frozen before any run)

**Stage A: index state, from long history**
- **Data:** FYERS 1m 2019-2026 plus Drive ticks 2020/22/23. Drive ticks are trade-only and non-FYERS, so they're used for pattern learning only. They never give a verdict.
- **Output** at each minute: a small **state**:
  - vol bucket (HAR forecast vs VIX)
  - trend day vs range day (since-open move, HMM regime)
  - time-of-day bucket
  - expiry-day flag
- These come from existing Atlas models. There's no new learning here, just their outputs.

**Stage B: OBI settings per state, on recorded tapes**
- For each state, choose the OBI entry threshold, hold time and strike from a small grid,
  on training sessions only. This is a lookup table with few parameters.

**Stage C: meta-model (only if B shows anything)**
- **Inputs:** the level-0 outputs at decision time: OBI5/OBI50, OFI, VPIN,
  microprice gap, HAR RV, HMM state probabilities, VIX change, since-open move, and CNN p(big move).
- **Candidates, simplest first:** (a) logistic/ridge stacking, (b) gradient boosting,
  (c) a small CNN/TCN on the same feature sequences.
- **Target:** the net rupee outcome of the sell trade the simulator would make (P&L after
  fills and costs), not "did price go up".

**Why not CNN first?**
- With about 40 sessions, a CNN has far more parameters than independent samples, so it will fit noise.
- It stays in the study, but only as candidate (c). It must beat the ridge stack on validation sessions to be used.
- Where the CNN already helps is as a **feature** (its move-size forecast), and it's included as one.

### Leak rules
- Every feature at time t uses only data with feed time ≤ t.
- Labels start after the latency delay.
- HMM, HAR, scalers and the CNN are fitted on training sessions only, then frozen per fold.
- Whole sessions go to one side, never split within a day. A 1-session embargo sits between train and test.
- The existing leak check runs on every run.

### Splits
- **Walk-forward** on clean recorded sessions: train on sessions 1..k, test on the next 5,
  then roll forward.
- **Holdout:** the **last 10 clean sessions** before the verdict date are locked and
  run **once**, at the end. Stage A also has the Drive 2023 holdout.

### What decides "it works" (judged on P&L, not accuracy)
- **Primary:** mean **net rupees per session** on the holdout, after all costs at 250 ms
  latency, with a session-level t-stat ≥ 2.
- **Also required:**
  - still positive at 1000 ms latency
  - beats the three baselines: no trade, plain S1 straddle, and OBI alone at default thresholds
  - max drawdown within limits
  - no single session supplies more than 30% of the profit
- **At most 20 settings tried** in total, with a multiple-testing correction (deflated Sharpe).
  Every run is logged, including the failures.
- **Pass:** paper-trade it live, then 1 lot LIVE per the roadmap. **Fail:** the study stops and the
  record says why.

## 5. Order of work and timing

| Step | What | Needs |
|---|---|---|
| 1 | Session check + push paper ledger + fix the stalled altair-data push | server |
| 2 | Replay player feeding the existing pages | desktop |
| 3 | Simulator screen + batch scoreboard | desktop |
| 4 | Option fill realism + sell-only gate in the sim | engine |
| 5 | Stage A state labels on long history | your PC |
| 6 | Stage B per-state OBI thresholds, walk-forward on tapes | your PC |
| 7 | Stage C stacking vs CNN, only if 6 shows a result | your PC |
| 8 | Holdout run, once (about 40 clean sessions incl. 6+ NIFTY weekly expiry days, around early Jan 2027) | your PC |

Steps 1-4 can start now and are useful even before there's enough tape. Steps 5-7 can be
built and dry-run on the sessions we have, but their numbers count only on the holdout.

## Decisions (Smit, 2026-11-19)
1. **NIFTY weekly first.** Weekly expiry gives more theta decay for the option seller.
   The OBI signal comes from the NIFTY near future's 5-level book, and the trades are
   NIFTY weekly options, ATM / 1-OTM. BANKNIFTY monthly comes second.
   - The holdout must include **at least 6 weekly expiry days (Tuesdays)**.
   - Results are reported separately for expiry day and the other days, because they
     behave differently.
2. **No FYERS Prime needed.** The 5-level book plus tick-by-tick prints are enough:
   - OBI A-D already read **OBI5** (5 levels). The NIFTY future and ATM ±10 strikes
     have 5-level books on every tape, and every strike has top-of-book quotes.
   - We trade 1 lot (65 NIFTY). That is small against the size shown in the top 5
     levels, so a sell at the bid fills within level 1-2. The extra 45 levels would barely
     change the fill estimate.
   - The queue estimate from the 5 levels is conservative (it can't see cancellations
     ahead), which errs on the safe side.
   - Drive tick data has prints only, no book, so it can't replace depth. But it isn't
     needed for depth: it's used for the long-history state labels (stage A).
   - Only revisit this if the sim shows fills limited by depth beyond level 5. That would
     mean upgrading to FYERS API Prime (5 symbols of 50-level per connection, up to 3
     connections) and setting the recorder to `--depth50 15`.
