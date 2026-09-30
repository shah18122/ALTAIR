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

About 8 minutes for all seven tracks on one core (`--only daily`: about 3).
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
| Ensembles | Vote (majority of the learning models), Champion (best record in finished stages), Hedge (exponential weights on finished stages) |

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

Run on the dataset pushed 2026-09-29 (all seven tracks, under 8 minutes on
one core, **zero look-ahead refusals**; two runs gave identical numbers). 21 models × 7 tracks = 147 tests, so a
result needs p < 0.05 / 147 to count. Full tables: the Summary sheet.

| Track | Decisions | Up-rate | Best model (accuracy, 95 % CI) | Beats a coin? | Beats the best constant call? |
|---|---|---|---|---|---|
| NIFTY daily | 2,884 | 53.5 % | Transformer 53.9 % [52.1, 55.8] | 3 models, significant | **No** — always-up scores 53.5 % |
| BANKNIFTY daily | 2,885 | 53.2 % | Always majority 53.0 % | No | No |
| NIFTY FUT daily | 2,731 | 52.2 % | GRU 53.9 % [52.0, 55.8] | 3 models, significant | **No** |
| INDIA VIX daily | 2,885 | 46.5 % | **Random forest 60.9 % [59.1, 62.7]** | yes, 7 models | **Yes** — RF, Champion, Hedge, logistic, Vote, kNN |
| NIFTY hourly | 17,208 | 50.8 % | Random forest 51.5 % [50.7, 52.2] | RF and kNN, significant | **No** |
| BANKNIFTY hourly | 17,208 | 50.5 % | Logistic 51.1 % | No | No |
| INDIA VIX hourly | 17,214 | 43.8 % | Always down 56.2 % | yes, 15 models | **No** — none beats always down (VIX falls on 56 % of hours) |

What the record says:

1. **Index direction is not forecastable from price and VIX alone** — daily
   or hourly, by any model here. The best daily accuracies (~53–54 %) equal
   what calling UP every day scores; hourly sits at 50–51.5 %. After the
   round-trip cost no index model makes money reliably: net bp per trade
   runs from about −10 to +5, and no model is positive on every index track.
2. **INDIA VIX next-day direction is forecastable.** The random forest
   reaches 60.9 % over 2,839 out-of-sample days against 53.5 % for the best
   constant call, and its accuracy rises, noisily, as the window doubles:
   50 % learning 24 days, 58 % on 96, 53 % on 192, 58 % on 384, 62 % on
   768, 63 % on 1,536. The Hedge ensemble learned to follow it (100 % of the
   weight by the last stage). VIX mean-reverts around its 20-day level; the
   tree models find that.
3. **More data helps only where there is signal.** On VIX the curve rises
   stage by stage; on the indices it wanders around the up-rate whatever
   the window.
4. **Price error**: the learning models are within about ±2 % of the
   random walk's RMSE (the coin and the rules, which forecast a full average
   move every time, are ~20 % worse). By the scorecard's paired test only
   4 of 147 price forecasts beat the random walk: random forest, logistic
   and kNN on VIX daily (+2.0, +0.9, +0.8 % skill) and the always-down drift
   on VIX hourly (+0.1 %). 90 are measurably worse. On the indices the best
   price forecast is still "no change".
5. **Reliably wrong** (significant after correction): mean reversion on
   NIFTY and NIFTY FUT daily (46 %, the mirror of momentum's 53.5–53.7 %)
   and on VIX hourly (48 %), and the SVM on VIX hourly (44 %: it leans up on
   a series that mostly falls).

What would improve it next: broker-verified data (run
`ops\broker_audit.ps1`, then repeat); inputs beyond price — option-chain
OI/PCR and IV skew, FII/DII flows, GIFT Nifty and the US close; and using
the VIX forecast where it matters, timing option premium rather than index
direction.
