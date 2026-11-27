#!/usr/bin/env python3
"""research/pnl/paper_breakdown.py -- per-model breakdown of the live engine's PAPER round trips.

Reads the "[trades] INFO paper ..." lines the engine writes to its daily log (pushed to
altair-data/logs/altair-<day>.log; the same trade is printed again at the nightly re-run, so lines
are de-duplicated). Gross is (exit - entry) x qty x side, costs = gross - net (config/charges.toml
via risk/cost.hpp; brokerage is NOT in it while [broker.fyers] configured = false).

    python research/pnl/paper_breakdown.py ../altair-data/logs [--bars ../altair-data/merged/spot/banknifty/5m/clean.csv.gz]

--bars adds a trending/ranging tag per day: efficiency ratio |close - open| / sum |5-min moves|,
trending if >= 0.35 (FYERS 5-min bars of the index).
"""
from __future__ import annotations

import argparse
import glob
import os
import re

import numpy as np
import pandas as pd

RX = re.compile(r"^(\d{4}-\d\d-\d\d)T\S+ \[trades\] INFO (paper|live) (.+?) (long|short) (\S+)(?: \S+->\S+)? "
                r"x(\d+) ([\d.]+)->([\d.]+) net (-?[\d.]+); in: (.*?); out: (.*)$")


def brokerage_rt(sym: str, qty: int, entry: float, exit_: float) -> float:
    """ESTIMATE, not in charges.toml: FYERS's published F&O/intraday rule, Rs 20 or 0.03% of the order
    value (lower) per executed order; options Rs 20 flat. Two orders a round trip. Check the actual plan."""
    if sym.endswith(("CE", "PE")):
        return 40.0
    return min(20.0, 3e-4 * qty * entry) + min(20.0, 3e-4 * qty * exit_)


def parse(logdir: str) -> pd.DataFrame:
    rows, seen = [], set()
    for f in sorted(glob.glob(os.path.join(logdir, "altair-*.log"))):
        day = os.path.basename(f)[7:17]
        for line in open(f, encoding="utf-8", errors="replace"):
            m = RX.match(line.strip())
            if not m:
                continue
            key = (day,) + m.groups()[2:]
            if key in seen:
                continue
            seen.add(key)
            _, mode, model, side, sym, q, e, x, net, why_in, why_out = m.groups()
            q, e, x, net = int(q), float(e), float(x), float(net)
            gross = (1 if side == "long" else -1) * (x - e) * q
            rows.append(dict(day=day, mode=mode, model=model, side=side, sym=sym, qty=q, entry=e, exit=x,
                             gross=gross, net=net, brokerage=brokerage_rt(sym, q, e, x), cost=gross - net, exit_rule=re.split(r"[:(]", why_out)[0].strip()))
    return pd.DataFrame(rows)


def summary(g: pd.DataFrame) -> pd.Series:
    w, lo = g[g.net > 0], g[g.net <= 0]
    return pd.Series(dict(trades=len(g), hit_pct=100 * len(w) / len(g), avg_win=w.net.mean(), avg_loss=lo.net.mean(),
                          gross=g.gross.sum(), costs=g.cost.sum(), net=g.net.sum(),
                          net_with_brokerage=g.net.sum() - g.brokerage.sum()))


def regimes(path: str) -> pd.Series:
    b = pd.read_csv(path)
    b["day"] = b.time.str[:10]
    def er(g):
        return abs(g.close.iloc[-1] - g.open.iloc[0]) / max(np.abs(np.diff(g.close.to_numpy())).sum(), 1e-9)
    e = b.groupby("day").apply(er)
    return pd.Series(np.where(e >= 0.35, "trending", "ranging"), index=e.index)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("logdir")
    ap.add_argument("--bars")
    ap.add_argument("--out", default="research/pnl/results")
    a = ap.parse_args()
    d = parse(a.logdir)
    os.makedirs(a.out, exist_ok=True)
    d.to_csv(os.path.join(a.out, "paper_trades.csv"), index=False)
    pd.set_option("display.width", 250)
    pd.set_option("display.max_columns", 20)
    by_model = d.groupby("model").apply(summary).sort_values("net")
    by_model.round(0).to_csv(os.path.join(a.out, "by_model.csv"))
    print(by_model.round(0).to_string())
    print(d.groupby(["model", "side"]).net.agg(["count", "sum", "mean"]).round(0).to_string())
    print(d.groupby("exit_rule").net.agg(["count", "sum", "mean"]).round(0).sort_values("sum").head(12).to_string())
    print(d.groupby("day").apply(summary).round(0).to_string())
    if a.bars:
        d["regime"] = d.day.map(regimes(a.bars)).fillna("no bars")
        print(d.groupby(["regime", "side"]).apply(summary).round(0).to_string())


if __name__ == "__main__":
    main()
