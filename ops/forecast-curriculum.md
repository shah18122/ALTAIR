# Forecast curriculum — learn 3 days, forecast, keep score, refit on 6, 12, 24 …

`altair_forecast_curriculum` walks every forecaster in `models/` through the
same schedule on `dataset/` and keeps a record of every call, right or wrong.

```
stage 0   learn days [0, 3)      forecast days [3, 6)
stage 1   learn days [0, 6)      forecast days [6, 12)
stage k   learn days [0, W)      forecast days [W, 2W)      … to the end of the data
```

Inside a block the model is **frozen**: it never sees the outcomes it is
forecasting. After the block it is refitted from scratch on everything seen
so far — that refit is the "update". A model that cannot fit yet (three rows
do not make a boosted tree) **abstains**, and the abstention is recorded, not
scored.

## Run it

```powershell
build\net\app\altair_forecast_curriculum.exe --dataset dataset --out data\verified
```

About 13 minutes for all twelve tracks on one core (`--only daily`: about 6).
Options: `--first-days N` (default 3), `--cap-days N` (stop doubling once a
block would exceed N days and walk on in N-day blocks; 0 = pure doubling),
`--other-cost-bp X`, `--only TEXT`, `--no-log`.

## Output (`data/verified/`, git-ignored)

| File | What |
|---|---|
| `forecast_curriculum.xlsx` | **Summary** (every model on every track: accuracy, 95 % interval, p vs a coin with Bonferroni, always-up on the same bars, Brier, price skill vs the random walk, trades that clear cost, verdict) · **Data** (what cleaning did, per track) · **Method** · one **learning-curve sheet per track** (accuracy per stage and so far, right/scored, what each stage's fit chose, the Champion and Hedge weights going into each stage) |
| `forecast_curriculum.txt` | The summary as text. |
| `forecast_log/<track>.csv` | Every forecast: time, stage, days learned, model, last price, next price, forecast price, P(up), call, what moved, RIGHT/WRONG/FLAT, net bp if traded. |

## Tracks

| Track | Decision | Outcome |
|---|---|---|
| NIFTY, BANKNIFTY, NIFTY FUT, INDIA VIX daily | 15:30, from that day's bar | next trading day's close |
| … + VIX fc (daily and hourly index tracks) | the same, plus the INDIA VIX model's own forecast as a feature | the same |
| NIFTY, BANKNIFTY, INDIA VIX hourly | close of each of a full day's first six hourly bars (10:15 … 15:15) | the next hourly close (no overnight hour) |

Features (all known at the decision; standardised on the training window
only): recent returns (1, 2, 3, 5, 20 days or the last two bars), 10-day
volatility, range, where the close sits in the range, the opening gap,
weekday or hour, previous-day return and range (hourly), and **INDIA VIX**
(level, change, level against its 20-day mean) on every index track. The
futures track takes its price features from the spot index plus the
**basis** and its change, so a roll never enters a feature.

## Models

| Family | Models |
|---|---|
| Baselines | Coin flip (seeded), Always majority, Momentum, Mean reversion |
| Classical | Logistic regression (L2 tuned), SVM (RBF), k-nearest neighbours (k tuned) |
| Trees | Random forest (depth tuned), Gradient boosting |
| Neural | MLP (backprop), LSTM, GRU, causal Transformer |
| Time series | AR(2), ARMA(1,1), Ornstein–Uhlenbeck, 3-state Markov chain |
| Regime | k-means regimes |
| Ensembles | Vote (majority of the learning models), Champion (best record in finished stages), Hedge (exponential weights on finished stages), **Stack** (a logistic regression on every model's past out-of-sample calls), **Stack (confident third)** and **Consensus 75%** (filters that abstain unless confident) |

Tuning uses the last quarter of each training window only. Limits, stated:
the transformer trains by finite differences (the repository's reference
backend) with 400 SGD steps a stage, so it is the least-trained network; the
SVM learns from its window's latest 1,500 rows.

## Data rules (from the data audit)

Seconds floored; repeated stamps keep the first bar; impossible OHLC widened
and counted (its close is kept); hourly forecasts only on full 09:15–15:15
days (Muhurat, special sessions and the partial 2026-09-25 excluded); rows
without a same-time INDIA VIX bar dropped; history before INDIA VIX begins
(2015) not used; NIFTY futures **daily only** (the intraday files hold the
next-month contract for July–August 2026) with outcomes that cross an expiry
excluded. The expiry calendar is checked on every run: the basis should be ~0
on expiry and jump the next day (Data sheet).

## No look-ahead

Enforced by `models/curriculum.hpp`, not by care: targets are readable only
inside the training window; features, lagged outcomes and prices only up to
the row being forecast; the track is refused if any outcome is known after
the next decision. Every refused read is counted — a clean run reports zero
— and `models/tests/test_curriculum.cpp` changes one outcome and proves no
call made before it moves.

## Scoring

* **Accuracy** = right / (right + wrong). An unchanged close is neither; a
  call with no direction counts as wrong.
* **p vs coin**: two-sided, then **Bonferroni** over every model on every
  track. Twenty-one models on seven tracks will produce a few "significant"
  ones by luck alone.
* **Always-up on the same bars**: the honest directional null on an index
  that drifts up. Beating a coin is not enough.
* **Skill vs RW**: 1 − RMSE(forecast price) / RMSE(last price), from
  `models/forecast_scorecard.hpp`.
* **Trades**: a call is acted on only when its expected move exceeds that
  day's round trip (futures STT 2 bp, 5 bp from 2026-04-01 per
  `config/charges.toml`, plus 1.3 bp other charges and one tick). INDIA VIX
  is not tradable.

## Results

Run on the dataset pushed 2026-09-29: 12 tracks (the 7 above, plus 5 index
tracks with the VIX forecast as an input), 24 models each (18 forecasters,
6 ensembles). 13 minutes on one core, **zero look-ahead refusals**, and
identical numbers across repeated runs. 288 model-track tests, so both
significance tests (against a coin, and against the best constant call)
are Bonferroni-corrected by 288. Full tables: the Summary sheet.

| Track | Up-rate | Best single model | Beats a coin (corrected) | Beats the best constant call (corrected) |
|---|---|---|---|---|
| NIFTY daily | 53.5 % | Transformer 53.9 % [52.1, 55.8] | Transformer, Momentum, Consensus 75 % | **none** |
| BANKNIFTY daily | 53.2 % | Always majority 53.0 % | none | none |
| NIFTY FUT daily | 52.2 % | GRU 53.9 % [52.0, 55.8] | GRU, Consensus 75 % | none |
| **INDIA VIX daily** | 46.5 % | **Random forest 60.9 % [59.1, 62.7]** | 10 models | **Random forest, Champion, Hedge, Stack, logistic** |
| NIFTY hourly | 50.8 % | Random forest 51.5 % [50.7, 52.2] | random forest, kNN | none |
| BANKNIFTY hourly | 50.5 % | Logistic 51.1 % | none | none |
| INDIA VIX hourly | 43.8 % | "Always down" 56.2 % | 17 models | none (VIX falls on 56 % of hours) |

1. **Index direction cannot be forecast from price and VIX alone**, daily
   or hourly. The best daily accuracies (~53–54 %) are what calling UP every
   day scores; hourly sits at 50–51.5 %. After the round-trip cost, net bp
   per trade runs from about −10 to +5 and no single model is positive on
   every index track.
2. **INDIA VIX next-day direction can be forecast.** The random forest
   scores 60.9 % over 2,839 out-of-sample days against 53.5 % for the best
   constant call, and it improves, noisily, as the window doubles: 50 %
   learning 24 days, 58 % on 96, 53 % on 192, 58 % on 384, 62 % on 768,
   63 % on 1,536. Hedge learned to follow it (all of the weight by the last
   stage). VIX mean-reverts around its 20-day level, and the tree models
   find that. RF, logistic and kNN are also the only real price skill
   against the random walk (+2.0, +0.9, +0.8 %).
3. **Reliably wrong** (corrected): mean reversion on NIFTY daily (46 %, the
   mirror of momentum's 53.7 %) and on VIX hourly (48 %); the SVM on VIX
   hourly (44 %: it leans up on a series that mostly falls).

### Merging models

Three kinds of merge, all built only from finished stages:

| Track | Best single | Stack | Stack (confident third) | Consensus 75 % |
|---|---|---|---|---|
| INDIA VIX daily | RF 60.9 % | 57.5 % | 59.5 % on 32 % of days | 60.3 % on 38 % |
| NIFTY daily | 53.9 % | 51.4 % | 54.8 % on 33 % | **57.4 % on 27 %**, +7.1 bp/trade, t = 1.6 |
| NIFTY FUT daily | 53.9 % | 52.5 % | 53.5 % on 12 % | **57.3 % on 34 %**, +3.9 bp/trade, t = 0.9 |
| BANKNIFTY daily | 53.0 % | 53.1 % | 53.7 % on 29 % | 54.8 % on 32 % |
| NIFTY / BANKNIFTY hourly | ~51 % | ~50 % | ~50.5–51 % | ~51 %; BANKNIFTY loses after cost (t = −3.9) |

* **Stacking does not beat the best single model.** When most inputs are
  coin-level, the meta-model spreads its weight over noise. On VIX it trails
  the random forest by 3.5 points.
* **Consensus is the one merge that lifts index accuracy.** When at least
  three quarters of the learning models agree, NIFTY and NIFTY FUT daily are
  right about 57 % of the time, on roughly a third of the days. But the
  up-rate on those same days is 54–56 %. The models mostly agree on up-days
  in up-trends, so the edge over "always up" on those days is only 1–3
  points, and not significant after correction. The P&L after cost is
  positive (NIFTY +7.1 bp a trade over 713 trades), with t = 1.6. That is
  **promising, not proven**. It is a candidate for paper trading and for
  re-testing on data after this run, not for live money.
* **The VIX forecast as an input does not help the index models.** The
  "+ VIX fc" tracks score within noise of the originals, e.g. NIFTY daily
  Consensus 57.4 % both ways; NIFTY FUT daily Consensus 57.3 % → 54.2 %.

What would improve it next:
- broker-verified data: run `ops\broker_audit.ps1`, then repeat;
- inputs beyond price: option-chain OI/PCR and IV skew, FII/DII flows, GIFT Nifty and the US close;
- a forward test of Consensus 75 % on NIFTY daily from October 2026, the data this run has never seen;
- using the VIX forecast where it bites: timing option premium, not index direction.
