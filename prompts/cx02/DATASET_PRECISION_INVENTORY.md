# CX02-A3 — which stored rows C14-001 could have rounded

Read-only inventory. Nothing under `dataset/` was written, and no refetch was made.

## What this can and cannot say

`altair_kite_update` wrote prices with `%g` and `altair_kite_fetch` with an ostream at
default precision: both SIX significant digits. A price needs more than six only when it
is at least 10,000 with paise — NIFTY 24,123.45 became 24123.5, BANKNIFTY 51,234.65
became 51234.7. India VIX at 10.68 needs four, so it was never at risk from this.

So, per series, rows are counted three ways:

- **at risk** — a price at or above 10,000, where six digits cannot carry paise;
- **proven exact** — of those, a row carrying two decimals AND more than six significant
  digits. A lossy writer could not have produced it;
- **plausibly rounded** — of those, a row with at most one decimal. Consistent with
  rounding, and ALSO what an exact writer produces for a close of 24,123.50, or a feed
  that reported one decimal. It is an upper bound on the damage, not a count of it.

No row is repaired by re-running the updater: the 1 bp rule treats a 0.02 bp difference as
decimal places and keeps what is stored (`app/dataset_merge.hpp`). Recovery is a refetch,
which needs a Kite login and a separately confirmed replacement scope — a user decision,
not a side effect.

**Totals:** 4,208,576 rows in 1737 files · 2,467,150 at risk · 1,237,797 proven exact · 1,229,353 plausibly rounded

| series | files | rows | at risk | proven exact | plausibly rounded | affected range |
|---|---:|---:|---:|---:|---:|---|
| `fut/nifty/15m` | 3 | 1,277 | 1,277 | 0 | 1,277 | 2026-07-01 … 2026-09-08 |
| `fut/nifty/1d` | 1 | 2,875 | 2,180 | 0 | 2,180 | 2017-07-26 … 2026-09-08 |
| `fut/nifty/1m` | 3 | 19,020 | 19,020 | 0 | 19,020 | 2026-07-01 … 2026-09-08 |
| `fut/nifty/5m` | 3 | 3,804 | 3,804 | 0 | 3,804 | 2026-07-01 … 2026-09-08 |
| `fut/nifty/60m` | 3 | 350 | 350 | 0 | 350 | 2026-07-01 … 2026-09-08 |
| `spot/banknifty/15m` | 141 | 72,018 | 72,018 | 64,566 | 7,452 | 2015-01-09 … 2026-09-15 |
| `spot/banknifty/1d` | 2 | 6,625 | 3,865 | 3,489 | 376 | 2007-12-12 … 2026-09-11 |
| `spot/banknifty/1m` | 141 | 1,080,101 | 1,080,101 | 952,641 | 127,460 | 2015-01-09 … 2026-09-15 |
| `spot/banknifty/5m` | 141 | 216,037 | 216,037 | 192,847 | 23,190 | 2015-01-09 … 2026-09-15 |
| `spot/banknifty/60m` | 141 | 20,167 | 20,167 | 18,128 | 2,039 | 2015-01-12 … 2026-09-15 |
| `spot/indiavix/15m` | 140 | 71,644 | 0 | 0 | 0 | — |
| `spot/indiavix/1d` | 1 | 2,898 | 0 | 0 | 0 | — |
| `spot/indiavix/1m` | 140 | 1,074,423 | 0 | 0 | 0 | — |
| `spot/indiavix/5m` | 140 | 214,900 | 0 | 0 | 0 | — |
| `spot/indiavix/60m` | 140 | 20,065 | 0 | 0 | 0 | — |
| `spot/indiavix/_superseded_othersource` | 9 | 2,499 | 0 | 0 | 0 | — |
| `spot/nifty/15m` | 140 | 71,670 | 54,074 | 0 | 54,074 | 2017-07-25 … 2026-09-15 |
| `spot/nifty/1d` | 1 | 8,765 | 2,182 | 2,079 | 103 | 2017-09-18 … 2026-09-11 |
| `spot/nifty/1m` | 141 | 1,080,049 | 810,434 | 0 | 810,434 | 2017-07-25 … 2026-09-15 |
| `spot/nifty/5m` | 140 | 214,979 | 162,146 | 0 | 162,146 | 2017-07-25 … 2026-09-15 |
| `spot/nifty/60m` | 140 | 20,072 | 15,157 | 0 | 15,157 | 2017-07-25 … 2026-09-15 |
| `spot/nifty/_superseded_1m_perday` | 4 | 1,207 | 1,207 | 1,124 | 83 | 2026-08-26 … 2026-08-31 |
| `spot/nifty/_superseded_othersource` | 22 | 3,131 | 3,131 | 2,923 | 208 | 2024-11-12 … 2026-08-31 |

Rows or fields this scan could not read: 0.

## What the numbers say, and what would settle it

**BANKNIFTY spot looks undamaged, and the arithmetic says why.** At 51,234.65 a two-decimal
source produces a value whose hundredths digit is 0 about one time in ten, and such a value is
written with one decimal by an exact writer too. So roughly 10% "plausibly rounded" is the rate
CHANCE produces. BANKNIFTY 1m is 127,460 of 1,080,101 — 11.8%. There is no evidence of
systematic rounding there.

**NIFTY is the opposite, and it is not chance.** Every at-risk row in `spot/nifty/1m` (810,434),
`5m`, `15m`, `60m` and in all five `fut/nifty` series carries at most one decimal: NOT ONE row
proves itself exact. Under a two-decimal source that would be 0.1^810434. Two explanations fit:

1. those series were written through the six-significant-digit path (C14-001), or
2. the source reported NIFTY with one decimal, and nothing was ever lost.

This scan cannot tell them apart — both produce identical bytes — and `spot/nifty/1d`, which is
2,079 proven-exact rows against 103, shows the daily series did NOT come through the same path
as the intraday ones.

**What would settle it, as a user action.** After a Kite login, fetch one recent NIFTY minute
candle and look at the raw API body: if Kite returns 24123.45 and the stored row says 24123.5,
explanation 1 is confirmed for that series and a refetch is worth its cost. If Kite itself
returns 24123.5, nothing was lost and the series is as good as its source. Until then this is an
open question, not a defect count — and no refetch should be run on the strength of it.

**Nothing repairs itself.** Re-running `altair_kite_update` will not rewrite these rows: a
0.02 bp difference is "decimal places" under the 1 bp rule and the stored value stays. That is
deliberate (`app/dataset_merge.hpp`), and it is why recovery has to be an explicit, scoped
refetch rather than a side effect of the next update.

