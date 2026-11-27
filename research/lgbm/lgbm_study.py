#!/usr/bin/env python3
"""research/lgbm/lgbm_study.py -- LightGBM on BANKNIFTY futures microstructure features, labels net of
costs, triple-barrier exits and a meta-label. Pre-registered in README.md; walk-forward, out of sample.

Input: .npz files from research/signals/load.py (Drive ticks, source "drive-ticks", NOT FYERS).

    python research/lgbm/lgbm_study.py --dev bnf2020.npz bnf2022.npz --holdout bnf2023.npz --out research/lgbm/results
    python research/lgbm/lgbm_study.py --selftest      # synthetic data: plumbing + leak check
"""
from __future__ import annotations

import argparse
import os
import time

import numpy as np
import pandas as pd

T0 = 9 * 3600 + 15 * 60
N = 6 * 3600 + 15 * 60
SAMPLE = 5
START, STOP = 5 * 60, 6 * 3600 + 5 * 60          # 09:20 .. 15:20
HORIZONS = (60, 300)
COST = 6.0                                        # bp round trip, 2026 futures schedule + ~0.5 bp spread
BARRIER_K = 1.5
MIN_TRAIN_DAYS, REFIT = 60, 20
TRAIN_EVERY = 3                                   # train on every 3rd row (15 s) to keep fits fast
LEADERS = ("HDFCBANK", "ICICIBANK")
PARAMS = dict(learning_rate=0.05, num_leaves=31, min_data_in_leaf=500, feature_fraction=0.8,
              bagging_fraction=0.8, bagging_freq=1, lambda_l2=1.0, verbose=-1, num_threads=0, seed=20261127)
ROUNDS = 200


def lag(x, k):
    y = np.full_like(x, np.nan)
    y[..., k:] = x[..., :-k]
    return y


def rsum(x, k):
    c = np.nancumsum(x, axis=-1)
    return c - lag(c, k)


def ewma_rows(x, half):
    a = 1.0 - 0.5 ** (1.0 / half)
    return pd.DataFrame(x.T).ewm(alpha=a, ignore_na=True).mean().to_numpy().T


def features(path: str):
    """One .npz -> (rows DataFrame, day list). Every feature at second t uses data <= t only."""
    z = np.load(path)
    days, syms = z["days"], [str(s) for s in z["syms"]]
    F = np.log(z["fpx"].astype(np.float64))
    S = np.log(z["spx"])                                         # float32 D x K x N
    fvol, fsv, foi, fn = z["fvol"], z["fsv"], z["foi"], z["fn"]
    expiry = z["expiry"]
    D, K, _ = S.shape
    cols = np.arange(START, STOP, SAMPLE)

    # basket betas from the 20 PRIOR days only
    i60 = np.arange(START, STOP, 60)
    rF60 = (F[:, i60[1:]] - F[:, i60[:-1]]) * 1e4
    rS60 = (S[:, :, i60[1:]] - S[:, :, i60[:-1]]).astype(np.float64) * 1e4
    beta = np.full((D, K), np.nan)
    for i in range(20, D):
        X = rS60[i - 20:i].transpose(0, 2, 1).reshape(-1, K)
        y = rF60[i - 20:i].reshape(-1)
        ok = np.isfinite(X).all(1) & np.isfinite(y)
        if ok.sum() < 200:
            continue
        X, y = X[ok], y[ok]
        beta[i] = np.linalg.solve(X.T @ X + 1e-2 * len(X) * np.eye(K) * X.var(0).mean(), X.T @ y)
    B = np.stack([np.nansum(beta[i][:, None] * S[i].astype(np.float64), 0) if np.isfinite(beta[i]).all()
                  else np.full(N, np.nan) for i in range(D)])
    gap = F - B
    f = {}
    for k in (10, 60, 300):
        f[f"ret{k}"] = (F - lag(F, k)) * 1e4
    for name in LEADERS:
        if name not in syms:
            continue
        s = S[:, syms.index(name)].astype(np.float64)
        for k in (5, 10, 30):
            f[f"lead_{name}_{k}"] = ((s - lag(s, k)) - (F - lag(F, k))) * 1e4
    for k in (10, 30):
        f[f"leadgap{k}"] = ((B - lag(B, k)) - (F - lag(F, k))) * 1e4
    f["misprice"] = -(gap - ewma_rows(gap, 1800)) * 1e4
    up = np.sign(S - lag(S, 60))
    w = np.abs(beta)[:, :, None]
    f["breadth"] = np.nansum(w * up, 1) / np.nansum(w, 1)
    del up
    for k in (10, 60, 300):
        v = rsum(fvol, k)
        f[f"flow{k}"] = rsum(fsv, k) / np.where(v > 0, v, np.nan)
    f["intensity"] = rsum(fn, 60) / (rsum(fn, 1800) / 30.0 + 1e-9)
    f["oi"] = np.sign(F - lag(F, 900)) * (foi - lag(foi, 900)) / foi * 1e2
    r10 = (F - lag(F, 10)) * 1e4
    sq10 = np.where(np.arange(N) % 10 == 0, r10 ** 2, 0.0)
    sq10 = np.where(np.isfinite(sq10), sq10, 0.0)
    f["rv5m"] = np.sqrt(rsum(sq10, 300))
    f["rv30m"] = np.sqrt(rsum(sq10, 1800) / 6)
    f["rvday"] = np.sqrt(np.cumsum(sq10, 1) / np.maximum(np.arange(N) / 300.0, 1))

    rows = {k: v[:, cols].reshape(-1) for k, v in f.items()}
    rows["tod"] = np.tile(cols, D).astype(np.float64)
    dd = pd.to_datetime(pd.Series(days).astype(str), format="%Y%m%d")
    ex = pd.to_datetime(pd.Series(expiry).astype(str), format="%Y%m%d")
    rows["dte"] = np.repeat((ex - dd).dt.days.to_numpy(), len(cols)).astype(np.float64)
    rows["day"] = np.repeat(days, len(cols))
    rows["col"] = np.tile(cols, D)

    # targets, from t+1 (entry latency); triple-barrier long outcome (short = minus it)
    for h in HORIZONS:
        win = np.lib.stride_tricks.sliding_window_view(F, h + 1, axis=1)[:, cols + 1]   # D x R x h+1
        path = (win - win[:, :, :1]) * 1e4
        rows[f"y{h}"] = path[:, :, -1].reshape(-1)
        b = np.maximum(COST, BARRIER_K * f["rv5m"][:, cols] * np.sqrt(h / 300.0))[:, :, None]
        hit = (path >= b) | (path <= -b)
        first = np.where(hit.any(2), hit.argmax(2), h)
        tb = np.take_along_axis(path, first[:, :, None], 2)[:, :, 0]
        rows[f"tb{h}"] = tb.reshape(-1)
        rows[f"tbsec{h}"] = first.reshape(-1).astype(np.float64)
    return pd.DataFrame(rows), list(days)


FEATS_EXCLUDE = {"day", "col"} | {f"{p}{h}" for p in ("y", "tb", "tbsec") for h in HORIZONS}


def walk_forward(df: pd.DataFrame, days: list, h: int):
    import lightgbm as lgb
    feats = [c for c in df.columns if c not in FEATS_EXCLUDE]
    y = df[f"y{h}"].to_numpy()
    lab = np.where(y > COST, 2, np.where(y < -COST, 0, 1))
    ok = np.isfinite(y) & np.isfinite(df[f"tb{h}"].to_numpy())
    day_ix = df["day"].map({d: i for i, d in enumerate(days)}).to_numpy()
    X = df[feats].to_numpy(np.float32)
    P = np.full((len(df), 3), np.nan)
    meta = np.full(len(df), np.nan)
    tbl = df[f"tb{h}"].to_numpy()
    for s in range(MIN_TRAIN_DAYS, len(days), REFIT):
        tr = ok & (day_ix < s) & (df["col"].to_numpy() % (SAMPLE * TRAIN_EVERY) == START % (SAMPLE * TRAIN_EVERY))
        te = ok & (day_ix >= s) & (day_ix < s + REFIT)
        m = lgb.train({**PARAMS, "objective": "multiclass", "num_class": 3},
                      lgb.Dataset(X[tr], lab[tr]), ROUNDS)
        P[te] = m.predict(X[te])
        # meta: past OUT-OF-SAMPLE primary signals only, label = triple-barrier net > 0
        past = ok & (day_ix < s) & np.isfinite(P[:, 0])
        side = np.where(P[:, 2] > 0.5, 1, np.where(P[:, 0] > 0.5, -1, 0))
        mt = past & (side != 0)
        if mt.sum() >= 300:
            Xm = np.column_stack([X[mt], P[mt]])
            ym = (side[mt] * tbl[mt] - COST > 0).astype(int)
            mm = lgb.train({**PARAMS, "objective": "binary", "min_data_in_leaf": 50}, lgb.Dataset(Xm, ym), ROUNDS)
            tsel = te & (side != 0)
            if tsel.any():
                meta[tsel] = mm.predict(np.column_stack([X[tsel], P[tsel]]))
    return P, meta, ok


def simulate(df, P, meta, ok, h, use_meta):
    """One position at a time per day; exit at the triple barrier; returns per-trade frame."""
    side = np.where(P[:, 2] > 0.5, 1, np.where(P[:, 0] > 0.5, -1, 0))
    take = ok & (side != 0) & np.isfinite(P[:, 0])
    if use_meta:
        take &= np.nan_to_num(meta, nan=0.0) > 0.5
    tb, sec = df[f"tb{h}"].to_numpy(), df[f"tbsec{h}"].to_numpy()
    day, col = df["day"].to_numpy(), df["col"].to_numpy()
    out = []
    busy_day, busy_until = None, -1
    for i in np.flatnonzero(take):
        if day[i] == busy_day and col[i] + 1 <= busy_until:
            continue
        g = side[i] * tb[i]
        out.append((day[i], side[i], g, g - COST))
        busy_day, busy_until = day[i], col[i] + 1 + sec[i]
    return pd.DataFrame(out, columns=["day", "side", "gross", "net"])


def judge(tr: pd.DataFrame, scored_days: list):
    """Daily P&L over every scored day (0 on days without a trade); thirds are thirds of those days."""
    nd = len(scored_days)
    if len(tr) == 0:
        return dict(trades=0, per_day=0.0, net_bp=np.nan, t_daily=np.nan, third1=0.0, third2=0.0, third3=0.0)
    daily = tr.groupby("day").net.sum().reindex(scored_days, fill_value=0.0)
    t = daily.mean() / (daily.std(ddof=1) / np.sqrt(nd)) if nd > 2 and daily.std() > 0 else np.nan
    w, lo = tr[tr.net > 0], tr[tr.net <= 0]
    thirds = [float(x.sum()) for x in np.array_split(daily.to_numpy(), 3)]
    return dict(trades=len(tr), per_day=len(tr) / nd, hit=100 * len(w) / len(tr),
                avg_win=w.net.mean(), avg_loss=lo.net.mean(), long_share=100 * (tr.side > 0).mean(),
                gross_bp=tr.gross.mean(), net_bp=tr.net.mean(), t_daily=t,
                third1=thirds[0], third2=thirds[1], third3=thirds[2])


def run(dev_paths, hold_paths, outdir):
    os.makedirs(outdir, exist_ok=True)
    parts, days, split = [], [], {}
    for p, sp in [(p, "dev") for p in dev_paths] + [(p, "holdout") for p in hold_paths]:
        t0 = time.time()
        df, d = features(p)
        parts.append(df)
        days += d
        split.update({x: sp for x in d})
        print(f"{p}: {len(d)} days, {len(df)} rows, {time.time() - t0:.0f} s", flush=True)
    df = pd.concat(parts, ignore_index=True)
    del parts
    res = []
    for h in HORIZONS:
        t0 = time.time()
        P, meta, ok = walk_forward(df, days, h)
        sig = P[:, 2] - P[:, 0]
        for sp in ("dev", "holdout"):
            m = ok & np.isfinite(sig) & (df["day"].map(split).to_numpy() == sp)
            ic = pd.DataFrame({"d": df["day"][m], "s": sig[m], "y": df[f"y{h}"][m]}).groupby("d").apply(
                lambda g: g.s.corr(g.y, method="spearman")).mean()
            sd = [d for d in days[MIN_TRAIN_DAYS:] if split[d] == sp]
            for v, um in (("primary", False), ("primary+meta", True)):
                tr = simulate(df[m].reset_index(drop=True), P[m], meta[m], ok[m], h, um)
                tr.to_csv(os.path.join(outdir, f"trades_{sp}_{h}s_{v.replace('+', '_')}.csv"), index=False)
                res.append(dict(split=sp, horizon_s=h, variant=v, days=len(sd), ic=ic, **judge(tr, sd)))
        print(f"h={h}s walk-forward {time.time() - t0:.0f} s", flush=True)
    r = pd.DataFrame(res)
    # pass bar (README): dev and holdout net>0, t>=2.5, >=100 trades, holdout thirds all > 0
    def passed(g):
        d, ho = g[g.split == "dev"].iloc[0], g[g.split == "holdout"].iloc[0]
        return bool(d.trades >= 100 and np.isfinite(d.t_daily) and np.isfinite(ho.t_daily)
                    and ho.trades >= 100 and d.net_bp > 0 and ho.net_bp > 0
                    and d.t_daily >= 2.5 and ho.t_daily >= 2.5 and min(ho.third1, ho.third2, ho.third3) > 0)
    r["pass"] = r.groupby(["horizon_s", "variant"]).apply(lambda g: pd.Series(passed(g), index=g.index)).reset_index(
        level=[0, 1], drop=True)
    r.to_csv(os.path.join(outdir, "summary.csv"), index=False, float_format="%.3f")
    print(r.round(2).to_string())
    return r


def selftest(tmp):
    """Synthetic npz: random-walk future + 12 stocks. A model must find nothing (net < 0), and a planted
    future-leak feature must be impossible (features use lag only; targets start at t+1)."""
    rng = np.random.default_rng(1)
    paths = []
    for yr, nd in (("2020", 90), ("2023", 40)):
        D, K = nd, 12
        days = np.array([int(f"{yr}{m:02d}{d:02d}") for m in range(1, 13) for d in range(1, 29)][:D])
        f = 30000 * np.exp(np.cumsum(rng.normal(0, 1e-4, (D, N)), 1))
        s = 1000 * np.exp(np.cumsum(rng.normal(0, 1e-4, (D, K, N)), 2)).astype(np.float32)
        vol = rng.integers(0, 50, (D, N)).astype(float)
        p = os.path.join(tmp, f"syn{yr}.npz")
        np.savez_compressed(p, days=days, expiry=np.full(D, int(f"{yr}1231")), syms=np.array(
            ["HDFCBANK", "ICICIBANK"] + [f"S{i}" for i in range(K - 2)]), fpx=f, fvol=vol,
            fsv=vol * rng.choice([-1, 1], (D, N)), foi=np.full((D, N), 1e6), fn=(vol > 0).astype(float),
            spx=s, svol=np.ones((D, K, N), np.float32))
        paths.append(p)
    r = run([paths[0]], [paths[1]], os.path.join(tmp, "out"))
    assert not r["pass"].any(), "random walk must not pass"
    print("selftest OK: nothing passes on a random walk")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--dev", nargs="*", default=[])
    ap.add_argument("--holdout", nargs="*", default=[])
    ap.add_argument("--out", default="research/lgbm/results")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        import tempfile
        selftest(tempfile.mkdtemp())
    else:
        run(a.dev, a.holdout, a.out)
