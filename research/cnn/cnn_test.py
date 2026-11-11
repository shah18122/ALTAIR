#!/usr/bin/env python3
"""research/cnn/cnn_test.py -- can ALTAIR's CNN make money? Walk-forward, after costs.

The model is the curriculum CNN (models/curriculum.hpp CurriculumCnn on
models/trainable_cnn.hpp TrainableTcn<4, 8, 3, 3, 16>): three dilated causal
convolutions (width 3, dilations 1/2/4, 8 channels, ReLU), read out at the last
of 16 steps by a logistic unit, trained on up/down with cross-entropy and Adam
(lr 2e-3 / (1 + 0.5 epoch), 4 epochs as the curriculum uses for > 2000 rows).
Re-implemented here in numpy (mini-batches of 64 instead of one row at a time)
and gradient-checked by --check.

Data: the bar tree tools/drive_ticks_to_bars.py writes (source drive-ticks, NOT
FYERS; docs/DATA_POLICY.md). Series fut/nifty, fut/banknifty, spot/nifty;
timeframes 1m 5m 15m 60m 1d.

Per bar, 4 inputs: close-to-close log return, log(high/low), close location in
the bar, log volume vs its 20-bar mean (0 for spot). Standardised on the train
window only.

Walk-forward: every calendar month is a test fold; the nets are refit from
scratch on all bars before it (newest --cap rows). Nothing in a fold is seen
before it is forecast.

  DIRECTION  CNN p(up) vs: no change (flat, 0 bp) and AR(5) ridge on returns.
             Return forecast = p*mean_up + (1-p)*mean_down (as the curriculum),
             RMSE skill vs no change + Newey-West Diebold-Mariano t.
             Trading: long if p>0.5 else short, held one bar, intraday flat at
             the close; cost charged on turnover (--cost-bp per round trip).
             "all" trades every bar, "top10" only when |p-0.5| is in the top
             10% of the train window's own confidences.
  SIZE       CNN p(next |return| > train median) vs HAR(+time of day) ridge on
             |return|: AUC for the same "big move" label, and HAR R2 vs the
             train mean.

    python research/cnn/cnn_test.py --bars data/drive_ticks --out research/cnn/results
    python research/cnn/cnn_test.py --check     # gradient check, no data
"""
from __future__ import annotations

import argparse
import csv
import math
import sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np

T, CIN, C, LAYERS, K = 16, 4, 8, 3, 3
SERIES = ("fut/nifty", "fut/banknifty", "spot/nifty")
TFS = ("1m", "5m", "15m", "60m", "1d")


# --------------------------------------------------------------------- the net
class Tcn:
    def __init__(self, seed: int):
        rng = np.random.default_rng(seed)
        self.p = []
        for l in range(LAYERS):
            ci = CIN if l == 0 else C
            self.p.append(rng.normal(0, math.sqrt(2.0 / (ci * K)), (C, K * ci)))
            self.p.append(np.zeros(C))
        self.p.append(rng.normal(0, math.sqrt(1.0 / C), C))
        self.p.append(np.zeros(1))
        self.m = [np.zeros_like(x) for x in self.p]
        self.v = [np.zeros_like(x) for x in self.p]
        self.t = 0

    @staticmethod
    def _cols(h: np.ndarray, d: int) -> np.ndarray:
        b, t, ci = h.shape
        pad = np.concatenate([np.zeros((b, (K - 1) * d, ci)), h], axis=1)
        return np.stack([pad[:, k * d:k * d + t, :] for k in range(K)], axis=2).reshape(b, t, K * ci)

    def forward(self, x: np.ndarray):
        h, cache = x, []
        for l in range(LAYERS):
            cols = self._cols(h, 2 ** l)
            z = cols @ self.p[2 * l].T + self.p[2 * l + 1]
            cache.append((h.shape, cols, z))
            h = np.maximum(z, 0.0)
        logit = h[:, -1, :] @ self.p[-2] + self.p[-1][0]
        return logit, h, cache

    def prob(self, x: np.ndarray) -> np.ndarray:
        out = np.empty(len(x))
        for i in range(0, len(x), 4096):
            out[i:i + 4096] = sigmoid(self.forward(x[i:i + 4096])[0])
        return out

    def loss_grad(self, x: np.ndarray, y: np.ndarray):
        logit, h, cache = self.forward(x)
        p = sigmoid(logit)
        b = len(x)
        loss = -np.mean(y * np.log(p + 1e-12) + (1 - y) * np.log(1 - p + 1e-12))
        dz = (p - y) / b
        g = [None] * len(self.p)
        g[-2] = h[:, -1, :].T @ dz
        g[-1] = np.array([dz.sum()])
        dh = np.zeros_like(h)
        dh[:, -1, :] = np.outer(dz, self.p[-2])
        for l in reversed(range(LAYERS)):
            shape, cols, z = cache[l]
            dzl = dh * (z > 0)
            g[2 * l] = dzl.reshape(-1, C).T @ cols.reshape(-1, cols.shape[-1])
            g[2 * l + 1] = dzl.sum(axis=(0, 1))
            if l == 0:
                break
            dcols = (dzl @ self.p[2 * l]).reshape(shape[0], shape[1], K, shape[2])
            d = 2 ** l
            dpad = np.zeros((shape[0], shape[1] + (K - 1) * d, shape[2]))
            for k in range(K):
                dpad[:, k * d:k * d + shape[1], :] += dcols[:, :, k, :]
            dh = dpad[:, (K - 1) * d:, :]
        return loss, g

    def step(self, g, lr: float, b1=0.9, b2=0.999, eps=1e-8):
        self.t += 1
        for i, gi in enumerate(g):
            self.m[i] = b1 * self.m[i] + (1 - b1) * gi
            self.v[i] = b2 * self.v[i] + (1 - b2) * gi * gi
            mh = self.m[i] / (1 - b1 ** self.t)
            vh = self.v[i] / (1 - b2 ** self.t)
            self.p[i] -= lr * mh / (np.sqrt(vh) + eps)

    def fit(self, x: np.ndarray, y: np.ndarray, seed: int, epochs: int = 4, batch: int = 64):
        rng = np.random.default_rng(seed)
        for e in range(epochs):
            lr = 2e-3 / (1.0 + 0.5 * e)
            idx = rng.permutation(len(x))
            for i in range(0, len(x), batch):
                j = idx[i:i + batch]
                self.step(self.loss_grad(x[j], y[j])[1], lr)


def sigmoid(z):
    return 1.0 / (1.0 + np.exp(-np.clip(z, -40, 40)))


def grad_check() -> bool:
    rng = np.random.default_rng(1)
    x, y = rng.normal(size=(5, T, CIN)), (rng.random(5) > 0.5).astype(float)
    net = Tcn(3)
    for q in net.p:
        q += rng.normal(0, 0.1, q.shape)   # move biases off zero so ReLUs vary
    _, g = net.loss_grad(x, y)
    worst = 0.0
    for i, q in enumerate(net.p):
        for j in range(q.size):
            old = q.flat[j]
            q.flat[j] = old + 1e-6
            lp = net.loss_grad(x, y)[0]
            q.flat[j] = old - 1e-6
            lm = net.loss_grad(x, y)[0]
            q.flat[j] = old
            fd = (lp - lm) / 2e-6
            worst = max(worst, abs(fd - g[i].flat[j]) / max(1e-6, abs(fd) + abs(g[i].flat[j])))
    print(f"gradient check: worst relative error {worst:.2e}")
    return worst < 1e-4


# ---------------------------------------------------------------------- data
def load(path: Path):
    with path.open() as f:
        rows = list(csv.DictReader(f))
    o = np.array([float(r["open"]) for r in rows])
    h = np.array([float(r["high"]) for r in rows])
    l = np.array([float(r["low"]) for r in rows])
    c = np.array([float(r["close"]) for r in rows])
    v = np.array([float(r.get("volume") or 0.0) for r in rows])
    day = np.array([r["time"][:10] for r in rows])
    month = np.array([r["time"][:7] for r in rows])
    slot = np.array([r["time"][11:16] if len(r["time"]) > 10 else "" for r in rows])
    return o, h, l, c, v, day, month, slot


def features(h, l, c, v):
    n = len(c)
    r = np.zeros(n)
    r[1:] = 1e4 * np.log(c[1:] / c[:-1])
    rng = 1e4 * np.log(h / l)
    loc = np.where(h > l, (c - l) / np.where(h > l, h - l, 1.0) - 0.5, 0.0)
    lv = np.log1p(v)
    cs = np.concatenate([[0.0], np.cumsum(lv)])
    lo = np.maximum(0, np.arange(n) - 19)
    mean20 = (cs[np.arange(n) + 1] - cs[lo]) / (np.arange(n) + 1 - lo)
    vol = lv - mean20
    return np.stack([r, rng, loc, vol], axis=1), r


def windows(f: np.ndarray, idx: np.ndarray) -> np.ndarray:
    return np.stack([f[i - T + 1:i + 1] for i in idx])


def ridge(x, y, lam=1e-3):
    xm, ym = x.mean(0), y.mean()
    xc = x - xm
    b = np.linalg.solve(xc.T @ xc + lam * len(x) * np.diag(xc.var(0) + 1e-12), xc.T @ (y - ym))
    return lambda z: ym + (z - xm) @ b


def auc(score, label):
    order = np.argsort(score, kind="mergesort")
    ranks = np.empty(len(score))
    ranks[order] = np.arange(1, len(score) + 1)
    pos = label.sum()
    neg = len(label) - pos
    return (ranks[label == 1].sum() - pos * (pos + 1) / 2) / (pos * neg) if pos and neg else float("nan")


def nw_t(d: np.ndarray) -> float:
    n = len(d)
    if n < 30:
        return float("nan")
    mu = d.mean()
    e = d - mu
    var = (e @ e) / n
    lag = int(n ** (1 / 3))
    for k in range(1, lag + 1):
        var += 2 * (1 - k / (lag + 1)) * (e[k:] @ e[:-k]) / n
    return mu / math.sqrt(var / n) if var > 0 else float("nan")


# ------------------------------------------------------------------ the test
def run(bars: str, series: str, tf: str, cost: float, cap: int, seed: int):
    o, h, l, c, v, day, month, slot = load(Path(bars) / series / tf / "all.csv")
    f, r = features(h, l, c, v)
    n = len(c)
    intraday = tf != "1d"
    absr = np.abs(r)
    # target for bar i is bar i+1, same session for intraday
    has_y = np.zeros(n, bool)
    has_y[T:n - 1] = True
    if intraday:
        has_y[:-1] &= day[1:] == day[:-1]
    y = np.zeros(n)
    y[:-1] = r[1:]
    # HAR inputs on |r|, causal
    def trail(k):
        cs = np.concatenate([[0.0], np.cumsum(absr)])
        i = np.arange(n)
        lo = np.maximum(0, i - k + 1)
        return (cs[i + 1] - cs[lo]) / (i + 1 - lo)
    har = np.stack([trail(1), trail(5), trail(22)], axis=1)
    lags = np.stack([np.roll(r, k) for k in range(5)], axis=1)   # r_t .. r_{t-4}

    months = sorted(set(month))
    min_train = 120 if not intraday else 0
    first = 6 if intraday else 12
    out = {k: [] for k in ("y", "cnn_p", "cnn_f", "cnn_top", "ar_f", "ar_top", "size_p", "har_f",
                           "big", "day", "last", "train_mean")}
    for m in months[first:]:
        test = np.where((month == m) & has_y)[0]
        train = np.where((month < m) & has_y)[0]
        if len(train) < max(min_train, 64) or len(test) == 0:
            continue
        # the train target of the last train bar must be known before the fold
        train = train[month[train + 1] < m][-cap:]   # no train label from inside the fold
        mu, sd = f[train].mean(0), f[train].std(0) + 1e-12
        fz = np.clip((f - mu) / sd, -5, 5)
        xtr, xte = windows(fz, train), windows(fz, test)
        ytr = y[train]
        # direction CNN
        net = Tcn(seed)
        net.fit(xtr, (ytr > 0).astype(float), seed)
        p_tr_tail = net.prob(xtr[-max(len(xtr) // 5, 50):])
        thr = np.quantile(np.abs(p_tr_tail - 0.5), 0.9)
        p = net.prob(xte)
        mu_up, mu_dn = ytr[ytr > 0].mean(), ytr[ytr <= 0].mean()
        # AR(5) ridge
        ar = ridge(lags[train], ytr)
        ar_tr = ar(lags[train][-max(len(train) // 5, 50):])
        ar_thr = np.quantile(np.abs(ar_tr), 0.9)
        arf = ar(lags[test])
        # size: CNN on big-move label vs HAR (+ time-of-day) ridge on |r_next|
        med = np.median(np.abs(ytr))
        snet = Tcn(seed + 1)
        snet.fit(xtr, (np.abs(ytr) > med).astype(float), seed + 1)
        sp = snet.prob(xte)
        if intraday:
            prof = {}
            for s_, a_ in zip(slot[train + 1], np.abs(ytr)):
                prof.setdefault(s_, []).append(a_)
            prof = {k_: float(np.mean(v_)) for k_, v_ in prof.items()}
            tod = lambda idx: np.array([prof.get(s_, med) for s_ in slot[idx + 1]])[:, None]
            xh_tr, xh_te = np.hstack([har[train], tod(train)]), np.hstack([har[test], tod(test)])
        else:
            xh_tr, xh_te = har[train], har[test]
        hf = ridge(xh_tr, np.abs(ytr))(xh_te)
        out["y"].append(y[test]); out["cnn_p"].append(p)
        out["cnn_f"].append(p * mu_up + (1 - p) * mu_dn)
        out["cnn_top"].append(np.abs(p - 0.5) >= thr)
        out["ar_f"].append(arf); out["ar_top"].append(np.abs(arf) >= ar_thr)
        out["size_p"].append(sp); out["har_f"].append(hf)
        out["train_mean"].append(np.full(len(test), np.abs(ytr).mean()))
        out["big"].append((np.abs(y[test]) > med).astype(int))
        out["day"].append(day[test])
        # last bar of the session: position closes there
        last = ~np.isin(test + 1, test)            # gaps and fold end
        if intraday:
            last[:-1] |= day[test[1:]] != day[test[:-1]]
        out["last"].append(last)
    a = {k: np.concatenate(v_) for k, v_ in out.items()}
    yv = a["y"]

    def pnl(pos):
        """bp per bar after turnover cost; positions open at bar close, flat at session end."""
        gross = pos * yv
        prev = np.concatenate([[0.0], pos[:-1]])
        prev[np.concatenate([[True], a["last"][:-1]])] = 0.0   # new session starts flat
        turn = np.abs(pos - prev) + np.where(a["last"], np.abs(pos), 0.0)
        net = gross - cost / 2 * turn
        days = {}
        for d_, x_ in zip(a["day"], net):
            days[d_] = days.get(d_, 0.0) + x_
        dn = np.array(list(days.values()))
        trades = (turn > 0).sum() / 2
        return dict(gross_bp_per_bar=float(gross[pos != 0].mean()) if (pos != 0).any() else 0.0,
                    net_bp_total=float(net.sum()), trades=float(trades),
                    net_bp_per_trade=float(net.sum() / trades) if trades else 0.0,
                    hit=float((np.sign(pos[pos != 0]) == np.sign(yv[pos != 0])).mean()) if (pos != 0).any() else float("nan"),
                    daily_t=nw_t(dn) if len(dn) > 30 else float("nan"))

    sign = lambda z: np.where(z > 0, 1.0, -1.0)
    res = dict(series=series, tf=tf, test_bars=len(yv), first_test=months[first],
               cost_bp=cost)
    rm0 = math.sqrt(np.mean(yv ** 2))
    for name, fc in (("cnn", a["cnn_f"]), ("ar5", a["ar_f"])):
        res[f"{name}_rmse_skill_pct"] = 100 * (1 - math.sqrt(np.mean((fc - yv) ** 2)) / rm0)
        res[f"{name}_dm_t"] = nw_t((fc - yv) ** 2 - yv ** 2)
    for name, s_, top in (("cnn", a["cnn_p"] - 0.5, a["cnn_top"]), ("ar5", a["ar_f"], a["ar_top"])):
        for mode, pos in (("all", sign(s_)), ("top10", np.where(top, sign(s_), 0.0))):
            for k_, v_ in pnl(pos).items():
                res[f"{name}_{mode}_{k_}"] = v_
    res["size_cnn_auc"] = auc(a["size_p"], a["big"])
    res["size_har_auc"] = auc(a["har_f"], a["big"])
    ay = np.abs(yv)
    res["size_har_r2_pct"] = 100 * (1 - np.mean((a["har_f"] - ay) ** 2) / np.mean((a["train_mean"] - ay) ** 2))
    return res


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bars", default="data/drive_ticks")
    ap.add_argument("--out", default="research/cnn/results")
    ap.add_argument("--cost-bp", type=float, default=6.0, help="round trip, bp")
    ap.add_argument("--cap", type=int, default=100_000, help="newest train rows per fold")
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--tfs", default=",".join(TFS))
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    if a.check:
        return 0 if grad_check() else 1
    if not grad_check():
        return 1
    jobs = [(a.bars, s, tf, a.cost_bp, a.cap, a.seed) for s in SERIES for tf in a.tfs.split(",")]
    with ProcessPoolExecutor(a.jobs) as ex:
        rows = list(ex.map(run, *zip(*jobs)))
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    keys = list(rows[0].keys())
    with (out / "summary.csv").open("w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=keys)
        w.writeheader()
        for r in rows:
            w.writerow({k: (f"{v:.4g}" if isinstance(v, float) else v) for k, v in r.items()})
    print((out / "summary.csv").read_text())
    return 0


if __name__ == "__main__":
    sys.exit(main())
