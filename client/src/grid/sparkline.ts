// client/src/grid/sparkline.ts -- drawing 300 ticks in 50 pixels.
//
// P11-07.
//
// A SPARKLINE IS A SAMPLING PROBLEM, AND DECIMATION ALIASES.
//
// This is the card, and it is the same Nyquist argument the whole engine is
// built on. CLAUDE.md: "Order-book imbalance decays in 10-200 ms. The
// predecessor polled at 500 ms and could not, even in principle, see it."
// A sparkline that keeps every Nth point is that same mistake at the width of
// a table cell, and it is drawn next to the number it is supposed to explain.
//
// Decimation -- take points 0, 6, 12, 18 -- is sampling the series at one
// sixth of its rate. Anything oscillating faster than that folds down to a
// lower apparent frequency, and at some strides it folds all the way to ZERO
// and the sparkline draws a straight line through a series that never stopped
// moving.
//
// Measured on 300 ticks alternating up and down by 50 paise -- an ordinary
// bid-ask bounce, which is exactly the signal two-scale realised variance
// exists to see past.
//
//     panel width   stride   decimation draws   envelope draws
//        50 px         6          0 paise           50 paise
//        60 px         5         50 paise           50 paise
//
// AND THE WIDTH IS WHY THIS SURVIVES REVIEW. An odd stride walks both phases
// of a period-2 signal and looks perfectly correct; an even stride lands on
// the same phase every time and draws a flat line. Across widths 2..150 px
// the decimated sparkline collapses completely at 11 of them -- precisely the
// widths that divide 300 with an even quotient.
//
// So the bug is not reproducible by resizing a panel, which is the worst way
// for a bug to behave: it works, until a column resize or a different tick
// count silently turns a live series into a straight line.
//
// The fix is not a better sampling rate -- there isn't one, the pixel budget
// is fixed. The fix is to stop sampling and start SUMMARISING: each pixel
// column takes the min and the max of the ticks that fall in it, and the
// sparkline draws that envelope. Nothing is dropped, because nothing is
// sampled; every tick is inside some column and contributes to its extent.
//
// That is the same move as two-scale realised variance and as `oppty_log`'s
// exact aggregates plus a reservoir: when you cannot keep everything, keep a
// summary that is exact about the thing you care about, rather than a subset
// that is exact about a tenth of it.
//
// AN ENVELOPE IS NOT A LINE, AND THE RENDERER MUST KNOW. `Column` carries min
// and max separately. Drawing only `close` and calling it an envelope would
// reintroduce the decimation with extra steps.

export interface Column {
  /** Lowest value among the ticks landing in this pixel column. */
  readonly lo: number;
  /** Highest. Equal to `lo` when only one tick landed here. */
  readonly hi: number;
  /** First and last in time, for a renderer that also wants a direction. */
  readonly open: number;
  readonly close: number;
  readonly ticks: number;
}

export interface Sparkline {
  readonly columns: readonly Column[];
  /** Extremes across the whole series, for scaling the vertical axis. */
  readonly lo: number;
  readonly hi: number;
  readonly ticks: number;
}

/**
 * Summarise a series into `widthPx` columns.
 *
 * Every tick lands in exactly one column and widens that column's extent.
 * Nothing is skipped, so nothing can alias.
 */
export function envelope(
  values: readonly number[],
  widthPx: number,
): Sparkline | null {
  if (values.length === 0 || widthPx <= 0) return null;
  const n = values.length;
  const cols = Math.min(widthPx, n);

  const columns: Column[] = [];
  let lo = Number.POSITIVE_INFINITY;
  let hi = Number.NEGATIVE_INFINITY;

  for (let c = 0; c < cols; c++) {
    // Half-open [start, end) so no tick is counted twice and none is missed.
    const start = Math.floor((c * n) / cols);
    const end = Math.max(start + 1, Math.floor(((c + 1) * n) / cols));
    let cl = Number.POSITIVE_INFINITY;
    let ch = Number.NEGATIVE_INFINITY;
    for (let i = start; i < end && i < n; i++) {
      const v = values[i];
      if (v === undefined) continue;
      if (v < cl) cl = v;
      if (v > ch) ch = v;
    }
    if (cl > ch) continue;
    const open = values[start] ?? cl;
    const close = values[Math.min(end, n) - 1] ?? ch;
    columns.push({ lo: cl, hi: ch, open, close, ticks: end - start });
    if (cl < lo) lo = cl;
    if (ch > hi) hi = ch;
  }
  if (columns.length === 0) return null;
  return { columns, lo, hi, ticks: n };
}

/**
 * What a decimating sparkline draws. Present ONLY so the test can measure what
 * it loses; nothing in the render path calls it.
 *
 * This is the implementation everyone writes first, and it is a sampler.
 */
export function decimate(
  values: readonly number[],
  widthPx: number,
): number[] {
  if (values.length === 0 || widthPx <= 0) return [];
  const n = values.length;
  const cols = Math.min(widthPx, n);
  const out: number[] = [];
  for (let c = 0; c < cols; c++) {
    const v = values[Math.floor((c * n) / cols)];
    if (v !== undefined) out.push(v);
  }
  return out;
}

/** Peak-to-trough of whatever was drawn. The number the two approaches
 *  disagree about. */
export function drawnRange(sl: Sparkline): number {
  return sl.hi - sl.lo;
}

export function rangeOf(values: readonly number[]): number {
  if (values.length === 0) return 0;
  let lo = Number.POSITIVE_INFINITY;
  let hi = Number.NEGATIVE_INFINITY;
  for (const v of values) {
    if (v < lo) lo = v;
    if (v > hi) hi = v;
  }
  return hi - lo;
}
