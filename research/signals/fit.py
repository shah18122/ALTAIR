"""research/signals/fit.py -- fit the BANKNIFTY basket-alpha model on Drive ticks, for the live engine.

Fits, on every Drive day of every .npz given (load.py output; source drive-ticks, not FYERS),
one ridge per horizon (10 s and 60 s) of the future's return after a 1-s delay on the
features live/basket_alpha.hpp computes tick by tick:

  rev10, rev60     the future's own last 10 s / 60 s log return, bp
  misprice         -(gap - EWMA_1800s(gap)) bp, gap = log F - sum_j beta_j log P_j
  leadgap10/30     basket return minus future return over the last 10 s / 30 s, bp
  breadth          sum_j |beta_j| sign(log P_j(t) - log P_j(t-60)) / sum_j |beta_j|
  flow60/300       tick-rule signed future volume / volume over the last 60 s / 300 s

Before scoring, each day's basket uses betas fitted on the 20 days before it (as in study.py), so
the features the coefficients are fitted on are what the live engine will see. The live betas are
the last 20 days of the newest file. Only stocks the FYERS recorder streams today are used
(BANDHANBNK is not one). The last 15% of days of each file are held out and scored, so the file
also says how well the fitted model did on days it never saw.

Writes config/signals/basket_bnf.csv ("key,value" lines; read by live/basket_alpha.hpp).

    python3 research/signals/fit.py OUT.csv bnf2020.npz bnf2022.npz bnf2023.npz [--until YYYYMMDD]

--until fits on days BEFORE that day only (betas from the 20 days before it), for an honest replay
of the days after it (research/signals/basket_replay.cpp).
"""
from __future__ import annotations

import sys

import numpy as np

STOCKS = ["HDFCBANK", "ICICIBANK", "SBIN", "KOTAKBANK", "AXISBANK", "INDUSINDBK", "BANKBARODA", "AUBANK",
          "FEDERALBNK", "IDFCFIRSTB", "PNB"]
FEATS = ["rev10", "rev60", "misprice", "leadgap10", "leadgap30", "breadth", "flow60", "flow300"]
HORIZONS = (10, 60)
START, STOP, SAMPLE = 5 * 60, 6 * 3600 + 5 * 60, 5
EW_HALF = 1800.0
BETA_DAYS = 20
HOLDOUT = 0.15
RIDGE = 1e-3


def lag(x, k):
    y = np.full_like(x, np.nan)
    y[..., k:] = x[..., :-k]
    return y


def lead(x, k):
    y = np.full_like(x, np.nan)
    y[..., :-k] = x[..., k:]
    return y


def rsum(x, k):
    c = np.nancumsum(x, axis=-1)
    return c - lag(c, k)


def ewma_rows(g, half):
    a = 1.0 - 0.5 ** (1.0 / half)
    out = np.full_like(g, np.nan)
    for i in range(g.shape[0]):
        m = np.nan
        row, o = g[i], out[i]
        for t in range(row.shape[0]):
            v = row[t]
            if v == v:
                m = v if m != m else m + a * (v - m)
            o[t] = m
    return out


def fit_betas(S, F, i0, i1):
    """ridge of 60-s future returns on 60-s stock returns over days [i0, i1)."""
    idx = np.arange(START, STOP, 60)
    rF = (F[i0:i1, idx[1:]] - F[i0:i1, idx[:-1]]) * 1e4
    rS = (S[i0:i1][:, :, idx[1:]] - S[i0:i1][:, :, idx[:-1]]) * 1e4
    K = S.shape[1]
    X = rS.transpose(0, 2, 1).reshape(-1, K)
    y = rF.reshape(-1)
    ok = np.isfinite(X).all(1) & np.isfinite(y)
    X, y = X[ok], y[ok]
    return np.linalg.solve(X.T @ X + 1e-2 * len(X) * np.eye(K) * X.var(0).mean(), X.T @ y)


def features(path, until=None):
    z = np.load(path)
    if until is not None:
        keep = z["days"] < until
        z = {k: (z[k][keep] if k in ("days", "expiry", "fpx", "fvol", "fsv", "foi", "fn", "spx", "svol") else z[k]) for k in z.files}
    syms = list(z["syms"])
    sel = [syms.index(s) for s in STOCKS]
    F = np.log(z["fpx"])
    S = np.log(z["spx"][:, sel].astype(np.float64))
    fvol, fsv = z["fvol"], z["fsv"]
    D = F.shape[0]
    beta = np.full((D, len(STOCKS)), np.nan)
    for i in range(BETA_DAYS, D):
        beta[i] = fit_betas(S, F, i - BETA_DAYS, i)
    live_beta = fit_betas(S, F, D - BETA_DAYS, D)
    B = np.einsum("dk,dkn->dn", beta, S)
    gap = F - B
    x = {}
    x["rev10"] = (F - lag(F, 10)) * 1e4
    x["rev60"] = (F - lag(F, 60)) * 1e4
    x["misprice"] = -(gap - ewma_rows(gap, EW_HALF)) * 1e4
    x["leadgap10"] = ((B - lag(B, 10)) - (F - lag(F, 10))) * 1e4
    x["leadgap30"] = ((B - lag(B, 30)) - (F - lag(F, 30))) * 1e4
    w = np.abs(beta)[:, :, None]
    x["breadth"] = np.nansum(w * np.sign(S - lag(S, 60)), 1) / np.nansum(w, 1)
    v60, v300 = rsum(fvol, 60), rsum(fvol, 300)
    x["flow60"] = rsum(fsv, 60) / np.where(v60 > 0, v60, np.nan)
    x["flow300"] = rsum(fsv, 300) / np.where(v300 > 0, v300, np.nan)
    T = np.arange(START, STOP, SAMPLE)
    X = np.stack([x[k][:, T] for k in FEATS], -1)[BETA_DAYS:]
    Y = {h: ((lead(F, 1 + h) - lead(F, 1)) * 1e4)[:, T][BETA_DAYS:] for h in HORIZONS}
    return X, Y, live_beta, z["days"][BETA_DAYS:]


def ridge_fit(X, y):
    ok = np.isfinite(X).all(1) & np.isfinite(y)
    X, y = X[ok], y[ok]
    mu, sd = X.mean(0), X.std(0) + 1e-12
    Z = (X - mu) / sd
    b = np.linalg.solve(Z.T @ Z / len(Z) + RIDGE * np.eye(Z.shape[1]), Z.T @ (y - y.mean()) / len(Z))
    coef = b / sd
    return y.mean() - mu @ coef, coef


def score(X, y, c0, coef):
    p = c0 + X @ coef
    ok = np.isfinite(p) & np.isfinite(y)
    r2 = 1 - np.sum((y[ok] - p[ok]) ** 2) / np.sum(y[ok] ** 2)
    ics = [np.corrcoef(p[i][ok[i]], y[i][ok[i]])[0, 1] for i in range(p.shape[0]) if ok[i].sum() > 50]
    return r2, float(np.mean(ics)), float(np.mean(np.array(ics) > 0)), len(ics)


def main(out, paths, until=None):
    parts = [features(p, until) for p in paths]
    parts = [p for p in parts if p[0].shape[0] > 0]
    live_beta = parts[-1][2]
    lines = ["# config/signals/basket_bnf.csv -- written by research/signals/fit.py; read by live/basket_alpha.hpp.",
             "# Fitted on Drive ticks (source drive-ticks, NOT FYERS): " + ", ".join(
                 f"{p[3][0]}..{p[3][-1]}" for p in parts) + ".",
             "# forecast_h (bp, the future's log return over h s starting 1 s later) = c0_h + sum coef_h_<feature> * feature.",
             "key,value", f"ew_half_s,{EW_HALF:g}"]
    for s, b in zip(STOCKS, live_beta):
        lines.append(f"beta_{s},{b:.6f}")
    report = []
    for h in HORIZONS:
        tr_X, tr_y, te = [], [], []
        for X, Y, _, _ in parts:
            n = X.shape[0]
            cut = int(n * (1 - HOLDOUT))
            tr_X.append(X[:cut].reshape(-1, X.shape[-1]))
            tr_y.append(Y[h][:cut].reshape(-1))
            te.append((X[cut:], Y[h][cut:]))
        c0, coef = ridge_fit(np.vstack(tr_X), np.concatenate(tr_y))
        for (Xt, yt), p in zip(te, parts):
            r2, ic, pos, nd = score(Xt, yt, c0, coef)
            report.append(f"# held out {h:>2} s, last {nd} days of {p[3][0] // 10000}: OOS R2 {r2 * 100:+.3f}%, "
                          f"mean daily IC {ic:+.4f}, IC>0 on {pos * 100:.0f}% of days")
        # final coefficients: all days
        c0, coef = ridge_fit(np.vstack([X.reshape(-1, X.shape[-1]) for X, _, _, _ in parts]),
                             np.concatenate([Y[h].reshape(-1) for _, Y, _, _ in parts]))
        lines.append(f"c0_{h},{c0:.6f}")
        for k, b in zip(FEATS, coef):
            lines.append(f"coef_{h}_{k},{b:.6f}")
    text = "\n".join(lines[:3] + report + lines[3:]) + "\n"
    open(out, "w").write(text)
    print(text)


if __name__ == "__main__":
    args = sys.argv[2:]
    until = None
    if "--until" in args:
        k = args.index("--until")
        until = int(args[k + 1])
        args = args[:k] + args[k + 2:]
    main(sys.argv[1], args, until)
