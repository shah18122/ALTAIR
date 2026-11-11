"""research/signals/replay_events.py -- Drive tick prints as one time-ordered stream for basket_replay.

    python3 research/signals/replay_events.py FUT_DIR EQ_DIR CONFIG.csv FROM TO > events.txt

Days FROM..TO (YYYYMMDD, inclusive). The future is the front month (nearest expiry on or after the
day); the stocks are the config's beta_* names, in its order. Source drive-ticks, not FYERS.
"""
from __future__ import annotations

import glob
import os
import sys

import pandas as pd

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import load  # noqa: E402


def main(fut_dir, eq_dir, cfg, d0, d1):
    stocks = [l.split(",")[0][5:] for l in open(cfg) if l.startswith("beta_")]
    futs = []
    for f in glob.glob(os.path.join(fut_dir, "BANKNIFTY*FUT.csv")):
        df = load.read_ticks(f)
        df = df[(df["d"] >= d0) & (df["d"] <= d1 + 100)]
        if len(df):
            futs.append((int(load.read_ticks(f)["d"].max()), df))
    eq = {s: load.read_ticks(os.path.join(eq_dir, s + ".csv")) for s in stocks}
    eq = {s: df[(df["d"] >= d0) & (df["d"] <= d1)] for s, df in eq.items()}
    days = sorted(set.intersection(*[set(df["d"].unique()) for df in eq.values()]))
    out = sys.stdout
    for d in days:
        cands = [(exp, df) for exp, df in futs if exp >= d and (df["d"] == d).any()]
        if not cands:
            continue
        fd = min(cands, key=lambda c: c[0])[1]
        fd = fd[fd["d"] == d]
        parts = [pd.DataFrame({"s": fd["s"], "j": -1, "px": fd["px"], "q": fd["q"]})]
        for j, s in enumerate(stocks):
            e = eq[s][eq[s]["d"] == d]
            parts.append(pd.DataFrame({"s": e["s"], "j": j, "px": e["px"], "q": e["q"]}))
        ev = pd.concat(parts).sort_values("s", kind="stable")
        ev["s"] = ev["s"] + load.T0
        out.write(f"D,{d}\n")
        ev.to_csv(out, header=False, index=False)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]), int(sys.argv[5]))
