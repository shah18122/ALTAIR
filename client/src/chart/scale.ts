// client/src/chart/scale.ts -- getting coordinates onto a GPU without
// destroying them.
//
// P11-10.
//
// A NANOSECOND TIMESTAMP IN A float32 VERTEX ATTRIBUTE IS NOT A TIMESTAMP.
//
// This is the card, and it is P11-01's problem again one layer further down.
// There the hazard was a JavaScript number: 53 bits of mantissa, 256 ns of
// resolution at today's epoch. A WebGL vertex attribute is float32 -- TWENTY-
// FOUR bits of mantissa -- and at 1.79e18 the spacing between representable
// values is about 1.4e11 nanoseconds.
//
// That is a hundred and forty seconds. Every tick in a two-minute window
// collapses onto one x-coordinate, and a day of trading becomes roughly six
// hundred distinct positions. The chart does not error; it draws a staircase,
// and the staircase looks like low-frequency structure in the market.
//
// The fix is not a bigger float -- WebGL1 has no float64 attribute and WebGL2's
// support is not something to rest a chart on. The fix is to REBASE: pick an
// origin inside the visible domain, subtract it in double precision on the
// CPU, and send the GPU a small offset. A two-hour window is 7.2e12 ns, and
// float32 at that magnitude has a spacing of about 500,000 ns -- half a
// millisecond, finer than any pixel at any zoom this chart offers.
//
// So `Domain` carries its origin and `toFloat32Offsets` does the subtraction
// before anything crosses the boundary. `float32Resolution` exists so the loss
// can be computed for a given window rather than assumed away, and the test
// prints both.
//
// PRICE IS FINE. NOTIONAL IS NOT. THIS ONE WAS MEASURED THE OTHER WAY ROUND.
//
// The tempting second claim is that a cheap option charted on an index-scaled
// axis becomes a staircase. It does not, and the numbers say why: NIFTY at
// 24,000 is 2,400,000 paise, where float32 resolves to 0.25 paise against a
// 5-paise tick size -- twenty times the headroom needed. The first draft
// asserted the staircase; the test found 40 of 40 distinct prices surviving on
// either axis, so the assertion came out.
//
// Where it IS exposed is the notional and equity-curve axes. A Rs 50 crore
// book is 5e10 paise, and float32 there resolves to 4,096 paise -- Rs 41. An
// equity curve moving in Rs 10 steps, which on a book that size is ordinary
// intraday granularity, falls inside one float32 step: 500 distinct levels
// draw as 123.
//
// So the axis is rebased PER SERIES, and the reason is not "prices are big"
// but that the headroom on a price axis is only about twenty and the headroom
// on a notional axis is negative. Rebasing costs a subtraction and removes the
// need to know which axis you are on.

/** IEEE-754 binary32: 1 sign, 8 exponent, 23 stored mantissa bits. */
export const FLOAT32_MANTISSA_BITS = 24;

/** Spacing between representable float32 values at magnitude `v`. */
export function float32Resolution(v: number): number {
  const a = Math.abs(v);
  if (a === 0) return 0;
  const exponent = Math.floor(Math.log2(a));
  return 2 ** (exponent - (FLOAT32_MANTISSA_BITS - 1));
}

/** What a value becomes after a round trip through a float32 attribute. */
export function throughFloat32(v: number): number {
  return Math.fround(v);
}

/**
 * A visible range, with the origin that gets subtracted before the GPU.
 *
 * `origin` is a bigint because the x domain is nanoseconds and the whole point
 * is that it does not fit anywhere smaller. The SPAN fits everywhere.
 */
export interface Domain {
  readonly originNs: bigint;
  readonly spanNs: bigint;
}

export function domainFrom(startNs: bigint, endNs: bigint): Domain | null {
  if (endNs <= startNs) return null;
  return { originNs: startNs, spanNs: endNs - startNs };
}

/**
 * Convert timestamps to float32-safe offsets from the domain origin.
 *
 * The subtraction happens in bigint, the result is a small number of
 * nanoseconds, and only then does it become a float. Doing it the other way
 * round -- converting to float and subtracting there -- loses the precision
 * before the subtraction can save it, which is the version that looks correct
 * in a diff.
 */
export function toFloat32Offsets(
  timestampsNs: readonly bigint[],
  domain: Domain,
): Float32Array {
  const out = new Float32Array(timestampsNs.length);
  for (let i = 0; i < timestampsNs.length; i++) {
    const t = timestampsNs[i];
    if (t === undefined) continue;
    out[i] = Number(t - domain.originNs);
  }
  return out;
}

/** The naive version: straight to float32, no rebasing. Present only so the
 *  test can measure what it destroys. */
export function naiveFloat32(timestampsNs: readonly bigint[]): Float32Array {
  const out = new Float32Array(timestampsNs.length);
  for (let i = 0; i < timestampsNs.length; i++) {
    const t = timestampsNs[i];
    if (t === undefined) continue;
    out[i] = Number(t);
  }
  return out;
}

/** How many of these values remain distinct after a float32 round trip. The
 *  number that decides whether a chart is drawing data or a staircase. */
export function distinctAfterFloat32(values: Float32Array): number {
  return new Set(Array.from(values)).size;
}

// ---------------------------------------------------------------------------
// Price axis
// ---------------------------------------------------------------------------

export interface PriceAxis {
  /** Subtracted before the GPU, in paise. */
  readonly originPaise: bigint;
  readonly spanPaise: bigint;
}

/**
 * Build a price axis from a series' OWN range, not the chart's.
 *
 * A cheap option charted on an index-scaled axis inherits the index's
 * resolution. Rebasing per series is what keeps its chart from being a
 * staircase while the index chart beside it looks perfect.
 */
export function priceAxis(
  values: readonly bigint[],
  padFraction = 0.05,
): PriceAxis | null {
  if (values.length === 0) return null;
  let lo = values[0];
  let hi = values[0];
  if (lo === undefined || hi === undefined) return null;
  for (const v of values) {
    if (v < lo) lo = v;
    if (v > hi) hi = v;
  }
  const raw = hi - lo;
  // A flat series still needs a non-zero span, or the shader divides by zero.
  const span = raw > 0n ? raw : 1n;
  const pad = (span * BigInt(Math.round(padFraction * 1000))) / 1000n;
  return { originPaise: lo - pad, spanPaise: span + 2n * pad };
}

export function toFloat32Prices(
  values: readonly bigint[],
  axis: PriceAxis,
): Float32Array {
  const out = new Float32Array(values.length);
  for (let i = 0; i < values.length; i++) {
    const v = values[i];
    if (v === undefined) continue;
    out[i] = Number(v - axis.originPaise);
  }
  return out;
}
