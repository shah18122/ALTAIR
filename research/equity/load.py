"""research/equity/load.py -- Drive 5-minute stock bars and index-future bars -> aligned arrays.

SOURCE: Smit's Drive files (source "drive-ticks", NOT FYERS; never merged into FYERS series).
  stocks:  <EQ>/<year>/<SYM>_5min_OHLCV.csv   timestamp,open,high,low,close,volume (bar start label)
  futures: <BARS>/fut/<inst>/5m/all.csv        time,open,high,low,close,volume,oi (tools/drive_ticks_to_bars.py)

Grid: one row per session, 75 slots (09:15 .. 15:25 bar starts). Missing bars: close carried
forward within the day, open = that carried close, volume 0.
"""
from __future__ import annotations

import os
from pathlib import Path

import numpy as np
import pandas as pd

S = 75  # 5-minute bars 09:15 .. 15:25


def _slot(t: pd.Series) -> np.ndarray:
    return ((t.dt.hour * 60 + t.dt.minute) - (9 * 60 + 15)).to_numpy() // 5


def read_universe(path: Path) -> list[str]:
    return [l.strip() for l in open(path) if l.strip() and not l.startswith("#")]


def load_stocks(eq_dir: Path, years: list[str], syms: list[str]):
    """Returns days (list of date), syms (list), dict of arrays (D, S, K): open, close, vol."""
    frames = {}
    for y in years:
        for s in syms:
            f = Path(eq_dir) / y / f"{s}_5min_OHLCV.csv"
            if not f.exists():
                continue
            d = pd.read_csv(f)
            t = pd.to_datetime(d.timestamp)
            sl = _slot(t)
            keep = (sl >= 0) & (sl < S) & ((t.dt.hour * 60 + t.dt.minute).to_numpy() >= 555) & (d.close.to_numpy() > 0)
            d = d[keep].copy()
            d["day"] = t[keep].dt.date.to_numpy()
            d["slot"] = sl[keep]
            frames.setdefault(s, []).append(d)
    syms = [s for s in syms if s in frames]
    allday = sorted(set().union(*[set(pd.concat(v).day) for v in frames.values()]))
    di = {d: i for i, d in enumerate(allday)}
    D, K = len(allday), len(syms)
    O = np.full((D, S, K), np.nan)
    C = np.full((D, S, K), np.nan)
    V = np.zeros((D, S, K))
    for k, s in enumerate(syms):
        d = pd.concat(frames[s])
        r = d.day.map(di).to_numpy()
        O[r, d.slot.to_numpy(), k] = d.open.to_numpy()
        C[r, d.slot.to_numpy(), k] = d.close.to_numpy()
        V[r, d.slot.to_numpy(), k] = d.volume.to_numpy()
    has = ~np.isnan(C)
    present = has.any(axis=1)  # (D, K): stock traded that day
    # carry close forward within the day; before the first bar of the day use that bar's open
    Cf = C.copy()
    for s in range(1, S):
        m = np.isnan(Cf[:, s]) & ~np.isnan(Cf[:, s - 1])
        Cf[:, s][m] = Cf[:, s - 1][m]
    first_open = np.full((D, K), np.nan)
    for s in range(S - 1, -1, -1):
        m = ~np.isnan(O[:, s])
        first_open[m] = O[:, s][m]
    for s in range(S):
        m = np.isnan(Cf[:, s])
        Cf[:, s][m] = first_open[m]
    Of = np.where(np.isnan(O), np.concatenate([Cf[:, :1], Cf[:, :-1]], axis=1), O)
    Of[:, 0] = np.where(np.isnan(O[:, 0]), first_open, O[:, 0])
    Of[~np.broadcast_to(present[:, None, :], Of.shape)] = np.nan
    Cf[~np.broadcast_to(present[:, None, :], Cf.shape)] = np.nan
    return allday, syms, {"open": Of, "close": Cf, "vol": V, "present": present}


def load_future(bars: Path, inst: str, days: list):
    """Front-future 5-minute open/close on the given days, (D, S); NaN where missing."""
    d = pd.read_csv(Path(bars) / "fut" / inst / "5m" / "all.csv")
    t = pd.to_datetime(d.time.str[:19])
    d["day"] = t.dt.date
    d["slot"] = _slot(t)
    d = d[(d.slot >= 0) & (d.slot < S)]
    di = {x: i for i, x in enumerate(days)}
    d = d[d.day.isin(di)]
    O = np.full((len(days), S), np.nan)
    C = np.full((len(days), S), np.nan)
    r = d.day.map(di).to_numpy()
    O[r, d.slot.to_numpy()] = d.open.to_numpy()
    C[r, d.slot.to_numpy()] = d.close.to_numpy()
    Cf = pd.DataFrame(C).T.ffill().bfill().T.to_numpy().copy()
    Of = np.where(np.isnan(O), np.concatenate([Cf[:, :1], Cf[:, :-1]], axis=1), O)
    nb = np.sum(~np.isnan(C), axis=1)
    bad = nb < 60
    Of[bad] = np.nan
    Cf[bad] = np.nan
    return Of, Cf
