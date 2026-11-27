# Paper P&L breakdown, 2026-11-27

Smit: "hit rate is the same but PnL is dipping; break it down before changing
models." `paper_breakdown.py` reads the engine's paper round trips from the
daily logs in altair-data. **The newest pushed log is 2026-11-12**: the server
has been offline and the 15:47 push has not run since, so anything after 12 Nov
is not in this table.

Run: `python research/pnl/paper_breakdown.py ../altair-data/logs --bars ../altair-data/merged/spot/banknifty/5m/clean.csv.gz`

## 4-12 Nov 2026 (5 sessions, 2,779 round trips, net Rs -1,65,706)

| Group | Trades | Hit | Avg win | Avg loss | Gross | Costs | Net |
|---|---|---|---|---|---|---|---|
| OBI A-D futures, NYKAA | 68 | 7-18% | 656-1,667 | -1,436 to -3,146 | -71,563 | 51,605 | **-1,23,168** |
| OBI option-sell + quoter + diffusion, NYKAA/BNF | 105 | 0-50% | | | -36,660 | 10,348 | -47,008 |
| OHL (stock futures) | 33 | 39% | 6,336 | -4,304 | +11,506 | 15,209 | -3,703 |
| Strangle 80% stop2x (NIFTY, BNF) | 8 | | | | -11,348 | 594 | -11,941 |
| Straddles / strangles without stops (9 models) | 34 | 33-75% | | | +17,689 | 2,708 | +14,981 |
| Cross-exchange arbitrage | 2,531 | 93% | 10 | -95 | +45,958 | 40,826 | +5,132 |

What it says:

1. **The loss is one family, not a slow payoff decay.** OBI (futures and
   options) loses Rs 1.70 lakh, more than the whole Rs 1.66 lakh; OBI futures
   on NYKAA alone is 74%. Its gross is already negative (hit 7-18%, losses
   twice the wins), and costs of about Rs 760 a trade (6 bp of a Rs 12.6 lakh
   lot) nearly double it.
   NYKAA was dropped from OBI on 12 Nov (UI reorg), so this should stop.
2. **Stops on short options lose.** "Premium doubled" stop: 2 exits, Rs -15,937;
   the same strangles without the stop made money. Matches the 2026-10-25 study
   (stops hurt every variant).
3. **Cross-exchange arbitrage is a cost artefact.** Rs 2 a trade net, and
   brokerage is not priced (`[broker.fyers] configured = false` in
   config/charges.toml). At FYERS's published Rs 20 or 0.03% an order it is
   **Rs -16,953**. Same direction for every model, small for the rest.
4. **Long vs short:** OHL longs +21,795 (9), shorts -25,497 (24). Too few to
   call a short bias, but OHL mostly shorted.
5. **Trending vs ranging:** all 4 sessions with bars were ranging (efficiency
   ratio < 0.35), so there is no trending day to compare yet.
6. **Feature drift:** OBI thresholds were never fitted on a training period
   (docs/MODEL_AUDIT.md: "thresholds unswept"), so there is no training
   distribution to drift from.

## Assumption checks

| Item | Finding |
|---|---|
| Lot sizes | From the instrument master each day (NIFTY 65, BANKNIFTY 30, NYKAA 3,125 in the trades). No literals. |
| Expiry days | From the master (NIFTY weekly 9 Nov moved to Monday for the 10 Nov holiday, read correctly). No weekday hard-coded. |
| STT | Post-Budget-2026 schedule from 2026-04-01: futures 0.05% sell, options 0.15% of premium sell. `last_verified` is the owner's instruction, not a circular check. |
| Brokerage | **Missing** (see 3). Set `[broker.fyers]` to the real plan. |
| Timestamps | Live paper fills act on ticks in **receive** order, which is right: a fill can only use what has arrived. The recorder keeps the exchange time on prints (`exch_time`, flag `exch_time_is_recv` when FYERS sends none); books/quotes carry receive time. `research/signals/obi_study.py` grids on receive time, so no cross-symbol lookahead. Sorting several symbols by exchange time would be the lookahead. |
| Paper vs tape fills | Not checked yet: needs the server's recorded tapes for the same days. |
