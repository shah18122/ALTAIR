# `client/` — retired

This is the TypeScript SPA built in Phase 11. **It is not built, not shipped,
and not part of any card manifest.** Smit chose a Qt UI on 2026-09-03; the
frontend now lives in [`desktop/`](../desktop).

It is kept because the *findings* cost more than the code did, and several of
them are load-bearing for `desktop/`. Deleting the directory would delete the
only place they are written down with a test attached.

Do not add it to a build. Do not `npm install` it. If you are reading this
because you found `.ts` files and wondered whether they run — they do
(`npm run check` still passes, 79/79) — but nothing depends on them.

---

## What survived into `desktop/`

These are language-independent. Every one of them is a real hazard in a Qt
grid too, and several are *worse* there:

| Finding | Where it was | Why it still bites in Qt |
|---|---|---|
| **A patch is addressed by row IDENTITY, never by index** | P11-03 | **Worse in Qt.** `QAbstractItemModel` is index-addressed by design, so the temptation is built into the framework. Use `QModelIndex::internalId()` for a stable `RowId`. Measured: 511 of 512 patches landed on the wrong instrument after one re-sort. |
| **Select-all means the rows you can SEE** | P11-08 | `QSortFilterProxyModel` makes it easy to select the *source* model by accident. Measured: 4,956 extra rows, Rs 10.26 crore of notional never looked at. |
| **A selection is a set of ids, not an index range** | P11-08 | `QItemSelectionModel` stores `QModelIndex`, which is exactly the fragile thing. Measured: an index range retained **0 of 100** rows after one re-sort. |
| **Absence is not zero** | P11-03/04/05 | A blank P&L cell rendered as `0.00` is a claim the position is flat. Sort absent LAST in both directions, not into the middle with the genuinely flat rows. |
| **Extensive vs intensive aggregation** | P11-05 | You cannot sum an implied volatility. Five positions at ~18% IV "total" 91.80% in a footer that looks exactly like the P&L total beside it. |
| **An unweighted average price is wrong** | P11-05 | 1 lot at ₹100 + 1000 lots at ₹200 → unweighted ₹150.00 vs weighted ₹199.90. **25% low** on the number a trader reads as "what I paid". |
| **Money sums in integer paise** | P11-05/09 | A float reduction drifted 12 paise past 2^53. In C++ this is `std::int64_t`, which is easier — but `double` in a `QVariant` is one careless cast away. |
| **CSV formula injection** | P11-09 | Excel does not care which language wrote the file. 4 of 8 realistic broker rejection strings would be **evaluated on open**. Quoting is *not* the fix — a quoted `=1+1` is still a formula. |
| **A spreadsheet retypes identifiers** | P11-09 | `1E5`, `007`, `SEP-24` silently become numbers or dates. 5 of 8 tested. |
| **float32 in the GPU path** | P11-10 | **Still applies.** `QOpenGLWidget` vertex attributes are float32. A nanosecond timestamp resolves to **137 seconds** there; 72,000 ticks collapse to 53 distinct x positions. Rebase in int64 *then* convert. |
| **Notional axes need rebasing too** | P11-10 | At ₹50 crore (5e10 paise) float32 resolves to ₹41; an equity curve moving in ₹10 steps draws 500 levels as 123. Price in paise is safe; notional is not. |
| **Bucket boundaries belong to one candle** | P11-10 | Half-open `[start, end)`. Closed intervals reported 61,200 against a true 60,000 — **+2.0%**, in the number a volume rule keys on. |
| **A forming candle is not a candle** | P11-10 | Rule 7 at the chart. Its close is the latest trade, not a close. |
| **Independently coalesced sides draw a crossed book** | P11-11 | **13,333 of 20,000 frames** rendered crossed — exactly the fraction on which price moved. The consistency unit is the frame, not the field. |
| **The ladder is a tick grid** | P11-11 | An empty level is a present, blank row. Tick size from the spec store, never a literal. |
| **The aggressor is INFERRED** | P11-11 | NSE does not publish it. The quote rule is right on 100% of what it classifies and declines 30%; the tick rule guesses and is wrong. Carry `unknownVolume`. |
| **Interpolate total variance, not IV** | P11-12 | Linear-in-sigma across a wide expiry gap produced **27 calendar arbitrages** between two well-behaved quoted slices. |
| **A smile is in log-moneyness** | P11-12 | On a strike axis the curve slides 50,000 paise as spot moves and the shape is buried under the translation. |
| **"Net gamma" has no sign until you say whose book** | P11-12 | Dealer and customer answers are exact negations. |
| **Replay look-ahead** | P11-13 | A *centred* moving average correlates **0.363** with the not-yet-happened move against **0.023** trailing — and "smooth this series" means a centred window in every plotting library, Qt included. |
| **A flag renders when it OCCURRED** | P11-13 | Not when it was delivered, or the replay disagrees with the audit trail about when the system knew. |
| **Cones widen as √h** | P11-13 | A linear cone is identical at h=1, where a reviewer checks, and 4× too wide at h=16. |
| **"Max loss" off a grid is not max loss** | P11-14 | Same short call, two grids: **−₹47,185.80** and **−₹3,97,185.80**. Decide boundedness from the legs' end slope. |
| **The low end is exact, not unbounded** | P11-14 | Spot cannot go below zero, so a short put's max loss is strike less premium — a real number. Calling it "unlimited" is the same error mirrored. |
| **Sign is never carried by colour alone** | P11-07 | ~1 man in 12. And green-up is not universal — red-up is China/Japan/Korea. |
| **Heatmaps normalise over the filtered set** | P11-07 | Window-normalised, one fixed value took 3 different colours while scrolling. |
| **Sparklines summarise, they do not sample** | P11-07 | Decimation drew a flat line through a moving market at **11 of 149 widths** — so it works until a column resize. |
| **No single key is destructive** | P11-08 | The kill switch needs two modifiers, is nowhere near the arrow keys, and still only produces a *confirmation request*. |
| **Rule 10's five stamps are required** | P11-14 | A blank column reads as "nothing happened there" when it means "this cannot be re-run". |

## What was JavaScript-specific and dies here

None of these exist in C++, and carrying them forward would be cargo cult:

- **The 2^53 timestamp hazard** (P11-01/02a) — a JS `number` is a double, so
  nanosecond time needed `BigInt`. C++ has `std::int64_t` natively. *The wire
  protocol itself survives* for the remote client; only the TS decoder goes.
- **`1000n === 1000` is false** (P11-04) — bigint/number strict equality. No
  analogue.
- **`new Function` / `eval`** (P11-06) — the derived-column expression parser
  is still needed and still must not be a script engine, but the specific
  remote-code-execution framing was about JS.
- **JS bitwise operators are 32-bit signed** (P11-01b) — `b[3] << 24` going
  negative. Not a thing in C++.
- **The 16.7M px scroll-height cap** (P11-03) — a browser limit. `QTableView`
  virtualises differently and this needs **re-measuring**, not assuming.

## Running it, if you ever want to

```powershell
cd client
npm install
npm run check      # tsc --noEmit + node --test  →  79/79
```

Zero runtime dependencies; TypeScript and `@types/node` are the only
devDependencies and neither ships.
