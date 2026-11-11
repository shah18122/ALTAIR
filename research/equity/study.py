#!/usr/bin/env python3
"""Run the pre-registered equity hypotheses E1-E6 (HYPOTHESES.md) on the Drive 5-minute bars.

    python research/equity/study.py --eq EQ_DIR --bars BARS_DIR --split dev
    python research/equity/study.py --eq EQ_DIR --bars BARS_DIR --split holdout   # once, after RESULTS dev

EQ_DIR holds <year>/<SYM>_5min_OHLCV.csv (S:\\tick_data\\NSE Equities (5min)); BARS_DIR is the tree
from tools/drive_ticks_to_bars.py. Source drive-ticks, NOT FYERS. dev = 2020 + 2022, holdout = 2023.
Writes results/<split>_summary.csv and results/<split>_e6.csv and prints both.
"""
from __future__ import annotations

import argparse
import datetime as dt
from pathlib import Path

import numpy as np
import pandas as pd

from load import S, load_future, load_stocks, read_universe

HERE = Path(__file__).resolve().parent
SEED = 20261111
NPLACEBO = 2000
CASH_BP, SFUT_BP, IFUT_RT_BP = 4.0, 5.0, 7.0      # one-way, one-way, round trip
T_TRADE, T_VOL = 2.69, 2.50                        # Bonferroni bars from HYPOTHESES.md
BANK = ["HDFCBANK", "ICICIBANK", "KOTAKBANK", "AXISBANK", "SBIN", "INDUSINDBK", "BANKBARODA", "AUBANK",
        "FEDERALBNK", "IDFCFIRSTB", "PNB", "BANDHANBNK", "RBLBANK", "YESBANK"]
N_NIFTY = 58  # the first line of universe.txt (NIFTY 50 names 2020-2023)


class LeakError(AssertionError):
    pass


def check_time(sig_end, entry):
    """Times are (day, slot-boundary) pairs: bar s opens at boundary s and closes at s+1."""
    if tuple(sig_end) > tuple(entry):
        raise LeakError(f"signal ends {sig_end} after entry {entry}")


def tstat(x):
    x = np.asarray(x, float)
    x = x[~np.isnan(x)]
    return (x.mean() / (x.std(ddof=1) / np.sqrt(len(x)))) if len(x) > 2 else np.nan


def nw_t(d, lag):
    d = np.asarray(d, float)
    n = len(d)
    e = d - d.mean()
    v = e @ e / n
    for L in range(1, lag + 1):
        v += 2 * (1 - L / (lag + 1)) * (e[L:] @ e[:-L]) / n
    return d.mean() / np.sqrt(v / n)


# ---------------------------------------------------------------- data

def consecutive(days):
    """ok[i]: day i-1 is the previous trading session (same data year, gap <= 5 calendar days)."""
    ok = np.zeros(len(days), bool)
    for i in range(1, len(days)):
        ok[i] = (days[i] - days[i - 1]).days <= 5
    return ok


def prepare(eq, years):
    syms = read_universe(HERE / "universe.txt")
    days, syms, A = load_stocks(eq, years, syms)
    O, C, V, P = A["open"], A["close"], A["vol"], A["present"]
    D, K = P.shape
    cons = consecutive(days)
    close = C[:, S - 1]                                         # 15:25 bar close
    rcc = np.full((D, K), np.nan)
    rcc[1:] = np.log(close[1:] / close[:-1])
    rcc[~cons] = np.nan
    corp = np.abs(rcc) > 0.20                                     # unadjusted split/bonus
    tv = np.nansum(C * V, axis=1)                                 # traded value per day
    tv[~P] = np.nan
    elig = np.zeros((D, K), bool)
    for i in range(20, D):
        if not cons[i - 19:i + 1].all():
            continue
        w = tv[i - 20:i]
        n = np.sum(~np.isnan(w), axis=0)
        med = np.where(n >= 18, np.nanmedian(np.where(np.isnan(w), np.nan, w), axis=0), np.nan)
        med[~P[i]] = np.nan
        ok = ~np.isnan(med)
        if ok.sum() == 0:
            continue
        top = np.argsort(-np.where(ok, med, -np.inf))[:min(100, ok.sum())]
        elig[i, top] = True
    return dict(days=days, syms=syms, O=O, C=C, V=V, P=P, cons=cons, rcc=rcc, corp=corp, elig=elig)


# ---------------------------------------------------------------- cross-sectional engine

def deciles(sig, long_low):
    """Equal-weight long-short weights from a 1-D signal (NaN = not ranked). Gross 1."""
    w = np.zeros_like(sig)
    ok = ~np.isnan(sig)
    n = ok.sum()
    q = n // 10
    if q < 3:
        return w
    idx = np.where(ok)[0]
    order = idx[np.argsort(sig[ok], kind="stable")]
    lo, hi = order[:q], order[-q:]
    a, b = (lo, hi) if long_low else (hi, lo)
    w[a] = 0.5 / q
    w[b] = -0.5 / q
    return w


def run_xs(periods, one_way_bp, long_low, leak=False, perm=None):
    """periods: list of dict(sig_end, entry, sig (K,), ret (K,), day). Weights held entry -> exit.
    Costs on turnover: one_way_bp x sum|w_t - w_{t-1}|; a new day (E1, E2) starts flat."""
    out = []
    wprev = None
    prev_day = None
    for p in periods:
        check_time(p["sig_end"], p["entry"])
        sig = p["sig"].copy()
        if perm is not None:
            ok = np.where(~np.isnan(sig))[0]
            sig[ok] = sig[perm.permutation(ok)]
        if leak:  # the trade's own return as the signal: it ends at the exit, so it must be refused
            check_time(p["exit"], p["entry"])
        w = deciles(sig, long_low)
        if wprev is None or (p.get("flat_each_day") and p["day"] != prev_day):
            if wprev is not None and out:
                out[-1]["cost"] += one_way_bp * 1e-4 * np.abs(wprev).sum()
            turn = np.abs(w).sum()
        else:
            turn = np.abs(w - wprev).sum()
        r = np.nan_to_num(p["ret"])
        out.append(dict(day=p["day"], gross=float(w @ r), cost=one_way_bp * 1e-4 * turn, n=int((w != 0).sum())))
        wprev, prev_day = w, p["day"]
    if out and wprev is not None:
        out[-1]["cost"] += one_way_bp * 1e-4 * np.abs(wprev).sum()
    df = pd.DataFrame(out)
    df["net"] = df.gross - df.cost
    return df


def adj(x, m):
    x = np.where(m, x, np.nan)
    return x - np.nanmean(x)


def periods_e1(d, delay=0):
    O, C, rcc, corp, elig, cons = d["O"], d["C"], d["rcc"], d["corp"], d["elig"], d["cons"]
    out = []
    for i in range(2, len(d["days"])):
        if not (cons[i] and cons[i - 1]):
            continue
        m = elig[i] & ~corp[i - 1] & ~np.isnan(rcc[i - 1])
        if m.sum() < 30:
            continue
        sig = adj(rcc[i - 1], m)
        e, x = 1 + delay, 73 + delay
        ret = O[i, x] / O[i, e] - 1
        ret = np.where(m & ~np.isnan(ret), ret, 0.0)
        out.append(dict(day=d["days"][i], sig_end=(i - 1, S), entry=(i, e), exit=(i, x), sig=sig, ret=ret,
                        flat_each_day=True))
    return out


def periods_e2(d, delay=0):
    O, C, V, elig = d["O"], d["C"], d["V"], d["elig"]
    out = []
    for i in range(len(d["days"])):
        if elig[i].sum() < 30:
            continue
        for b in range(6, 67, 6):
            m = elig[i] & (V[i, b - 1] > 0)
            r30 = C[i, b - 1] / O[i, b - 6] - 1
            m &= ~np.isnan(r30)
            if m.sum() < 30:
                continue
            e, x = b + delay, b + 6 + delay
            ret = O[i, x] / O[i, e] - 1
            ret = np.where(m & ~np.isnan(ret), ret, 0.0)
            out.append(dict(day=d["days"][i], sig_end=(i, b), entry=(i, e), exit=(i, x), sig=adj(r30, m), ret=ret,
                            flat_each_day=True))
    return out


def periods_e3(d, delay=0):
    O, C, corp, elig, cons, days = d["O"], d["C"], d["corp"], d["elig"], d["cons"], d["days"]
    close = C[:, S - 1]
    reb = [i for i in range(1, len(days)) if days[i].isocalendar()[:2] != days[i - 1].isocalendar()[:2]]
    out = []
    for a, b in zip(reb[:-1], reb[1:]):
        if a < 22 or not cons[a - 21:b + 1 + delay].all() or b + delay >= len(days):
            continue
        m = elig[a] & ~corp[a - 21:a - 1].any(axis=0)
        sig = np.log(close[a - 2] / close[a - 22])
        m &= ~np.isnan(sig)
        if m.sum() < 30:
            continue
        e, x = a + delay, b + delay
        hold_ok = ~corp[e + 1:x + 1].any(axis=0)                 # cleaning: unadjusted prices only
        ret = O[x, 1] / O[e, 1] - 1
        ret = np.where(m & hold_ok & ~np.isnan(ret), ret, 0.0)
        out.append(dict(day=days[a], sig_end=(a - 2, S), entry=(e, 1), exit=(x, 1), sig=adj(sig, m),
                        ret=ret))
    return out


def xs_test(name, d, maker, one_way_bp, long_low, rng):
    base = run_xs(maker(d), one_way_bp, long_low)
    late = run_xs(maker(d, delay=1), one_way_bp, long_low)
    try:
        run_xs(maker(d), one_way_bp, long_low, leak=True)
        leak_caught = False
    except LeakError:
        leak_caught = True
    ps = maker(d)
    pl = np.array([run_xs(ps, one_way_bp, long_low, perm=rng).net.mean() for _ in range(NPLACEBO)])
    return summarise(name, base, late, pl, leak_caught)


# ---------------------------------------------------------------- index side (E4, E5, E6)

def stock_bar_returns(d, names):
    k = [d["syms"].index(s) for s in names if s in d["syms"]]
    C = d["C"][:, :, k]
    r = np.full(C.shape, np.nan)
    r[:, 1:] = np.log(C[:, 1:] / C[:, :-1])
    return r, [d["syms"][j] for j in k]


def ridge_basket(rf, rs, cons, lam=0.1):
    """Basket return per bar, weights refit at each month start on the previous 60 sessions."""
    D = rf.shape[0]
    days = DAYS
    bask = np.full(rf.shape, np.nan)
    w = None
    for i in range(D):
        if w is None or days[i].month != days[i - 1].month:
            w = None
            if i >= 60 and cons[i - 59:i + 1].all():
                X = rs[i - 60:i, 1:].reshape(-1, rs.shape[2])
                y = rf[i - 60:i, 1:].reshape(-1)
                cols = ~np.all(np.isnan(X), axis=0) & (np.mean(np.isnan(X), axis=0) < 0.2)
                ok = ~np.isnan(y)
                Xc = np.nan_to_num(X[ok][:, cols])
                yy = y[ok]
                XtX = Xc.T @ Xc
                L = lam * np.mean(np.diag(XtX))
                wc = np.linalg.solve(XtX + L * np.eye(cols.sum()), Xc.T @ yy)
                w = np.zeros(rs.shape[2])
                w[cols] = wc
        if w is not None:
            bask[i, 1:] = np.nan_to_num(rs[i, 1:]) @ w
    return bask


def fut_returns(Cf):
    r = np.full(Cf.shape, np.nan)
    r[:, 1:] = np.log(Cf[:, 1:] / Cf[:, :-1])
    return r


def e4_trades(g, Of, cons, delay=0, leak=False):
    D = g.shape[0]
    out = []
    for i in range(20, D):
        if not cons[i - 19:i + 1].all():
            continue
        sd = np.nanstd(g[i - 20:i, 1:])
        if not np.isfinite(sd) or sd == 0 or np.isnan(g[i, 1:]).all():
            continue
        for t in range(1, 73 - delay):
            e, x = t + 1 + delay, t + 2 + delay
            if x > S - 1:
                break
            sig = g[i, t]
            if leak:
                sig = Of[i, x] / Of[i, e] - 1
                check_time((i, x), (i, e))
            check_time((i, t + 1), (i, e))
            if np.isnan(sig) or abs(sig) <= 2 * sd:
                continue
            gr = np.sign(sig) * (Of[i, x] / Of[i, e] - 1)
            if np.isfinite(gr):
                out.append(dict(day=DAYS[i], slot=t, side=np.sign(sig), gross=gr))
    return pd.DataFrame(out)


def e5_trades(g, Of, cons, delay=0, leak=False):
    D = g.shape[0]
    G = np.nancumsum(np.where(np.isnan(g), 0, g), axis=1)
    G[:, 0] = np.nan
    out = []
    for i in range(20, D):
        if not cons[i - 19:i + 1].all() or np.isnan(g[i, 1:]).all():
            continue
        sd = np.nanstd(G[i - 20:i, 1:])
        if not np.isfinite(sd) or sd == 0:
            continue
        nxt = 0
        for t in range(5, 66):
            if t < nxt:
                continue
            e, x = t + 1 + delay, t + 7 + delay
            if x > S - 1:
                break
            z = G[i, t] / sd
            if leak:
                z = 3 * np.sign(Of[i, x] / Of[i, e] - 1)
                check_time((i, x), (i, e))
            check_time((i, t + 1), (i, e))
            if abs(z) <= 2:
                continue
            gr = np.sign(z) * (Of[i, x] / Of[i, e] - 1)
            if np.isfinite(gr):
                out.append(dict(day=DAYS[i], slot=t, side=np.sign(z), gross=gr))
                nxt = t + 6
    return pd.DataFrame(out)


def index_test(name, g, Of, cons, fn, rng):
    base = fn(g, Of, cons)
    late = fn(g, Of, cons, delay=1)
    try:
        fn(g, Of, cons, leak=True)
        caught = False
    except LeakError:
        caught = True
    for df in (base, late):
        if len(df):
            df["cost"] = IFUT_RT_BP * 1e-4
            df["net"] = df.gross - df.cost
    if len(base):
        s = rng.choice([-1.0, 1.0], size=(NPLACEBO, len(base)))
        pl = (s * (base.gross / base.side).to_numpy()).mean(axis=1) - IFUT_RT_BP * 1e-4
    else:
        pl = np.array([np.nan])
    return summarise(name, base, late, pl, caught)


def summarise(name, base, late, pl, caught):
    if len(base) == 0:
        return dict(rule=name, n=0)
    yrs = pd.Series([d.year for d in base.day])
    by = {y: base.net[yrs.to_numpy() == y].mean() * 1e4 for y in sorted(yrs.unique())}
    daily = base.groupby("day").net.sum()
    return dict(rule=name, n=len(base), gross_bp=base.gross.mean() * 1e4, cost_bp=base.cost.mean() * 1e4,
                net_bp=base.net.mean() * 1e4, t=tstat(base.net), t_daily=tstat(daily),
                by_year=" ".join(f"{y}:{v:+.2f}" for y, v in by.items()),
                placebo95_bp=np.nanpercentile(pl, 95) * 1e4,
                late_net_bp=late.net.mean() * 1e4 if len(late) else np.nan,
                leak_caught=caught)


# ---------------------------------------------------------------- E6 volatility

def logv(x):
    return np.log(np.maximum(x, 1e-12))


def e6(rf, rs, days, test_years, train_start=60):
    """Returns per-forecast errors for HAR and augmented, hourly (a) and daily (b)."""
    D = rf.shape[0]
    cons = consecutive(days)
    sq = rf ** 2
    rv_day = np.nansum(sq[:, 1:], axis=1)
    rvd = np.where(np.sum(~np.isnan(rf[:, 1:]), axis=1) > 60, rv_day, np.nan)
    # stock side per bar: mean constituent squared return, dispersion, basket variance
    rsa = rs - np.nanmean(rs, axis=2, keepdims=True)
    msq = np.nanmean(rs ** 2, axis=2)
    disp = np.nanmean(rsa ** 2, axis=2)
    bsq = np.nanmean(rs, axis=2) ** 2

    def win(a, i, lo, hi):
        return np.nansum(a[i, lo:hi])

    def stock_feats(i, lo, hi, days_back=0):
        if days_back:
            js = range(i - days_back, i)
            m = np.nanmean([np.nansum(msq[j, 1:]) for j in js])
            dd = np.nanmean([np.nansum(disp[j, 1:]) for j in js])
            b = np.nanmean([np.nansum(bsq[j, 1:]) for j in js])
        else:
            m, dd, b = win(msq, i, lo, hi), win(disp, i, lo, hi), win(bsq, i, lo, hi)
        return [logv(m), logv(dd), logv(b / max(m, 1e-12))]

    rows_h, rows_d = [], []
    origins = [12, 24, 36, 48, 60]
    for i in range(22, D):
        if not cons[i - 21:i + 1].all() or np.isnan(rvd[i - 22:i]).any():
            continue
        last1, last5, last22 = rvd[i - 1], np.mean(rvd[i - 5:i]), np.mean(rvd[i - 22:i])
        if not np.isnan(rvd[i]):
            base = [logv(last1), logv(last5), logv(last22)]
            aug = base + stock_feats(i - 1, 1, S) + stock_feats(i, 0, 0, 5) + stock_feats(i, 0, 0, 22)
            rows_d.append(dict(i=i, day=days[i], y=logv(rvd[i]), b=base, a=aug))
        if np.isnan(rf[i, 1:]).mean() > 0.2:
            continue
        for h, o in enumerate(origins):
            hi = S if o == 60 else o + 12
            y = np.nansum(sq[i, o:hi])
            base = [logv(win(sq, i, max(1, o - 12), o)), logv(win(sq, i, 1, o)), logv(last1), logv(last5)]
            base += [1.0 if h == k else 0.0 for k in range(1, 5)]
            aug = base + stock_feats(i, max(1, o - 12), o) + stock_feats(i, 1, o) + stock_feats(i - 1, 1, S) \
                + stock_feats(i, 0, 0, 5)
            rows_h.append(dict(i=i, day=days[i], y=logv(y), b=base, a=aug))

    def walk(rows):
        if not rows:
            return pd.DataFrame()
        df = pd.DataFrame(rows)
        Xb = np.array(df.b.tolist())
        Xa = np.array(df.a.tolist())
        y = df.y.to_numpy()
        ok_all = np.isfinite(Xa).all(axis=1) & np.isfinite(y)
        out = []
        months = pd.Series([(d.year, d.month) for d in df.day])
        first_day = days[train_start]
        for mth in sorted(months.unique()):
            te = (months == mth).to_numpy() & ok_all & np.array([d.year in test_years for d in df.day])
            if not te.any() or df.day[te].iloc[0] < first_day:
                continue
            tr = np.array([d < df.day[te].iloc[0] for d in df.day]) & ok_all
            if tr.sum() < 100:
                continue
            for name, X in (("har", Xb), ("aug", Xa)):
                Z = np.c_[np.ones(len(X)), X]
                beta = np.linalg.lstsq(Z[tr], y[tr], rcond=None)[0]
                df.loc[te, name] = Z[te] @ beta
            out.append(df[te])
        r = pd.concat(out)
        r["e_har"] = r.y - r.har
        r["e_aug"] = r.y - r.aug
        return r[["day", "y", "har", "aug", "e_har", "e_aug"]]

    return walk(rows_h), walk(rows_d)


def e6_summary(name, r, lag):
    d = r.e_har ** 2 - r.e_aug ** 2
    ql = lambda y, f: np.exp(y - f) - (y - f) - 1  # QLIKE on variance with log forecasts
    yrs = np.array([x.year for x in r.day])
    by = " ".join(f"{y}:{100 * (1 - (r.e_aug[yrs == y] ** 2).mean() / (r.e_har[yrs == y] ** 2).mean()):+.1f}%"
                  for y in sorted(set(yrs)))
    return dict(test=name, n=len(r), mse_har=(r.e_har ** 2).mean(), mse_aug=(r.e_aug ** 2).mean(),
                gain_pct=100 * (1 - (r.e_aug ** 2).mean() / (r.e_har ** 2).mean()), dm_t=nw_t(d.to_numpy(), lag),
                qlike_gain_pct=100 * (1 - ql(r.y, r.aug).mean() / ql(r.y, r.har).mean()), by_year=by)


# ---------------------------------------------------------------- main

DAYS: list = []


def main():
    global DAYS
    ap = argparse.ArgumentParser()
    ap.add_argument("--eq", required=True)
    ap.add_argument("--bars", required=True)
    ap.add_argument("--split", choices=["dev", "holdout"], required=True)
    ap.add_argument("--only", choices=["all", "xs", "index"], default="all", help="part to run (rerun after a crash)")
    a = ap.parse_args()
    years = ["2020", "2022"] if a.split == "dev" else ["2020", "2022", "2023"]
    test_years = {2020, 2022} if a.split == "dev" else {2023}
    rng = np.random.default_rng(SEED)
    d = prepare(a.eq, years)
    DAYS = d["days"]
    keep = lambda df: df[[x.year in test_years for x in df.day]] if len(df) else df
    rows = []

    # E1-E3: cross-sectional (holdout only scores 2023 periods; 2022 is history for signals)
    for name, maker, bp, low in [] if a.only == "index" else (("E1 daily reversal", periods_e1, CASH_BP, True),
                                 ("E2 30-min reversal", periods_e2, CASH_BP, True),
                                 ("E3 1-month momentum", periods_e3, SFUT_BP, False)):
        mk = (lambda m: (lambda dd, delay=0: [p for p in m(dd, delay) if p["day"].year in test_years]))(maker)
        rows.append(xs_test(name, d, mk, bp, low, rng))
        print(rows[-1], flush=True)

    # E4, E5: index futures vs basket
    e6rows = []
    for inst, names in [] if a.only == "xs" else (("banknifty", BANK), ("nifty", read_universe(HERE / "universe.txt")[:N_NIFTY])):
        Of, Cf = load_future(a.bars, inst, DAYS)
        rf = fut_returns(Cf)
        rs, used = stock_bar_returns(d, names)
        bask = ridge_basket(rf, rs, d["cons"])
        g = bask - rf
        for nm, fn in (("E4 basket lead 5m", e4_trades), ("E5 mispricing level", e5_trades)):
            r = index_test(f"{nm} {inst.upper()}", g, Of, d["cons"], lambda *x, fn=fn, **k: keep(fn(*x, **k)), rng)
            rows.append(r)
            print(r, flush=True)
        rh, rd = e6(rf, rs, DAYS, test_years)
        e6rows.append(e6_summary(f"E6a next-hour RV {inst.upper()}", rh, 5))
        e6rows.append(e6_summary(f"E6b next-day RV {inst.upper()}", rd, 1))
        print(e6rows[-2], e6rows[-1], flush=True)

    res = HERE / "results"
    res.mkdir(exist_ok=True)
    s = pd.DataFrame(rows)
    tag = "" if a.only == "all" else f"_{a.only}"
    s.to_csv(res / f"{a.split}_summary{tag}.csv", index=False, float_format="%.4f")
    v = pd.DataFrame(e6rows)
    v.to_csv(res / f"{a.split}_e6{tag}.csv", index=False, float_format="%.5f")
    with pd.option_context("display.width", 250, "display.max_columns", 30):
        print(s.to_string(index=False))
        print(v.to_string(index=False))


if __name__ == "__main__":
    main()
