# Forecast curriculum — learn 3 days, forecast, keep score, refit on 6, 12, 24 …

`altair_forecast_curriculum` walks every forecaster in the Model Atlas that
can make a next-bar call through the same schedule on `dataset/`, at every
timeframe, and keeps a record of every call, right or wrong. It asks two
questions of each track:

* **Direction**: will the next close be up or down? (`models/curriculum.hpp`)
* **Range**: will the next close land inside a band that aims to be right
  80 % of the time, and how narrow can that band be?
  (`models/band_curriculum.hpp`)

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

The first pass runs 20 tracks; the second pass runs 42 more, each a first-pass
track with other models' forecasts as inputs (see "Other models as inputs").
That takes about 90 minutes on four cores, most of it the 1-minute tracks.
`--only daily` takes about 2 minutes.

Options:
- `--first-days N` (default 3).
- `--cap-days N`: stop doubling once a block would exceed N days, then walk on in N-day blocks. 0 (the default) is pure doubling.
- `--jobs N`: how many tracks run in parallel. The default is the machine's core count.
- `--other-cost-bp X`.
- `--only TEXT`: run only tracks whose name contains it, e.g. `--only 5m`.
- `--no-bands`, `--no-log`.
- `--no-feeds`: skip the second pass.
- `--leak-check`: prove that no forecast saw its future (see "No look-ahead"). It adds about 60 % to the run
  time and exits with code 3 if anything leaked.

## Output (`data/verified/`, git-ignored)

| File | What |
|---|---|
| `forecast_curriculum.xlsx` | **Tradability**: each track's mean move, cost, the accuracy a call needs to pay that cost (at the history's cost and at today's), and the best models against it. **Feeds**: every second-pass model against its first pass over the same period. **Summary**: direction, every model on every track (accuracy, 95 % interval, p against a coin and against the best constant call, both Bonferroni-corrected, coverage, Brier, price skill against the random walk, trades that clear cost, verdict). **Bands**: every band model on every track (hit rate, width, interval score, skill against the constant band). **Frontier**: accuracy of each model's most confident 0.1 % … 100 % of calls, and the largest slice still at or above 80 %. **Atlas coverage**: every Model Atlas row, and what the run did with it. **Data** · **Method** · one **sheet per track** with its learning curves. |
| `forecast_curriculum.txt` | The summary as text. |
| `forecast_log/<track>.csv` | Every forecast on the daily and hourly tracks: time, stage, days learned, model, last price, next price, forecast price, P(up), call, what moved, RIGHT/WRONG/FLAT, net bp if traded. The 1–15-minute logs would run to gigabytes and are not written. |
| `leak_check.csv` | With `--leak-check`: for every track and cut, the rows rebuilt and the calls and bands rerun, how many differed (always 0 on a clean run), and the first difference. |

## Tracks

| Track | Decision | Outcome |
|---|---|---|
| NIFTY, BANKNIFTY, NIFTY FUT, INDIA VIX daily | 15:30, from that day's bar | next trading day's close |
| NIFTY, BANKNIFTY 0920-close and 1015-close (horizons) | 09:20 or 10:15, the close of that 5-minute bar | the same day's 15:30 close |
| … + vol / cross / models / all (second pass) | the first-pass track's decision, plus other models' forecasts made by then | the same |
| NIFTY, BANKNIFTY, INDIA VIX hourly | close of each of the day's first six hourly bars (10:15 … 15:15), while the bars run on the 09:15 grid | the next hourly close (no overnight hour) |
| NIFTY, BANKNIFTY, INDIA VIX 15m, 5m, 1m | close of every bar of the 09:15–15:30 session but the last, while the bars run on the grid | the next bar's close (no overnight bar) |

Features (all known at the decision; standardised on the training window
only — and where a model tunes a hyperparameter (k, λ, depth, the L2 weight),
it tunes on an inner split of that window, re-standardised on the inner
training rows alone, so the rows it is scored on never shape a choice):
recent returns (1, 2, 3, 5, 20 days or the last two bars), 10-day
volatility, range, where the close sits in the range, the opening gap,
weekday or hour, previous-day return and range (hourly), and **INDIA VIX**
(level, change, level against its 20-day mean) on every index track. The
futures track takes its price features from the spot index plus the
**basis** and its change, so a roll never enters a feature.

## Models

34 direction forecasters and 11 ensembles:

| Family | Models |
|---|---|
| Baselines | Coin flip (seeded), Always majority, Momentum, Mean reversion |
| Statistical alpha | Momentum (tuned lookback), Mean reversion (z-score band, abstains inside it), Pairs (cointegration, NIFTY against BANKNIFTY and back) |
| Classical | Logistic regression (L2 tuned), Ridge regression (λ tuned), SVM (RBF), k-nearest neighbours (k tuned), Autoencoder + logistic |
| Trees | Decision tree, Random forest (depth tuned), Gradient boosting |
| Neural | MLP, LSTM, GRU, causal Transformer and CNN (three layers of dilated causal convolution), all trained by backpropagation |
| Reinforcement | DQN, PPO and actor-critic, each as a contextual bandit (state: the features; actions: long or short; reward: the next standardised return with the action's sign) |
| Time series | AR(2), ARMA(1,1), Seasonal AR (SARIMA), VAR(1), Kalman filter (drift), Ornstein–Uhlenbeck, Markov chain, Hidden Markov model, Hurst regime switch, **Seasonal mean** (each bar of the day's — each weekday's on a daily track — mean return, shrunk toward zero by its own noise; a slot inside its standard error forecasts no change) |
| Regime | k-means regimes |
| Ensembles | Vote, Champion, Hedge, **Stack** (a logistic regression on every model's past out-of-sample calls), and the filters that abstain unless confident: **Stack (confident third / 10 % / 2 %)**, **Consensus 75 % / 90 %** |
| Drift ensembles (the size of the move, for RMSE against the random walk) | **Shrunk average**: the learning models' mean expected move × a slope fitted on the finished stages, shrunk by its t-statistic and kept in [0, 1]. **Ridge stack (drift)**: a ridge regression of the move on every base model's expected move; its penalty is chosen on the last finished stage, with "no change" (the random walk) itself a candidate, then refitted on every finished stage. Each keeps its drift only if a fit made without the last finished stage beat no change on it by more than one standard error; otherwise it forecasts no change |

12 band models and 2 band ensembles:

| Family | Models |
|---|---|
| Benchmark | Constant sigma (GBM) |
| Simulation | Bootstrap quantile, Jump diffusion (Merton, Monte Carlo) |
| Volatility | Historical vol (20), EWMA (0.94), GARCH(1,1), GJR-GARCH, EGARCH, Heston variance drift, Seasonal vol (time of day) |
| Learned | Gradient boosting on \|r\|, Random forest on \|r\| |
| Ensembles | Vol ensemble (geometric mean), Best band so far |

The Atlas coverage sheet lists every one of the Atlas's 85 rows. Rows that
cannot make a single instrument's next-bar call are listed with the reason:
- execution (they carry out a decided trade);
- risk and portfolio construction (they size and combine positions);
- option pricing (no option chain in the dataset);
- order-book microstructure (no order book in the dataset);
- text and fundamentals (no such data).

Tuning uses the last quarter of each training window only. **There are no
row caps**: every model learns from every row of its window, and states what
it fitted in the stage notes. Three things make that affordable at 1-minute
scale (up to 575,000 rows a window):
- **Backpropagation.** The transformer trains by backpropagation
  (`models/transformer.hpp` `analytic_gradients`). It is checked against the
  finite-difference gradient it used to train by, parameter by parameter, to
  3e-7. The same schedule as the LSTM and GRU applies: 30, 10 or 4 epochs over
  every row.
- **An exact k-d tree** (`CurriculumKnnIndex`) for kNN. It returns exactly
  `KnnModel`'s brute-force neighbours, checked on 300 forecasts, at about a
  fifth of the cost.
- **Random Fourier features for the SVM.** Up to 1,500 rows it is the exact
  SMO. Above that, its RBF kernel is approximated by 256 random Fourier
  features, and it is trained on every row by dual coordinate descent. On a
  1,536-row AR(1) window it reaches 0.665 against a ceiling of 0.667, where the
  exact SMO (three passes) reaches 0.574.

## Data rules (from the data audit)

Seconds floored; repeated stamps keep the first bar; impossible OHLC widened
and counted (its close is kept); an intraday day is forecast along the 09:15
grid up to its first missing or off-grid bar and no further — a row needs the
bars to its own outcome, never the rest of the day, because whether a session
will run its full length is not known at 10:15 (Muhurat and other off-grid
sessions get no rows; the partial 2026-09-25 is forecast to 12:45); rows
without a same-time INDIA VIX bar dropped; history before INDIA VIX begins
(2015) not used; NIFTY futures **daily only** (the intraday files hold the
next-month contract for July–August 2026) with outcomes that cross an expiry
excluded. The expiry calendar is checked on every run: the basis should be ~0
on expiry and jump the next day (Data sheet).

## Other models as inputs (second pass)

`models/curriculum_feeds.hpp`. A finished run holds every model's
out-of-sample forecast for every row from its first test block on. Each
forecast is made at the row's decision, from models fitted on finished stages
only. A second pass appends some of them to another track's features:

| Group | Inputs |
|---|---|
| **vol** | The track's own 80 % band widths (GJR-GARCH, seasonal, GBDT on \|r\|, vol ensemble), the HMM's forecast scale and drift, and the Kalman drift. |
| **cross** | Hedge's forecast on the next faster and next slower timeframe and on the daily track, the other index's forecast and band, and INDIA VIX's forecast. |
| **models** | All 32 first-pass models' signed confidence: +1 sure up, −1 sure down, 0 abstained. The coin flip is left out. |
| **all** | Everything above, about 45 more inputs. |

How the groups run:
- **Daily and horizon tracks:** every group, an ablation that shows which input helps.
- **5m, 15m and hourly:** "all" only.
- **1m:** none. No accuracy pays a 1-minute trade's costs.

**The join is as-of.** A row reads, from each feed, the latest value made at
or before its decision. An intraday source counts only on its own day; a daily
one counts for up to a week. A feed that cannot supply a value reads 0 ("no
opinion"), except the band and self feeds, whose rows are dropped instead.
`models/tests/test_curriculum_feeds.cpp` runs both passes, changes one
outcome, and checks that none of the 8,190 second-pass calls made before it
moves.

## Tradability

A direction call that is right a share p of the time, on moves of mean size
E|r|, nets (2p − 1)·E|r| − cost a trade. Break-even is therefore
p* = ½ + cost / (2·E|r|), assuming being right does not depend on the size of
the move.

The **Tradability** sheet gives p* at two costs:
- the history's average cost (mostly 2 bp STT);
- today's cost (5 bp STT since 2026-04-01, plus 1.3 bp).

It then sets the best models against p*. Measured on the 2026-10-01 run, at
today's cost (6.3 bp a round trip):

| Horizon | Mean move NIFTY / BANKNIFTY | Break-even NIFTY / BANKNIFTY | Best model accuracy |
|---|---|---|---|
| 1m | 2.4 / 3.4 bp | 180 % / 142 %: impossible | 51–54 % |
| 5m | 5.5 / 7.5 bp | 108 % / 92 % | 52–57 % |
| 15m | 9.2 / 12.6 bp | 84 % / 75 % | 52–53 % |
| hourly | 17 / 23 bp | 68 % / 64 % | 51–55 % |
| 09:20→close | 55 / 74 bp | 55.8 % / 54.2 % | 53–55 % |
| 10:15→close | 48 / 64 bp | 56.6 % / 54.9 % | 53–56 % |
| daily | 69 / 91 bp | 54.5 % / 53.5 % | 53–55.5 % |

Only the daily and horizon tracks have break-evens a model reaches, and
only on the confident slices: Consensus 75 % is right 55.5 % of the time on
24 % of NIFTY days and 54.0 % on 35 % of BANKNIFTY days.

## No look-ahead: how a leak gets in, and how this run proves there is none

A backtest **leaks** when a decision at time T uses anything that was not
known at T. A leaky backtest is not a bit optimistic. It is wrong, and it is
most often wrong by showing exactly the accuracy people hope to see. Leaks
come in by seven doors:

1. **A feature computed with later data.** A centred or forward-looking
   window, a rolling statistic that includes the bar being forecast, the
   day's close used at 10:15, a daily series joined by date to an intraday
   row (the day's INDIA VIX close at 10:15 is the commonest one).
2. **Normalisation over the whole sample.** A scaler, z-score or PCA fitted
   on every row, so the test rows shape the training inputs.
3. **Targets that overlap the split.** A label whose horizon reaches past the
   end of the training window into the block being forecast.
4. **Selection on the future.** Keeping only the days that turned out
   complete, the stocks that still exist, or the model that did best.
5. **Tuning on the test block.** A hyper-parameter, a threshold or a "top x %"
   cut-off chosen with the rows it is then scored on.
6. **Stacking on in-sample predictions.** An ensemble or second-pass model fed
   a first model's fitted values, not the forecasts it made before each
   outcome.
7. **A back door in the code.** A model handed the whole data set, or a
   revised history (bars corrected after the fact) treated as what was known
   then.

**What the code does about each door:**

| Door | Guard |
|---|---|
| 1 | Every feature is built from bars at or before the decision; INDIA VIX is joined at the same stamp, and its previous close comes from an earlier day. |
| 2 | The scaler is fitted on the training window only. |
| 3 | A track is refused if any outcome is known after the next decision. |
| 5 | Hyper-parameters tune on an inner split, re-standardised on its own rows. |
| 6 | Ensembles weigh only finished stages, and the second pass joins first-pass forecasts as-of their time. |
| 7 | Targets are readable only inside the training window, and every refused read is counted. |

**Fixed in this round (2026-10-19):**
- **The `track()` back door (door 7).** A model could reach the raw track,
  and so every future outcome, past the refusal counter. That accessor is
  gone. Models now get only guarded reads: decision stamp, slot, season,
  sequence columns, and whether there is a pair.
- **Day selection (door 4).** Intraday days are no longer chosen by whether
  they later turned out complete (see Data rules).
- **The Frontier (door 5).** It set its top-x % cut-offs from the whole test
  period's confidences. It is now ex ante: each stage's cut-offs come from
  finished stages only.

**The proof: `--leak-check`.** Guards only cover the doors someone thought
of. The leak check tests the property instead (`app/forecast_leak_check.hpp`).
A forecast made at T is a function of what was known at T, so give the
pipeline a different world after T and it must say exactly the same things up
to T. On every track, at two cuts (the middle of the third- and
second-to-last stages):
- **Builders.** The track is rebuilt from the bars known at the cut, and again
  from bars whose prices after the cut are all moved. Every row decided by the
  cut must come out bit for bit the same, its outcome too where that was known
  by then.
- **Models.** Every model, every ensemble and every 80 % band is rerun on the
  rows up to the cut, with the cut row's own outcome changed. Every call up to
  and including the cut row must be bit for bit the same.

Any difference names the first row and field that moved, and by definition it
is a leak.

`app/tests/test_forecast_leak.cpp` runs the same check on synthetic data. It
also plants leaks to show they are caught:
- a feature built from the next close;
- a whole-sample mean;
- days kept only if they turned out complete;
- a model that reads its row's outcome through a pointer it was handed.

The last one is the case the look-ahead counter cannot see: it reads 0. A
random-walk placebo is run as well, where no model may beat a coin after the
Bonferroni correction.

**What no code can rule out:**
- **Revised data.** A fetch can correct old bars, so history is "as known
  today", not as it was seen live.
- **Research choices.** Running variants, reading the results and keeping the
  best is door 5 at the scale of the project. The cure is to decide before
  looking and to keep a final period untouched until a model is chosen.

## Scoring

* **Accuracy** = right / (right + wrong). An unchanged close is neither; a
  call with no direction counts as wrong. The one exception is a forecast
  of no change from a model that forecasts price by design (the seasonal
  mean on a slot it shrank away, a drift ensemble that chose the random
  walk): it is flagged price only, scored for RMSE and left out of the
  direction record. A rule such as Momentum that makes no call still
  counts as wrong.
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
* **Bands**: each band is k × the model's forecast scale, either side of the
  last close. k is the 80th percentile of the model's own past
  out-of-sample |move| / scale (split conformal over finished stages;
  in-sample until 50 such errors exist).
  - Any band hits 80 % if it is wide enough, so hit rate alone proves
    nothing.
  - The models compete on the **interval (Winkler) score**: width + 2/α ×
    any miss. It is a proper score, so it cannot be gamed by width in
    either direction.
  - Each model is also measured against the constant-sigma band.
* **Frontier (ex ante)**: the accuracy of each probabilistic model's most
  confident 0.1 %, 0.5 %, 1 % … 100 % of calls, and the largest slice still
  at or above 80 % on 30+ calls.
  - Each stage's cut-off for a slice comes from the confidences
    |P(up) − 0.5| of the calls the model made in stages already finished.
    That is the rule a trader could have run live.
  - Until 2026-10-19 the slices were cut from the whole test period's
    confidences at once, which told a call it was in the top 1 % using calls
    not yet made. Frontier numbers in results before that date are after
    the fact.
  - The "largest slice ≥ 80 %" is the best of every model × slice, so read
    it as chosen after seeing the results.

## Results

### Run of 2026-10-19: all 5 timeframes, leak-checked

`altair_forecast_curriculum --leak-check --jobs 4`, on the dataset pushed on
2026-09-29, after the leak fixes of 2026-10-19 and before the drift
ensembles (their numbers are under "Price against the random walk" below).
- **Scope:** 54 tracks and 4.79 million forecast bars:
  - 20 first-pass tracks: NIFTY, BANKNIFTY and INDIA VIX at 1m, 5m, 15m, 60m and 1d; NIFTY FUT daily; the 09:20 and 10:15 → close session tracks;
  - 34 second-pass tracks (+ vol, + cross, + models, + all).
- **Models:** 42 direction models (33 base, 9 ensembles) and 14 band models; 2,268 direction tests, Bonferroni-corrected by 2,268.
- **Run:** 7,825 s with 4 parallel jobs; zero look-ahead refusals on every track.
- **Leak check:** 108 cuts (2 per track). 4,946,800 rebuilt rows, 120,552,012 rerun calls and 40,184,004 rerun bands were compared with the full run, and **every one was identical**: no forecast saw its future (`leak_check.csv`).

**Price against the random walk** (best model on ≥ 90 % of the bars; skill = 1 − RMSE / RMSE of no change):

| Track | Random walk RMSE | Best model | Skill | Models beating it |
|---|---|---|---|---|
| NIFTY 1m | 3.8 bp | Gradient boosting | +0.12 % | 13 / 33 |
| BANKNIFTY 1m | 5.4 bp | Seasonal AR (SARIMA) | +0.01 % | 4 / 33 |
| INDIA VIX 1m | 30.5 bp | Ridge regression | +1.28 % | 20 / 32 |
| NIFTY 5m | 8.4 bp | — | −0.00 % | 0 / 32 |
| BANKNIFTY 5m | 11.9 bp | — | −0.00 % | 0 / 33 |
| INDIA VIX 5m | 56.7 bp | Ridge regression | +0.39 % | 17 / 33 |
| NIFTY 15m | 14.4 bp | ARMA(1,1) | +0.03 % | 3 / 34 |
| BANKNIFTY 15m | 19.8 bp | ARMA(1,1) | +0.05 % | 3 / 34 |
| INDIA VIX 15m | 85.0 bp | Random forest | +0.15 % | 11 / 33 |
| NIFTY hourly | 27.9 bp | ARMA(1,1) | +0.03 % | 3 / 33 |
| BANKNIFTY hourly | 38.1 bp | — | −0.02 % | 0 / 32 |
| INDIA VIX hourly | 147.2 bp | Always majority | +0.11 % | 4 / 33 |
| NIFTY daily | 102.4 bp | Markov chain | +0.08 % | 1 / 33 |
| BANKNIFTY daily | 135.5 bp | — | −0.03 % | 0 / 32 |
| INDIA VIX daily | 528.9 bp | Random forest | +1.96 % | 9 / 32 |
| NIFTY FUT daily | 103.1 bp | — | −0.12 % | 0 / 31 |
| NIFTY 09:20 → close | 80.6 bp | AR(2) | +0.21 % | 1 / 33 |
| BANKNIFTY 09:20 → close | 110.6 bp | — | −0.02 % | 0 / 32 |
| NIFTY 10:15 → close | 75.0 bp | ARMA(1,1) | +0.70 % | 6 / 33 |
| BANKNIFTY 10:15 → close | 99.5 bp | ARMA(1,1) | +0.74 % | 5 / 34 |

- **The second pass** (other models' forecasts as inputs) helps most on the
  10:15 → close tracks: + vol takes NIFTY to +0.84 % and BANKNIFTY to
  +0.98 %. On the 5m, 15m and hourly index tracks it moves the best skill
  by at most 0.03 points, and on the daily tracks by −0.13 to +0.04.
- **Direction:** the only ex-ante slice at or above 80 % anywhere is INDIA
  VIX daily, Stack (confident 10 %): 81.0 % on 42 calls (Wilson low 66.7 %).
  It is the best of every model × slice, so it was chosen after the fact.
  The INDIA VIX 1m slices of the 2026-09-29 run (85 %) are gone: their
  cut-offs used calls not yet made.
- **Bands:** the 80 % bands are calibrated at every timeframe (78.6–82.0 %
  hit rate), except NIFTY FUT daily at 76.9 %. The best band beats the
  constant band by 11–18 % on the index tracks: Seasonal vol (time of day)
  intraday, GJR-GARCH daily, Heston variance drift on the session tracks.
  On INDIA VIX it beats it by 4–8 % (gradient boosting on |r| intraday, the
  vol ensemble hourly and daily).

### Run of 2026-09-29 (before the leak fixes)

Run on the dataset pushed on 2026-09-29, with every model learning from every row.
- **Scope:** 21 tracks (NIFTY, BANKNIFTY and INDIA VIX at 1m, 5m, 15m, 60m and 1d; NIFTY FUT daily; five "+ VIX fc" tracks) and 4.13 million forecast bars, about 174 million model decisions in all.
- **Models:** 42 direction models (33 base models and 9 ensembles) and 14 band models per track.
- **Run:** 60 minutes with 4 parallel jobs, with **zero look-ahead refusals** in both curricula.
- **Tests:** 882 direction model-track tests, so both significance tests are Bonferroni-corrected by 882.

The full tables are in the Summary, Bands and Frontier sheets.

### Direction: 80 % is not there

The **Frontier** sheet ranks each model's calls by confidence. It then
reports the largest top slice that is right at least 80 % of the time on at
least 30 calls.

No such slice exists on any NIFTY, BANKNIFTY or NIFTY FUT track, at any
timeframe, for any model.

The only ones anywhere are two slices of INDIA VIX 1m:
- Stack (confident 2 %), top 0.5 %: 85.2 % on 54 calls (Wilson low 73.4 %).
- Stack (confident 10 %), top 0.1 %: 83.1 % on 71 calls.

They aren't a result, for four reasons:
- They are one call every 40–53 trading days.
- Their accuracy equals the share of up-moves on those same bars.
- VIX cannot be traded.
- They are the best of 6,201 slices tried.

The best honest direction numbers, counting only slices of at least 100 calls:

| Track | Most confident 1 % of calls | Most confident 5 % | Best model that calls ≥ 90 % of bars |
|---|---|---|---|
| NIFTY 1m / 5m / 15m / hourly | 55.8 / 55.8 / 53.4 / 59.1 % | 54.7 / 55.0 / 55.3 / 56.5 % | 51.4–51.8 % |
| BANKNIFTY 1m / 5m / 15m / hourly | 55.6 / 56.3 / 55.4 / 60.2 % | 53.8 / 56.3 / 54.9 / 56.6 % | 51.1–52.6 % |
| NIFTY / BANKNIFTY / FUT daily | too few calls | 59.4 / 60.4 / 61.2 % (about 140 calls each) | 53.8 / 53.0 / 53.9 %; "always up" is 53.4 / 53.2 / 52.2 % |
| INDIA VIX 1m / 5m / 15m / hourly / daily | 78.7 / 61.6 / 61.1 / 65.1 / — % | 73.1 / 62.4 / 60.9 / 60.0 / 71.7 % | 54.3–60.9 % (forecastable, not tradable) |

**What is statistically real is small, and none of it pays.** On the index
tracks, 98 model-track pairs beat both a coin and the best constant call after
the 882-test correction. All of them are on 1m, 5m or 15m bars, at
50.4–56.9 %. None is on hourly or daily bars.

No index model has a positive net return after costs with t > 3 on 100 or more
trades. Two examples:
- **The strongest pair:** BANKNIFTY 5m Stack (confident 2 %) is right 56.9 %
  of the time, on 1,869 calls (0.9 % of bars). It loses 2.7 bp a trade.
- **1-minute mean reversion:** on BANKNIFTY 1m the z-score band is right
  53.7 % of the time over 82,871 calls. It loses 3.3 bp a trade after costs
  (47,601 trades, t = −115). It is the index's stale-price bounce, not an edge.

### What lifting the row caps changed

Every model that was capped, or untrained, now learns from every row. The
accuracy on every call, capped run → uncapped run:

| Model | Index tracks (1m–hourly) | INDIA VIX (1m–hourly) | Why |
|---|---|---|---|
| SVM (RBF) | +0.5 to +2.2 pts (e.g. NIFTY 1m 49.8 → 51.5, BANKNIFTY 5m 50.2 → 52.4) | +4 to +10 pts (hourly 44.4 → 54.7) | It had learned from the latest 1,500 rows only |
| CNN | random kernels → trained: 49.3–50.0 → 49.9–51.2 | 43.8–50.9 → 53.6–55.2 | The random-kernel version was below a coin on several tracks |
| Transformer | +0.1 to +1.6 pts | +0.5 to +3.8 pts (1m 52.2 → 56.0) | Backpropagation over every row in place of 400 finite-difference steps |
| kNN | ±0.5 pts | ±0.9 pts | 20,000 rows was already plenty for 13 features |
| HMM | unchanged | unchanged | A 2-state Gaussian on returns is the same model on 50,000 or 575,000 of them |
| DQN | ±0.5 pts | 1m 55.2 → 52.1, 5m 52.9 → 52.3 | The full replay dilutes the recent regime the 4,096-row replay tracked |
| PPO / actor-critic (new) | 49.5–51.6 % | 50.0–55.0 % | Linear policies: logistic regression learned by policy gradient |

The most confident slices hardly move: the best index slices stay at 53–61 %.
Lifting the caps made the weak models less weak. It did not create an edge.

### Range: 80 %, on every bar, at every timeframe

Unchanged by lifting the caps: the winning band models (seasonal intraday,
GJR-GARCH daily, GBDT on |r| for VIX) never ran into them.

The band models aim at 80 %, calibrated on their own past errors. On every
intraday track they land at 79.3–82.0 %. On daily tracks they land at
76.9–79.7 %; NIFTY FUT under-covers by 3 points. Ranked on the interval
score, the winners beat the constant-width band by 11.5–16.3 % on every index
track:

| Track | Best band model | Hit rate | 80 % band (± from the last close) | Constant band | Score skill |
|---|---|---|---|---|---|
| NIFTY 1m | Seasonal (time of day) | 79.8 % | ±3.8 bp | ±3.9 bp, 80.7 % | +15.5 % |
| NIFTY 5m | Seasonal | 79.8 % | ±8.7 bp | ±9.0 bp | +13.1 % |
| NIFTY 15m | Seasonal | 79.6 % | ±14.7 bp | ±14.9 bp | +11.6 % |
| NIFTY hourly | Seasonal | 79.7 % | ±27.8 bp | ±28.2 bp | +12.7 % |
| NIFTY daily | GJR-GARCH | 79.2 % | ±106 bp | ±122 bp, 83.3 % | +12.8 % |
| BANKNIFTY 1m / 5m / 15m / hourly | Seasonal | 79.3–79.8 % | ±5.5 / 12.0 / 20.0 / 36.9 bp | ±5.7 / 12.5 / 20.4 / 37.8 bp | +13.5–16.3 % |
| BANKNIFTY daily | GJR-GARCH | 78.6 % | ±133 bp | ±159 bp | +15.5 % |
| NIFTY FUT daily | GJR-GARCH | 76.9 % | ±101 bp | ±118 bp | +11.5 % |
| INDIA VIX 1m / 5m / 15m | GBDT on \|r\| | 80.7–82.0 % | ±26 / 52 / 87 bp | | +7.1–8.5 % |
| INDIA VIX hourly / daily | Vol ensemble | 81.1 / 79.7 % | ±146 / 545 bp | | +4.7 / 3.9 % |

Intraday, the seasonal band's average width is within 0.4–4 % of the
constant band's. It wins by putting the width where the risk is: wider at
the open, narrower at midday. Its hit rate is 81.5–82.7 % in its wide half
and 76.6–78.1 % in its narrow half.

Daily, GJR-GARCH wins by following volatility clusters. Its band is
13.6–16.7 % narrower on average than the constant band, and it still covers
77–79 %.

**The honest 80 % forecast** is the range, not the direction. For example,
"the next 5-minute NIFTY close will be within ±8.7 bp of the last one" is
right 79.8 % of the time on every bar, and the band widens and narrows with
the time of day. That is what an option seller needs.

### Merging models (this run)

- **Filtered ensembles** (Stack's confident slices and the Consensus filters)
  have the best accuracy on the calls they make on most tracks. They make few
  calls, and none reaches 80 % on an index.
- **Consensus 75 % on the daily index tracks** is right 54.2–57.3 % of the
  time on 24–37 % of days. That is never significant against the best constant
  call on those days.
- **Adding the VIX model's own forecast as an input** moves the daily
  Consensus 75 % by less than a point:
  - NIFTY: 56.4 → 57.3 %.
  - BANKNIFTY: 54.2 → 54.2 %.
  - FUT: 55.3 → 54.8 %.
  - None of these moves is significant.

### Other models as inputs (second pass, 2026-10-01)

The full two-pass run took 100 minutes on 4 jobs and refused nothing for
look-ahead. It covered 21 first-pass tracks, 4 horizon tracks and 34
second-pass tracks: 1,428 model comparisons on the same rows, and 2,268
model-track tests in all.

- **Feeding models each other's forecasts does not reliably help.**
  - The mean change in accuracy across a track's models is −1.4 to +1.1
    points, mostly within ±0.5.
  - McNemar on the same rows: 60 models nominally better, 32 nominally worse,
    of 934 with a difference.
  - Bonferroni-corrected, one change is significant, and it is for the
    worse (DQN on NIFTY daily + models).
- **The most consistent small gain is NIFTY daily + all:** 31 models better
  and 8 worse, a mean of +0.7 points. None is significant on its own.
- **The best net result at today's cost** is NIFTY 09:20→close + vol,
  ARMA(1,1): +11.4 bp a trade, t = 2.45 over 546 trades. With 2,268 tests,
  significance needs t ≈ 4, so it is a candidate for a forward test, not a
  finding.
- **Bands:** the best 80 % band (Heston variance drift) hits 79.1 % at 196 bp,
  a 20 % better score than the constant band. The bands stay the forecast
  that works.

How a calibrated band does, and does not, turn into option P&L is
measured in [ops/demo-trading.md](demo-trading.md). Selling the touch of a
band edge loses. Selling both edges at 09:20 earns only on the variance
clock, and mostly from the volatility premium.

### Magnitude: right on the days that matter?

Accuracy counts a 5 bp day and a 300 bp day alike. The Summary sheet now also
gives each model two magnitude measures:
- **magnitude-weighted accuracy:** the share of the market's movement the
  model was on the right side of;
- **gross edge, bp a call:** the mean of direction × move.

`altair_magnitude` reads the per-forecast logs. It adds the |move| on right
calls against wrong ones, and a **value gate** (`models/magnitude.hpp`): take
a call only when q·gain − (1 − q)·loss − cost clears zero by `--gate-z`
standard errors.
- q is the model's own probability, calibrated walk-forward, with its
  standard error.
- gain and loss are the moves the model's right and wrong calls actually
  caught, in units of σ (σ from a HAR forecast of that window's 5-minute
  realised variance), learned walk-forward and shrunk toward a normal's
  √(2/π) while there are few. A model right on small moves and wrong on big
  ones has loss > gain, and the gate sees it.
- The uncertainty is the delta method over q, gain and loss.
- Everything uses only what was known when the call was made.

The gate used to be (2q − 1) × E|r| > cost. That assumes a right call and a
wrong one move the same |r|, the very thing this section measures and
finds false.

On the 2026-10-02 run, at 6.3 bp, with the inner-fold tuning and this gate
at z = 1 (the daily, 09:20 and 10:15 tracks, first pass;
`altair_forecast_curriculum --only daily|0920|1015 --no-bands --no-feeds`, then
`altair_magnitude`). The last column is the old (2q − 1) gate on the same
forecasts:

| Track | Median accuracy | Median magnitude-weighted | Models better on big moves | Median net bp / call | Gated share | Median gated net | Old gate: share / net |
|---|---|---|---|---|---|---|---|
| NIFTY daily | 52.1 % | 51.1 % | 14 / 42 | −4.8 | 0 % | −19.5 | 10 % / −4.7 |
| BANKNIFTY daily | 51.4 % | 50.6 % | 11 / 42 | −5.3 | 1 % | −12.1 | 11 % / −7.5 |
| NIFTY FUT daily | 51.5 % | 50.9 % | 11 / 41 | −5.1 | 1 % | −20.5 | 7 % / −12.9 |
| NIFTY 09:20→close | 51.2 % | 52.4 % | 37 / 42 | −3.6 | 2 % | −10.3 | 6 % / −0.3 |
| BANKNIFTY 09:20→close | 50.5 % | 49.8 % | 20 / 42 | −6.6 | 0 % | −11.3 | 6 % / −8.2 |
| NIFTY 10:15→close | 52.3 % | 53.1 % | 35 / 42 | −3.3 | 1 % | −11.1 | 7 % / −3.3 |
| BANKNIFTY 10:15→close | 51.3 % | 51.0 % | 22 / 42 | −5.0 | 0 % | +0.1 | 6 % / −3.0 |

(Median gated net is over the models the gate opened for at least once.)

- **Tuning on an inner fold barely moves the medians.** Only the tuned
  models (logistic, kNN, forest, ridge) can change, and the accuracies match
  the 2026-10-01 run to a tenth of a point. The old gate's shares and nets
  match too.
- **On the daily tracks, accuracy flatters the models.** Most are slightly
  *less* right on big days than small ones.
- **On the NIFTY session tracks it is the reverse.** 35–37 of 42 models are
  more right on the big moves.
- **The conditional gate is far stricter than (2q − 1)·E|r|.** It opens on
  0–2 % of calls, not 6–11 %. Charging a model the loss it actually takes
  when wrong, with the uncertainty of q, gain and loss, shuts most of what
  the symmetric gate let through.
- **The best gated results** are all on 10:15→close:
  - BANKNIFTY ARMA(1,1): +21.0 bp a trade, 57.6 % right, t = 3.01 over 347
    calls;
  - NIFTY SARIMA: +18.0 bp, t = 2.03;
  - BANKNIFTY VAR(1): +17.6 bp, t = 1.98.
- **None survives the 106 gated tests,** which need t > 3.50. They are
  candidates for the forward test the live engine runs, not findings.
- The earlier tables in this file predate the inner-fold fix. The
  accuracies above suggest it moves little, but they are not re-run here.

### Price against the random walk (RMSE, 2026-10-19)

Skill = 1 − RMSE(model) / RMSE(random walk). The random walk forecasts no
change, and on returns this close to unpredictable it is very hard to beat:
a model's own drift usually adds more noise than signal, so most models
come out slightly *worse* than doing nothing.

The drift ensembles and the seasonal mean were added for exactly that.
First pass, every model learning from every row,
`altair_forecast_curriculum --only daily|hourly|15m|5m|1m --no-feeds --no-bands`.
"Best" is the best model on at least 90 % of the bars.

| Track | Random walk RMSE | Best before | Best now | Shrunk average | Ridge stack (drift) | Seasonal mean |
|---|---|---|---|---|---|---|
| NIFTY daily | 102.4 bp | Markov chain +0.08 % | Markov chain +0.08 % | -0.01 % | +0.00 % | -0.01 % |
| BANKNIFTY daily | 135.5 bp | Markov chain -0.03 % | Shrunk average +0.00 % | +0.00 % | +0.00 % | -0.00 % |
| INDIA VIX daily | 528.9 bp | Random forest +1.96 % | Seasonal mean +2.76 % | +0.85 % | +2.08 % | +2.76 % |
| NIFTY FUT daily | 103.1 bp | Always majority -0.12 % | Shrunk average +0.00 % | +0.00 % | +0.00 % | -0.10 % |
| NIFTY hourly | 27.9 bp | ARMA(1,1) +0.03 % | ARMA(1,1) +0.03 % | +0.01 % | -0.12 % | -0.04 % |
| BANKNIFTY hourly | 38.1 bp | Seasonal AR (SARIMA) -0.02 % | Shrunk average +0.00 % | +0.00 % | +0.00 % | -0.04 % |
| INDIA VIX hourly | 147.3 bp | Always majority +0.11 % | Seasonal mean +0.19 % | +0.05 % | -0.08 % | +0.19 % |
| NIFTY 15m | 14.4 bp | ARMA(1,1) +0.03 % | ARMA(1,1) +0.03 % | +0.02 % | +0.00 % | -0.07 % |
| BANKNIFTY 15m | 19.8 bp | ARMA(1,1) +0.05 % | ARMA(1,1) +0.05 % | +0.02 % | -0.15 % | -0.05 % |
| INDIA VIX 15m | 85.0 bp | Random forest +0.15 % | Shrunk average +0.16 % | +0.16 % | +0.07 % | -0.05 % |
| NIFTY 5m | 8.4 bp | Always majority -0.00 % | Shrunk average +0.01 % | +0.01 % | -0.00 % | -0.07 % |
| BANKNIFTY 5m | 11.9 bp | Always majority -0.00 % | Shrunk average +0.01 % | +0.01 % | +0.01 % | -0.08 % |
| INDIA VIX 5m | 56.7 bp | Ridge regression +0.39 % | Ridge stack (drift) +0.42 % | +0.34 % | +0.42 % | -0.06 % |
| NIFTY 1m | 3.8 bp | Gradient boosting +0.12 % | Gradient boosting +0.12 % | +0.08 % | +0.12 % | -0.07 % |
| BANKNIFTY 1m | 5.4 bp | Seasonal AR (SARIMA) +0.01 % | Seasonal AR (SARIMA) +0.01 % | +0.00 % | +0.00 % | -0.08 % |
| INDIA VIX 1m | 30.6 bp | Ridge regression +1.28 % | Ridge stack (drift) +1.63 % | +1.08 % | +1.63 % | +0.23 % |

- **Index tracks (NIFTY, BANKNIFTY, NIFTY FUT): the honest best is about
  zero.** The Shrunk average stays within 0.01 % of the random walk on every
  timeframe, and on most of them it chooses no change. The best single
  models (Markov chain daily, ARMA(1,1) at 15 minutes) beat it by 0.03–0.08 %.
  A large skill here would point to a leak, not a better model.
- **INDIA VIX is where the structure is.** It mean-reverts and has a weekday
  and time-of-day pattern. The Seasonal mean takes the daily skill from
  +1.96 % to +2.76 %, and the Ridge stack the 1-minute skill from +1.28 % to
  +1.63 % and the 5-minute from +0.39 % to +0.42 %.
- **The Ridge stack can still lose** (−0.12 % NIFTY hourly, −0.15 % BANKNIFTY
  15m): with 35 weights, one finished stage is a thin test of whether its fit
  will hold. The Shrunk average fits one slope and is the safer of the two.
- **The Seasonal mean loses on the index intraday tracks** (−0.04 to −0.08 %):
  the time-of-day means it keeps there do not persist.
- **Read the gate as chosen on this data.** The "beat no change by one
  standard error" rule was set after a first daily run that kept drifts
  which then lost. The forward test is the clean measure.

### What would improve it next
- **Broker-verified data:** run `ops\broker_audit.ps1`, then repeat the run.
- **Inputs beyond price:** option-chain OI/PCR and IV skew, FII/DII flows, GIFT Nifty and the US close.
- **Order-book data:** the Atlas's microstructure rows (order-book imbalance, microprice, VPIN, Kyle's lambda) need recorded depth and trades, and they are the Atlas's likeliest source of short-horizon direction edge.
- **A forward test** of the daily Consensus 75 % filter from October 2026, on data this run has never seen.
- **Sell volatility the way a desk does:** set a realised-volatility
  forecast against the option's implied vol and hedge the delta
  (`strategies/vol_premium.hpp`), on real option prices rather than VIX-priced
  ones.
