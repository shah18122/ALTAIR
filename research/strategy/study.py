#!/usr/bin/env python3
"""Run the pre-registered futures rules in HYPOTHESES.md on the Drive tick bars.

    python research/strategy/study.py --bars data/drive_ticks --split dev
    python research/strategy/study.py --bars data/drive_ticks --split holdout

--bars is the tree written by tools/drive_ticks_to_bars.py (source drive-ticks,
NOT FYERS). dev = sessions up to 2022-12-31, holdout = 2023 (run once).
Writes results/<split>_trades.csv and results/<split>_summary.csv, prints a table.
"""
from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np
import pandas as pd

N = 375                      # 1-minute bars 09:15 .. 15:29
COST_BP = 7.0                # round trip, 2026 futures schedule + slippage
COST_OLD_BP = 3.5            # pre-2026 STT
SEED = 20261111
DEV_END = pd.Timestamp("2022-12-31").date()


class LeakError(AssertionError):
    pass


def load(bars: Path, inst: str):
    d = pd.read_csv(bars / "fut" / inst / "1m" / "all.csv")
    t = pd.to_datetime(d.time.str[:19])
    d["day"] = t.dt.date
    d["m"] = (t.dt.hour * 60 + t.dt.minute) - (9 * 60 + 15)
    d = d[(d.m >= 0) & (d.m < N)]
    days = sorted(d.day.unique())
    idx = {k: i for i, k in enumerate(days)}
    arr = {c: np.full((len(days), N), np.nan) for c in ("open", "high", "low", "close", "oi")}
    r = d.day.map(idx).to_numpy()
    for c in arr:
        arr[c][r, d.m.to_numpy()] = d[c].to_numpy()
    nbars = np.sum(~np.isnan(arr["close"]), axis=1)
    # Fill missing minutes: close forward, then open/high/low = that close.
    cl = pd.DataFrame(arr["close"]).T.ffill().bfill().T.to_numpy()
    for c in ("open", "high", "low"):
        arr[c] = np.where(np.isnan(arr[c]), cl, arr[c])
    arr["close"] = cl
    oi = pd.DataFrame(arr["oi"]).T.ffill().bfill().T.to_numpy()
    # Previous-close rules need yesterday to be the same contract and adjacent.
    ok_prev = np.zeros(len(days), bool)
    for i in range(1, len(days)):
        gapdays = (days[i] - days[i - 1]).days
        roll = abs(np.log(oi[i, 0] / oi[i - 1, -1])) > 0.25
        ok_prev[i] = gapdays <= 5 and not roll and nbars[i - 1] >= 300
    full = nbars >= 300
    return days, arr, ok_prev, full


class Book:
    """Collects trades and refuses any whose signal is not strictly before entry."""

    def __init__(self, rule):
        self.rule, self.rows = rule, []

    def add(self, di, sig_m, ent_m, side, ent_px, ext_px, size=1.0, ext_m=None):
        if not sig_m < ent_m:
            raise LeakError(f"{self.rule}: signal minute {sig_m} not before entry {ent_m}")
        if ext_m is not None and not ext_m > ent_m:
            raise LeakError(f"{self.rule}: exit {ext_m} not after entry {ent_m}")
        self.rows.append((self.rule, di, sig_m, ent_m, side, ent_px, ext_px, size))


def range_forecast(arr, i, prevc):
    """Forecast session range (fraction) for day i from days before i only."""
    rng = (arr["high"].max(1) - arr["low"].min(1)) / arr["open"][:, 0]
    out = np.full(len(rng), np.nan)
    for k in range(22, len(rng)):
        out[k] = 0.5 * (rng[k - 5:k].mean() + rng[k - 22:k].mean())
    return out


def rules(days, arr, ok_prev, full, delay=0, leak=False):
    o, h, l, c = arr["open"], arr["high"], arr["low"], arr["close"]
    D = len(days)
    fc = range_forecast(arr, None, None)
    books = {k: Book(k) for k in ("H1", "H2", "H3a", "H3b", "H4", "H5")}

    # H1 intraday momentum
    for i in range(1, D):
        if not (full[i] and ok_prev[i]):
            continue
        r1 = c[i, 29] / c[i - 1, -1] - 1
        if r1 == 0:
            continue
        e = 345 + delay
        sig = 374 if leak else 29
        side = np.sign(c[i, 374] / o[i, 345] - 1) if leak else np.sign(r1)
        books["H1"].add(i, sig, e, side, o[i, e], c[i, 374])

    # H2 shock reversal on 5-minute returns
    r5 = np.full((D, N // 5), np.nan)
    for j in range(1, N // 5):
        r5[:, j] = c[:, 5 * j + 4] / c[:, 5 * j - 1] - 1
    for i in range(20, D):
        if not full[i]:
            continue
        busy = -1
        for j in range(1, N // 5):
            sm = 5 * j + 4
            if sm >= 355 or sm < busy:
                continue
            past = r5[i - 20:i, j]
            if not np.all(np.isfinite(past)) or not all(full[i - 20:i]):
                continue
            usual = np.sqrt(np.mean(past ** 2))
            if usual > 0 and abs(r5[i, j]) > 3 * usual:
                e, x = sm + 1 + delay, sm + 16
                books["H2"].add(i, sm, e, -np.sign(r5[i, j]), o[i, e], o[i, x], ext_m=x)
                busy = x

    # H3a / H3b opening range breakout
    for i in range(1, D):
        if not full[i]:
            continue
        hi, lo = h[i, :15].max(), l[i, :15].min()
        for k in range(15, 373):
            if c[i, k] > hi or c[i, k] < lo:
                side = 1.0 if c[i, k] > hi else -1.0
                e = k + 1 + delay
                if e > 373:
                    break
                books["H3a"].add(i, k, e, side, o[i, e], c[i, 374])
                if i >= 82 and np.isfinite(fc[i]):
                    hist = fc[i - 60:i]
                    if np.all(np.isfinite(hist)) and fc[i] >= np.quantile(hist, 2 / 3):
                        books["H3b"].add(i, k, e, side, o[i, e], c[i, 374])
                break

    # H4 opening gap fade
    gaps = np.full(D, np.nan)
    for i in range(1, D):
        if ok_prev[i] and full[i]:
            gaps[i] = o[i, 0] / c[i - 1, -1] - 1
    for i in range(1, D):
        if not np.isfinite(gaps[i]):
            continue
        past = gaps[:i][np.isfinite(gaps[:i])][-60:]
        if len(past) < 60 or gaps[i] == 0:
            continue
        if abs(gaps[i]) > np.quantile(np.abs(past), 0.75):
            e = 5 + delay
            books["H4"].add(i, 0, e, -np.sign(gaps[i]), o[i, e], o[i, 60], ext_m=60)

    # H5 overnight drift: signal is the calendar (known), entry yesterday 15:29 close.
    for i in range(1, D):
        if ok_prev[i] and full[i]:
            # entry minute -1 = previous session's last bar; exit = today's open
            books["H5"].add(i, -2, -1, 1.0, c[i - 1, -1], o[i, 0])

    out = []
    for b in books.values():
        out += b.rows
    t = pd.DataFrame(out, columns=["rule", "di", "sig_m", "ent_m", "side", "ent_px", "ext_px", "size"])
    t["day"] = [days[k] for k in t.di]
    t["gross_bp"] = t.side * (t.ext_px / t.ent_px - 1) * 1e4
    t["fc"] = [fc[k] for k in t.di]
    return t


def tstat(x):
    x = np.asarray(x, float)
    return x.mean() / (x.std(ddof=1) / np.sqrt(len(x))) if len(x) > 2 and x.std() > 0 else np.nan


def summarise(t, inst, delayed, rng):
    rows = []
    for rule, g in t.groupby("rule"):
        g = g.sort_values("day")
        net = g.gross_bp - COST_BP
        thirds = [x.mean() for x in np.array_split(net.to_numpy(), 3)]
        # placebo: same trades, random signs (H5 all-long: n/a)
        if rule != "H5":
            raw = (g.gross_bp * g.side).to_numpy()  # unsigned move
            sims = np.array([(raw * rng.choice([-1, 1], len(raw))).mean() for _ in range(2000)])
            p_plac = float((sims >= g.gross_bp.mean()).mean())
        else:
            p_plac = np.nan
        # vol overlay: size = trailing mean forecast / today's forecast (past only)
        w = g.fc.rolling(60, min_periods=20).mean().shift(1) / g.fc
        w = (w / w.mean()).fillna(1.0)
        daily = net.groupby(g.day).sum()
        dailyw = (net * w).groupby(g.day).sum()
        dg = delayed[delayed.rule == rule].gross_bp - COST_BP
        rows.append(dict(
            inst=inst, rule=rule, n=len(g),
            gross_bp=g.gross_bp.mean(), net_bp=net.mean(), t_net=tstat(net),
            net_3p5=(g.gross_bp - COST_OLD_BP).mean(), t_3p5=tstat(g.gross_bp - COST_OLD_BP),
            breakeven_bp=g.gross_bp.mean(),
            win=(net > 0).mean(), third1=thirds[0], third2=thirds[1], third3=thirds[2],
            p_placebo=p_plac, delay1_net=dg.mean(),
            sharpe=daily.mean() / daily.std() * np.sqrt(252) if daily.std() > 0 else np.nan,
            sharpe_vol=dailyw.mean() / dailyw.std() * np.sqrt(252) if dailyw.std() > 0 else np.nan,
        ))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bars", required=True, type=Path)
    ap.add_argument("--split", choices=["dev", "holdout"], required=True)
    ap.add_argument("--out", type=Path, default=Path(__file__).parent / "results")
    a = ap.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(SEED)
    allrows, alltr = [], []
    for inst in ("banknifty", "nifty"):
        days, arr, ok_prev, full = load(a.bars, inst)
        # Look-ahead check: the leaked H1 must be refused by the harness.
        try:
            rules(days, arr, ok_prev, full, leak=True)
            raise SystemExit("LEAK CHECK FAILED: leaked H1 was accepted")
        except LeakError as e:
            print(f"[{inst}] leak check ok: {e}")
        t = rules(days, arr, ok_prev, full)
        td = rules(days, arr, ok_prev, full, delay=1)
        sel = (lambda x: x.day <= DEV_END) if a.split == "dev" else (lambda x: x.day > DEV_END)
        t, td = t[sel(t)], td[sel(td)]
        t.insert(0, "inst", inst)
        alltr.append(t)
        allrows += summarise(t, inst, td, rng)
    s = pd.DataFrame(allrows)
    pd.concat(alltr).to_csv(a.out / f"{a.split}_trades.csv", index=False, float_format="%.6g")
    s.to_csv(a.out / f"{a.split}_summary.csv", index=False, float_format="%.4g")
    with pd.option_context("display.width", 250, "display.max_columns", 30):
        print(s.round(3).to_string(index=False))


if __name__ == "__main__":
    main()
