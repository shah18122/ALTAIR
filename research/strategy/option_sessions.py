#!/usr/bin/env python3
"""Replay sell-only option variants on the FYERS chains ALTAIR recorded.

    python research/strategy/option_sessions.py --data ../altair-data [--days 2026-11-04,...]

Reads altair-data/chain_iv/<yyyy>/<day>.csv.gz (every option each minute:
mid price, Black-76 IV, delta, forward; source fyers_tape). For each session
and index, nearest expiry (the next one on expiry day, except the 0DTE row):

  straddle_open   sell the ATM straddle at 09:20 (or the first recorded minute), buy back 15:20
  straddle_late   same, sold at 13:00
  strangle_25d    sell the call nearest +0.25 delta and the put nearest -0.25, at the open
  straddle_0dte   on expiry day only, sell the straddle expiring that day

Prices are minute mids, so a half-spread is charged on every fill (HALF_SPREAD
below, an assumption until the tapes give real spreads). Statutory costs are
config/charges.toml schedule 2 (equity_options). Sell only: every leg is
opened by a sale and closed by a buy.

Days the recorder caught only part of are used from the first to the last
minute it has; the summary says which. Not a validation: five sessions.
"""
from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np
import pandas as pd

LOT = {"BANKNIFTY": 30, "NIFTY": 65}
HALF_SPREAD = {"BANKNIFTY": 1.0, "NIFTY": 0.25}   # rupees per unit, per fill (assumed)
STT_SELL, EXCH, STAMP_BUY, GST, BROKER = 0.0015, 0.0003503, 0.00003, 0.18, 20.0


def leg_costs(sell, buy, lot, hs):
    prem_s, prem_b = sell * lot, buy * lot
    exch = EXCH * (prem_s + prem_b)
    stat = STT_SELL * prem_s + exch + STAMP_BUY * prem_b + GST * (exch + 2 * BROKER) + 2 * BROKER
    return stat + 2 * hs * lot


def _mins(hm):
    return int(hm[:2]) * 60 + int(hm[3:5])


def snapshot(g, hm, max_age=5):
    """Each option's latest MID quote at or before hm, no older than max_age minutes.
    The recorded rows are sparse (a row only when that option quoted), so this
    is the book as a trader would have seen it at hm."""
    t = _mins(hm)
    w = g[(g.m <= t) & (g.m >= t - max_age) & (g.price_from == "mid")]
    if w.empty:
        return w
    w = w.sort_values("m").groupby("fyers").tail(1).copy()
    w["forward"] = float(g[g.m <= t].sort_values("m").forward.iloc[-1])
    return w


FUT_COST_BP = 3.5   # one side of a futures trade (7 bp round trip, schedule 2)


def hedge_pnl(g, legs, lot, ent, last, every=15, lots=1):
    """Delta-hedge one short-straddle lot with whole futures lots (same lot
    size), re-set every `every` minutes at the forward. With `lots` straddle
    lots the hedge rounds lots*delta; returned per straddle lot. (pnl, cost)."""
    pnl = cost = 0.0
    h, f_prev = 0, None
    t = _mins(ent)
    while True:
        hm = f"{t // 60:02d}:{t % 60:02d}"
        snap = snapshot(g, min(hm, last))
        f = float(snap.forward.iloc[0]) if not snap.empty else f_prev
        if f_prev is not None:
            pnl += h * (f - f_prev) * lot
        if hm >= last:
            cost += abs(h) * f * lot * FUT_COST_BP / 1e4
            break
        d = snap[snap.fyers.isin(legs)].delta
        if len(d) == 2 and d.notna().all():
            want = int(round(float(d.sum()) * lots))      # short straddle delta is -sum; hedge +sum
            cost += abs(want - h) * f * lot * FUT_COST_BP / 1e4
            h = want
        f_prev = f
        t += every
    return pnl / lots, cost / lots


def trade(g, und, ent_hm, legs_fn, exit_hm="15:20", hedge=0):
    g = g.assign(m=g.hm.map(_mins))
    first, last = g.hm.min(), min(g.hm.max(), exit_hm)
    ent = max(ent_hm, first)
    if ent >= last:
        return None
    at_e, at_x = snapshot(g, ent), snapshot(g, last)
    if at_e.empty or at_x.empty:
        return None
    legs = legs_fn(at_e)
    if legs is None:
        return None
    lot, hs = LOT[und], HALF_SPREAD[und]
    gross = cost = credit = 0.0
    ivs = []
    for sym in legs:
        e = at_e[at_e.fyers == sym]
        x = at_x[at_x.fyers == sym]
        if e.empty or x.empty:
            return None
        s_, b = float(e.price.iloc[0]), float(x.price.iloc[0])
        gross += (s_ - b) * lot
        cost += leg_costs(s_, b, lot, hs)
        credit += s_
        ivs.append(float(e.iv.iloc[0]))
    if hedge:
        hp, hc = hedge_pnl(g, legs, lot, ent, last, lots=hedge)
        gross += hp
        cost += hc
    f0, f1 = float(at_e.forward.iloc[0]), float(at_x.forward.iloc[0])
    return dict(entry=ent, exit=last, legs=" ".join(x.split(":")[1] for x in legs),
                credit_pts=credit, move_pts=abs(f1 - f0), iv_entry=np.nanmean(ivs),
                gross_rs=gross, cost_rs=cost, net_rs=gross - cost)


def atm(at):
    f = float(at.forward.iloc[0])
    both = set(at[at.right == "CE"].strike) & set(at[at.right == "PE"].strike)
    if not both:
        return None
    k = min(both, key=lambda x: abs(x - f))
    if abs(k - f) > 0.01 * f:
        return None
    ce = at[(at.strike == k) & (at.right == "CE")]
    pe = at[(at.strike == k) & (at.right == "PE")]
    return [ce.fyers.iloc[0], pe.fyers.iloc[0]]


def strangle(at, d=0.25):
    f = float(at.forward.iloc[0])
    ce = at[(at.right == "CE") & (at.strike > f) & at.delta.notna()]
    pe = at[(at.right == "PE") & (at.strike < f) & at.delta.notna()]
    if ce.empty or pe.empty:
        return None
    c = ce.iloc[(ce.delta - d).abs().argsort().iloc[0]]
    p = pe.iloc[(pe.delta + d).abs().argsort().iloc[0]]
    return [c.fyers, p.fyers]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", required=True, type=Path)
    ap.add_argument("--days", default="")
    ap.add_argument("--out", type=Path, default=Path(__file__).parent / "results")
    a = ap.parse_args()
    files = sorted((a.data / "chain_iv").glob("*/*.csv.gz"))
    want = set(filter(None, a.days.split(",")))
    rows = []
    for f in files:
        day = f.name[:10]
        if want and day not in want:
            continue
        d = pd.read_csv(f)
        d = d[(d.time.str[:10] == day) & (d.source == "fyers_tape") & (d.price > 0)]
        if d.empty:
            continue
        d["hm"] = d.time.str[11:16]
        d = d[(d.hm >= "09:15") & (d.hm <= "15:30")]
        for und, g in d.groupby("underlying"):
            if und not in LOT:
                continue
            exps = sorted(e for e in g.expiry.unique() if e >= day)
            span = f"{g.hm.min()}-{g.hm.max()}"
            full = g.hm.min() <= "09:25" and g.hm.max() >= "15:20"
            near = [e for e in exps if e > day][0]
            gn = g[g.expiry == near]
            variants = [("straddle_open", gn, "09:20", atm),
                        ("straddle_open_hedged", gn, "09:20", atm),
                        ("straddle_open_hedged_x4", gn, "09:20", atm),
                        ("straddle_late", gn, "13:00", atm),
                        ("strangle_25d", gn, "09:20", strangle)]
            if exps[0] == day:
                variants.append(("straddle_0dte", g[g.expiry == day], "09:20", atm))
            for name, gg, hm, fn in variants:
                r = trade(gg, und, hm, fn, hedge=4 if name.endswith("x4") else int("hedged" in name))
                if r:
                    rows.append(dict(day=day, und=und, variant=name, expiry=gg.expiry.iloc[0],
                                     recorded=span, full_day=full, **r))
    t = pd.DataFrame(rows)
    a.out.mkdir(parents=True, exist_ok=True)
    t.to_csv(a.out / "option_sessions.csv", index=False, float_format="%.4g")
    with pd.option_context("display.width", 250, "display.max_columns", 30):
        print(t.drop(columns=["legs"]).round(3).to_string(index=False))
        s = t.groupby(["und", "variant"]).agg(n=("net_rs", "size"), gross=("gross_rs", "sum"),
                                              net=("net_rs", "sum"), wins=("net_rs", lambda x: (x > 0).sum()))
        print(s.round(0))


if __name__ == "__main__":
    main()
