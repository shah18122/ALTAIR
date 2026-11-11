#!/usr/bin/env python3
"""research/cnn/combo.py -- combine the CNN with every other signal: is there alpha?

Smit, 2026-11-11: "finish the CNN work by finding a combination of models and
understanding how the market works to generate the best alpha".

Data: 5-minute bars from tools/drive_ticks_to_bars.py (source drive-ticks, NOT
FYERS): NIFTY and BANKNIFTY front futures (traded) and NIFTY spot (basis only).

Part 1, HOW THE MARKET WORKS: every feature's rank correlation (IC) with the
next 5/15/30/60-minute and rest-of-day futures return, per year (2020, 2022,
2023), so a sign that flips between years is visible. No fitting at all.

Part 2, THE COMBINATION: at every horizon, a walk-forward (monthly refit, past
only) ridge, a gradient-boosted tree model and their average are fitted on all
features, including the walk-forward CNN p(up) and p(big move) from
research/cnn/cnn_test.py. Trades: non-overlapping entries at the bar close,
held h bars, flat by the session close; cost --cost-bp per round trip.
  "edge"  trade only when |forecast| > cost (the model thinks it beats costs)
  "top10" trade only the top 10% |forecast| of the train window
  "hivol" "edge" and HAR expects a top-quartile move
Pass bar (fixed before running): net bp/trade > 0 with daily t >= 3.2 (Bonferroni
over every model x mode x horizon x series) AND net > 0 in each year.

    python research/cnn/combo.py --bars data/drive_ticks --out research/cnn/results
Needs numpy and scikit-learn.
"""
from __future__ import annotations

import argparse
import csv
import math
import sys
from pathlib import Path

import numpy as np
from sklearn.ensemble import HistGradientBoostingRegressor

sys.path.insert(0, str(Path(__file__).resolve().parent))
from cnn_test import T, Tcn, features as cnn_features, nw_t, ridge, windows  # noqa: E402

HORIZONS = {"5m": 1, "15m": 3, "30m": 6, "60m": 12, "close": 0}


def load(path: Path) -> dict[str, tuple]:
    out = {}
    with path.open() as f:
        for r in csv.DictReader(f):
            out[r["time"]] = (float(r["open"]), float(r["high"]), float(r["low"]), float(r["close"]),
                              float(r.get("volume") or 0), float(r.get("oi") or 0))
    return out


def trail(x: np.ndarray, k: int) -> np.ndarray:
    cs = np.concatenate([[0.0], np.cumsum(x)])
    i = np.arange(len(x))
    lo = np.maximum(0, i - k + 1)
    return (cs[i + 1] - cs[lo]) / (i + 1 - lo)


def build(bars: Path):
    nf = load(bars / "fut/nifty/5m/all.csv")
    bf = load(bars / "fut/banknifty/5m/all.csv")
    sp = load(bars / "spot/nifty/5m/all.csv")
    times = sorted(set(nf) & set(bf))
    n = len(times)
    day = np.array([t[:10] for t in times])
    month = np.array([t[:7] for t in times])
    mins = np.array([int(t[11:13]) * 60 + int(t[14:16]) - 555 for t in times], float)  # minutes since 09:15
    dow = np.array([np.datetime64(d).astype("datetime64[D]").view("int64") % 7 for d in day])  # 0 = Thu
    new_day = np.concatenate([[True], day[1:] != day[:-1]])
    # last Thursday of the month = monthly expiry (2020-2023)
    dd = np.array([np.datetime64(d) for d in day])
    days_to_m_exp = np.zeros(n)
    for i, d in enumerate(dd):
        y_, m_ = int(str(d)[:4]), int(str(d)[5:7])
        nxt = np.datetime64(f"{y_ + (m_ == 12)}-{(m_ % 12) + 1:02d}-01")
        last = nxt - 1
        while last.astype("int64") % 7 != 0:   # 1970-01-01 was a Thursday
            last -= 1
        days_to_m_exp[i] = (last - d).astype(int)
    S = {}
    for name, src in (("n", nf), ("b", bf)):
        a = np.array([src[t] for t in times])
        S[name] = dict(o=a[:, 0], h=a[:, 1], l=a[:, 2], c=a[:, 3], v=a[:, 4], oi=a[:, 5])
    spot_c = np.array([sp[t][3] if t in sp else np.nan for t in times])
    # session open close index per bar
    day_start = np.maximum.accumulate(np.where(new_day, np.arange(n), 0))
    day_end = np.empty(n, int)
    end = n - 1
    for i in range(n - 1, -1, -1):
        day_end[i] = end
        if new_day[i]:
            end = i - 1
    return times, day, month, mins, dow, days_to_m_exp, new_day, day_start, day_end, S, spot_c


def feats_for(S, me: str, other: str, spot_c, mins, dow, dexp, new_day, day_start):
    c, o = S[me]["c"], S[other]["c"]
    lc, lo = np.log(c), np.log(o)
    n = len(c)
    F = {}

    def ret(lx, k):
        r = np.zeros(n)
        r[k:] = 1e4 * (lx[k:] - lx[:-k])
        start = np.arange(n) - k < day_start   # never reach into yesterday
        r[start] = 1e4 * (lx[start] - lx[day_start[start]])
        return r
    for k in (1, 3, 6, 12):
        F[f"own_r{k}"] = ret(lc, k)
        F[f"oth_r{k}"] = ret(lo, k)
    F["own_since_open"] = 1e4 * (lc - lc[day_start])
    F["oth_since_open"] = 1e4 * (lo - lo[day_start])
    prev_close = np.roll(lc, 1)
    gap = np.where(new_day, 1e4 * (lc - prev_close), 0.0)   # first bar incl. overnight (and roll)
    F["gap"] = gap[day_start]
    F["rel_since_open"] = F["own_since_open"] - F["oth_since_open"]
    # basis vs spot (NIFTY only; BANKNIFTY uses the NIFTY basis as market tone)
    ls = np.log(spot_c)
    basis = 1e4 * (np.log(S["n"]["c"]) - ls)
    basis = np.where(np.isnan(basis), np.nan, basis)
    F["basis_chg_day"] = np.nan_to_num(basis - basis[day_start])
    F["basis_chg_1"] = np.nan_to_num(basis - np.roll(basis, 1)) * (~new_day)
    sr = np.zeros(n)
    sr[1:] = 1e4 * (ls[1:] - ls[:-1])
    F["spot_minus_fut_r1"] = np.nan_to_num(sr - ret(np.log(S["n"]["c"]), 1)) * (~new_day)
    absr = np.abs(F["own_r1"])
    for k in (1, 6, 22):
        F[f"har{k}"] = trail(absr, k)
    F["range"] = 1e4 * np.log(S[me]["h"] / S[me]["l"])
    F["close_loc"] = np.where(S[me]["h"] > S[me]["l"], (c - S[me]["l"]) / np.maximum(S[me]["h"] - S[me]["l"], 1e-9) - 0.5, 0)
    lv = np.log1p(S[me]["v"])
    F["vol_z"] = lv - trail(lv, 20)
    oi = S[me]["oi"]
    F["oi_chg_day"] = np.where(oi[day_start] > 0, 1e4 * np.log(np.maximum(oi, 1) / np.maximum(oi[day_start], 1)), 0)
    F["oi_x_ret"] = np.sign(F["oi_chg_day"]) * F["own_since_open"]
    F["mins"] = mins
    F["dow"] = dow.astype(float)
    F["days_to_expiry"] = dexp
    return F


def spearman(a, b):
    ra = np.argsort(np.argsort(a)).astype(float)
    rb = np.argsort(np.argsort(b)).astype(float)
    return float(np.corrcoef(ra, rb)[0, 1])


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bars", default="data/drive_ticks")
    ap.add_argument("--out", default="research/cnn/results")
    ap.add_argument("--cost-bp", type=float, default=6.0)
    ap.add_argument("--seed", type=int, default=7)
    a = ap.parse_args()
    bars, out = Path(a.bars), Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    times, day, month, mins, dow, dexp, new_day, day_start, day_end, S, spot_c = build(bars)
    n = len(times)
    year = np.array([t[:4] for t in times])
    months = sorted(set(month))
    ic_rows, res_rows = [], []
    for me, other, label in (("n", "b", "NIFTY fut"), ("b", "n", "BANKNIFTY fut")):
        F = feats_for(S, me, other, spot_c, mins, dow, dexp, new_day, day_start)
        lc = np.log(S[me]["c"])
        # walk-forward CNN p(up), p(big) on this series' own 5m bars (monthly refit)
        f4, r1 = cnn_features(S[me]["h"], S[me]["l"], S[me]["c"], S[me]["v"])
        y1 = np.zeros(n)
        y1[:-1] = r1[1:]
        ok1 = np.zeros(n, bool)
        ok1[T:n - 1] = day[T + 1:] == day[T:n - 1]   # next bar in the same session
        cnn_up, cnn_big = np.full(n, 0.5), np.full(n, 0.5)
        for m in months[3:]:
            te = np.where((month == m) & (np.arange(n) >= T))[0]
            tr = np.where((month < m) & ok1)[0]
            tr = tr[month[np.minimum(tr + 1, n - 1)] < m][-60000:]
            if len(tr) < 2000 or len(te) == 0:
                continue
            mu, sd = f4[tr].mean(0), f4[tr].std(0) + 1e-12
            fz = np.clip((f4 - mu) / sd, -5, 5)
            xtr, xte = windows(fz, tr), windows(fz, te)
            net = Tcn(a.seed)
            net.fit(xtr, (y1[tr] > 0).astype(float), a.seed)
            cnn_up[te] = net.prob(xte)
            med = np.median(np.abs(y1[tr]))
            net2 = Tcn(a.seed + 1)
            net2.fit(xtr, (np.abs(y1[tr]) > med).astype(float), a.seed + 1)
            cnn_big[te] = net2.prob(xte)
        F["cnn_p_up"] = cnn_up - 0.5
        F["cnn_p_big"] = cnn_big - 0.5
        names = list(F)
        X = np.column_stack([np.nan_to_num(F[k]) for k in names])
        for hname, h in HORIZONS.items():
            tgt = day_end if h == 0 else np.minimum(np.arange(n) + h, day_end)
            valid = (tgt > np.arange(n)) & (np.arange(n) >= T)
            if h:
                valid &= (np.arange(n) + h) <= day_end
            y = 1e4 * (lc[tgt] - lc)
            # ---- part 1: IC per feature per year
            for k, col in zip(names, X.T):
                row = dict(series=label, horizon=hname, feature=k)
                for yr in ("2020", "2022", "2023"):
                    sel = valid & (year == yr) & (F["cnn_p_up"] != 0 if k.startswith("cnn") else True)
                    row[f"ic_{yr}"] = round(spearman(col[sel], y[sel]), 4) if sel.sum() > 100 else ""
                ic_rows.append(row)
            # ---- part 2: walk-forward combination
            # non-overlapping entry grid: every h-th bar of the session (h=0: one entry per bar, scored per day)
            step = max(h, 1)
            grid = valid & (((np.arange(n) - day_start) % step) == 0)
            pred = {m_: np.full(n, np.nan) for m_ in ("ridge", "gbm", "avg")}
            thr = {m_: np.full(n, np.nan) for m_ in pred}
            for m in months[6:]:
                te = np.where((month == m) & grid)[0]
                tr = np.where((month < m) & valid)[0]
                tr = tr[month[tgt[tr]] < m]
                if len(tr) < 3000 or len(te) == 0:
                    continue
                tr = tr[-80000:]
                mu, sd = X[tr].mean(0), X[tr].std(0) + 1e-12
                Z = np.clip((X - mu) / sd, -6, 6)
                yc = np.clip(y[tr], -np.quantile(np.abs(y[tr]), 0.995), np.quantile(np.abs(y[tr]), 0.995))
                rf = ridge(Z[tr], yc, lam=1e-2)
                gb = HistGradientBoostingRegressor(max_iter=150, learning_rate=0.05, max_leaf_nodes=15,
                                                   min_samples_leaf=200, l2_regularization=1.0,
                                                   random_state=a.seed).fit(Z[tr], yc)
                tail = tr[-max(len(tr) // 5, 500):]
                for m_, f_ in (("ridge", rf), ("gbm", gb.predict)):
                    pred[m_][te] = f_(Z[te])
                    thr[m_][te] = np.quantile(np.abs(f_(Z[tail])), 0.9)
                pred["avg"][te] = 0.5 * (pred["ridge"][te] + pred["gbm"][te])
                thr["avg"][te] = np.quantile(np.abs(0.5 * (rf(Z[tail]) + gb.predict(Z[tail]))), 0.9)
            har = F["har6"]
            for m_ in pred:
                sel = ~np.isnan(pred[m_])
                if h == 0:
                    sel &= (mins % 30 == 0)   # rest-of-day: one decision every 30 min, scored as separate trades
                p_, yv, dv, yr_ = pred[m_][sel], y[sel], day[sel], year[sel]
                hv = har[sel] >= np.quantile(har[valid], 0.75)
                ic = spearman(p_, yv)
                for mode, use in (("edge", np.abs(p_) > a.cost_bp), ("top10", np.abs(p_) >= thr[m_][sel]),
                                  ("hivol", (np.abs(p_) > a.cost_bp) & hv)):
                    net = np.sign(p_[use]) * yv[use] - a.cost_bp
                    gross = net + a.cost_bp
                    days = {}
                    for d_, x_ in zip(dv[use], net):
                        days[d_] = days.get(d_, 0.0) + x_
                    dn = np.array(list(days.values()))
                    r = dict(series=label, horizon=hname, model=m_, mode=mode, ic=round(ic, 4),
                             trades=int(use.sum()), gross_bp=round(float(gross.mean()), 2) if use.any() else 0,
                             net_bp=round(float(net.mean()), 2) if use.any() else 0,
                             hit=round(float((gross > 0).mean()), 3) if use.any() else "",
                             daily_t=round(nw_t(dn), 2) if len(dn) > 30 else "")
                    for y_ in ("2020", "2022", "2023"):
                        u = yr_[use] == y_
                        r[f"net_{y_}"] = round(float(net[u].mean()), 2) if u.any() else ""
                    res_rows.append(r)
                    print(r, flush=True)
    for fn, rows in (("combo_ic.csv", ic_rows), ("combo_summary.csv", res_rows)):
        with (out / fn).open("w", newline="") as fh:
            w = csv.DictWriter(fh, fieldnames=list(rows[0]))
            w.writeheader()
            w.writerows(rows)
    return 0


if __name__ == "__main__":
    sys.exit(main())
