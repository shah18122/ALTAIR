# Data audit — dataset/ across timeframes, sources and brokers

`altair_data_audit` answers two questions for every series in `dataset/`
(`<segment>/<instrument>/<tf>/`, tf = 1m 5m 15m 60m 1d):

1. **Do the timeframes agree with each other?** Every coarser stored timeframe
   is rebuilt from every finer one (open = first, high = max, low = min,
   close = last, volume = sum, OI = last) and compared bar by bar. Only
   complete buckets are compared, so a gap is reported as a gap, not as a wrong
   bar. Other sources kept in `_superseded_*` folders are compared too.
2. **Does the data agree with the broker?** FYERS and Kite candles, fetched
   read-only into `data/broker_audit/`, are compared bar by bar.

Each series is also checked on its own: unparseable rows, duplicate and
conflicting timestamps, impossible OHLC, bars outside the session or off the
timeframe grid, weekend sessions, days with missing bars, and stamps carrying
seconds (`09:21:01`, floored to the minute and counted).

Tolerance: **exact** ≤ ½ paisa; **within 1 bp** is rounding; above 1 bp is a
**mismatch**. Cash/index sessions are 09:15–15:30 IST; F&O series are audited
to 15:40 (the dataset's futures bars and FYERS' symbol master both run there).

## Run it

On your PC, after `build.bat net` and linking FYERS and Kite:

```powershell
powershell -ExecutionPolicy Bypass -File ops\broker_audit.ps1
```

That fetches both brokers year by year (resumable; missing years are logged to
`data\broker_audit\fetch_log.txt` and skipped), runs the audit and opens the
workbook. Options: `-From 2020-01-01`, `-SkipKite`, `-SkipFyers`,
`-AuditOnly` (compare what is already fetched), `-Force` (re-fetch),
`-NoOpen`.

Without brokers (cross-timeframe only):

```powershell
build\net\app\altair_data_audit.exe --dataset dataset --out data\verified
```

## Output (`data/verified/`, git-ignored)

| File | What |
|---|---|
| `data_audit.xlsx` | **Summary** sheet (every series, every comparison) and **one sheet per instrument**: manifest provenance, integrity per timeframe, every comparison, then the issue list (mismatches worst first, missing bars, integrity notes). |
| `data_audit.txt` | The summary as text. |
| `merged/<segment>_<instrument>_<tf>.csv` | One sorted, de-duplicated file per instrument per timeframe (all monthly files joined; seconds floored). NIFTY 1m is ~1.08 M rows, over Excel's sheet limit, hence CSV. |

Status: **PASS** nothing to report · **WARN** rounding, gaps, special
sessions or duplicates · **FAIL** mismatches, conflicts or impossible OHLC ·
**CLOSE ONLY** a 1d close differs from the last intraday close while open,
high and low agree (NSE's official index close and F&O settlement are 30-minute
averages, not the last trade).

## Findings on the dataset pushed 2026-09-29 (cross-timeframe run)

| Series | Finding | Meaning / action |
|---|---|---|
| NIFTY 1d vs intraday | Close differs on ~88 % of days (median 5.6 bp) | Expected: official close ≠ last trade. |
| NIFTY 1d vs intraday | High/low differ on ~10 % of days, **2015–2023 only** (median ~1.8 bp); worst 2020-03-23 high 8159.25 vs 8355.55 | The old 1m feed misses or adds extremes; the 1d high matches NSE's official 8159.25, so the **1m bars around the COVID circuit halts are wrong**. Broker check will arbitrate. |
| NIFTY 1m | 10,693 bars stamped `hh:mm:01` (Jun–Nov 2015); 4 minutes held twice with different values once floored | Floored in the merged file; the 4 conflicts are listed. |
| NIFTY / VIX 15m | 2 bars on 2024-01-17 13:15–13:30 differ from their 1m/5m | Isolated source glitch. |
| INDIA VIX 1m | **2,547 bars (Dec 2018–2019) with open/close outside high–low** | Source defect; the 5m/15m/60m inherit it (255/56/11 bars). Replace from broker. |
| INDIA VIX 1d vs intraday | Close differs almost every day (median ~41 bp) | VIX official close ≠ last print; expected class. |
| BANKNIFTY | 5m/15m/60m/1d match 1m 100 % | **By construction**: its manifest says they were resampled from the 1m file, and its 1d close is the last trade, not NSE's official close. Only the broker check verifies BANKNIFTY. |
| NIFTY FUT 5m/15m/60m vs 1m | 100 % once the 15:40 F&O session is used | Consistent. |
| NIFTY FUT 1m vs 1d | **July and 1–25 Aug 2026 intraday is a different contract** (~+250 pts, ~40× lower volume, session ends 15:29); from the Aug expiry on it matches the daily | The intraday files hold the next-month contract until expiry while the daily series is continuous near-month. Re-fetch intraday as a continuous series (FYERS `--continuous`). |
| Other sources (`_superseded_*`) | NIFTY 60m and VIX 1m opens differ on 12–45 % of bars (median 5–11 bp); highs/lows/closes mostly agree | A different vendor's first print; the broker decides which is right. |
| All intraday | 40–44 short days per series, 11 weekend sessions | Muhurat and special sessions (listed in each sheet) plus the partial last day, 2026-09-25 (ends 12:47). |
