# How the paper straddle sellers are judged

Written down on 2026-11-02, BEFORE any paper record under the new rules
exists, so the verdict cannot be fitted to what the record turns out to show.
A rule changed later restarts that model's count under a new name.

## The models under test

From `altair_live_engine` (live/straddle.hpp), per index (NIFTY weekly,
BANKNIFTY monthly), each one lot a leg, sold intraday and bought back by 15:20:

| Model | What it is |
|---|---|
| `Straddle IV>=RV <index>` | the control: ATM straddle when trading-time IV (bid) >= the session RV forecast |
| `Straddle IV>=RV books <index>` | the control plus the book rules: news days, calm vol, 50-day trend, profit taking |
| `Strangle 16d IV>=RV <index>` | the 16-delta short strangle on the same gate |
| `Straddle IV>=RV 0DTE <index>` | the control, but on expiry day it sells the contract expiring that day |

The count starts on the first session after the 2026-11-02 change (one clock,
one window; FYERS-only, fresh history). Sessions the engine refused (stale or
non-FYERS history) are not sessions of the test; they are counted apart.

## What is measured

From `data/live/paper/trades.csv` and `decisions.csv` (`altair_paper_report` prints
most of this, with serial-dependence intervals), net of expenses from a
VERIFIED `config/charges.toml` (an UNVERIFIED run is not evidence):

- daily net P&L per lot, a no-trade day counted as zero;
- mean, standard deviation, t-statistic of the daily net;
- worst day, maximum drawdown of the cumulative net;
- return on margin: net over the margin the trade blocked (the FYERS margin
  when the broker reports it; `live/margin.hpp`'s estimate until then);
- for each variant, the paired daily difference against the control.

## The verdict

**Too early** until a model has 60 test sessions and 40 trades.

**Kill** (stop it, keep the record) at any of:
- a drawdown past Rs 1 lakh a lot (the vol_premium halt);
- after 60 sessions, a mean daily net below zero;
- after 120 sessions, a t-statistic below 1.

**LIVE candidate** only when all hold:
- 120 sessions or more, and a t-statistic of 2 or more on the daily net;
- the deflated Sharpe ratio (Bailey & Lopez de Prado) at 95 % or more,
  counting every variant tried on that index as a trial: these four, plus the
  ones tried before (bhavcopy studies, the combos search), at least 20;
- its worst day within the per-trade loss limit Smit sets for real money
  (the 4 % gap loss each entry logs is the yardstick);
- a variant beats the control on the paired difference (t >= 2); otherwise
  the simpler control is the candidate.

A candidate goes LIVE only by Smit's decision, with broker-side stop orders and
the risk limits in docs/research/vol-premium-validation.md, never on this page
alone.

## What does not count

- The NSE bhavcopy results (Rs 5.12 L monthly, Rs 8.5 L intraday weekly, the
  IV > RV variants): bhavcopy is not used any more, and they priced fills at
  closes or opening prints.
- Their rupee figures were per lot of NIFTY 75 / BANKNIFTY 35; the lots are
  65 / 30 now (config/lot_size_history.csv), so in today's lots they read
  about 13-14 % high even before the data question.
- `altair_trader combos` option legs: priced off INDIA VIX (a 30-day index
  from NIFTY's monthly options), not off any weekly or BANKNIFTY chain.
