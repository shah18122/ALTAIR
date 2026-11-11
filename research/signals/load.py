"""research/signals/load.py -- Drive tick files -> one 1-second grid per day.

SOURCE: Smit's Google Drive tick files (source "drive-ticks", NOT FYERS; never
merged into FYERS series). Rows "YYYYMMDD,HH:MM:SS,price,qty,oi", no header.
Futures are ~1 print every 1-2 s; equities ~1 snapshot a second.

Grid: 09:15:00 .. 15:29:59 IST, one slot a second. Per slot:
  last price (forward filled; NaN before the day's first print), quantity
  traded in the slot, tick-rule signed quantity (futures only), last OI.
The front future for a day is the contract with the nearest expiry on or
after that day (expiry = the last day in its file).

    python3 research/signals/load.py FUT_DIR EQ_DIR OUT.npz SYM1,SYM2,...
"""
from __future__ import annotations

import glob
import os
import sys

import numpy as np
import pandas as pd

T0 = 9 * 3600 + 15 * 60
N = 6 * 3600 + 15 * 60  # 22,500 one-second slots


def read_ticks(path: str) -> pd.DataFrame:
    df = pd.read_csv(path, header=None, names=["d", "t", "px", "q", "oi"], dtype={"d": np.int32, "t": str},
                     engine="c")
    hh = df["t"].str.slice(0, 2).astype(np.int32)
    mm = df["t"].str.slice(3, 5).astype(np.int32)
    ss = df["t"].str.slice(6, 8).astype(np.int32)
    df["s"] = hh * 3600 + mm * 60 + ss - T0
    df = df[(df["s"] >= 0) & (df["s"] < N) & (df["px"] > 0)]
    return df[["d", "s", "px", "q", "oi"]]


def grid_day(g: pd.DataFrame, signed: bool):
    s = g["s"].to_numpy()
    px = g["px"].to_numpy(dtype=np.float64)
    q = g["q"].to_numpy(dtype=np.float64)
    last = np.full(N, np.nan)
    last[s] = px  # later rows in a slot overwrite earlier: last price of the slot
    last = pd.Series(last).ffill().to_numpy()
    vol = np.bincount(s, weights=q, minlength=N)
    out = {"px": last, "vol": vol}
    if signed:
        # tick rule: up-tick buyer, down-tick seller, zero-tick keeps the last sign
        dp = np.diff(px, prepend=px[0])
        sg = np.sign(dp)
        sg = pd.Series(np.where(sg == 0, np.nan, sg)).ffill().fillna(0).to_numpy()
        out["sv"] = np.bincount(s, weights=sg * q, minlength=N)
        oi = np.full(N, np.nan)
        oi[s] = g["oi"].to_numpy(dtype=np.float64)
        out["oi"] = pd.Series(oi).ffill().to_numpy()
        out["n"] = np.bincount(s, minlength=N).astype(np.float64)
    return out


def main(fut_dir: str, eq_dir: str, out: str, syms: list[str]) -> None:
    futs = {}
    for f in sorted(glob.glob(os.path.join(fut_dir, "BANKNIFTY*FUT.csv"))):
        df = read_ticks(f)
        futs[os.path.basename(f)] = (int(df["d"].max()), df)
    days_by_front = {}
    all_days = sorted(set().union(*[set(df["d"].unique()) for _, df in futs.values()]))
    for d in all_days:
        cands = [(exp, name) for name, (exp, df) in futs.items() if exp >= d and (df["d"] == d).any()]
        if cands:
            days_by_front[d] = min(cands)[1]
    eq = {s: read_ticks(os.path.join(eq_dir, s + ".csv")) for s in syms}
    eq_g = {s: dict(tuple(df.groupby("d"))) for s, df in eq.items()}
    days = [d for d in sorted(days_by_front) if all(d in eq_g[s] for s in syms)]
    D, K = len(days), len(syms)
    F = {k: np.full((D, N), np.nan) for k in ("px", "vol", "sv", "oi", "n")}
    P = np.full((D, K, N), np.nan, dtype=np.float32)
    V = np.zeros((D, K, N), dtype=np.float32)
    expiry = np.zeros(D, dtype=np.int32)
    for i, d in enumerate(days):
        name = days_by_front[d]
        expiry[i] = futs[name][0]
        fd = futs[name][1]
        gd = grid_day(fd[fd["d"] == d], signed=True)
        for k in F:
            F[k][i] = gd[k]
        for j, s in enumerate(syms):
            ge = grid_day(eq_g[s][d], signed=False)
            P[i, j] = ge["px"]
            V[i, j] = ge["vol"]
    np.savez_compressed(out, days=np.array(days), expiry=expiry, syms=np.array(syms), fpx=F["px"], fvol=F["vol"],
                        fsv=F["sv"], foi=F["oi"], fn=F["n"], spx=P, svol=V)
    print(f"{D} days, {K} stocks -> {out}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4].split(","))
