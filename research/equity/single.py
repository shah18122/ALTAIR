#!/usr/bin/env python3
"""Pre-registered single-stock rules P1-P6 (SINGLE_STOCK_HYPOTHESES.md) on Drive 5-minute bars.

    python research/equity/single.py --eq EQ_DIR --bars BARS_DIR --split dev
    python research/equity/single.py --eq EQ_DIR --bars BARS_DIR --split holdout   # once

Source drive-ticks, NOT FYERS. Writes results/single_<split>.csv (HDFCBANK) and
results/single_<split>_replication.csv (ten other names, descriptive only).
"""
from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np
import pandas as pd

from load import S, load_future, load_stocks
from study import LeakError, check_time, consecutive, tstat

HERE = Path(__file__).resolve().parent
SEED = 20261112
NPL = 2000
CASH, FUT = 8e-4, 10e-4
MAIN = "HDFCBANK"
PAIR = "ICICIBANK"
REPL = ["RELIANCE", "ICICIBANK", "INFY", "TCS", "SBIN", "AXISBANK", "KOTAKBANK", "LT", "ITC", "BHARTIARTL"]


def T(rec, sig_end, entry, exit_, side, px_in, px_out, cost, leak, day):
    if leak:
        sig_end = exit_  # the trade's own return as signal ends at the exit
    check_time(sig_end, entry)
    g = side * (px_out / px_in - 1)
    if np.isfinite(g):
        rec.append(dict(day=day, side=side, gross=g, cost=cost))


def p1(O, C, days, cons, leak=False, delay=0, **_):
    out = []
    for i in range(1, len(days)):
        if not cons[i]:
            continue
        sig = C[i, 5] / C[i - 1, S - 1] - 1
        if not np.isfinite(sig) or sig == 0:
            continue
        e, x = 69 + delay, 73 + delay
        T(out, (i, 6), (i, e), (i, x), np.sign(sig), O[i, e], O[i, x], CASH, leak, days[i])
    return out


def p2(O, C, days, cons, leak=False, delay=0, **_):
    out = []
    for i in range(len(days)):
        hi = np.nanmax(np.r_[O[i, :3], C[i, :3]])
        lo = np.nanmin(np.r_[O[i, :3], C[i, :3]])
        for t in range(3, 72):
            side = 1 if C[i, t] > hi else (-1 if C[i, t] < lo else 0)
            if side:
                e = t + 1 + delay
                if e < 73:
                    T(out, (i, t + 1), (i, e), (i, 73), side, O[i, e], O[i, 73], CASH, leak, days[i])
                break
    return out


def p3(O, C, days, cons, leak=False, delay=0, **_):
    out = []
    gap = np.full(len(days), np.nan)
    for i in range(1, len(days)):
        if cons[i]:
            gap[i] = O[i, 0] / C[i - 1, S - 1] - 1
    for i in range(61, len(days)):
        hist = np.abs(gap[i - 60:i])
        if not cons[i - 59:i + 1].all() or np.isnan(gap[i]) or np.isnan(hist).sum() > 5:
            continue
        if abs(gap[i]) > np.nanpercentile(hist, 75):
            e, x = 1 + delay, 12 + delay
            T(out, (i, 1), (i, e), (i, x), -np.sign(gap[i]), O[i, e], O[i, x], CASH, leak, days[i])
    return out


def p4(O, C, days, cons, leak=False, delay=0, **_):
    out = []
    for i in range(1, len(days)):
        if not cons[i]:
            continue
        # entry at the 15:25 bar's close = boundary (i-1, S); exit at next 09:15 open (delay: 09:20)
        T(out, (i - 1, S), (i - 1, S), (i, delay), 1.0, C[i - 1, S - 1], O[i, delay], FUT, leak, days[i])
    return out


def p5(O, C, days, cons, fut=None, leak=False, delay=0, **_):
    rs = np.full(C.shape, np.nan)
    rs[:, 1:] = np.log(C[:, 1:] / C[:, :-1])
    rf = np.full(fut.shape, np.nan)
    rf[:, 1:] = np.log(fut[:, 1:] / fut[:, :-1])
    res = np.full(C.shape, np.nan)
    b = None
    for i in range(len(days)):
        if b is None or days[i].month != days[i - 1].month:
            b = None
            if i >= 60 and cons[i - 59:i + 1].all():
                x, y = rf[i - 60:i, 1:].ravel(), rs[i - 60:i, 1:].ravel()
                ok = np.isfinite(x) & np.isfinite(y)
                if ok.sum() > 1000:
                    b = (x[ok] @ y[ok]) / (x[ok] @ x[ok])
        if b is not None:
            res[i, 1:] = rs[i, 1:] - b * rf[i, 1:]
    E = np.nancumsum(np.nan_to_num(res), axis=1)
    E[np.isnan(res).all(axis=1)] = np.nan
    out = []
    for i in range(20, len(days)):
        if not cons[i - 19:i + 1].all() or np.isnan(E[i]).all():
            continue
        sd = np.nanstd(E[i - 20:i, 1:])
        if not np.isfinite(sd) or sd == 0:
            continue
        nxt = 0
        for t in range(5, 66):
            if t < nxt:
                continue
            z = E[i, t] / sd
            if abs(z) > 2:
                e, x = t + 1 + delay, t + 7 + delay
                T(out, (i, t + 1), (i, e), (i, x), -np.sign(z), O[i, e], O[i, x], CASH, leak, days[i])
                nxt = t + 6
    return out


def p6(O, C, days, cons, O2=None, C2=None, leak=False, delay=0, **_):
    s = np.log(C[:, S - 1] / C2[:, S - 1])
    out = []
    i = 20
    while i < len(days) - 1:
        w = s[i - 19:i + 1]
        if not cons[i - 19:i + 2].all() or np.isnan(w).any():
            i += 1
            continue
        z = (s[i] - w.mean()) / w.std(ddof=1)
        if abs(z) <= 2:
            i += 1
            continue
        side = -np.sign(z)  # side on leg 1 (HDFCBANK); leg 2 opposite
        e = i + 1 + delay
        j = e
        while j < len(days) - 1 and j - e < 10 and cons[j + 1]:
            wj = s[j - 19:j + 1]
            zj = (s[j] - wj.mean()) / wj.std(ddof=1)
            if abs(zj) < 0.5:
                break
            j += 1
        x = j + 1
        if x >= len(days) or e >= len(days):
            break
        sig_end = (x, 1) if leak else (i, S)
        check_time(sig_end, (e, 1))
        g = 0.5 * side * (O[x, 1] / O[e, 1] - 1) - 0.5 * side * (O2[x, 1] / O2[e, 1] - 1)
        if np.isfinite(g):
            out.append(dict(day=days[e], side=side, gross=g, cost=FUT))
        i = x
    return out


RULES = {"P1 intraday momentum": p1, "P2 opening range breakout": p2, "P3 gap fade": p3,
         "P4 overnight drift": p4, "P5 residual reversion vs BANKNIFTY": p5, "P6 pair HDFCBANK/ICICIBANK": p6}


def evaluate(name, fn, kw, test_years, rng, intraday_ref=None):
    keep = lambda L: pd.DataFrame([r for r in L if r["day"].year in test_years])
    base = keep(fn(**kw))
    late = keep(fn(**kw, delay=1))
    try:
        fn(**kw, leak=True)
        caught = False
    except LeakError:
        caught = True
    if len(base) == 0:
        return dict(rule=name, n=0)
    base["net"] = base.gross - base.cost
    late_net = (late.gross - late.cost).mean() if len(late) else np.nan
    if name.startswith("P4"):
        ref = intraday_ref[[d.year in test_years for d in intraday_ref.index]] - FUT
        pl = np.array([rng.choice(ref.to_numpy(), len(ref)).mean() for _ in range(NPL)])
    else:
        sg = rng.choice([-1.0, 1.0], size=(NPL, len(base)))
        pl = (sg * (base.gross / base.side).to_numpy()).mean(axis=1) - base.cost.mean()
    yrs = np.array([d.year for d in base.day])
    return dict(rule=name, n=len(base), gross_bp=base.gross.mean() * 1e4, cost_bp=base.cost.mean() * 1e4,
                net_bp=base.net.mean() * 1e4, t=tstat(base.net),
                by_year=" ".join(f"{y}:{base.net[yrs == y].mean() * 1e4:+.2f}" for y in sorted(set(yrs))),
                placebo95_bp=np.percentile(pl, 95) * 1e4, late_net_bp=late_net * 1e4, leak_caught=caught,
                total_pct=base.net.sum() * 100)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--eq", required=True)
    ap.add_argument("--bars", required=True)
    ap.add_argument("--split", choices=["dev", "holdout"], required=True)
    a = ap.parse_args()
    years = ["2020", "2022"] if a.split == "dev" else ["2022", "2023"]
    test_years = {2020, 2022} if a.split == "dev" else {2023}
    rng = np.random.default_rng(SEED)
    days, syms, A = load_stocks(a.eq, years, sorted(set([MAIN, PAIR] + REPL)))
    cons = consecutive(days)
    _, fut = load_future(a.bars, "banknifty", days)
    k = {s: syms.index(s) for s in syms}
    res = HERE / "results"
    res.mkdir(exist_ok=True)

    def kw_for(sym):
        O, C = A["open"][:, :, k[sym]], A["close"][:, :, k[sym]]
        other = PAIR if sym != PAIR else MAIN
        return dict(O=O, C=C, days=days, cons=cons, fut=fut, O2=A["open"][:, :, k[other]],
                    C2=A["close"][:, :, k[other]])

    def run(sym):
        kw = kw_for(sym)
        ref = pd.Series(kw["C"][:, S - 1] / kw["O"][:, 0] - 1, index=days).dropna()
        rows = []
        for name, fn in RULES.items():
            nm = name if sym == MAIN else name.replace("HDFCBANK", sym).replace("ICICIBANK/" + sym, sym + "/HDFCBANK")
            rows.append(dict(sym=sym, **evaluate(nm, fn, kw, test_years, rng, ref)))
        return rows

    main_rows = pd.DataFrame(run(MAIN))
    main_rows.to_csv(res / f"single_{a.split}.csv", index=False, float_format="%.4f")
    rep = pd.DataFrame([r for s in REPL for r in run(s)])
    rep.to_csv(res / f"single_{a.split}_replication.csv", index=False, float_format="%.4f")
    with pd.option_context("display.width", 250, "display.max_columns", 30):
        print(main_rows.to_string(index=False))
        rep["rule_id"] = rep.rule.str[:2]
        print(rep.groupby("rule_id").agg(pos=("net_bp", lambda x: int((x > 0).sum())), n=("net_bp", "size"),
                                         gross_med=("gross_bp", "median"), net_med=("net_bp", "median")))


if __name__ == "__main__":
    main()
