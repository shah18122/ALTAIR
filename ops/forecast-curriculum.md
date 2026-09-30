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

All 21 tracks take about an hour on four cores; the 1-minute tracks are most
of it (`--only daily`: about 2 minutes).

Options:
- `--first-days N` (default 3).
- `--cap-days N`: stop doubling once a block would exceed N days, then walk on in N-day blocks. 0 (the default) is pure doubling.
- `--jobs N`: how many tracks run in parallel. The default is the machine's core count.
- `--other-cost-bp X`.
- `--only TEXT`: run only tracks whose name contains it, e.g. `--only 5m`.
- `--no-bands`, `--no-log`.

## Output (`data/verified/`, git-ignored)

| File | What |
|---|---|
| `forecast_curriculum.xlsx` | **Summary**: direction, every model on every track (accuracy, 95 % interval, p against a coin and against the best constant call, both Bonferroni-corrected, coverage, Brier, price skill against the random walk, trades that clear cost, verdict). **Bands**: every band model on every track (hit rate, width, interval score, skill against the constant band). **Frontier**: accuracy of each model's most confident 0.1 % … 100 % of calls, and the largest slice still at or above 80 %. **Atlas coverage**: every Model Atlas row, and what the run did with it. **Data** · **Method** · one **sheet per track** with its learning curves. |
| `forecast_curriculum.txt` | The summary as text. |
| `forecast_log/<track>.csv` | Every forecast on the daily and hourly tracks: time, stage, days learned, model, last price, next price, forecast price, P(up), call, what moved, RIGHT/WRONG/FLAT, net bp if traded. The 1–15-minute logs would run to gigabytes and are not written. |

## Tracks

| Track | Decision | Outcome |
|---|---|---|
| NIFTY, BANKNIFTY, NIFTY FUT, INDIA VIX daily | 15:30, from that day's bar | next trading day's close |
| … + VIX fc (daily and hourly index tracks) | the same, plus the INDIA VIX model's own forecast as a feature | the same |
| NIFTY, BANKNIFTY, INDIA VIX hourly | close of each of a full day's first six hourly bars (10:15 … 15:15) | the next hourly close (no overnight hour) |
| NIFTY, BANKNIFTY, INDIA VIX 15m, 5m, 1m | close of every bar of a full 09:15–15:30 session but the last | the next bar's close (no overnight bar) |

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
31 direction forecasters and 9 ensembles:

| Family | Models |
|---|---|
| Baselines | Coin flip (seeded), Always majority, Momentum, Mean reversion |
| Statistical alpha | Momentum (tuned lookback), Mean reversion (z-score band, abstains inside it), Pairs (cointegration, NIFTY against BANKNIFTY and back) |
| Classical | Logistic regression (L2 tuned), Ridge regression (λ tuned), SVM (RBF), k-nearest neighbours (k tuned), Autoencoder + logistic |
| Trees | Decision tree, Random forest (depth tuned), Gradient boosting |
| Neural | MLP (backprop), LSTM, GRU, causal Transformer, CNN (random dilated kernels, ROCKET-style), DQN (reinforcement, as a contextual bandit) |
| Time series | AR(2), ARMA(1,1), Seasonal AR (SARIMA), VAR(1), Kalman filter (drift), Ornstein–Uhlenbeck, Markov chain, Hidden Markov model, Hurst regime switch |
| Regime | k-means regimes |
| Ensembles | Vote, Champion, Hedge, **Stack** (a logistic regression on every model's past out-of-sample calls), and the filters that abstain unless confident: **Stack (confident third / 10 % / 2 %)**, **Consensus 75 % / 90 %** |

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

Tuning uses the last quarter of each training window only. Truncations are
visible, and each model states its own in the stage notes:
- The transformer trains by finite differences with 400 SGD steps a stage, so it is the least-trained network.
- Row caps on what a model learns from:

  | Model | Rows |
  |---|---|
  | SVM | the window's latest 1,500 |
  | kNN | 20,000 |
  | HMM (Baum–Welch) | 50,000 |
  | DQN (replay) | 4,096 |
  | GARCH / GJR / EGARCH (fit) | the latest 100,000 returns, then filtered over all of them |

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
* **Frontier**: each probabilistic model's calls are ranked by confidence
  |P(up) − 0.5|. The report gives the accuracy of the top 0.1 %, 0.5 %,
  1 % … 100 %, and the largest slice still at or above 80 % on 30+ calls.
  - The ranking uses the whole test period's confidences but never its
    outcomes. The thresholds are therefore not ones a trader could have
    known in advance.
  - The ex-ante versions are the Stack (confident …) and Consensus
    ensembles, which are scored like any other model.

## Results

Run on the dataset pushed on 2026-09-29.
- **Scope:** 21 tracks (NIFTY, BANKNIFTY and INDIA VIX at 1m, 5m, 15m, 60m and 1d; NIFTY FUT daily; five "+ VIX fc" tracks) and 4.13 million forecast bars, about 134 million model decisions in all.
- **Models:** 40 direction models and 14 band models per track.
- **Run:** 36 minutes with 4 parallel jobs, with **zero look-ahead refusals** in both curricula.
- **Tests:** 840 direction model-track tests, so both significance tests are Bonferroni-corrected by 840.

The full tables are in the Summary, Bands and Frontier sheets.

### Direction: 80 % is not there

The **Frontier** sheet ranks each model's calls by confidence. It then
reports the largest top slice that is right at least 80 % of the time on at
least 30 calls.

No such slice exists on any NIFTY, BANKNIFTY or NIFTY FUT track, at any
timeframe, for any model.

The only one anywhere is INDIA VIX 1m: Stack (confident 10 %), top 0.1 %, at
81.6 % on 76 calls (Wilson low 71.4 %). It isn't a result, for four reasons:
- It is one call every 37 trading days.
- Its accuracy equals the share of up-moves on those same bars.
- VIX cannot be traded.
- It is the best of 5,823 slices tried.

The best honest direction numbers, counting only slices of at least 100 calls:

| Track | Most confident 1 % of calls | Most confident 5 % | Best model that calls ≥ 90 % of bars |
|---|---|---|---|
| NIFTY 1m / 5m / 15m / hourly | 54.9 / 56.3 / 55.4 / 56.7 % | 54.6 / 54.5 / 55.3 / 56.0 % | 51.4–51.8 % |
| BANKNIFTY 1m / 5m / 15m / hourly | 55.1 / 57.0 / 56.7 / 60.2 % | 55.8 / 55.8 / 55.3 / 53.3 % | 51.0–52.6 % |
| NIFTY / BANKNIFTY / FUT daily | too few calls | 61.5 / 60.4 / 61.2 % (about 140 calls each) | 54.0 / 53.0 / 53.9 %; "always up" is 53.4 / 53.2 / 52.2 % |
| INDIA VIX 1m / 5m / 15m / hourly / daily | 75.9 / 61.6 / 61.5 / 66.3 / — % | 72.5 / 62.4 / 60.1 / 60.6 / 71.7 % | 54.5–60.9 % (forecastable, not tradable) |

**What is statistically real is small.** On the index tracks, 90 model-track
pairs beat both a coin and the best constant call after the 840-test
correction. All of them are on 1m, 5m or 15m bars, at 50.3–54.9 %. None is on
hourly or daily bars.

The strongest is 1-minute mean reversion: on BANKNIFTY 1m the z-score band is
right 53.7 % of the time over 82,871 calls. Traded, it loses 3.3 bp a trade
after costs (47,601 trades, t = −115). It is the index's stale-price bounce,
not an edge.

### Range: 80 %, on every bar, at every timeframe

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
  have the best accuracy on the calls they make on every track. They make few
  calls, and none reaches 80 %.
- **Consensus 75 % on the daily index tracks** is right 54–58 % of the time
  on 21–27 % of days. That is never significant against the best constant
  call on those days.
- **Adding the VIX model's own forecast as an input** moves the daily
  Consensus 75 % by up to 2 points:
  - NIFTY: 55.9 → 58.0 %.
  - BANKNIFTY: 55.5 → 56.3 %.
  - FUT: 54.2 → 55.6 %.
  - It lowers Vote, Hedge and Stack slightly.
  - None of these moves is significant.

### What would improve it next
- **Broker-verified data:** run `ops\broker_audit.ps1`, then repeat the run.
- **Inputs beyond price:** option-chain OI/PCR and IV skew, FII/DII flows, GIFT Nifty and the US close.
- **A forward test** of the daily Consensus 75 % filter from October 2026, on data this run has never seen.
- **Use the bands where they bite:** strike selection and position sizing for option selling, where a calibrated 80 % range is directly the product.
