#!/usr/bin/env python3
"""F1 (and F4 on new years) from FLOWS_HYPOTHESES.md: rule P6 on FYERS daily candles, unseen years.

    python research/equity/flows_oos.py --daily <altair-data>/research/flows/fyers_daily \
        --fpi <altair-data>/research/flows/fpi_daily.csv

z on the daily close, enter and exit at the next session's open, 10 bp round trip.
Periods: 2015-01..2019-12 and 2024-01..latest. Source FYERS (and NSDL for the FPI flags).
"""
from __future__ import annotations

import argparse
import itertools
from pathlib import Path

import numpy as np
import pandas as pd

from flows import NEWS_HI, NEWS_LO, SECTOR_AV, calendar_days, cluster_ols, fpi_days
from pairs import FUT, GROUPS, pair_trades
from study import consecutive, tstat

HERE = Path(__file__).resolve().parent
G = {k: [n for n in v if n != "TATAMOTORS"] for k, v in GROUPS.items()}  # TMPV listing spans the demerger
PERIODS = {"2015-2019": (2015, 2019), "2024-2026": (2024, 2026)}


def load(daily, names):
    fr = {n: pd.read_csv(Path(daily) / f"{n}.csv", parse_dates=["date"]).set_index("date") for n in names}
    idx = sorted(set().union(*[f.index for f in fr.values()]))
    C = np.column_stack([fr[n].close.reindex(idx).to_numpy(float) for n in names])
    O = np.column_stack([fr[n].open.reindex(idx).to_numpy(float) for n in names])
    V = np.column_stack([fr[n].volume.reindex(idx).to_numpy(float) for n in names])
    return [d.date() for d in idx], C, O, V


def run_period(days, C, O, V, names, lo, hi, flowday, groups):
    keep = np.array([lo <= d.year <= hi for d in days])
    ix = np.where(keep)[0]
    d_ = [days[i] for i in ix]
    C, O, V, fd = C[ix], O[ix], V[ix], flowday[ix]
    cons = consecutive(d_)
    r = np.full(C.shape, np.nan)
    r[1:] = np.log(C[1:] / C[:-1])
    corp = np.abs(np.nan_to_num(r)) > 0.20
    di = {d: i for i, d in enumerate(d_)}
    rows = []
    for grp in groups:
        for a, b in itertools.combinations(G[grp], 2):
            ka, kb = names.index(a), names.index(b)
            s = np.log(C[:, ka] / C[:, kb])
            OA = np.column_stack([O[:, ka], O[:, ka]])  # slot 1 = the open
            OB = np.column_stack([O[:, kb], O[:, kb]])
            for t in pair_trades(s, OA, OB, corp[:, ka] | corp[:, kb], d_, cons):
                e = di[t["entry"]]
                d = e - 1
                cls, asym = "OTHER", np.nan
                if d - 22 >= 0:
                    base = np.nanmedian(V[d - 22:d - 2][:, [ka, kb]], axis=0)
                    av = V[d - 2:d + 1][:, [ka, kb]] / base
                    news = bool((((av[:, 0] >= NEWS_HI) & (av[:, 1] < NEWS_LO)) |
                                 ((av[:, 1] >= NEWS_HI) & (av[:, 0] < NEWS_LO))).any())
                    flow = (not news) and bool(fd[d - 2:d + 1].any())
                    sector = (not news) and (not flow) and bool(((av >= SECTOR_AV).all(axis=1)).any())
                    cls = "NEWS" if news else "FLOW" if flow else "SECTOR" if sector else "OTHER"
                rows.append(dict(group=grp, pair=f"{a}/{b}", **t, cls=cls))
    df = pd.DataFrame(rows)
    df["net"] = df.gross - FUT
    df["month"] = [x.strftime("%Y-%m") for x in df.entry]
    return df


def summarise(df, label):
    m = df.groupby("month").net.mean()
    return dict(set=label, trades=len(df), months=len(m), net_bp=df.net.mean() * 1e4,
                t_monthly=tstat(m), win=(df.net > 0).mean())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--daily", required=True)
    ap.add_argument("--fpi", required=True)
    a = ap.parse_args()
    names = sorted({n for g in GROUPS.values() for n in g} - {"TATAMOTORS"})
    days, C, O, V = load(a.daily, names)
    flowday = calendar_days(days) | fpi_days(days, a.fpi)
    groups_all = list(G)
    res = HERE / "results"
    out, trades = [], []
    for p, (lo, hi) in PERIODS.items():
        bank = run_period(days, C, O, V, names, lo, hi, flowday, ["banks"]).assign(period=p)
        allp = run_period(days, C, O, V, names, lo, hi, flowday, groups_all).assign(period=p)
        out += [summarise(bank, f"banks {p}"), summarise(allp, f"all pairs {p}")]
        trades.append(allp)
    tr = pd.concat(trades, ignore_index=True)
    bank = tr[tr.group == "banks"]
    out.insert(0, summarise(bank, "banks both periods (F1)"))
    s = pd.DataFrame(out)
    f1 = (s.iloc[0].net_bp > 0 and s.iloc[0].t_monthly >= 2 and
          all(s[s.set == f"banks {p}"].net_bp.iloc[0] > 0 for p in PERIODS))
    print(s.to_string(index=False, float_format=lambda x: f"{x:.2f}"))
    print("F1 pass:", f1)
    by = lambda x: x.groupby("cls").net.agg(trades="size", net_bp=lambda v: v.mean() * 1e4)
    print("-- new years, all pairs, by class --\n" + by(tr).to_string())
    print("-- new years, banks, by class (F4) --\n" + by(bank).to_string())
    fn = tr[tr.cls.isin(["FLOW", "NEWS"])]
    b, t = cluster_ols(fn.net.to_numpy(), (fn.cls == "FLOW").to_numpy(float), fn.month.to_numpy())
    print(f"new years FLOW-NEWS {b[1]*1e4:+.1f} bp, t {t[1]:.2f}")
    print("-- by group --\n" + tr.groupby(["period", "group"]).net.agg(trades="size", net_bp=lambda v: v.mean() * 1e4).to_string())
    tr.to_csv(res / "flows_oos_trades.csv", index=False, float_format="%.6f")
    s.assign(f1_pass=f1).to_csv(res / "flows_oos_summary.csv", index=False, float_format="%.4f")


if __name__ == "__main__":
    main()
