"""research/signals/score_live.py -- score the live basket-alpha forecasts (FYERS live or simulation).

Reads <live dir>/basket_alpha/forecasts_*.csv (written every 5 s by live/basket_alpha.hpp) and
compares each forecast with what the future then did. The realised move is measured from the NEXT
logged row (5 s later, a stricter delay than the 1 s the model was fitted with) over 10 s / 60 s.

    python3 research/signals/score_live.py data/live/basket_alpha
"""
from __future__ import annotations

import glob
import os
import sys

import numpy as np
import pandas as pd


def main(d: str) -> None:
    files = sorted(glob.glob(os.path.join(d, "forecasts_*.csv")))
    if not files:
        print("no forecasts_*.csv under", d)
        return
    print(f"{'day file':<24} {'rows':>6} | {'10 s: R2':>9} {'IC':>7} {'hit':>5} | {'60 s: R2':>9} {'IC':>7} {'hit':>5}")
    tot = {10: [], 60: []}
    for f in files:
        df = pd.read_csv(f).drop_duplicates("epoch_s").set_index("epoch_s").sort_index()
        lf = np.log(df["future"].where(df["future"] > 0))
        out = [os.path.basename(f), len(df)]
        for h in (10, 60):
            start = lf.reindex(df.index + 5).to_numpy()
            end = lf.reindex(df.index + 5 + h).to_numpy()
            y = (end - start) * 1e4
            p = df[f"f{h}_bp"].to_numpy()
            ok = np.isfinite(y) & np.isfinite(p)
            if ok.sum() < 30:
                out += [np.nan, np.nan, np.nan]
                continue
            r2 = 1 - np.sum((y[ok] - p[ok]) ** 2) / np.sum(y[ok] ** 2)
            ic = np.corrcoef(p[ok], y[ok])[0, 1]
            nz = ok & (y != 0)
            hit = np.mean(np.sign(p[nz]) == np.sign(y[nz]))
            out += [r2, ic, hit]
            tot[h].append((p[ok], y[ok]))
        print(f"{out[0]:<24} {out[1]:>6} | {out[2] * 100:>8.3f}% {out[3]:>+7.4f} {out[4] * 100:>4.0f}% | "
              f"{out[5] * 100:>8.3f}% {out[6]:>+7.4f} {out[7] * 100:>4.0f}%")
    for h in (10, 60):
        if tot[h]:
            p = np.concatenate([a for a, _ in tot[h]])
            y = np.concatenate([b for _, b in tot[h]])
            print(f"all days {h:>2} s: R2 {(1 - np.sum((y - p) ** 2) / np.sum(y ** 2)) * 100:+.3f}%, "
                  f"IC {np.corrcoef(p, y)[0, 1]:+.4f}, n {len(y)}  (fitted on Drive ticks: IC ~+0.05 to +0.17)")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "data/live/basket_alpha")
