#!/usr/bin/env python3
"""Pairs-premium decay, 2015-2026, on FYERS daily candles only (paper: "The disappearing pairs premium").

    python research/equity/decay.py --daily <altair-data>/research/flows/fyers_daily

Rule P6 (PAIRS_HYPOTHESES.md) on 62 same-sector pairs (TATAMOTORS left out: FYERS's file is the
post-demerger TMPV listing): z of log(A/B) over 20 closes, |z| > ENTRY -> enter at next open,
exit at the next open after |z| < 0.5 or 10 sessions. Year-by-year net, robustness (cost,
entry delay, entry threshold), sector splits, trend tests and spread half-lives.
Writes results/decay_*.csv. The decay was first seen in FLOWS_RESULTS.md (F1), so everything
here is descriptive, not a pre-registered test.
"""
from __future__ import annotations

import argparse
import itertools
from pathlib import Path

import numpy as np
import pandas as pd

from flows_oos import G, load
from study import consecutive, nw_t, tstat

HERE = Path(__file__).resolve().parent


def trades(s, OA, OB, corp, days, cons, entry=2.0, delay=0):
    out, i, n = [], 20, len(days)
    while i < n - 1:
        w = s[i - 19:i + 1]
        if not cons[i - 19:i + 2].all() or np.isnan(w).any() or corp[i - 18:i + 1].any():
            i += 1
            continue
        z = (s[i] - w.mean()) / w.std(ddof=1)
        if abs(z) <= entry:
            i += 1
            continue
        side, e = -np.sign(z), i + 1 + delay
        j = e
        while j < n - 1 and j - e < 10 and cons[j + 1]:
            wj = s[j - 19:j + 1]
            if abs((s[j] - wj.mean()) / wj.std(ddof=1)) < 0.5:
                break
            j += 1
        x = j + 1
        if x >= n:
            break
        if not corp[e + 1:x + 1].any():
            g = 0.5 * side * (OA[x] / OA[e] - 1) - 0.5 * side * (OB[x] / OB[e] - 1)
            if np.isfinite(g):
                out.append(dict(entry=days[e], exit=days[x], gross=g))
        i = x
    return out


def run(days, C, O, names, cons, corp, entry=2.0, delay=0):
    rows = []
    for grp, mem in G.items():
        for a, b in itertools.combinations(mem, 2):
            ka, kb = names.index(a), names.index(b)
            s = np.log(C[:, ka] / C[:, kb])
            for t in trades(s, O[:, ka], O[:, kb], corp[:, ka] | corp[:, kb], days, cons, entry, delay):
                rows.append(dict(group=grp, pair=f"{a}/{b}", **t))
    df = pd.DataFrame(rows)
    df["year"] = [d.year for d in df.entry]
    df["month"] = [d.strftime("%Y-%m") for d in df.entry]
    return df


def half_life(s, cons):
    """Daily AR(1) of the deviation from its trailing 20-day mean -> half-life in sessions."""
    m = pd.Series(s).rolling(20).mean().to_numpy()
    d = s - m
    ok = np.isfinite(d[1:]) & np.isfinite(d[:-1]) & cons[1:]
    if ok.sum() < 30:
        return np.nan
    x, y = d[:-1][ok], d[1:][ok]
    rho = (x @ y) / (x @ x)
    return np.log(0.5) / np.log(rho) if 0 < rho < 1 else np.nan


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--daily", required=True)
    a = ap.parse_args()
    names = sorted({n for v in G.values() for n in v})
    days, C, O, V = load(a.daily, names)
    cons = consecutive(days)
    r = np.full(C.shape, np.nan)
    r[1:] = np.log(C[1:] / C[:-1])
    corp = np.abs(np.nan_to_num(r)) > 0.20
    res = HERE / "results"
    base = run(days, C, O, names, cons, corp)
    cost = 10e-4
    base["net"] = base.gross - cost
    yr = base.groupby("year").agg(trades=("net", "size"), gross_bp=("gross", lambda v: v.mean() * 1e4),
                                  net_bp=("net", lambda v: v.mean() * 1e4), win=("net", lambda v: (v > 0).mean()))
    yr["t_monthly"] = [tstat(base[base.year == y].groupby("month").net.mean()) for y in yr.index]
    print("== by year (10 bp) ==\n" + yr.to_string(float_format=lambda x: f"{x:.2f}"))

    m = base.groupby("month").net.mean()
    mi = pd.PeriodIndex(m.index, freq="M")
    tt = np.array([(p.year - 2015) + (p.month - 1) / 12 for p in mi])
    X = np.column_stack([np.ones(len(tt)), tt])
    b = np.linalg.lstsq(X, m.to_numpy(), rcond=None)[0]
    e = m.to_numpy() - X @ b
    # Newey-West (6 lags) on the slope
    xc = tt - tt.mean()
    u = xc * e
    v = u @ u
    for L in range(1, 7):
        v += 2 * (1 - L / 7) * (u[L:] @ u[:-L])
    t_slope = b[1] / (np.sqrt(v) / (xc @ xc))
    early = base[base.year <= 2019].groupby("month").net.mean()
    late = base[base.year >= 2024].groupby("month").net.mean()
    diff = early.mean() - late.mean()
    t_diff = diff / np.sqrt(early.var(ddof=1) / len(early) + late.var(ddof=1) / len(late))
    trend = dict(slope_bp_per_year=b[1] * 1e4, t_nw=t_slope, early_bp=early.mean() * 1e4,
                 late_bp=late.mean() * 1e4, diff_bp=diff * 1e4, t_diff=t_diff,
                 all_bp=base.net.mean() * 1e4, all_t_monthly=tstat(m), months=len(m))
    print("== trend ==")
    for k, v_ in trend.items():
        print(f"{k:20s} {v_:.3f}")

    per = lambda y: "2015-19" if y <= 2019 else "2020-23" if y <= 2023 else "2024-26"
    base["period"] = base.year.map(per)
    sec = base.pivot_table(index="group", columns="period", values="net", aggfunc=lambda v: v.mean() * 1e4)
    print("== sector x period (net bp) ==\n" + sec.to_string(float_format=lambda x: f"{x:.1f}"))

    rob = []
    for label, entry, delay, c in [("base z2 10bp", 2.0, 0, 10), ("cost 5bp", 2.0, 0, 5), ("cost 15bp", 2.0, 0, 15),
                                   ("enter 1 day late", 2.0, 1, 10), ("z > 1.5", 1.5, 0, 10), ("z > 2.5", 2.5, 0, 10)]:
        d = base if (entry, delay) == (2.0, 0) else run(days, C, O, names, cons, corp, entry, delay)
        d = d.assign(net=d.gross - c * 1e-4, period=d.year.map(per))
        row = dict(variant=label, trades=len(d))
        for p in ("2015-19", "2020-23", "2024-26"):
            q = d[d.period == p]
            row[f"{p}_bp"] = q.net.mean() * 1e4
            row[f"{p}_t"] = tstat(q.groupby("month").net.mean())
        rob.append(row)
    rob = pd.DataFrame(rob)
    print("== robustness ==\n" + rob.to_string(index=False, float_format=lambda x: f"{x:.2f}"))

    hl = []
    years = np.array([d.year for d in days])
    for grp, mem in G.items():
        for a_, b_ in itertools.combinations(mem, 2):
            s = np.log(C[:, names.index(a_)] / C[:, names.index(b_)])
            for y in range(2015, 2027):
                k = years == y
                hl.append(dict(group=grp, pair=f"{a_}/{b_}", year=y, hl=half_life(s[k], cons[k]),
                               spread_vol_bp=np.nanstd(np.diff(s[k])) * 1e4))
    hl = pd.DataFrame(hl)
    hly = hl.groupby("year").agg(median_half_life=("hl", "median"), spread_vol_bp=("spread_vol_bp", "median"))
    print("== spread half-life and daily spread vol (median over pairs) ==\n" + hly.to_string(float_format=lambda x: f"{x:.2f}"))

    base.to_csv(res / "decay_trades.csv", index=False, float_format="%.6f")
    yr.to_csv(res / "decay_by_year.csv", float_format="%.4f")
    pd.DataFrame([trend]).to_csv(res / "decay_trend.csv", index=False, float_format="%.4f")
    sec.to_csv(res / "decay_sector_period.csv", float_format="%.2f")
    rob.to_csv(res / "decay_robustness.csv", index=False, float_format="%.3f")
    hly.to_csv(res / "decay_half_life.csv", float_format="%.3f")


if __name__ == "__main__":
    main()
