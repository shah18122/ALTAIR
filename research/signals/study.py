"""research/signals/study.py -- do the advanced signals predict BANKNIFTY futures? Walk-forward, out of sample.

Input: the .npz from load.py (Drive ticks, source "drive-ticks", not FYERS).

Every SAMPLE seconds from 09:20 to 15:20 a row is made with only what was known
at that second. Targets start ONE SECOND LATER (entry latency), so a print's
bid/ask bounce at t cannot be traded back at t:
    ret_h  = log F(t+1+h) - log F(t+1), bp, h in HORIZONS
    rv300  = sqrt(sum of squared 10-s log returns over (t+1, t+301]), bp
Models are linear (ridge), refitted before every test day on all EARLIER days
only (expanding window, first MIN_TRAIN days are never scored).

Signals (each one known at t):
  rev       the future's own last 10 s and 60 s return (bounce/reversal control)
  misprice  index vs constituents: log F minus the beta-weighted basket of the
            12 bank stocks, minus its 30-min EWMA (carry and drift); betas from a
            ridge of 60-s future returns on 60-s stock returns over the 20 prior days
  leadgap   basket return minus future return over the last 10 s and 30 s
  breadth   beta-weighted share of stocks up minus down over the last 60 s
  flow      tick-rule signed future volume / volume, last 60 s and 300 s
  oi        sign of the 15-min price change x the 15-min OI change (% of OI):
            long build-up / short covering (+) vs short build-up / long unwinding (-)
  stockflow volume surge of the basket (last 60 s vs last 30 min) x sign of its move
Vol signals (target rv300; baseline = HAR: rv last 5 min, 30 min, day so far, time-of-day mean of past days):
  intensity trades in the last 60 s / per-minute average over 30 min
  dispersion cross-sectional std of the stocks' 5-min returns
  corr      average pairwise correlation of 30-s stock returns over 30 min
  vpin      |signed volume| / volume over the last 5 min (bulk toxicity)
  absmis    |misprice|

    python3 research/signals/study.py bnf2023.npz [report.md]
"""
from __future__ import annotations

import sys

import numpy as np

SAMPLE = 5
START, STOP = 5 * 60, 6 * 3600 + 5 * 60  # 09:20 .. 15:20 in slots from 09:15
HORIZONS = (10, 60, 300)
MIN_TRAIN = 40
RIDGE = 1e-3
COST_2026 = 6.0  # bp round trip, BANKNIFTY future: STT 0.05% sell + exch + stamp + GST + ~0.5 bp spread
COST_2023 = 3.0  # bp round trip at the 2023 STT (0.0125%)


def lag(x, k):
    """x[t-k] along the last axis, NaN where unknown."""
    y = np.full_like(x, np.nan)
    y[..., k:] = x[..., :-k]
    return y


def lead(x, k):
    y = np.full_like(x, np.nan)
    y[..., :-k] = x[..., k:]
    return y


def rsum(x, k):
    """sum of x over the last k slots (t-k, t]."""
    c = np.nancumsum(x, axis=-1)
    return c - lag(c, k)


def ewma(x, half):
    a = 1.0 - 0.5 ** (1.0 / half)
    out = np.empty_like(x)
    m = np.nan
    for i, v in enumerate(x):
        if np.isnan(v):
            out[i] = m
            continue
        m = v if np.isnan(m) else m + a * (v - m)
        out[i] = m
    return out


def ridge(X, y, lam=RIDGE):
    ok = np.isfinite(X).all(1) & np.isfinite(y)
    X, y = X[ok], y[ok]
    mu, sd = X.mean(0), X.std(0) + 1e-12
    Z = (X - mu) / sd
    ym = y.mean()
    b = np.linalg.solve(Z.T @ Z + lam * len(Z) * np.eye(Z.shape[1]), Z.T @ (y - ym))
    return lambda Xn: ym + ((Xn - mu) / sd) @ b


def main(path, out=None):
    z = np.load(path)
    days, syms = z["days"], list(z["syms"])
    F = np.log(z["fpx"])                  # D x N
    S = np.log(z["spx"].astype(np.float64))  # D x K x N
    fvol, fsv, foi, fn = z["fvol"], z["fsv"], z["foi"], z["fn"]
    svol = z["svol"].astype(np.float64)
    D, K, N = S.shape
    print(f"{D} days {days[0]}..{days[-1]}, {K} stocks")

    # --- basket betas, walk-forward: ridge of 60-s future returns on 60-s stock returns, 20 prior days
    idx60 = np.arange(START, STOP, 60)
    rF60 = (F[:, idx60[1:]] - F[:, idx60[:-1]]) * 1e4
    rS60 = (S[:, :, idx60[1:]] - S[:, :, idx60[:-1]]) * 1e4
    beta = np.full((D, K), np.nan)
    for i in range(20, D):
        X = rS60[i - 20:i].transpose(0, 2, 1).reshape(-1, K)
        y = rF60[i - 20:i].reshape(-1)
        ok = np.isfinite(X).all(1) & np.isfinite(y)
        X, y = X[ok], y[ok]
        beta[i] = np.linalg.solve(X.T @ X + 1e-2 * len(X) * np.eye(K) * X.var(0).mean(), X.T @ y)
    B = np.einsum("dk,dkn->dn", beta, S)                       # basket log level
    gap = F - B
    gap_ew = np.vstack([ewma(g, 1800) for g in gap])
    misprice = -(gap - gap_ew) * 1e4                         # +: future cheap vs basket
    # betas from the day's own opening (09:15-09:20) seed the EWMA; misprice is only used from 09:20

    feats = {}
    feats["rev10"] = (F - lag(F, 10)) * 1e4
    feats["rev60"] = (F - lag(F, 60)) * 1e4
    feats["misprice"] = misprice
    feats["leadgap10"] = ((B - lag(B, 10)) - (F - lag(F, 10))) * 1e4
    feats["leadgap30"] = ((B - lag(B, 30)) - (F - lag(F, 30))) * 1e4
    w = np.abs(beta)[:, :, None]
    up = np.sign(S - lag(S, 60))
    feats["breadth"] = np.nansum(w * up, 1) / np.nansum(w, 1)
    v60, v300 = rsum(fvol, 60), rsum(fvol, 300)
    feats["flow60"] = rsum(fsv, 60) / np.where(v60 > 0, v60, np.nan)
    feats["flow300"] = rsum(fsv, 300) / np.where(v300 > 0, v300, np.nan)
    feats["oi"] = np.sign(F - lag(F, 900)) * (foi - lag(foi, 900)) / foi * 1e2
    bv = np.einsum("dk,dkn->dn", np.abs(beta), svol / np.nanmean(svol, axis=2, keepdims=True))
    surge = rsum(bv, 60) / (rsum(bv, 1800) / 30.0)
    feats["stockflow"] = np.log(surge) * np.sign(B - lag(B, 60))

    # --- vol features
    r10 = (F - lag(F, 10)) * 1e4
    sq10 = np.where(np.arange(N) % 10 == 0, r10 ** 2, 0.0)
    rv = lambda k: np.sqrt(rsum(sq10, k))
    vf = {}
    vf["rv5m"], vf["rv30m"] = rv(300), rv(1800) / np.sqrt(6)
    day_cum = np.nancumsum(sq10, axis=1)
    vf["rvday"] = np.sqrt(day_cum / np.maximum(np.arange(N), 1) * 300)
    fut_sq = lead(np.nancumsum(sq10, 1), 301) - lead(np.nancumsum(sq10, 1), 1)
    target_rv = np.sqrt(fut_sq)
    # time-of-day mean of past days' target (walk-forward)
    tod = np.full_like(target_rv, np.nan)
    cs = np.nancumsum(np.nan_to_num(target_rv), 0)
    tod[1:] = cs[:-1] / np.arange(1, D)[:, None]
    vf["tod"] = tod
    n60, n1800 = rsum(fn, 60), rsum(fn, 1800) / 30
    vf["intensity"] = n60 / np.where(n1800 > 0, n1800, np.nan)
    r300s = (S - lag(S, 300)) * 1e4
    vf["dispersion"] = np.nanstd(r300s, axis=1)
    vf["vpin"] = np.abs(rsum(fsv, 300)) / np.where(v300 > 0, v300, np.nan)
    vf["absmis"] = np.abs(misprice)
    # average pairwise correlation of 30-s stock returns over 30 min, on a 30-s grid then held
    corr = np.full((D, N), np.nan)
    g30 = np.arange(0, N, 30)
    rs30 = S[:, :, g30[1:]] - S[:, :, g30[:-1]]
    for i in range(D):
        for j in range(60, rs30.shape[2]):
            blk = rs30[i, :, j - 60:j]
            if not np.isfinite(blk).all():
                continue
            c = np.corrcoef(blk)
            corr[i, g30[j + 1]:g30[j + 1] + 30] = (c.sum() - K) / (K * (K - 1))
    vf["corr"] = corr

    T = np.arange(START, STOP, SAMPLE)
    rows = {k: v[:, T] for k, v in feats.items()}
    vrows = {k: v[:, T] for k, v in vf.items()}
    targets = {f"ret{h}": ((lead(F, 1 + h) - lead(F, 1)) * 1e4)[:, T] for h in HORIZONS}
    yrv = target_rv[:, T]

    lines = [f"# Advanced signals on BANKNIFTY futures, walk-forward out of sample\n",
             f"Data: Drive ticks (source drive-ticks, NOT FYERS), {D} days {days[0]}..{days[-1]}, front-month "
             f"BANKNIFTY future + {K} bank constituents, 1-s grid, rows every {SAMPLE} s 09:20-15:20, entry 1 s after the signal. "
             f"Ridge refitted before every day on all earlier days; first {MIN_TRAIN} days never scored "
             f"({D - MIN_TRAIN} test days).\n"]

    def wf(Xd, yd):
        """Ridge on standardised X, refit before each day from per-day sufficient statistics of EARLIER days."""
        Dn, _, P = Xd.shape
        M = np.zeros((Dn, P + 2, P + 2))
        for i in range(Dn):
            ok = np.isfinite(Xd[i]).all(1) & np.isfinite(yd[i])
            Z = np.column_stack([np.ones(ok.sum()), Xd[i][ok], yd[i][ok]])
            M[i] = Z.T @ Z
        C = np.cumsum(M, 0)
        pred = np.full_like(yd, np.nan)
        for i in range(MIN_TRAIN, Dn):
            S_ = C[i - 1]
            n = S_[0, 0]
            mx = S_[0, 1:P + 1] / n
            my = S_[0, P + 1] / n
            Sxx = S_[1:P + 1, 1:P + 1] / n - np.outer(mx, mx)
            Sxy = S_[1:P + 1, P + 1] / n - mx * my
            sd = np.sqrt(np.diag(Sxx)) + 1e-12
            R = Sxx / np.outer(sd, sd)
            b = np.linalg.solve(R + RIDGE * np.eye(P), Sxy / sd) / sd
            pred[i] = my + (Xd[i] - mx) @ b
        return pred

    def score(pred, y, base=None):
        p, a = pred[MIN_TRAIN:], y[MIN_TRAIN:]
        ok = np.isfinite(p) & np.isfinite(a)
        bp = np.zeros_like(p) if base is None else base[MIN_TRAIN:]
        ok &= np.isfinite(bp)
        r2 = 1 - np.sum((a[ok] - p[ok]) ** 2) / np.sum((a[ok] - bp[ok]) ** 2)
        ics = []
        for i in range(p.shape[0]):
            o = ok[i]
            if o.sum() > 50:
                ics.append(np.corrcoef(p[i, o], a[i, o])[0, 1])
        ics = np.array(ics)
        t = ics.mean() / ics.std() * np.sqrt(len(ics))
        return r2, ics.mean(), t, (ics > 0).mean()

    def trade(pred, y, cost):
        p, a = pred[MIN_TRAIN:], y[MIN_TRAIN:]
        ok = np.isfinite(p) & np.isfinite(a) & (np.abs(p) > cost)
        if ok.sum() == 0:
            return 0, np.nan, np.nan
        g = np.sign(p[ok]) * a[ok]
        return int(ok.sum()), g.mean(), g.mean() - cost

    def top(pred, y, q):
        """gross bp per trade when trading only the top q share of |forecast| (threshold from TRAIN days)."""
        p, a = pred[MIN_TRAIN:], y[MIN_TRAIN:]
        ok = np.isfinite(p) & np.isfinite(a)
        thr = np.nanquantile(np.abs(pred[MIN_TRAIN:MIN_TRAIN + 20]), 1 - q)
        sel = ok & (np.abs(p) >= thr)
        return (np.sign(p[sel]) * a[sel]).mean() if sel.any() else np.nan

    results = {}
    for h in HORIZONS:
        y = targets[f"ret{h}"]
        lines.append(f"\n## Next {h} s return of the future (bp, std {np.nanstd(y[MIN_TRAIN:]):.1f})\n")
        lines.append("| signal(s) | OOS R2 vs no change | mean daily IC | IC t-stat | days IC>0 | "
                     f"trades with |forecast| > {COST_2026:.0f} bp | net bp/trade | gross bp, top 1% forecasts | gross bp, top 0.1% |")
        lines.append("|---|---|---|---|---|---|---|---|---|")
        sets = [["rev10", "rev60"]] + [[k] for k in feats if not k.startswith("rev")] + \
               [["rev10", "rev60", k] for k in feats if not k.startswith("rev")] + [list(feats)]
        for s in sets:
            X = np.stack([rows[k] for k in s], -1)
            pred = wf(X, y)
            r2, ic, t, pos = score(pred, y)
            n, g, net = trade(pred, y, COST_2026)
            name = "+".join(s) if len(s) < 4 else "ALL"
            results[(h, name)] = (r2, ic, t)
            lines.append(f"| {name} | {r2 * 100:+.3f}% | {ic:+.4f} | {t:+.1f} | {pos * 100:.0f}% | {n} | "
                         f"{net:+.2f} | {top(pred, y, 0.01):+.2f} | {top(pred, y, 0.001):+.2f} |")
            print(h, name, f"r2 {r2 * 100:+.3f}% ic {ic:+.4f} t {t:+.1f} trades {n} net {net:+.2f}", flush=True)

    # Staleness check: the Drive future prints every ~2 s, the stocks every ~1 s. Score leadgap only
    # at rows where the future printed in that very second (its price is fresh, not carried forward).
    fresh = (fn[:, T] > 0) & (lead(fn, 1)[:, T] > 0)
    lines.append("\n## Staleness check: only rows where the future printed at t and at t+1 s\n")
    lines.append("| horizon | signal | OOS R2 | mean daily IC | IC t-stat | gross bp, top 1% |")
    lines.append("|---|---|---|---|---|---|")
    for h in (10, 60):
        y = np.where(fresh, targets[f"ret{h}"], np.nan)
        for s_ in (["leadgap10"], ["rev10", "rev60", "leadgap10"], list(feats)):
            pred = wf(np.stack([rows[k] for k in s_], -1), y)
            r2, ic, t, pos = score(pred, y)
            lines.append(f"| {h} s | {'+'.join(s_) if len(s_) < 4 else 'ALL'} | {r2 * 100:+.3f}% | {ic:+.4f} | {t:+.1f} | {top(pred, y, 0.01):+.2f} |")
            print("fresh", h, s_, r2, ic, flush=True)

    lines.append(f"\n## Next 5 min realised vol of the future (bp, mean {np.nanmean(yrv[MIN_TRAIN:]):.1f})\n")
    lines.append("| model | OOS R2 vs HAR | mean daily IC | IC t-stat | days IC>0 |")
    lines.append("|---|---|---|---|---|")
    har = ["rv5m", "rv30m", "rvday", "tod"]
    Xh = np.stack([vrows[k] for k in har], -1)
    base = wf(Xh, yrv)
    r2, ic, t, pos = score(base, yrv, np.full_like(yrv, np.nanmean(yrv[:MIN_TRAIN])))
    lines.append(f"| HAR (baseline; R2 here vs train mean) | {r2 * 100:+.2f}% | {ic:+.3f} | {t:+.1f} | {pos * 100:.0f}% |")
    print("HAR", r2, ic, flush=True)
    for k in ["intensity", "dispersion", "corr", "vpin", "absmis"]:
        X = np.stack([vrows[j] for j in har + [k]], -1)
        p = wf(X, yrv)
        r2, ic, t, pos = score(p, yrv, base)
        lines.append(f"| HAR + {k} | {r2 * 100:+.2f}% | {ic:+.3f} | {t:+.1f} | {pos * 100:.0f}% |")
        print("HAR+", k, r2, flush=True)
    X = np.stack([vrows[j] for j in har + ["intensity", "dispersion", "corr", "vpin", "absmis"]], -1)
    p = wf(X, yrv)
    r2, ic, t, pos = score(p, yrv, base)
    lines.append(f"| HAR + all five | {r2 * 100:+.2f}% | {ic:+.3f} | {t:+.1f} | {pos * 100:.0f}% |")

    text = "\n".join(lines) + "\n"
    print(text)
    if out:
        open(out, "w").write(text)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None)
