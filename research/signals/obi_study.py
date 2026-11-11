"""research/signals/obi_study.py -- does the FYERS order book predict BANKNIFTY futures? Walk-forward by day.

Input: the recorder's decoded books, one folder per day:
    altair_store decode --day YYYY-MM-DD --ticks     ->  data/store/ticks/<day>/books.csv
(columns recv_time,feed_time,fyers,levels,bids,asks,source; levels "price:qty:orders;...").
FYERS data only (source fyers_live).

Per day, on a 1-second grid 09:20-15:20 (book state at the END of each second):
  l1        (B1 - A1) / (B1 + A1)                       touch imbalance
  obi5w     weighted 5-level imbalance, w_i = exp(-0.25 (i-1))   (as book/flow.hpp)
  micro     microprice - mid, bp
  ofi10     multi-level Cont-Kukanov-Stoikov OFI over the last 10 s / mean touch depth
  ofi60     same over 60 s
  basket    equal-weighted constituents' mid return over the last 10 s minus the future's (books
            of the bank stocks the recorder carries every day)
Target: mid of the future h seconds after t+1 s vs mid at t+1 s (bp), h = 10, 60, 300.
Trading test: cross the spread when |forecast| > half the actual spread at t+1 + fees (FEES bp a side).

    python3 research/signals/obi_study.py data/store/ticks report.md [BANKNIFTY]
"""
from __future__ import annotations

import glob
import os
import sys

import numpy as np
import pandas as pd

T0 = 9 * 3600 + 15 * 60
N = 6 * 3600 + 15 * 60
START, STOP, SAMPLE = 5 * 60, 6 * 3600 + 5 * 60, 1
HORIZONS = (10, 60, 300)
FEES = 3.0  # bp a side, futures, 2026 STT averaged over both sides + exchange + stamp + GST
BANKS = ["HDFCBANK", "ICICIBANK", "SBIN", "KOTAKBANK", "AXISBANK", "INDUSINDBK", "BANKBARODA", "AUBANK",
         "FEDERALBNK", "IDFCFIRSTB", "PNB", "CANBK"]
W = np.exp(-0.25 * np.arange(5))


def parse_side(s: str):
    p, q = np.zeros(5), np.zeros(5)
    if isinstance(s, str):
        for k, lv in enumerate(s.split(";")[:5]):
            a = lv.split(":")
            p[k], q[k] = float(a[0]), float(a[1])
    return p, q


def day_grid(books: pd.DataFrame):
    """books of ONE symbol, time ordered -> per-second arrays."""
    t = books["recv_time"].str.slice(11, 19)
    sec = (t.str.slice(0, 2).astype(int) * 3600 + t.str.slice(3, 5).astype(int) * 60 + t.str.slice(6, 8).astype(int)
           - T0).to_numpy()
    out = {k: np.full(N, np.nan) for k in ("mid", "spr", "l1", "obi5w", "micro", "depth")}
    ofi = np.zeros(N)
    prev = None
    for s, bs, as_ in zip(sec, books["bids"].to_numpy(), books["asks"].to_numpy()):
        if s < 0 or s >= N:
            continue
        bp, bq = parse_side(bs)
        ap, aq = parse_side(as_)
        if not (bp[0] > 0 and ap[0] > bp[0] and bq[0] > 0 and aq[0] > 0):
            prev = None
            continue
        mid = 0.5 * (bp[0] + ap[0])
        out["mid"][s] = mid
        out["spr"][s] = (ap[0] - bp[0]) / mid * 1e4
        out["l1"][s] = (bq[0] - aq[0]) / (bq[0] + aq[0])
        out["obi5w"][s] = (W @ bq - W @ aq) / (W @ bq + W @ aq)
        out["micro"][s] = ((ap[0] * bq[0] + bp[0] * aq[0]) / (bq[0] + aq[0]) - mid) / mid * 1e4
        out["depth"][s] = 0.5 * (bq[0] + aq[0])
        if prev is not None:
            pbp, pbq, pap, paq = prev
            e = 0.0
            for k in range(5):
                e += (bq[k] if bp[k] >= pbp[k] else 0) - (pbq[k] if bp[k] <= pbp[k] else 0) \
                     - (aq[k] if ap[k] <= pap[k] else 0) + (paq[k] if ap[k] >= pap[k] else 0)
            ofi[s] += e
        prev = (bp, bq, ap, aq)
    for k in out:
        out[k] = pd.Series(out[k]).ffill().to_numpy()
    out["ofi"] = ofi
    return out


def load_day(path: str, und: str):
    want_fut = f"NSE:{und}"
    chunks = []
    for ch in pd.read_csv(path, usecols=["recv_time", "fyers", "bids", "asks"], chunksize=2_000_000, dtype=str):
        m = ch["fyers"].str.startswith(want_fut) & ch["fyers"].str.endswith("FUT")
        m |= ch["fyers"].isin([f"NSE:{b}-EQ" for b in BANKS])
        chunks.append(ch[m])
    df = pd.concat(chunks)
    futs = df[df["fyers"].str.endswith("FUT")]["fyers"].value_counts()
    if futs.empty:
        return None
    fut = futs.index[0]  # the most active future of the day = the near month
    g = {"fut": day_grid(df[df["fyers"] == fut])}
    for b in BANKS:
        sub = df[df["fyers"] == f"NSE:{b}-EQ"]
        if len(sub) > 1000:
            g[b] = day_grid(sub)
    return fut, g


def lag(x, k):
    y = np.full_like(x, np.nan)
    y[k:] = x[:-k]
    return y


def lead(x, k):
    y = np.full_like(x, np.nan)
    y[:-k] = x[k:]
    return y


def rsum(x, k):
    c = np.cumsum(x)
    return c - lag(c, k)


def main(root: str, out: str | None, und: str = "BANKNIFTY"):
    days = []
    for p in sorted(glob.glob(os.path.join(root, "*", "books.csv"))):
        r = load_day(p, und)
        if r is None:
            continue
        days.append((os.path.basename(os.path.dirname(p)), r[0], r[1]))
        print("loaded", days[-1][0], r[0], "stocks with books:", [k for k in r[1] if k != "fut"], flush=True)
    if len(days) < 3:
        print("need at least 3 recorded days")
        return
    stocks = sorted(set.intersection(*[set(g) - {"fut"} for _, _, g in days]))
    T = np.arange(START, STOP, SAMPLE)
    X, Y, SPR, Rb = {}, {h: [] for h in HORIZONS}, [], []
    names = ["l1", "obi5w", "micro", "ofi10", "ofi60", "basket"]
    for k in names:
        X[k] = []
    for _, _, g in days:
        f = g["fut"]
        lm = np.log(f["mid"])
        dep = pd.Series(f["depth"]).rolling(300, min_periods=1).mean().to_numpy()
        X["l1"].append(f["l1"][T])
        X["obi5w"].append(f["obi5w"][T])
        X["micro"].append(f["micro"][T])
        X["ofi10"].append((rsum(f["ofi"], 10) / dep)[T])
        X["ofi60"].append((rsum(f["ofi"], 60) / dep)[T])
        if stocks:
            sr = np.mean([np.log(g[s]["mid"]) - lag(np.log(g[s]["mid"]), 10) for s in stocks], 0)
            X["basket"].append(((sr - (lm - lag(lm, 10))) * 1e4)[T])
        else:
            X["basket"].append(np.full(len(T), np.nan))
        for h in HORIZONS:
            Y[h].append(((lead(lm, 1 + h) - lead(lm, 1)) * 1e4)[T])
        SPR.append(lead(f["spr"], 1)[T])
    X = {k: np.array(v) for k, v in X.items()}
    SPR = np.array(SPR)
    D = len(days)
    lines = [f"# Order-book signals on {und} futures (FYERS books), walk-forward by day\n",
             f"{D} recorded days {days[0][0]}..{days[-1][0]}; constituents with books every day: "
             f"{', '.join(stocks) or 'none'}. Rows every {SAMPLE} s 09:20-15:20; target = mid move after a 1-s delay; "
             f"each day scored by an OLS fitted on the EARLIER days only (day 1 never scored). "
             f"Trading test crosses the spread and pays {FEES:.0f} bp fees a side.\n"]
    for h in HORIZONS:
        y = np.array(Y[h])
        lines.append(f"\n## Next {h} s mid move (bp, std {np.nanstd(y):.2f}; mean spread {np.nanmean(SPR):.2f} bp)\n")
        lines.append("| signal | OOS R2 vs no change | mean daily IC | days IC>0 | trades beyond spread+fees | net bp/trade |")
        lines.append("|---|---|---|---|---|---|")
        for s in [[k] for k in names] + [names]:
            Xd = np.stack([X[k] for k in s], -1)
            pred = np.full_like(y, np.nan)
            for i in range(1, D):
                Xtr, ytr = Xd[:i].reshape(-1, len(s)), y[:i].reshape(-1)
                ok = np.isfinite(Xtr).all(1) & np.isfinite(ytr)
                if ok.sum() < 1000:
                    continue
                A = np.column_stack([np.ones(ok.sum()), Xtr[ok]])
                b = np.linalg.lstsq(A, ytr[ok], rcond=None)[0]
                pred[i] = np.column_stack([np.ones(len(T)), Xd[i]]) @ b
            p, a, sp = pred[1:], y[1:], SPR[1:]
            ok = np.isfinite(p) & np.isfinite(a)
            r2 = 1 - np.sum((a[ok] - p[ok]) ** 2) / np.sum(a[ok] ** 2)
            ics = [np.corrcoef(p[i][ok[i]], a[i][ok[i]])[0, 1] for i in range(len(p)) if ok[i].sum() > 100]
            cost = sp + 2 * FEES
            tr = ok & np.isfinite(sp) & (np.abs(p) > cost)
            net = (np.sign(p[tr]) * a[tr] - cost[tr]).mean() if tr.any() else np.nan
            nm = "+".join(s) if len(s) < len(names) else "ALL"
            lines.append(f"| {nm} | {r2 * 100:+.3f}% | {np.mean(ics):+.4f} | {np.mean(np.array(ics) > 0) * 100:.0f}% | "
                         f"{int(tr.sum())} | {net:+.2f} |")
    text = "\n".join(lines) + "\n"
    print(text)
    if out:
        open(out, "w").write(text)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None, sys.argv[3] if len(sys.argv) > 3 else "BANKNIFTY")
