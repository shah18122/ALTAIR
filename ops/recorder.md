# Recorder, training store and nightly retrain

ALTAIR records the whole FYERS session every trading day, whether or not the
desktop is open. Each night it stores the day, merges your `dataset/` and the
FYERS data into one snapshot cut at one date, retrains on that snapshot with
the leak check, and pushes the compact part to the private data repo.
Everything it does goes into one log.

```
08:50  ALTAIR Recorder   ops\record_session.ps1
         altair_price_service --live (unless the desktop's is already up) ─► 127.0.0.1:7421
         altair_feed_recorder ─► data\store\tapes\<day>\*.tape          (until 15:45)
20:00  ALTAIR Nightly    ops\nightly.ps1
         altair_store decode    ─► data\store\ticks\<day>\ (bars, IV/VIX/RV, option IVs), data\store\tape_bars\
         altair_iv_record       ─► data\live\iv\
         altair_fyers_history   ─► data\store\fyers\   (official candles, 7-day backfill)
         altair_store snapshot  ─► data\store\snapshots\<day>\   (one cutoff)
         altair_forecast_curriculum --leak-check ─► data\store\runs\<day>\  (LATEST.txt if clean)
         ops\push_data.ps1      ─► github.com/shah18122/altair-data (private)
always data\logs\altair-<day>.log   (30 days kept, credentials masked)
```

## One-time setup (Windows, `S:\altair`)

```powershell
cd S:\altair
.\build.bat net                                    # builds altair_feed_recorder and altair_store too
# the data repo, cloned NEXT TO altair (any folder works):
git clone https://github.com/shah18122/altair-data S:\altair-data
"repo = 'S:\altair-data'" | Set-Content config\data_push.local.toml   # git-ignored
# the two scheduled tasks:
powershell -ExecutionPolicy Bypass -File ops\install_tasks.ps1          # dry run: shows what it registers
powershell -ExecutionPolicy Bypass -File ops\install_tasks.ps1 -Go
# first night only: all the FYERS price history there is (spot 1m from 2018, futures from 2019)
powershell -ExecutionPolicy Bypass -File ops\fetch_fyers_history.ps1 -Go
powershell -ExecutionPolicy Bypass -File ops\nightly.ps1 -Go
```

**Every morning:** log in to FYERS once (Brokers page, or
`altair_fyers_login`). The token lasts one day and needs the browser. Until
you do, the log says `waiting for today's FYERS login` every 30 minutes, and
the recorder starts by itself as soon as the feed comes up.

Check the tasks: `Get-ScheduledTask 'ALTAIR*' | Get-ScheduledTaskInfo`.
Remove them: `ops\install_tasks.ps1 -Remove -Go`.

The tasks run while you are logged on to Windows. With `-WhetherLoggedOn` they
also run when you're logged off, but then the push can't use your git login.
The PC must be on, or asleep with wake timers allowed. A task that missed its
time runs as soon as the PC is back.

## What is recorded

Everything `altair_price_service` streams, byte for byte. The recorder starts
it with a wider universe than the desktop's default:

| | |
|---|---|
| Indices | NIFTY 50, NIFTY BANK, INDIA VIX: LTP |
| Futures | NIFTY and BANKNIFTY near and next: LTP, volume, OI, bid/ask, 5-level book |
| Options | the nearest **3 expiries** of each index (NIFTY: the weeklies and the monthly; BANKNIFTY: monthlies), ATM ±40 strikes, CE and PE: LTP, volume, **OI**, bid/ask with sizes, OHLC, ATP, total buy/sell. 5-level book within ATM ±10 |
| Stocks | the NIFTY 50 and their futures, and the BSE twins: the same fields |
| 50-level book | FYERS TBT: as many instruments as your plan allows (Standard: 1, the near future unless the desktop has a scrip on screen; Prime: 5 per connection, `-Depth50 5`) |

The 50-level book is limited by FYERS, not by ALTAIR: a Standard plan gives
**one** 50-level symbol at a time, so every other instrument gets 5 levels.
Options: `record_session.ps1 -Expiries 4 -Strikes 60 -DepthStrikes 20`.
FYERS allows 5,000 symbols a socket, and the default is well under that.

A SIM or replay feed is never recorded. If the desktop's own feed (1 expiry,
ATM ±20) was already running at 08:50, that feed is what gets recorded. When
the desktop opens after 08:50, it connects to the recorder's feed. To run SIM
while the recorder's feed is up, stop that feed first.

A live day is several GB of tapes. The tapes are kept: they're the only tick
history there is. The nightly log reports free disk, and warns below 20 GB.
The recorder stops by itself below 2 GB free.

## IV, RV and VIX, every minute

The nightly decode turns the day's tapes into two more files in
`data\store\ticks\<day>\`, each reading stamped at the END of its minute
(when it was known):

| File | One row | Columns |
|---|---|---|
| `vol_1m.csv` | index × expiry × minute | spot, India VIX (as streamed), parity forward, ATM strike, ATM IV, the expiry's model-free vol by the India VIX strip (`term_vol`), the chain's own 30-day VIX (`chain_vix30`), RV over the last 15 and 60 minutes and the session so far |
| `chain_iv_1m.csv` | option × minute | price (mid of the best bid/ask, else last trade), Black-76 IV against that expiry's forward, delta, vega per vol point, OI |

IV, `term_vol` and RV are annualised fractions (0.15 is 15 vol); the VIX
columns are index points. RV is `sqrt(mean r² × 375 × 252)` from the index's
1-minute closes. A minute without a fresh price for an option uses the
previous minute's; with neither it is left out. `chain_vix30` leaves out
expiries of 7 days or less, as NSE does, and is empty with fewer than two
other expiries. A minute with no option priced still gets a row with spot,
India VIX and RV. `data\live\iv\` (the nearest expiry's ATM IV, which the
models already read) is still written as before.

## One schema, one cutoff: how the sources merge

Every bar in a snapshot is `time,open,high,low,close,volume,oi,source`:
- `time` is the bar start in IST (`2026-10-26T09:15:00+05:30`; daily bars are a date).
- A stamp in another offset, or with seconds, is converted and floored first.
- An absent volume or OI is left empty. It's never written as 0.

By default the snapshot is FYERS data only: `fyers` (official candles), with
`tape` (bars built from the recording) filling gaps. `nightly.ps1 -WithDataset`
adds your `dataset\` files under FYERS, and `-WithBhavcopy` the NSE bhavcopy.
With more than one source they go lowest priority first: `dataset`, then `fyers`.
For the same bar time:

| Case | Result |
|---|---|
| One source has it | kept, tagged with that source |
| Both agree within 1 bp on O/H/L/C | your `dataset` row stays, so a re-fetch never churns history |
| They differ by more than 1 bp | FYERS wins, and both values go to `_conflicts.csv` |
| One bar is impossible (open/close outside high-low, price ≤ 0) | the possible one wins |
| `tape` vs anyone | tape only adds a missing bar, never replaces one |

A row is never stitched together from two sources. `_provenance.csv` lists
which source each stretch of bars came from. Your `dataset\` files are never
modified: the snapshot is a separate tree.

Every series, plus the bhavcopy and recorded IV beside them, stops at the end
of the same IST day. A snapshot gets an id, a hash of its inputs and the rule,
and it is never rebuilt over. The run records the id it trained on, so any run
can be traced back to the exact data it saw. The curriculum's `--leak-check`
then proves that no forecast saw its own future. A run is promoted
(`runs\LATEST.txt`) only when that check passes.

## The activity log

`data\logs\altair-<day>.log`, one line an event:

```
2026-10-26T09:15:02.118+05:30 [recorder] INFO recording the live feed
2026-10-26T20:41:10.552+05:30 [store] INFO snapshot 2026-10-26 id 3fa1…: 22 series, 4.3M rows, 12 conflict(s) …
2026-10-26T23:02:47.004+05:30 [trades] INFO paper straddle SELL NIFTY… x75 …; in: IV 14.2 > RV 11.9 +2; out: …
```

It covers the recorder, the feed starts, the nightly steps, the push, and
every round trip of the day with its reasons in and out (paper and live, from
the journals). Values that look like credentials (`access_token=`, `Bearer`,
`pin`, `secret`, …) are masked before a line is written. The C++ side is
`app/activity_log.hpp`; the PowerShell side is `ops/altair_log.ps1`.

## The data repo

`shah18122/altair-data` (private). Only compact daily files go there: 1-minute
bars of every instrument, the FYERS candles, IV, the reports and the log
(`README.md` there has the layout), now also `vol\` and `chain_iv\`. Tapes never go. The push script refuses
any folder without the `.altair-data-repo` marker, refuses a clone whose
origin is the public ALTAIR repo, and checks every file for credentials by
name and by content before it commits. It pushes with your own git login and
retries 5 times. A failed push stays committed locally, and the next night's
push carries it.

## By hand

```powershell
build\net\app\altair_feed_recorder.exe --go                    # record now, to 15:45
build\net\app\altair_store.exe decode --day 2026-10-26 --ticks # also every trade, quote and book as CSV
build\net\app\altair_store.exe snapshot --cutoff 2026-10-26 --source dataset=dataset --source fyers=data\store\fyers --fill tape=data\store\tape_bars
powershell -ExecutionPolicy Bypass -File ops\nightly.ps1 -Go -Day 2026-10-26 -Only daily   # quick retrain
```
