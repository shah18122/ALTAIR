# Source check before merge

Nothing merged. Bars aligned on IST bar start; daily bars by date.

## Each source on its own

| series | source | rows | first | last | dup times | impossible | missing px | outside 09:15-15:30 |
|---|---|---|---|---|---|---|---|---|
| fut/banknifty/15m | drive-ticks | 20543 | 2019-11-01 09:15 | 2023-12-28 15:15 | 0 | 0 | 0 | 0 |
| fut/banknifty/15m | fyers | 130 | 2026-11-04 09:15 | 2026-11-11 15:30 | 0 | 0 | 0 | 5 |
| fut/banknifty/1d | drive-ticks | 823 | 2019-11-01 00:00 | 2023-12-28 00:00 | 0 | 0 | 0 | 0 |
| fut/banknifty/1d | fyers | 5 | 2026-11-04 00:00 | 2026-11-11 00:00 | 0 | 0 | 0 | 0 |
| fut/banknifty/1m | drive-ticks | 299748 | 2019-11-01 09:15 | 2023-12-28 15:29 | 0 | 0 | 0 | 0 |
| fut/banknifty/1m | fyers | 1925 | 2026-11-04 09:15 | 2026-11-11 15:39 | 0 | 0 | 0 | 50 |
| fut/banknifty/5m | drive-ticks | 61266 | 2019-11-01 09:15 | 2023-12-28 15:25 | 0 | 0 | 0 | 0 |
| fut/banknifty/5m | fyers | 385 | 2026-11-04 09:15 | 2026-11-11 15:35 | 0 | 0 | 0 | 10 |
| fut/banknifty/60m | drive-ticks | 5761 | 2019-11-01 09:15 | 2023-12-28 15:15 | 0 | 0 | 0 | 0 |
| fut/banknifty/60m | fyers | 35 | 2026-11-04 09:15 | 2026-11-11 15:15 | 0 | 0 | 0 | 0 |
| fut/nifty/15m | dataset | 1277 | 2026-07-01 09:15 | 2026-09-08 15:30 | 0 | 0 | 0 | 27 |
| fut/nifty/15m | drive-ticks | 20567 | 2019-11-01 09:15 | 2023-12-28 15:15 | 0 | 0 | 0 | 0 |
| fut/nifty/15m | fyers | 130 | 2026-11-04 09:15 | 2026-11-11 15:30 | 0 | 0 | 0 | 5 |
| fut/nifty/1d | dataset | 2875 | 2015-02-02 00:00 | 2026-09-08 00:00 | 0 | 0 | 0 | 0 |
| fut/nifty/1d | drive-ticks | 823 | 2019-11-01 00:00 | 2023-12-28 00:00 | 0 | 0 | 0 | 0 |
| fut/nifty/1d | fyers | 5 | 2026-11-04 00:00 | 2026-11-11 00:00 | 0 | 0 | 0 | 0 |
| fut/nifty/1m | dataset | 19020 | 2026-07-01 09:15 | 2026-09-08 15:39 | 0 | 0 | 0 | 270 |
| fut/nifty/1m | drive-ticks | 302872 | 2019-11-01 09:15 | 2023-12-28 15:29 | 0 | 0 | 0 | 0 |
| fut/nifty/1m | fyers | 1925 | 2026-11-04 09:15 | 2026-11-11 15:39 | 0 | 0 | 0 | 50 |
| fut/nifty/5m | dataset | 3804 | 2026-07-01 09:15 | 2026-09-08 15:35 | 0 | 0 | 0 | 54 |
| fut/nifty/5m | drive-ticks | 61513 | 2019-11-01 09:15 | 2023-12-28 15:25 | 0 | 0 | 0 | 0 |
| fut/nifty/5m | fyers | 385 | 2026-11-04 09:15 | 2026-11-11 15:35 | 0 | 0 | 0 | 10 |
| fut/nifty/60m | dataset | 350 | 2026-07-01 09:15 | 2026-09-08 15:15 | 0 | 0 | 0 | 0 |
| fut/nifty/60m | drive-ticks | 5761 | 2019-11-01 09:15 | 2023-12-28 15:15 | 0 | 0 | 0 | 0 |
| fut/nifty/60m | fyers | 35 | 2026-11-04 09:15 | 2026-11-11 15:15 | 0 | 0 | 0 | 0 |
| spot/banknifty/15m | dataset | 72228 | 2015-01-09 09:15 | 2026-09-25 12:30 | 0 | 0 | 0 | 46 |
| spot/banknifty/15m | fyers | 125 | 2026-11-04 09:15 | 2026-11-11 15:15 | 0 | 0 | 0 | 0 |
| spot/banknifty/1d | dataset | 6633 | 2000-01-01 00:00 | 2026-09-24 00:00 | 0 | 0 | 0 | 0 |
| spot/banknifty/1d | fyers | 5 | 2026-11-04 00:00 | 2026-11-11 00:00 | 0 | 0 | 0 | 0 |
| spot/banknifty/1m | dataset | 1083243 | 2015-01-09 09:15 | 2026-09-25 12:47 | 0 | 0 | 0 | 687 |
| spot/banknifty/1m | fyers | 1875 | 2026-11-04 09:15 | 2026-11-11 15:29 | 0 | 0 | 0 | 0 |
| spot/banknifty/5m | dataset | 216665 | 2015-01-09 09:15 | 2026-09-25 12:40 | 0 | 0 | 0 | 138 |
| spot/banknifty/5m | fyers | 375 | 2026-11-04 09:15 | 2026-11-11 15:25 | 0 | 0 | 0 | 0 |
| spot/banknifty/60m | dataset | 20225 | 2015-01-09 09:15 | 2026-09-25 11:15 | 0 | 0 | 0 | 12 |
| spot/banknifty/60m | fyers | 35 | 2026-11-04 09:15 | 2026-11-11 15:15 | 0 | 0 | 0 | 0 |
| spot/indiavix/15m | dataset | 71854 | 2015-02-02 09:15 | 2026-09-25 12:30 | 0 | 56 | 0 | 45 |
| spot/indiavix/15m | fyers | 125 | 2026-11-04 09:15 | 2026-11-11 15:15 | 0 | 0 | 0 | 0 |
| spot/indiavix/1d | dataset | 2906 | 2015-01-01 00:00 | 2026-09-24 00:00 | 0 | 0 | 0 | 0 |
| spot/indiavix/1d | fyers | 5 | 2026-11-04 00:00 | 2026-11-11 00:00 | 0 | 0 | 0 | 0 |
| spot/indiavix/1m | dataset | 1077565 | 2015-02-02 09:15 | 2026-09-25 12:47 | 0 | 2547 | 0 | 572 |
| spot/indiavix/1m | fyers | 1875 | 2026-11-04 09:15 | 2026-11-11 15:29 | 0 | 0 | 0 | 0 |
| spot/indiavix/5m | dataset | 215528 | 2015-02-02 09:15 | 2026-09-25 12:40 | 0 | 255 | 0 | 116 |
| spot/indiavix/5m | fyers | 375 | 2026-11-04 09:15 | 2026-11-11 15:25 | 0 | 0 | 0 | 0 |
| spot/indiavix/60m | dataset | 20123 | 2015-02-02 09:15 | 2026-09-25 11:15 | 0 | 11 | 0 | 14 |
| spot/indiavix/60m | fyers | 35 | 2026-11-04 09:15 | 2026-11-11 15:15 | 0 | 0 | 0 | 0 |
| spot/nifty/15m | dataset | 71880 | 2015-02-02 09:15 | 2026-09-25 12:30 | 0 | 0 | 0 | 46 |
| spot/nifty/15m | drive-ticks | 18496 | 2020-01-01 09:15 | 2023-12-29 15:15 | 0 | 0 | 0 | 0 |
| spot/nifty/15m | fyers | 125 | 2026-11-04 09:15 | 2026-11-11 15:15 | 0 | 0 | 0 | 0 |
| spot/nifty/1d | dataset | 8773 | 1990-07-03 00:00 | 2026-09-24 00:00 | 0 | 0 | 0 | 0 |
| spot/nifty/1d | drive-ticks | 740 | 2020-01-01 00:00 | 2023-12-29 00:00 | 0 | 0 | 0 | 0 |
| spot/nifty/1d | fyers | 5 | 2026-11-04 00:00 | 2026-11-11 00:00 | 0 | 0 | 0 | 0 |
| spot/nifty/1m | dataset | 1083191 | 2015-01-09 09:15 | 2026-09-25 12:47 | 0 | 0 | 0 | 584 |
| spot/nifty/1m | drive-ticks | 277357 | 2020-01-01 09:15 | 2023-12-29 15:29 | 0 | 0 | 0 | 0 |
| spot/nifty/1m | fyers | 1875 | 2026-11-04 09:15 | 2026-11-11 15:29 | 0 | 0 | 0 | 0 |
| spot/nifty/5m | dataset | 215607 | 2015-02-02 09:15 | 2026-09-25 12:40 | 0 | 0 | 0 | 119 |
| spot/nifty/5m | drive-ticks | 55478 | 2020-01-01 09:15 | 2023-12-29 15:25 | 0 | 0 | 0 | 0 |
| spot/nifty/5m | fyers | 375 | 2026-11-04 09:15 | 2026-11-11 15:25 | 0 | 0 | 0 | 0 |
| spot/nifty/60m | dataset | 20130 | 2015-02-02 09:15 | 2026-09-25 11:15 | 0 | 0 | 0 | 14 |
| spot/nifty/60m | drive-ticks | 5180 | 2020-01-01 09:15 | 2023-12-29 15:15 | 0 | 0 | 0 | 0 |
| spot/nifty/60m | fyers | 35 | 2026-11-04 09:15 | 2026-11-11 15:15 | 0 | 0 | 0 | 0 |

## Where two sources overlap

Agreement = worst of the four OHLC gaps on each bar. Best shift != 0 means one source stamps bars a bar off.

| series | A vs B | overlap | only A | only B | exact | <=1bp | 1-10bp | 10-100bp | >100bp | median bp | p99 bp | close gap by shift (-1/0/+1) | volume A/B median |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| fut/nifty/1d | dataset vs drive-ticks | 823 | 2052 | 0 | 9 | 83 | 468 | 256 | 7 | 5.462 | 95.45 | 58.99 / 5.44 / 60.39 | 1.0002 |
| spot/nifty/15m | dataset vs drive-ticks | 18496 | 53384 | 0 | 2770 | 12976 | 2584 | 164 | 2 | 0.385 | 9.51 | 7.37 / 0.13 / 7.35 | - |
| spot/nifty/1d | dataset vs drive-ticks | 740 | 8033 | 0 | 0 | 13 | 440 | 285 | 2 | 8.04 | 46.44 | 57.27 / 5.3 / 57.89 | - |
| spot/nifty/1m | dataset vs drive-ticks | 277339 | 805852 | 18 | 60589 | 192190 | 24353 | 205 | 2 | 0.306 | 2.6 | 1.82 / 0.12 / 1.84 | - |
| spot/nifty/5m | dataset vs drive-ticks | 55478 | 160129 | 0 | 10445 | 38993 | 5865 | 173 | 2 | 0.333 | 4.53 | 4.17 / 0.12 / 4.16 | - |
| spot/nifty/60m | dataset vs drive-ticks | 5180 | 14950 | 0 | 622 | 3291 | 1103 | 163 | 1 | 0.465 | 16.95 | 14.2 / 0.14 / 14.24 | - |

1262 bars differ by more than 10 bp: conflicts.csv
