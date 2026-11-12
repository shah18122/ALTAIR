#!/usr/bin/env python3
"""Pre-registered fund-flow tests F2/F3 (FLOWS_HYPOTHESES.md) on the P6 pair trades.

    python research/equity/flows.py --eq EQ_DIR [--fpi fpi_daily.csv]

Without --fpi it runs F2a (calendar flow days only); with it, F2b. Volume from Drive 5-minute
bars (source drive-ticks, NOT FYERS). Writes results/flows_<f2a|f2b>_*.csv.
"""
from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np
import pandas as pd

from load import load_stocks
from pairs import GROUPS

HERE = Path(__file__).resolve().parent
NEWS_HI, NEWS_LO, SECTOR_AV, FPI_Q = 3.0, 1.5, 1.5, 0.90


def calendar_days(days):
    """Last session of Feb/May/Aug/Nov (MSCI) and Mar/Sep (NSE reconstitution), plus the session before."""
    flag = np.zeros(len(days), bool)
    for i, d in enumerate(days):
        last = i == len(days) - 1 or days[i + 1].month != d.month
        if last and d.month in (2, 3, 5, 8, 9, 11):
            flag[i] = True
            if i > 0:
                flag[i - 1] = True
    return flag


def fpi_days(days, path):
    f = pd.read_csv(path, parse_dates=["date"])
    f = f.sort_values("date")
    # reporting date -> trade date: each value belongs to the previous row's session (amendment)
    s = pd.Series(f.fpi_equity_net_cr.abs().to_numpy()[1:], index=f.date.dt.date.to_numpy()[:-1])
    thr = s.rolling(250, min_periods=250).quantile(FPI_Q).shift(1)
    hot = set(s.index[(s > thr).to_numpy()])
    return np.array([d in hot for d in days])


def cluster_ols(y, X, g):
    """OLS with standard errors clustered by g. Returns coef, t."""
    X = np.column_stack([np.ones(len(y)), X])
    XtX = np.linalg.inv(X.T @ X)
    b = XtX @ X.T @ y
    e = y - X @ b
    meat = np.zeros((X.shape[1], X.shape[1]))
    for k in np.unique(g):
        m = g == k
        u = X[m].T @ e[m]
        meat += np.outer(u, u)
    G, n, p = len(np.unique(g)), len(y), X.shape[1]
    V = XtX @ meat @ XtX * (G / (G - 1)) * ((n - 1) / (n - p))
    return b, b / np.sqrt(np.diag(V))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--eq", required=True)
    ap.add_argument("--fpi")
    a = ap.parse_args()
    tag = "f2b" if a.fpi else "f2a"
    names = sorted({n for g in GROUPS.values() for n in g})
    days, syms, A = load_stocks(a.eq, ["2020", "2022", "2023"], names)
    vol = A["vol"].sum(axis=1)  # (D, K)
    vol[~A["present"]] = np.nan
    di = {d: i for i, d in enumerate(days)}
    flowday = calendar_days(days)
    if a.fpi:
        flowday |= fpi_days(days, a.fpi)

    tr = pd.concat([pd.read_csv(HERE / "results" / f"pairs_{s}_trades.csv", parse_dates=["entry", "exit"])
                    .assign(split=s) for s in ("dev", "holdout")], ignore_index=True)
    rows = []
    for t in tr.itertuples():
        e = di[t.entry.date()]
        d = e - 1
        if d - 22 < 0:
            continue
        ka, kb = (syms.index(x) for x in t.pair.split("/"))
        base = np.nanmedian(vol[d - 22:d - 2], axis=0)[[ka, kb]]
        av = vol[d - 2:d + 1][:, [ka, kb]] / base  # (3, 2)
        news = bool((((av[:, 0] >= NEWS_HI) & (av[:, 1] < NEWS_LO)) |
                     ((av[:, 1] >= NEWS_HI) & (av[:, 0] < NEWS_LO))).any())
        flow = (not news) and bool(flowday[d - 2:d + 1].any())
        sector = (not news) and (not flow) and bool(((av >= SECTOR_AV).all(axis=1)).any())
        cls = "NEWS" if news else "FLOW" if flow else "SECTOR" if sector else "OTHER"
        asym = float(np.nanmax(np.abs(np.log(av[:, 0]) - np.log(av[:, 1]))))
        rows.append(dict(group=t.group, pair=t.pair, split=t.split, entry=t.entry.date(), net=t.net,
                         cls=cls, asym=asym, month=t.entry.strftime("%Y-%m")))
    df = pd.DataFrame(rows)
    res = HERE / "results"
    df.to_csv(res / f"flows_{tag}_trades.csv", index=False, float_format="%.6f")

    def table(x):
        return x.groupby("cls").net.agg(trades="size", net_bp=lambda v: v.mean() * 1e4)

    print(f"== {tag}: {len(df)} trades classified (of {len(tr)}) ==")
    print(table(df).to_string())
    fn = df[df.cls.isin(["FLOW", "NEWS"])]
    b, t = cluster_ols(fn.net.to_numpy(), (fn.cls == "FLOW").to_numpy(float), fn.month.to_numpy())
    news_mean, all_mean = df.net[df.cls == "NEWS"].mean(), df.net.mean()
    fa = df[np.isfinite(df.asym)]
    b3, t3 = cluster_ols(fa.net.to_numpy(), fa.asym.to_numpy(), fa.month.to_numpy())
    summ = dict(test=tag, trades=len(df), flow_minus_news_bp=b[1] * 1e4, t_cluster=t[1],
                news_bp=news_mean * 1e4, all_bp=all_mean * 1e4,
                f2_pass=bool(b[1] > 0 and t[1] >= 2 and news_mean < all_mean),
                f3_slope_bp=b3[1] * 1e4, f3_t=t3[1], f3_pass=bool(b3[1] < 0 and t3[1] <= -2))
    for k, v in summ.items():
        print(f"{k:20s} {v}")
    bysplit = df.groupby(["split", "cls"]).net.agg(trades="size", net_bp=lambda v: v.mean() * 1e4)
    banks = df[df.group == "banks"].groupby("cls").net.agg(trades="size", net_bp=lambda v: v.mean() * 1e4)
    print("-- by split --\n" + bysplit.to_string())
    print("-- banks (F4) --\n" + banks.to_string())
    pd.DataFrame([summ]).to_csv(res / f"flows_{tag}_summary.csv", index=False, float_format="%.4f")
    bysplit.to_csv(res / f"flows_{tag}_bysplit.csv", float_format="%.4f")
    banks.to_csv(res / f"flows_{tag}_banks.csv", float_format="%.4f")


if __name__ == "__main__":
    main()
