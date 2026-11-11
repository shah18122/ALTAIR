#!/usr/bin/env python3
"""tools/bar_forecast.py -- can we forecast the next bar of futures and spot? Pure Python, no AI.

Reads the bar tree tools/drive_ticks_to_bars.py writes (source drive-ticks, NOT
FYERS) or any dataset-shaped FYERS tree:

    <bars>/fut/nifty/<tf>/*.csv  <bars>/fut/banknifty/<tf>/*.csv  <bars>/spot/nifty/<tf>/*.csv
    columns time,open,high,low,close[,volume,oi]; time "YYYY-MM-DDTHH:MM:SS+05:30" or "YYYY-MM-DD"

For every series and timeframe (1m 5m 15m 60m 1d) it walks forward through the
whole history -- each forecast uses only bars already closed, every model is
refit as it goes -- and scores two questions:

  RETURN  the next bar's log return (bp). Baseline: no change (random walk).
          Models: expanding mean, AR(5) ridge, and for NIFTY spot also
          FUT-LEAD (this bar's futures return and basis change, AR(5) on top).
          Reported: RMSE skill vs the baseline, Diebold-Mariano t, direction
          hit rate, and the gross bp a trade earns trading the sign, against
          --cost-bp (round trip).
  SIZE    the next bar's |return| (bp). Baseline: the mean |return| so far.
          Models: last bar, EWMA, HAR (1/5/22-bar averages, ridge) and, intraday,
          HAR + time-of-day profile.

Intraday returns never cross a session (the first bar of a day has no
target from the day before). "Significant" = |DM t| >= the Bonferroni bar
for every test made AND the model beats the baseline in each third of the
data. Output: <out>/summary.csv and <out>/summary.md.

    python tools/bar_forecast.py --bars data/drive_ticks --out data/verified/bar_forecast [--logs] [--cost-bp 0]
"""
from __future__ import annotations

import argparse
import csv
import math
import sys
from pathlib import Path
from statistics import NormalDist

TFS = ("1m", "5m", "15m", "60m", "1d")
SERIES = (("fut", "nifty"), ("fut", "banknifty"), ("spot", "nifty"))


def load(folder: Path) -> list[tuple[str, str, float]]:
    """[(day, time, close)] sorted, one row per bar stamp."""
    rows: dict[str, float] = {}
    for f in sorted(folder.glob("*.csv")):
        with open(f, newline="") as fh:
            for r in csv.DictReader(fh):
                t = (r.get("time") or r.get("date") or "").strip()
                try:
                    c = float(r["close"])
                except (KeyError, ValueError):
                    continue
                if t and c > 0:
                    rows[t[:19]] = c
    return [(t[:10], t[11:16] if len(t) > 10 else "", rows[t]) for t in sorted(rows)]


class Ridge:
    """Walk-forward least squares on running sums; solve every `refit` updates."""

    def __init__(self, k: int, ridge: float = 1e-8, refit: int = 250):
        self.k, self.lam, self.refit = k + 1, ridge, refit
        self.xx = [[0.0] * self.k for _ in range(self.k)]
        self.xy = [0.0] * self.k
        self.n = 0
        self.b: list[float] | None = None

    def predict(self, x: list[float]) -> float | None:
        if self.b is None:
            return None
        return self.b[0] + sum(bi * xi for bi, xi in zip(self.b[1:], x))

    def update(self, x: list[float], y: float) -> None:
        z = [1.0] + x
        for i in range(self.k):
            zi = z[i]
            self.xy[i] += zi * y
            row = self.xx[i]
            for j in range(self.k):
                row[j] += zi * z[j]
        self.n += 1
        if self.n >= 50 and self.n % self.refit == 0:
            self.b = solve([[self.xx[i][j] + (self.lam * self.xx[i][i] + 1e-12 if i == j and i else 0.0)
                             for j in range(self.k)] for i in range(self.k)], list(self.xy))


def solve(a: list[list[float]], b: list[float]) -> list[float] | None:
    n = len(b)
    for c in range(n):
        p = max(range(c, n), key=lambda r: abs(a[r][c]))
        if abs(a[p][c]) < 1e-300:
            return None
        a[c], a[p], b[c], b[p] = a[p], a[c], b[p], b[c]
        for r in range(c + 1, n):
            f = a[r][c] / a[c][c]
            if f:
                for j in range(c, n):
                    a[r][j] -= f * a[c][j]
                b[r] -= f * b[c]
    x = [0.0] * n
    for c in range(n - 1, -1, -1):
        x[c] = (b[c] - sum(a[c][j] * x[j] for j in range(c + 1, n))) / a[c][c]
    return x


class Score:
    def __init__(self, names: list[str], base: str):
        self.names, self.base = names, base
        self.f: dict[str, list[float]] = {m: [] for m in names}
        self.y: list[float] = []

    def add(self, fc: dict[str, float], y: float) -> None:
        for m in self.names:
            self.f[m].append(fc[m])
        self.y.append(y)

    def rmse(self, m: str, a: int = 0, b: int | None = None) -> float:
        e = [(f - y) ** 2 for f, y in zip(self.f[m][a:b], self.y[a:b])]
        return math.sqrt(sum(e) / len(e)) if e else float("nan")

    def dm(self, m: str) -> float:
        d = [(f - y) ** 2 - (g - y) ** 2 for f, g, y in zip(self.f[m], self.f[self.base], self.y)]
        n = len(d)
        if n < 30:
            return float("nan")
        mu = sum(d) / n
        var = sum((x - mu) ** 2 for x in d) / n
        lag = int(n ** (1 / 3))   # Newey-West
        for k in range(1, lag + 1):
            cov = sum((d[i] - mu) * (d[i - k] - mu) for i in range(k, n)) / n
            var += 2 * (1 - k / (lag + 1)) * cov
        return mu / math.sqrt(var / n) if var > 0 else float("nan")

    def thirds(self, m: str) -> bool:
        n = len(self.y)
        cuts = (0, n // 3, 2 * n // 3, n)
        return all(self.rmse(m, cuts[i], cuts[i + 1]) < self.rmse(self.base, cuts[i], cuts[i + 1]) for i in range(3))


def returns(bars: list[tuple[str, str, float]], daily: bool) -> list[tuple[str, str, float]]:
    """[(day, time, log return bp)] -- intraday ones only between bars of one session."""
    out = []
    for (d0, _, c0), (d1, t1, c1) in zip(bars, bars[1:]):
        if daily or d0 == d1:
            out.append((d1, t1, 1e4 * math.log(c1 / c0)))
        else:
            out.append((d1, t1, float("nan")))   # session break: no return, resets the lags
    return out


def run(name: str, tf: str, rets: list[tuple[str, str, float]], lead: dict[tuple[str, str], tuple[float, float]] | None,
        cost: float, log_path: Path | None = None) -> list[dict]:
    daily = tf == "1d"
    rm = ["rw", "mean", "ar5"] + (["fut_lead"] if lead else [])
    sm = ["mean_abs", "last", "ewma", "har"] + ([] if daily else ["har_tod"])
    R, S = Score(rm, "rw"), Score(sm, "mean_abs")
    ar, fl, har, hart = Ridge(5), Ridge(7), Ridge(3), Ridge(4)
    tod: dict[str, list[float]] = {}
    hist: list[float] = []          # every in-session return so far (overnight gaps left out)
    absh: list[float] = []          # every |r| so far, for HAR
    s_mean = s_abs = 0.0
    n_all = 0
    ew = None
    pnl = {m: [] for m in rm}
    logf = logw = None
    if log_path is not None:
        log_path.parent.mkdir(parents=True, exist_ok=True)
        logf = open(log_path, "w", newline="")
        logw = csv.writer(logf)
        logw.writerow(["day", "bar", "actual_bp"] + [f"{m}_bp" for m in rm[1:]] + [f"{m}_call" for m in rm[1:]]
                      + [f"{m}_right" for m in rm[1:]] + [f"size_{m}_bp" for m in sm])
    for i in range(len(rets) - 1):
        d, t, r = rets[i]
        if math.isnan(r):
            continue   # overnight gap: not a return; lags carry over, targets stay in-session
        hist.append(r)
        absh.append(abs(r))
        n_all += 1
        s_mean += r
        s_abs += abs(r)
        ew = abs(r) if ew is None else 0.94 * ew + 0.06 * abs(r)
        tod.setdefault(t, []).append(abs(r))
        nd, nt, y = rets[i + 1]
        if math.isnan(y) or len(hist) < 5:
            continue
        x = hist[-5:][::-1]
        lx = None
        if lead:
            lv = lead.get((d, t))
            if lv:
                lx = x + [lv[0], lv[1]]
        h = [absh[-1], sum(absh[-5:]) / len(absh[-5:]), sum(absh[-22:]) / len(absh[-22:])]
        prof = tod.get(nt)
        tp = sum(prof) / len(prof) if prof and len(prof) >= 5 else s_abs / n_all
        fr = {"rw": 0.0, "mean": s_mean / n_all, "ar5": ar.predict(x)}
        if lead:
            fr["fut_lead"] = fl.predict(lx) if lx else None
        fs = {"mean_abs": s_abs / n_all, "last": abs(r), "ewma": ew, "har": har.predict(h)}
        if not daily:
            fs["har_tod"] = hart.predict(h + [tp])
        ready = all(v is not None for v in fr.values()) and all(v is not None for v in fs.values())
        if ready and n_all > 200:
            if logw is not None:
                logw.writerow([nd, nt, round(y, 3)] + [round(fr[m], 4) for m in rm[1:]]
                              + [("UP" if fr[m] > 0 else "DOWN" if fr[m] < 0 else "") for m in rm[1:]]
                              + [int(fr[m] * y > 0) if fr[m] and y else "" for m in rm[1:]]
                              + [round(fs[m], 3) for m in sm])
            R.add(fr, y)
            S.add(fs, abs(y))
            for m in rm:
                if fr[m] != 0:
                    pnl[m].append(math.copysign(1, fr[m]) * y)
        ar.update(x, y)
        if lx:
            fl.update(lx, y)
        har.update(h, abs(y))
        if not daily:
            hart.update(h + [tp], abs(y))
    if logf is not None:
        logf.close()
    rows = []
    for kind, sc, ms in (("return", R, rm), ("size", S, sm)):
        if len(sc.y) < 100:
            continue
        base_rmse = sc.rmse(sc.base)
        for m in ms:
            row = {"series": name, "tf": tf, "task": kind, "model": m, "n": len(sc.y),
                   "rmse_bp": round(sc.rmse(m), 4), "base_rmse_bp": round(base_rmse, 4),
                   "skill_pct": round(100 * (1 - sc.rmse(m) / base_rmse), 3) if base_rmse else "",
                   "dm_t": "" if m == sc.base else round(sc.dm(m), 2),
                   "thirds": "" if m == sc.base else sc.thirds(m)}
            if kind == "return":
                hits = [f * y > 0 for f, y in zip(sc.f[m], sc.y) if f != 0 and y != 0]
                row["hit_pct"] = round(100 * sum(hits) / len(hits), 2) if hits else ""
                big = sorted(abs(f) for f in sc.f[m] if f)
                if big:   # the most confident tenth of the calls
                    cut = big[int(0.9 * (len(big) - 1))]
                    top = [(f * y > 0, math.copysign(1, f) * y) for f, y in zip(sc.f[m], sc.y) if f and abs(f) >= cut and y]
                    row["top10_hit_pct"] = round(100 * sum(h for h, _ in top) / len(top), 2) if top else ""
                    row["top10_gross_bp"] = round(sum(g for _, g in top) / len(top), 3) if top else ""
                p = pnl[m]
                row["gross_bp_per_trade"] = round(sum(p) / len(p), 3) if p else ""
                row["net_bp_after_cost"] = round(sum(p) / len(p) - cost, 3) if p else ""
            rows.append(row)
    return rows


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bars", required=True, help="tree with fut/<x>/<tf>/ and spot/nifty/<tf>/")
    ap.add_argument("--out", required=True)
    ap.add_argument("--tfs", default=",".join(TFS))
    ap.add_argument("--cost-bp", type=float, default=0.0, help="round-trip cost per trade in bp (default 0: before expenses)")
    ap.add_argument("--logs", action="store_true", help="every forecast, one CSV per series and timeframe, in <out>/logs/")
    a = ap.parse_args()
    root, out = Path(a.bars), Path(a.out)
    src = (root / "SOURCE.txt").read_text(errors="replace").strip() if (root / "SOURCE.txt").is_file() else "unknown"
    print(f"bar_forecast: {root} (source: {src})", flush=True)
    rows: list[dict] = []
    for tf in a.tfs.split(","):
        loaded = {}
        for seg, inst in SERIES:
            p = root / seg / inst / tf
            if p.is_dir():
                loaded[f"{seg}/{inst}"] = load(p)
        for name, bars in loaded.items():
            rets = returns(bars, tf == "1d")
            lead = None
            if name == "spot/nifty" and "fut/nifty" in loaded:
                fb = loaded["fut/nifty"]
                fmap = {(d, t): c for d, t, c in fb}
                smap = {(d, t): c for d, t, c in bars}
                lead = {}
                prev = None
                for d, t, c in fb:
                    s = smap.get((d, t))
                    if prev and s and (tf == "1d" or prev[0] == d) and prev[3]:
                        lead[(d, t)] = (1e4 * math.log(c / prev[2]), 1e4 * (math.log(c / s) - math.log(prev[2] / prev[3])))
                    prev = (d, t, c, s)
                del fmap
            got = run(name, tf, rets, lead, a.cost_bp,
                      out / "logs" / f"{name.replace('/', '_')}_{tf}.csv" if a.logs else None)
            rows += got
            print(f"  {name} {tf}: {len(bars)} bars, {len(got)} scores", flush=True)
    tests = sum(1 for r in rows if r["dm_t"] != "")
    bar = NormalDist().inv_cdf(1 - 0.025 / max(tests, 1))
    for r in rows:
        r["significant"] = "" if r["dm_t"] == "" else bool(r["dm_t"] <= -bar and r["thirds"])
    out.mkdir(parents=True, exist_ok=True)
    keys = ["series", "tf", "task", "model", "n", "rmse_bp", "base_rmse_bp", "skill_pct", "dm_t", "thirds",
            "significant", "hit_pct", "gross_bp_per_trade", "net_bp_after_cost", "top10_hit_pct", "top10_gross_bp"]
    with open(out / "summary.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=keys)
        w.writeheader()
        for r in rows:
            w.writerow({k: r.get(k, "") for k in keys})
    md = [f"# Bar forecasts ({src})", "", f"{tests} tests; significant = DM t <= -{bar:.2f} and better in every third.",
          f"Expenses: {a.cost_bp} bp a round trip (0 = before expenses).", "",
          "Direction: hit % = calls right when the bar moved; gross = bp a bar earned trading the call, before expenses.", "",
          "| series | tf | task | model | n | skill % | DM t | sig | direction hit % | gross bp/call | net bp/call | top-10% hit % | top-10% gross bp |",
          "|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
    for r in rows:
        if r["dm_t"] == "":
            continue
        md.append(f"| {r['series']} | {r['tf']} | {r['task']} | {r['model']} | {r['n']} | {r['skill_pct']} | {r['dm_t']} | "
                  f"{'YES' if r['significant'] else ''} | {r.get('hit_pct', '')} | {r.get('gross_bp_per_trade', '')} | {r.get('net_bp_after_cost', '')} | {r.get('top10_hit_pct', '')} | {r.get('top10_gross_bp', '')} |")
    (out / "summary.md").write_text("\n".join(md) + "\n")
    print(f"wrote {out / 'summary.csv'} and summary.md ({len(rows)} rows)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
