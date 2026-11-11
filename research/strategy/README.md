# `research/strategy/`: a quant strategy study, hypothesis first

Smit, 2026-11-11: "create a proper quant strategy with study and hypothesis
considering this data". This folder is that study.

1. [HYPOTHESES.md](HYPOTHESES.md): six rules from the literature, written
   down with their pass bar and committed **before** they were run.
2. `study.py`: the harness (signal strictly before entry, next-bar-open fills,
   7 bp round trip, random-sign placebo, one-bar-delay check, a deliberate
   leak it must refuse). Results in `results/`.
3. [RESULTS.md](RESULTS.md): every rule fails on BANKNIFTY and NIFTY futures,
   in dev (2019-2022) and in the sealed 2023 holdout.
4. S1 below: the one strategy left standing, and how it gets validated.

## What we know, in one place

| Question | Answer | Where |
|---|---|---|
| Can a model forecast the next bar's price (1m to daily)? | No, never better than "no change" after costs | `docs/MODEL_AUDIT.md`, `research/cnn/` |
| Tick by tick? | Only the bid-ask bounce, not capturable | `docs/MODEL_AUDIT.md` §3 |
| Bank basket leading the future? | IC 0.07-0.17 at 10 s, ~1 bp, never clears ~6 bp | `research/signals/` |
| Event rules (momentum, breakout, gap, shock, overnight)? | No, all fail dev and holdout | `RESULTS.md` |
| Can the size of the next move be forecast? | **Yes**: HAR 16-35 % below random walk from 1 min to 1 hour, stable in every third | `docs/MODEL_AUDIT.md` |

The only forecastable quantity is volatility. So the strategy has to be one
that is paid for being right about volatility, and that means selling
options (never buying them).

<a id="s1"></a>
## S1: BANKNIFTY short straddle, sold when IV >= forecast RV

**Hypothesis.** BANKNIFTY options are priced at an implied vol above the vol
the index then realises (the variance risk premium), and HAR's forecast of
the session's realised vol is good enough to skip the days when it is not.
Selling the ATM straddle when IV (at the bid) >= forecast RV earns the premium
net of costs, and the gate earns more than selling every day.

**Rules** (already in `live/straddle.hpp`, model "Straddle IV>=RV BANKNIFTY",
paper; the timed copy "Straddle IV>=RV timed BANKNIFTY" times the legs on the
basket forecast):
- Nearest expiry; on expiry day, the next one. One straddle a session, one
  lot a leg, sold at the bid at the first tick where IV_bid >= RV forecast,
  both on trading-session time.
- RV forecast = realised so far + HAR's variance for the rest of the session.
- Held to the 15:20 square-off. Sell only; never a long option.

**Why it cannot be backtested now.** The Drive data has no options; FYERS
serves no expired option contracts; the banned exchange file is not used.
So the test is forward, on the chains the recorder saves each day
(`altair-data/chain_iv`, `vol/`).

**Forward test, fixed now** (judge after 60 recorded sessions, about
mid-February 2027; nothing gets tuned before then):
1. Per session: sale price (bid), buy-back price (ask at 15:20), costs from
   `config/charges.toml` schedule 2, P&L per lot.
2. Pass: mean net P&L per lot > 0 with t >= 2; positive in each third of the
   60 sessions; the gated seller beats the "sell every day" shadow (same
   rules, gate off) on mean P&L; worst session loss under the
   `max_drawdown` provision (Rs 1,00,000).
3. Placebo: the gate's days against random day sets of the same count from
   the "sell every day" shadow, 2,000 draws; the gate must sit above the
   95th percentile.
4. If it fails, it is reported as failed and stays paper, like the six
   rules here.

The first 5 recorded BANKNIFTY sessions (4-11 Nov 2026) are far too few to
say anything: 09:30 ATM IV was above that day's realised session vol on 4 of
5, and below it on 11 Nov (IV 16.9 %, realised 34.6 %), the kind of
day the gate has to keep it out of.
