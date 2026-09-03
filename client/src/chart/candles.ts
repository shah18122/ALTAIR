// client/src/chart/candles.ts -- ticks into candles, without inventing volume
// and without seeing the future.
//
// P11-10.
//
// A BUCKET BOUNDARY BELONGS TO EXACTLY ONE CANDLE.
//
// This is the card, and it is checkable rather than arguable, which is why it
// is the one to lead with. Bucketing is three lines and the interval is easy
// to write closed at both ends -- `t >= start && t <= end`. Then a tick landing
// exactly on a boundary is counted in the candle before it AND the candle
// after it, and the chart's total volume exceeds the tape's.
//
// It is not a rare interleaving either. Exchange timestamps are quantised, and
// a minute boundary is exactly the kind of instant a batch of orders is timed
// to. Measured on 60,000 ticks with 1,200 sharing a boundary timestamp, the
// closed-interval version reports 61,200 against a true 60,000 -- a 2%
// overstatement, in the number a volume-confirmation rule keys on.
//
// So buckets are HALF-OPEN, [start, end), and `checkConservation` totals the
// candles against the tape. Sum of the parts equals the whole, in integers,
// exactly -- the same runtime invariant CLAUDE.md requires of the ledger,
// applied to the chart.
//
// A CANDLE THAT IS STILL FORMING IS NOT A CANDLE.
//
// The second thing, and it is rule 7 -- no look-ahead, ever -- arriving at the
// chart. The last bucket of a live session is incomplete: its close is not a
// close, it is the latest trade. Draw it identically to the others and every
// overlay computed from closes -- and every eye reading the chart -- treats a
// number that will still change as one that has settled.
//
// `Candle.complete` says which it is, and `closedCandles` returns only the
// settled ones. A moving average over `candles` and one over `closedCandles`
// are different series, and the one that repaints its own last point is not a
// series anyone should trade off.
//
// AND A GAP IS NOT A ZERO.
//
// A minute with no trades has no candle. Emitting one with open = high = low =
// close = 0 draws a spike to the axis; emitting one with the previous close
// draws a flat line that says trading happened. Neither is true, so a bucket
// with no ticks produces NO candle and the renderer leaves a gap -- P5-02
// again, at the last surface.

export interface Tick {
  readonly timeNs: bigint;
  /** Paise. */
  readonly pricePaise: bigint;
  /** Contracts or shares. An integer count, never a float. */
  readonly quantity: bigint;
}

export interface Candle {
  /** Inclusive start of the bucket. */
  readonly startNs: bigint;
  /** Exclusive end. Half-open, so a tick on the boundary belongs to the next
   *  candle and to no other. */
  readonly endNs: bigint;
  readonly open: bigint;
  readonly high: bigint;
  readonly low: bigint;
  readonly close: bigint;
  readonly volume: bigint;
  readonly ticks: number;
  /** False for the bucket the session is still inside. Its close is not a
   *  close; it is the latest trade. */
  readonly complete: boolean;
}

/**
 * Bucket ticks into candles.
 *
 * `nowNs` is the engine's current tick time -- read off the tape, never from a
 * wall clock (rule 7). It decides which bucket is still forming. Passing a
 * wall clock here is how a replay renders its last candle as complete when it
 * is not, or as incomplete forever when the replay has finished.
 */
export function candles(
  ticks: readonly Tick[],
  bucketNs: bigint,
  nowNs: bigint,
): Candle[] {
  if (bucketNs <= 0n || ticks.length === 0) return [];

  const out: Candle[] = [];
  let bucketStart: bigint | null = null;
  let open = 0n;
  let high = 0n;
  let low = 0n;
  let close = 0n;
  let volume = 0n;
  let count = 0;

  const flush = (): void => {
    if (bucketStart === null || count === 0) return;
    const end = bucketStart + bucketNs;
    out.push({
      startNs: bucketStart,
      endNs: end,
      open,
      high,
      low,
      close,
      volume,
      ticks: count,
      // The bucket containing `now` is still forming. Everything before it
      // has settled.
      complete: end <= nowNs,
    });
  };

  for (const t of ticks) {
    // Floor division toward negative infinity, so pre-epoch timestamps bucket
    // the same way as post-epoch ones rather than folding toward zero.
    const q = t.timeNs / bucketNs;
    const start =
      (t.timeNs % bucketNs !== 0n && t.timeNs < 0n ? q - 1n : q) * bucketNs;

    if (bucketStart === null || start !== bucketStart) {
      flush();
      bucketStart = start;
      open = t.pricePaise;
      high = t.pricePaise;
      low = t.pricePaise;
      volume = 0n;
      count = 0;
    }
    if (t.pricePaise > high) high = t.pricePaise;
    if (t.pricePaise < low) low = t.pricePaise;
    close = t.pricePaise;
    volume += t.quantity;
    count++;
  }
  flush();
  return out;
}

/** The buggy version: boundaries counted in both buckets. Present only so the
 *  test can price the mistake; nothing calls it. */
export function candlesClosedInterval(
  ticks: readonly Tick[],
  bucketNs: bigint,
): Map<bigint, bigint> {
  const volumeByBucket = new Map<bigint, bigint>();
  const add = (start: bigint, q: bigint): void => {
    volumeByBucket.set(start, (volumeByBucket.get(start) ?? 0n) + q);
  };
  for (const t of ticks) {
    const start = (t.timeNs / bucketNs) * bucketNs;
    add(start, t.quantity);
    // The off-by-one: a tick exactly on a boundary is ALSO the closing tick of
    // the previous bucket, under `t >= start && t <= end`.
    if (t.timeNs % bucketNs === 0n) add(start - bucketNs, t.quantity);
  }
  return volumeByBucket;
}

export interface Conservation {
  readonly tapeVolume: bigint;
  readonly chartVolume: bigint;
  readonly tapeTicks: number;
  readonly chartTicks: number;
  readonly agrees: boolean;
}

/**
 * Total the candles against the tape.
 *
 * The chart's version of the engine's per-tick invariant. Meant to be run in
 * the debug build on every redraw: a chart whose volume does not match the
 * tape has a bucketing bug, and it is far cheaper to assert than to notice.
 */
export function checkConservation(
  ticks: readonly Tick[],
  built: readonly Candle[],
): Conservation {
  let tapeVolume = 0n;
  for (const t of ticks) tapeVolume += t.quantity;
  let chartVolume = 0n;
  let chartTicks = 0;
  for (const c of built) {
    chartVolume += c.volume;
    chartTicks += c.ticks;
  }
  return {
    tapeVolume,
    chartVolume,
    tapeTicks: ticks.length,
    chartTicks,
    agrees: tapeVolume === chartVolume && ticks.length === chartTicks,
  };
}

/** Only the candles that have settled. An overlay computed over these does
 *  not repaint its own last point. */
export function closedCandles(built: readonly Candle[]): Candle[] {
  return built.filter((c) => c.complete);
}

// ---------------------------------------------------------------------------
// Crosshair
// ---------------------------------------------------------------------------

export interface Crosshair {
  readonly index: number;
  readonly candle: Candle;
  /** True when the pointer is beyond the last candle, so the snap is to the
   *  nearest one rather than to something under the cursor. */
  readonly extrapolated: boolean;
}

/**
 * Snap the crosshair to a candle and report THAT candle's values.
 *
 * Not the interpolated value at the pixel: a readout that interpolates
 * between two candles shows a price at which nothing traded, to the paisa, in
 * a box the user is reading precisely because they want the real number.
 */
export function snapCrosshair(
  built: readonly Candle[],
  atNs: bigint,
): Crosshair | null {
  if (built.length === 0) return null;
  let best = 0;
  let bestDistance: bigint | null = null;
  for (let i = 0; i < built.length; i++) {
    const c = built[i];
    if (c === undefined) continue;
    const mid = c.startNs + (c.endNs - c.startNs) / 2n;
    const d = atNs > mid ? atNs - mid : mid - atNs;
    if (bestDistance === null || d < bestDistance) {
      bestDistance = d;
      best = i;
    }
  }
  const candle = built[best];
  if (candle === undefined) return null;
  const last = built[built.length - 1];
  return {
    index: best,
    candle,
    extrapolated: last !== undefined && atNs >= last.endNs,
  };
}
