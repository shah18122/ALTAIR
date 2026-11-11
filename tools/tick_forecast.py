#!/usr/bin/env python3
"""tools/tick_forecast.py -- the pure-Python twin of altair_tick_forecast
(models/tick_forecast.hpp + app/tick_forecast_main.cpp), for a machine with
no C++ compiler. Same tasks, models, walk-forward rules and scoring; standard
library only.

    python tools/tick_forecast.py --ticks DIR [--out DIR] [--only TEXT]

Source "drive-ticks": Smit's Google Drive tick files, NOT FYERS
(docs/DATA_POLICY.md).
"""
from __future__ import annotations

import argparse
import math
import os
import re
import sys
from collections import deque

OPEN, CLOSE = 9 * 3600 + 15 * 60, 15 * 3600 + 30 * 60
FUT_RE = re.compile(r"^(BANKNIFTY|NIFTY)\d\d[A-Z]{3}FUT\.csv$", re.I)


def parse_day(s: str) -> int:
    d = "".join(c for c in s if c.isdigit())
    if len(d) != 8:
        return 0
    year_first = len(s) >= 4 and s[0].isdigit() and s[2].isdigit() and (len(s) == 8 or s[4] in "-/")
    return int(d) if year_first else int(d[4:8] + d[2:4] + d[0:2])


def read_ticks(path):
    with open(path, "r", newline="") as f:
        for line in f:
            c = line.rstrip("\r\n").split(",")
            if len(c) < 3:
                continue
            date, time, pi = c[0], c[1], 2
            sp = max(c[0].find(" "), c[0].find("T"))
            if sp > 0:
                date, time, pi = c[0][:sp], c[0][sp + 1:], 1
            day = parse_day(date)
            if day == 0 or len(time) < 8 or time[2] != ":":
                continue
            try:
                sec = int(time[0:2]) * 3600 + int(time[3:5]) * 60 + int(time[6:8])
                px = float(c[pi])
                qty = float(c[pi + 1]) if pi + 1 < len(c) and c[pi + 1] else 0.0
            except ValueError:
                continue
            if px > 0:
                yield day, sec, px, qty


class Scorer:
    def __init__(self, names, block=2000):
        self.names, self.block = names, block
        self.bse, self.bae, self.bn = [], [], []
        self.cse = self.cae = None
        self.cn = 0

    def add(self, f, actual):
        if self.cn == 0:
            self.cse = [0.0] * len(self.names)
            self.cae = [0.0] * len(self.names)
        for m, fm in enumerate(f):
            e = fm - actual
            self.cse[m] += e * e
            self.cae[m] += abs(e)
        self.cn += 1
        if self.cn == self.block:
            self.flush()

    def flush(self):
        self.bse.append(self.cse); self.bae.append(self.cae); self.bn.append(self.cn); self.cn = 0

    def finish(self):
        if self.cn:
            self.flush()

    def n(self):
        return sum(self.bn)

    def rmse(self, m, a=0, b=None):
        b = len(self.bn) if b is None else b
        k = sum(self.bn[a:b])
        return math.sqrt(sum(x[m] for x in self.bse[a:b]) / k) if k else 0.0

    def mae(self, m):
        k = self.n()
        return sum(x[m] for x in self.bae) / k if k else 0.0

    def skill(self, m, base, a=0, b=None):
        r = self.rmse(base, a, b)
        return 1 - self.rmse(m, a, b) / r if r > 0 else 0.0

    def thirds(self, m, base):
        B = len(self.bn)
        return [self.skill(m, base, 0, B // 3), self.skill(m, base, B // 3, 2 * B // 3), self.skill(m, base, 2 * B // 3, B)]

    def dm(self, m, base):
        B = len(self.bn)
        if B < 10:
            return 0.0
        d = [(self.bse[i][m] - self.bse[i][base]) / self.bn[i] for i in range(B)]
        mu = sum(d) / B
        v = sum((x - mu) ** 2 for x in d) / (B - 1)
        return mu / math.sqrt(v / B) if v > 0 else 0.0


class Ridge:
    def __init__(self, k, half_life=2e6, refit=5000, ridge=1e-6, keep=20000):
        self.k, self.lam, self.refit, self.ridge, self.keep = k, math.exp(math.log(0.5) / half_life), refit, ridge, keep
        self.a = [[0.0] * k for _ in range(k)]
        self.b = [0.0] * k
        self.w = [0.0] * k
        self.fitted, self.since = False, 0
        self.tail = deque(maxlen=keep)

    def predict(self, x):
        return sum(wi * xi for wi, xi in zip(self.w, x))

    def update(self, x, y, oos):
        lam, a, b = self.lam, self.a, self.b
        for i in range(self.k):
            xi = x[i]
            ai = a[i]
            for j in range(self.k):
                ai[j] = lam * ai[j] + xi * x[j]
            b[i] = lam * b[i] + xi * y
        if self.fitted:
            self.tail.append((tuple(x), y, oos))
        self.since += 1
        if self.since >= self.refit:
            self.since = 0
            self.fit()

    def fit(self):
        K = self.k
        a = [row[:] for row in self.a]
        b = self.b[:]
        tr = sum(a[i][i] for i in range(K))
        for i in range(1, K):
            a[i][i] += self.ridge * tr / K
        for c in range(K):
            p = max(range(c, K), key=lambda r: abs(a[r][c]))
            if abs(a[p][c]) < 1e-12:
                return
            a[p], a[c] = a[c], a[p]
            b[p], b[c] = b[c], b[p]
            for r in range(c + 1, K):
                f = a[r][c] / a[c][c]
                for k in range(c, K):
                    a[r][k] -= f * a[c][k]
                b[r] -= f * b[c]
        w = [0.0] * K
        for c in range(K - 1, -1, -1):
            s = b[c] - sum(a[c][k] * w[k] for k in range(c + 1, K))
            w[c] = s / a[c][c]
        if all(math.isfinite(v) for v in w):
            self.w, self.fitted = w, True

    def overfit(self):
        if not self.tail:
            return float("nan"), float("nan")
        si = so = 0.0
        for x, y, oos in self.tail:
            si += (self.predict(x) - y) ** 2
            so += (oos - y) ** 2
        n = len(self.tail)
        return math.sqrt(si / n), math.sqrt(so / n)


class Hits:
    def __init__(self):
        self.n = self.hit = 0

    def add(self, ok):
        self.n += 1
        self.hit += 1 if ok else 0

    def rate(self):
        return self.hit / self.n if self.n else 0.0

    def z(self):
        return (self.hit - 0.5 * self.n) / math.sqrt(0.25 * self.n) if self.n else 0.0


RV_H = (60, 300)


class Engine:
    def __init__(self):
        self.ret = Scorer(["Random walk", "Running mean", "AR ridge"])
        self.gap = Scorer(["Last gap", "Running mean", "EWMA", "Ridge"])
        self.rv = [Scorer(["Last window", "Running mean", "Time of day", "HAR ridge"], 200),
                   Scorer(["Last window", "Running mean", "Time of day", "HAR ridge"], 50)]
        self.ar, self.gap_r, self.sign_r = Ridge(6), Ridge(6), Ridge(4)
        self.rv_r = [Ridge(5, 2e4, 200, 1e-6, 5000), Ridge(5, 5e3, 50, 1e-6, 2000)]
        self.ar_dir, self.persist, self.reverse, self.sign_ridge = Hits(), Hits(), Hits(), Hits()
        self.ticks = self.days = 0
        self.day = 0
        self.last_sec = 0
        self.last_px = 0.0
        self.have_last = self.pending_ret = self.pending_gap = False
        self.ret_f = [0.0, 0.0, 0.0]
        self.ret_x = [0.0] * 6
        self.ret_mean = self.ret_mean_n = 0.0
        self.gap_f = [0.0] * 4
        self.gap_x = [0.0] * 6
        self.gap_mean = self.gap_mean_n = 0.0
        self.gap_ewma, self.last_gap = -1.0, 0.0
        self.last_sign = self.sign_pred = 0
        self.sign_x = [0.0] * 4
        self.flow = deque(maxlen=20)
        self.hist = [0.0, 0.0, 0.0]
        self.vpq = self.vq = 0.0
        self.rvs = [dict(grid_end=0, cur=0.0, past=deque(maxlen=30), pending=False, partial=False, f=[0.0] * 4, x=[0.0] * 5,
                         mean=0.0, mean_n=0.0, seas=[], seas_n=[]) for _ in RV_H]

    def new_day(self, day, sec):
        self.day = day
        self.days += 1
        self.have_last = self.pending_ret = self.pending_gap = False
        self.last_sign = self.sign_pred = 0
        self.flow.clear()
        self.hist = [0.0, 0.0, 0.0]
        self.vpq = self.vq = 0.0
        for h, H in enumerate(RV_H):
            s = self.rvs[h]
            s["grid_end"] = OPEN + ((sec - OPEN) // H + 1) * H
            s["cur"] = 0.0
            s["past"].clear()
            s["pending"] = False
            s["partial"] = True
            if not s["seas"]:
                s["seas"] = [0.0] * ((CLOSE - OPEN) // H + 1)
                s["seas_n"] = [0.0] * ((CLOSE - OPEN) // H + 1)

    def advance_rv(self, h, sec, r):
        s, H, R, S = self.rvs[h], RV_H[h], self.rv_r[h], self.rv[h]
        while sec >= s["grid_end"] and s["grid_end"] <= CLOSE:
            vol = math.sqrt(s["cur"])
            if s["partial"]:
                s["partial"] = False
                s["cur"] = 0.0
                s["pending"] = False
                s["grid_end"] += H
                continue
            if s["pending"]:
                s["f"][3] = max(0.0, R.predict(s["x"])) if R.fitted else s["f"][2]
                S.add(s["f"], vol)
                R.update(s["x"], vol, s["f"][3])
            slot = (s["grid_end"] - OPEN) // H - 1
            if 0 <= slot < len(s["seas"]):
                s["seas_n"][slot] += 1
                s["seas"][slot] += (vol - s["seas"][slot]) / s["seas_n"][slot]
            s["mean_n"] += 1
            s["mean"] += (vol - s["mean"]) / s["mean_n"]
            s["past"].append(vol)
            s["cur"] = 0.0
            nxt = s["grid_end"] + H
            nslot = (nxt - OPEN) // H - 1
            if nxt <= CLOSE and s["mean_n"] > 0:
                p = list(s["past"])

                def avg_last(k):
                    q = p[-k:]
                    return sum(q) / len(q) if q else s["mean"]
                seas = s["seas"][nslot] if 0 <= nslot < len(s["seas"]) and s["seas_n"][nslot] > 0 else s["mean"]
                s["f"] = [vol, s["mean"], seas, 0.0]
                s["x"] = [1.0, vol, avg_last(5), avg_last(30), seas]
                s["pending"] = True
            else:
                s["pending"] = False
            s["grid_end"] = nxt
        s["cur"] += r * r

    def add(self, day, sec, px, qty):
        if not px > 0 or sec < OPEN or sec >= CLOSE:
            return
        self.ticks += 1
        if day != self.day:
            self.new_day(day, sec)
        r = 1e4 * math.log(px / self.last_px) if self.have_last else 0.0
        if self.have_last and self.pending_ret:
            f = self.ar.predict(self.ret_x) if self.ar.fitted else 0.0
            self.ret_f[2] = f
            self.ret.add(self.ret_f, r)
            if r != 0.0 and f != 0.0:
                self.ar_dir.add((r > 0) == (f > 0))
            self.ar.update(self.ret_x, r, f)
            self.ret_mean_n += 1
            self.ret_mean += (r - self.ret_mean) / self.ret_mean_n
        if self.have_last and self.pending_gap:
            g = float(sec - self.last_sec)
            self.gap_f[3] = max(0.0, self.gap_r.predict(self.gap_x)) if self.gap_r.fitted else self.gap_f[1]
            self.gap.add(self.gap_f, g)
            self.gap_r.update(self.gap_x, g, self.gap_f[3])
            self.gap_mean_n += 1
            self.gap_mean += (g - self.gap_mean) / self.gap_mean_n
            self.gap_ewma = g if self.gap_ewma < 0 else 0.95 * self.gap_ewma + 0.05 * g
            self.last_gap = g
        if self.have_last and r != 0.0:
            s = 1 if r > 0 else -1
            if self.last_sign != 0:
                self.persist.add(s == self.last_sign)
                self.reverse.add(s == -self.last_sign)
                if self.sign_pred != 0:
                    self.sign_ridge.add(s == self.sign_pred)
                self.sign_r.update(self.sign_x, float(s), float(self.sign_pred))
            self.last_sign = s
        for h in range(len(RV_H)):
            self.advance_rv(h, sec, r)
        sg = (self.flow[-1][0] if self.flow else 0.0) if r == 0.0 else (1.0 if r > 0 else -1.0)
        self.flow.append((sg, qty))
        self.hist = [r, self.hist[0], self.hist[1]]
        q1 = max(1.0, qty)
        self.vpq = 0.98 * self.vpq + 0.02 * px * q1
        self.vq = 0.98 * self.vq + 0.02 * q1
        self.last_px, self.last_sec, self.have_last = px, sec, True
        fs = fq = 0.0
        for a, b in self.flow:
            fs += a * b
            fq += b
        flow = fs / fq if fq > 0 else 0.0
        dev = 1e4 * math.log(px / (self.vpq / self.vq)) if self.vq > 0 else 0.0
        self.ret_x = [1.0, self.hist[0], self.hist[1], self.hist[2], flow, dev]
        self.ret_f = [0.0, self.ret_mean, 0.0]
        self.pending_ret = True
        tod = (sec - OPEN) / (CLOSE - OPEN)
        ew = self.gap_mean if self.gap_ewma < 0 else self.gap_ewma
        self.gap_f = [self.last_gap, self.gap_mean, ew, 0.0]
        self.gap_x = [1.0, self.last_gap, ew, math.log1p(qty), tod, tod * tod]
        self.pending_gap = True
        self.sign_x = [1.0, float(self.last_sign), flow, dev / 10.0]
        sp = self.sign_r.predict(self.sign_x) if self.sign_r.fitted else 0.0
        self.sign_pred = 1 if sp > 0 else -1 if sp < 0 else 0

    def finish(self):
        self.ret.finish(); self.gap.finish()
        for s in self.rv:
            s.finish()


def run_stream(name, files, by_qty, e):
    best = {}
    for i, p in enumerate(files):
        w = {}
        for day, sec, px, qty in read_ticks(p):
            w[day] = w.get(day, 0.0) + (qty if by_qty else 1.0)
        for d, x in w.items():
            if d not in best or x > best[d][0]:
                best[d] = (x, i)
    first = {}
    for d in sorted(best):
        first.setdefault(best[d][1], d)
    last_day = 0
    for d0, i in sorted((d, i) for i, d in first.items()):
        ticks = [t for t in read_ticks(files[i]) if t[0] in best and best[t[0]][1] == i and t[0] > last_day]
        ticks.sort(key=lambda t: (t[0], t[1]))
        for t in ticks:
            e.add(*t)
        if ticks:
            last_day = ticks[-1][0]
        print(f"    {os.path.basename(files[i])}: {len(ticks)} ticks fed", flush=True)
    e.finish()


def bonferroni_z(m):
    p = 0.05 / max(1, m)
    lo, hi = 0.0, 10.0
    for _ in range(80):
        mid = 0.5 * (lo + hi)
        if math.erfc(mid / math.sqrt(2)) > p:
            lo = mid
        else:
            hi = mid
    return hi


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ticks", required=True)
    ap.add_argument("--out", default="data/verified")
    ap.add_argument("--only", default="")
    a = ap.parse_args()
    print(f"tick_forecast.py: {a.ticks} (source drive-ticks, NOT FYERS)")
    streams = {"NIFTY FUT": [], "BANKNIFTY FUT": [], "NIFTY spot": []}
    for root, _, names in os.walk(a.ticks):
        for n in names:
            m = FUT_RE.match(n)
            if m:
                streams["NIFTY FUT" if m.group(1).upper() == "NIFTY" else "BANKNIFTY FUT"].append(os.path.join(root, n))
            elif n.upper() == "NIFTY.CSV" and "spot" in root.lower():
                streams["NIFTY spot"].append(os.path.join(root, n))
    done = []
    for name, files in streams.items():
        if not files or (a.only and a.only not in name):
            continue
        files.sort()
        print(f"  {name}: {len(files)} files", flush=True)
        e = Engine()
        run_stream(name, files, name != "NIFTY spot", e)
        done.append((name, e))
    if not done:
        print("no tick files")
        return 1
    tests = len(done) * (2 + 3 + 2 * 3 + 3)
    bar = bonferroni_z(tests)
    print(f"\nSignificant: |DM t| or |z| >= {bar:.2f} (Bonferroni over {tests} tests) and better in every third.")
    od = os.path.join(a.out, "tick_forecast")
    os.makedirs(od, exist_ok=True)
    csv = open(os.path.join(od, "summary.csv"), "w")
    csv.write("stream,task,model,n,rmse,mae,skill_vs_base,dm_vs_base,third1,third2,third3,in_sample_rmse,oos_rmse_same_rows,verdict\n")

    def table(stream, task, unit, s, base, ridge, ov):
        print(f"\n  {stream}, {task} ({s.n()} forecasts; {unit}; overfit check, last rows: ridge in-sample RMSE {ov[0]:.4g} vs out-of-sample {ov[1]:.4g})")
        print(f"    {'model':<14} {'RMSE':>10} {'MAE':>10} {'skill':>9} {'DM t':>8}  verdict")
        best = min(range(len(s.names)), key=s.rmse)
        for m, nm in enumerate(s.names):
            th, sk, dm = s.thirds(m, base), s.skill(m, base), s.dm(m, base)
            v = "baseline" if m == base else ("beats baseline (significant)" if dm <= -bar and min(th) > 0 else "no proven edge")
            print(f"    {nm:<14} {s.rmse(m):>10.4g} {s.mae(m):>10.4g} {100 * sk:>+8.2f}% {dm:>+8.1f}  {v}{'  <- lowest RMSE' if m == best else ''}")
            csv.write(f'"{stream}","{task}","{nm}",{s.n()},{s.rmse(m)},{s.mae(m)},{sk},{dm},{th[0]},{th[1]},{th[2]},'
                      f'{ov[0] if m == ridge else ""},{ov[1] if m == ridge else ""},"{v}"\n')

    def hits(stream, nm, h):
        v = "better than a coin (significant)" if h.z() >= bar else "no proven edge"
        print(f"    {nm:<24} {100 * h.rate():6.2f}% of {h.n} calls, z {h.z():+.1f}  {v}")
        csv.write(f'"{stream}","direction","{nm}",{h.n},,,{h.rate()},{h.z()},,,,,,"{v}"\n')

    for name, e in done:
        print(f"\n{name}: {e.ticks} ticks, {e.days} sessions")
        table(name, "next-trade return", "bp", e.ret, 0, 2, e.ar.overfit())
        print(f"\n  {name}, direction of the next price change")
        hits(name, "Persistence (same again)", e.persist)
        hits(name, "Reversal (bounce)", e.reverse)
        hits(name, "Ridge sign", e.sign_ridge)
        hits(name, "AR ridge, next trade", e.ar_dir)
        table(name, "seconds to next trade", "s", e.gap, 1, 3, e.gap_r.overfit())
        for h, H in enumerate(RV_H):
            table(name, f"realised vol next {H} s", "bp, from every tick", e.rv[h], 0, 3, e.rv_r[h].overfit())
    csv.close()
    print(f"\nwrote {os.path.join(od, 'summary.csv')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
