#!/usr/bin/env python3
"""Pre-registered pairs study (PAIRS_HYPOTHESES.md): rule P6 on 63 same-sector pairs.

    python research/equity/pairs.py --eq EQ_DIR --split dev
    python research/equity/pairs.py --eq EQ_DIR --split holdout   # once, after dev is committed

Source drive-ticks (Drive 5-minute bars), NOT FYERS. Writes results/pairs_<split>_trades.csv and
results/pairs_<split>_summary.csv.
"""
from __future__ import annotations

import argparse
import itertools
from pathlib import Path

import numpy as np
import pandas as pd

from load import S, load_stocks
from study import LeakError, check_time, consecutive, tstat

HERE = Path(__file__).resolve().parent
SEED = 20261112
NPL = 2000
FUT = 10e-4
GROUPS = {
    "banks": ["ICICIBANK", "KOTAKBANK", "AXISBANK", "SBIN", "INDUSINDBK"],
    "it": ["INFY", "TCS", "HCLTECH", "WIPRO", "TECHM"],
    "autos": ["MARUTI", "M&M", "TATAMOTORS", "BAJAJ-AUTO", "HEROMOTOCO", "EICHERMOT"],
    "metals": ["TATASTEEL", "JSWSTEEL", "HINDALCO", "VEDL"],
    "fmcg": ["HINDUNILVR", "ITC", "NESTLEIND", "BRITANNIA", "DABUR"],
    "pharma": ["SUNPHARMA", "CIPLA", "DRREDDY", "DIVISLAB"],
    "energy": ["RELIANCE", "ONGC", "BPCL", "IOC"],
}


def pair_trades(s, OA, OB, corp, days, cons, delay=0, leak=False):
    """s: daily log price ratio at the 15:25 close; corp: (D,) corporate-action night in either leg."""
    out = []
    i = 20
    n = len(days)
    while i < n - 1:
        w = s[i - 19:i + 1]
        if not cons[i - 19:i + 2].all() or np.isnan(w).any() or corp[i - 18:i + 1].any():
            i += 1
            continue
        z = (s[i] - w.mean()) / w.std(ddof=1)
        if abs(z) <= 2:
            i += 1
            continue
        side = -np.sign(z)
        e = i + 1 + delay
        j = e
        while j < n - 1 and j - e < 10 and cons[j + 1]:
            wj = s[j - 19:j + 1]
            if abs((s[j] - wj.mean()) / wj.std(ddof=1)) < 0.5:
                break
            j += 1
        x = j + 1
        if x >= n:
            break
        check_time((x, 1) if leak else (i, S), (e, 1))
        if not corp[e + 1:x + 1].any():
            g = 0.5 * side * (OA[x, 1] / OA[e, 1] - 1) - 0.5 * side * (OB[x, 1] / OB[e, 1] - 1)
            if np.isfinite(g):
                out.append(dict(entry=days[e], exit=days[x], side=side, gross=g))
        i = x
    return out


def run_all(A, syms, days, cons, delay=0, leak=False):
    C, O = A["close"], A["open"]
    close = C[:, S - 1]
    rcc = np.full(close.shape, np.nan)
    rcc[1:] = np.log(close[1:] / close[:-1])
    corp = np.abs(np.nan_to_num(rcc)) > 0.20
    rows = []
    for grp, names in GROUPS.items():
        for a, b in itertools.combinations(names, 2):
            if a not in syms or b not in syms:
                continue
            ka, kb = syms.index(a), syms.index(b)
            s = np.log(close[:, ka] / close[:, kb])
            for t in pair_trades(s, O[:, :, ka], O[:, :, kb], corp[:, ka] | corp[:, kb], days, cons, delay, leak):
                rows.append(dict(group=grp, pair=f"{a}/{b}", **t))
    df = pd.DataFrame(rows)
    df["net"] = df.gross - FUT
    return df


def monthly_t(df):
    m = df.groupby([d.strftime("%Y-%m") for d in df.entry]).net.mean()
    return tstat(m), len(m)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--eq", required=True)
    ap.add_argument("--split", choices=["dev", "holdout"], required=True)
    a = ap.parse_args()
    years = ["2020", "2022"] if a.split == "dev" else ["2022", "2023"]
    test_years = {2020, 2022} if a.split == "dev" else {2023}
    names = sorted({n for g in GROUPS.values() for n in g})
    days, syms, A = load_stocks(a.eq, years, names)
    cons = consecutive(days)
    keep = lambda df: df[[d.year in test_years for d in df.entry]].reset_index(drop=True)
    base = keep(run_all(A, syms, days, cons))
    late = keep(run_all(A, syms, days, cons, delay=1))
    try:
        run_all(A, syms, days, cons, leak=True)
        caught = False
    except LeakError:
        caught = True
    rng = np.random.default_rng(SEED)
    unsigned = (base.gross / base.side).to_numpy()
    pl = (rng.choice([-1.0, 1.0], size=(NPL, len(base))) * unsigned).mean(axis=1) - FUT
    t_m, n_m = monthly_t(base)
    yrs = np.array([d.year for d in base.entry])
    summ = dict(split=a.split, pairs=base.pair.nunique(), trades=len(base), months=n_m,
                gross_bp=base.gross.mean() * 1e4, net_bp=base.net.mean() * 1e4, t_monthly=t_m,
                t_trades=tstat(base.net), win_rate=(base.net > 0).mean(),
                by_year=" ".join(f"{y}:{base.net[yrs == y].mean() * 1e4:+.1f}" for y in sorted(set(yrs))),
                groups_positive=int((base.groupby("group").net.mean() > 0).sum()),
                placebo95_bp=np.percentile(pl, 95) * 1e4, late_net_bp=late.net.mean() * 1e4,
                leak_caught=caught, hold_days_med=float(np.median([(x - e).days for e, x in zip(base.entry, base.exit)])))
    res = HERE / "results"
    res.mkdir(exist_ok=True)
    base.to_csv(res / f"pairs_{a.split}_trades.csv", index=False, float_format="%.6f")
    pd.DataFrame([summ]).to_csv(res / f"pairs_{a.split}_summary.csv", index=False, float_format="%.4f")
    g = base.groupby("group").agg(trades=("net", "size"), net_bp=("net", lambda x: x.mean() * 1e4),
                                  t=("net", tstat))
    g.to_csv(res / f"pairs_{a.split}_groups.csv", float_format="%.4f")
    for k, v in summ.items():
        print(f"{k:16s} {v}")
    print(g.to_string())


if __name__ == "__main__":
    main()
