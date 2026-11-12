# Source check before merging (2026-11-12)

Smit asked to merge `dataset/` (the TradingView files in the repo root), FYERS
history and the Drive bars (not the Drive tick-by-tick), and to check first
whether rows and columns conflict. Nothing has been merged yet. Tool:
`tools/merge_check.py`; full tables in `report.md`, every bar more than 10 bp
apart in `conflicts.csv`.

## Sources checked

| source | where | covers |
|---|---|---|
| dataset | `dataset/` (TradingView) | NIFTY, BANKNIFTY, VIX spot 1m 2015 to 2026-09-25; NIFTY fut 1m Jul-Sep 2026, fut 1d long |
| drive-ticks | Drive bars (`S:\altair\data\drive_ticks`) | NIFTY spot 2020-2023, NIFTY and BANKNIFTY front-month futures Nov 2019 to Dec 2023 |
| fyers | `altair-data/candles` | 2026-11-04 to 2026-11-11 only |

The full FYERS history (1m from 2018/2019) is only on the EC2 server, which is
offline. It is checked when the server is reachable.

## What was found

1. **Time stamps agree.** Every overlap lines up at shift 0 (bar start), so no
   source is a bar off.
2. **NIFTY spot intraday, dataset vs Drive (277k 1m bars): 91% within 1 bp,
   median 0.3 bp.** 207 1m bars are more than 10 bp apart, mostly in the March
   2020 crash days (2020-03-12 to 03-30) and 2022-03-07.
3. **Both sources carry bad spikes.** 2022-03-07 10:07: Drive opens at 16196.2
   and the dataset's high is 16174.5, while that day's official high is 15944.6.
   2020-03-23 10:42: dataset open 7903 vs Drive 8224.3. A merge needs a spike
   filter (intraday high/low outside the day's official range).
4. **Daily bars differ by definition, not by error.** Dataset 1d open/close are
   the exchange's official open and close; Drive 1d is first and last tick.
   Median 8 bp, all in open and close (high and low agree). Daily bars should
   come from one official source, never rebuilt from ticks.
5. **Futures daily: dataset continuous vs Drive front-month differ at rolls**
   (close median 19 bp, e.g. expiry 2019-12-26; 2021-11-01 Drive volume is 52k vs
   9.2M, so Drive used a thin contract that day). Futures need an explicit roll
   rule per source and must not be mixed across sources within a contract.
6. **Within one source:** dataset VIX has 2,547 1m bars in 2018-2019 where
   open/close sit outside high/low (small, source quirk). Dataset has bars at
   18:30-19:30 once a year: Muhurat trading, real. FYERS and dataset futures
   have 15:30-15:39 bars (post-close), real.
7. **No overlap yet with FYERS:** dataset ends 2026-09-25 and the FYERS
   candles here start 2026-11-04.

## Update: FYERS history checked (same day)

The server sent the full FYERS history (altair-data e565af8). `report.md` and
`conflicts.csv.gz` (in the private altair-data repo, merged_check/, because it holds FYERS prices) now cover all three sources.

8. **FYERS vs dataset, spot NIFTY and BANKNIFTY 1m (808k bars each): median
   0.05 bp and 0.01 bp, under 0.03% of bars more than 10 bp apart.** Daily
   NIFTY bars agree exactly. They are the same prices.
9. **FYERS vs Drive futures 1m: median 0.02-0.04 bp, but ~10% of bars are
   10-100 bp apart. Nearly all of them fall on ~100 whole days around expiry**,
   when Drive's highest-volume contract is already the next month and FYERS'
   continuous series is still on the near month. It's a contract choice, not a
   bad price. The merge picks one source per day, so it never mixes contracts.
10. **dataset's NIFTY futures intraday (Jul-Aug 2026) is a different contract
    from FYERS: ~56 bp apart on every bar, with a tenth of the volume.** FYERS
    wins there.
11. **India VIX: the dataset rounds to 0.05 and FYERS to 0.0025**, so most
    bars sit 1-10 bp apart. VIX is exempt from the spike rule, because its real
    1-minute wicks are often more than 50 bp.
12. **FYERS gaps:** 2026-11-10 is missing from every FYERS series.
