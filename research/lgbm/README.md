# LightGBM on microstructure features, with a triple-barrier meta-label

Asked 2026-11-27 (Smit): "LightGBM on microstructure features (order flow
imbalance, queue imbalance, microprice, aggressor flow, lead-lag of HDFC
Bank/ICICI vs Bank Nifty futures), labels net of costs, meta-labeling on top
(triple barrier). Keep DeepLOB/TFT only if they beat LightGBM after costs."

## Pre-registration (written before the first run, not changed after)

**Data.** Drive ticks (source `drive-ticks`, NOT FYERS, see
docs/DATA_POLICY.md): front-month BANKNIFTY future plus 12 bank stocks on the
1-second grid from `research/signals/load.py`. Dev = 2020 + 2022, holdout =
2023 (scored once). No bhavcopy.

What the Drive data can and cannot give:

| Feature asked for | Here? |
|---|---|
| Aggressor flow | Yes, tick-rule signed volume (10 s, 60 s, 300 s) and trade intensity |
| HDFCBANK / ICICIBANK lead-lag vs the future | Yes, each stock's 5/10/30 s move minus the future's, plus the beta-weighted basket gap and mispricing |
| OFI, queue imbalance, microprice | **No.** The Drive files are trade prints with no bid/ask. They exist only on the recorder's FYERS book tapes on the server (about 10 clean sessions), far too few for a dev + holdout test. `research/signals/obi_study.py` is the test for those once ~60 sessions exist. |

Other features: own returns (10/60/300 s), breadth, OI build-up, realised vol
(5 min, 30 min, day so far), time of day, days to expiry.

**Rows and timing.** One row every 5 s, 09:20-15:20. Everything in a row is
known at second t. Entry is the future's price at t+1 s (one-second latency).

**Primary model.** LightGBM 3-class on the fixed-horizon label net of costs:
+1 if the future's return over (t+1, t+1+h] > cost, -1 if < -cost, else 0.
h = 60 s and 300 s, cost = 6 bp round trip (2026 futures STT 0.05% sell side
+ exchange + stamp + GST + ~0.5 bp spread). Trade the side whose probability is
above 0.5.

**Exit (triple barrier).** Upper/lower barrier at max(cost, 1.5 x the expected
move over h from the last 5 min's realised vol), vertical barrier at h. A
trade's P&L is where it exits, minus 6 bp.

**Meta-label.** A second LightGBM (binary) on the same features plus the
primary's probabilities, trained only on past primary trades, label = that
trade's net P&L > 0. Take the trade only if p > 0.5.

**Walk-forward.** Expanding window, refit every 20 trading days, the first 60
days never scored. Models see only days before the day they score.

**Baseline.** "No change" = never trade = 0 bp. A model has to beat it.

**Pass bar** (per horizon, primary and primary+meta separately): in dev AND in
holdout, mean net bp per trade > 0, with t >= 2 on daily P&L, at least 100
trades, and holdout positive in each third. Meta-label is kept only if it
raises mean net per trade AND total net over the primary in both splits.
4 tests (2 horizons x 2 variants): a pass needs t >= 2.5 (Bonferroni-ish).

**DeepLOB / TFT.** DeepLOB needs a limit order book (none in the Drive data;
~10 FYERS sessions). TFT would run on the same features; it is tried only if
LightGBM passes, since a model that cannot beat "no change" leaves nothing for
a heavier model to beat after costs.

## Run (on Smit's PC, where the ticks are)

    python research/signals/load.py "<Drive>/NSE Contract Futures 2020" "<Drive>/NSE Equities (Tick)/2020" bnf2020.npz HDFCBANK,ICICIBANK,SBIN,KOTAKBANK,AXISBANK,INDUSINDBK,BANKBARODA,AUBANK,FEDERALBNK,IDFCFIRSTB,PNB,BANDHANBNK
    (same for 2022 and 2023 from "NSE Contract Futures 2022-23")
    pip install lightgbm
    python research/lgbm/lgbm_study.py --dev bnf2020.npz bnf2022.npz --holdout bnf2023.npz --out research/lgbm/results

Results: `results/summary.csv`, `results/RESULTS.md`.
