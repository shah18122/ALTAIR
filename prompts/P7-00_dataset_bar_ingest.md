# P7-00 — `research/tools/ingest_bars.py`: TradingView bars → the `dataset/` partition

> Phase 7 · Card 0 of 5 · Status: **DONE** 2026-08-31
> Feeds: P6 (backtester), P7-03 (replication harness), P8 (training), P10 (VIX)
>
> **Architect's note.** This is the one piece of Python in the tree that is not
> reference material, and it is deliberate. CLAUDE.md bans Python from the
> **runtime**; this runs once a day on a workstation and is never linked into
> the engine. The engine reads the *normalised output* through a C++ loader
> (a later card), never this script's input. Writing a once-a-day CSV
> normaliser in dependency-free C++23 would be real cost for no benefit.

---

## 1. CONTEXT

Smit supplies bar data as TradingView exports and will drop 1-minute OHLCV
daily. Those exports are not usable as they arrive. They carry OHLCV wrapped in
up to 41 extra columns, and three separate hazards live in them.

## 2. THE TWO RULES THE TOOL IS BUILT ON

**R1 — It never parses a price as a number.**
`24080.4` round-tripped through a float can come back as
`24080.400000000001`. P1-04 exists because a price that drifts by one unit
rounds an order to the wrong tick; a research pipeline that reintroduces the
drift upstream would undo it. Prices are copied as **text, byte for byte**.
Conversion to paise happens later, in C++, through the P1-04 path.

**R2 — It refuses to overwrite history.**
If a timestamp is already on disk with different values, the script stops and
reports both rather than picking one. Two exports disagreeing about the same
instant is a real event; silently rewriting the past is how a backtest quietly
stops matching the world. Same philosophy as the P1-06 reconciler.

## 3. WHAT IS DROPPED, AND WHY

Only `time, open, high, low, close, volume` survive. Everything else is
discarded for one of three disqualifying reasons:

| Columns | Reason |
|---|---|
| `Expected Value Level`, `N Sigma Upper/Lower Level`, `Developing High/Low Outcome`, `Real/Random Maximum Change …` | A **forward projection**. Populated only on the last ~100 bars of the export, because that is the forecast horizon. A forward projection in a training set is look-ahead by definition — **rule 7**. |
| `Regular Bullish`, `Regular Bearish` (+ labels) | Divergence detection needs a **confirmed pivot**, known only several bars later. TradingView repaints these. A label that repaints is a label that leaks. |
| `RSI`, `RSI-based MA` | Neither look-ahead nor repainting, but a **black box**: unknown period, unknown smoothing, unrecoverable from the file. Rule 10 requires every live decision to be reproducible from a `feature_version`, and an indicator nobody can recompute cannot carry one. Derived features belong in `features/`, computed by our code, versioned. |

The raw dataset holds measurements. Nothing else.

## 4. LAYOUT

`dataset/<segment>/<symbol>/<timeframe>/<partition>.csv`, per CLAUDE.md.
Partition sized so it is a useful read unit *and* a fold boundary:

| Timeframe | Partition | Typical size |
|---|---|---|
| `1m` | one file per **trading date** | ~375 bars — and it is the shape the daily drop arrives in, so an ingest appends one file rather than rewriting a large one |
| `3m`..`240m` | one file per **month** | 15m ~500, 60m ~150. Per-date here means 7-bar files: 606 of them for two years of 60m, which is clutter, not a boundary |
| `1d` and up | a single file | small, always read whole |

Every choice still leaves a walk-forward fold selectable by filename glob,
which is the point of the partition.

## 5. VALIDATION THAT RUNS EVERY TIME

A 1-minute session-coverage report runs on **every ingest**, not as a one-off
audit — because the very first drop already contained a systematic hole (§6).
A finding is worth less than a check that repeats.

## 6. FINDINGS FROM THE FIRST DROP — 2026-08-31

Eight files: NIFTY and INDIAVIX at 1m/15m/60m/1D. 17'327 bars, 41 partitions.
Structurally clean — **zero** duplicate timestamps, zero non-monotonic rows,
zero OHLC violations (`high >= max(o,c)`, `low <= min(o,c)`) across all eight.

Four findings that constrain how the data may be used:

1. **NIFTY 1-minute is missing 15:16–15:27 on every single day.** Twelve
   consecutive minutes, all four days, plus one boundary bar. India VIX has all
   375. This is the **worst possible place** for a gap: it is where a
   10-minute-horizon label lives (the Phase 6 headline strategy), and it is
   where NIFTY and VIX stop being joinable bar for bar. Do not interpolate
   across it — an interpolated close-window bar is a fabricated measurement in
   the exact window a model is graded on. Either source those minutes from Kite
   historical, or make every close-window label explicitly unavailable.
2. **India VIX daily `open` is synthetic.** It equals the previous close on
   **524 of 526** bars. It is a copy, not a measurement, and carries no
   information. A model given it as a feature is modelling a TradingView
   artifact. Use VIX daily close, high and low; ignore the open.
3. **NIFTY daily has 1'408 zero-volume bars, all before 1997-01-01.** The
   series starts 1990-07-03 with `O=H=L=C=279.01999` and `volume=0` —
   backfilled index values, not traded sessions. Real volume begins
   **1997-01-01**; 7'348 of 8'756 daily bars are usable for anything
   volume-aware.
4. **Export row caps, not history.** TradingView capped these: NIFTY 1m and 15m
   at 1'206 rows, every INDIAVIX file at 526. So NIFTY 1m covers **4 trading
   days**, not four months. Intraday depth has to accumulate from the daily
   drop or come from a real historical source.

## 7. WHAT THIS DATA IS NOT

It is **bar data**, and it does **not** unblock P1-02, P1-03 or P1-05. Those
need *instrument master* files — lot size, tick size, strike step, expiry — a
different artifact entirely. Blocker 9 stands.
