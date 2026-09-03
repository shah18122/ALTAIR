// client/src/chart/surface.ts -- the smile, the surface, and the greeks
// profile.
//
// P11-12.
//
// INTERPOLATE TOTAL VARIANCE, NOT IMPLIED VOLATILITY.
//
// This is the card. A surface has slices at the expiries that trade -- weekly,
// monthly, quarterly -- and a chart needs values between them. The obvious
// interpolation is linear in sigma: halfway between a 30-day 18% and a 60-day
// 16% is a 45-day 17%.
//
// That is not merely imprecise. It MANUFACTURES CALENDAR ARBITRAGE. The
// no-arbitrage condition across expiries is on TOTAL IMPLIED VARIANCE,
// w(k, T) = sigma^2 * T, which must be non-decreasing in T at fixed
// log-moneyness -- the same Gatheral condition P5-07 checks. Linear-in-sigma
// interpolation does not preserve that, because squaring is convex: an
// interpolated sigma sits above the variance-linear one, but T is smaller, and
// the product can fall below the earlier slice's.
//
// Measured between two QUOTED slices that are themselves perfectly well
// behaved -- 18% at 30 days and 12% at 180, an ordinary post-event surface,
// whose total variances are 0.00266 and 0.00710 and so satisfy the condition.
// Interpolating in sigma across 201 intermediate tenors produces 27 points
// whose total variance falls below the running maximum. Interpolating in w
// produces 0, by construction.
//
// Whatever is wrong between those two slices was introduced by the
// interpolation, not inherited from the market -- and a trader reading a
// calendar spread off that stretch of the surface is reading an opportunity
// the chart invented.
//
// THE GAP HAS TO BE WIDE, AND THAT IS THE ORDINARY CASE. With intermediate
// knots at 60 and 90 days each segment is too short for the effect to appear,
// and the first draft of the test measured zero. The gap that matters is the
// one between the weekly and the six-month, where nothing trades in between
// and the chart has no choice but to interpolate a long way.
//
// A SMILE IS PLOTTED AGAINST MONEYNESS, NOT AGAINST STRIKE.
//
// The second thing, and it is P5-04 and P5-07 arriving at the chart. Plot IV
// against strike and the whole curve slides sideways every time spot moves --
// so this morning's smile and this afternoon's cannot be laid on top of one
// another, and the shape you are trying to see is buried under a translation.
//
// Log-moneyness k = ln(K/F) puts the at-the-money point at zero on every day
// and every underlying. `smilePoints` therefore requires the forward and emits
// k; there is no overload taking strikes alone, because the version that
// "just plots the strikes" is the one that looks right and compares nothing.
//
// AND "NET GAMMA" HAS NO SIGN UNTIL YOU SAY WHOSE BOOK.
//
// The third. Gamma exposure charts are everywhere and half of them are the
// negative of the other half, because a long call is long gamma for the holder
// and short gamma for the writer. "Net gamma at 24,000" is not a quantity
// until somebody says whether it is the dealer's or the customer's.
//
// `GammaProfile` carries `perspective` with no default, and the ordinal zero is
// `Unspecified`. A chart that cannot say whose book it is showing is a chart
// that will be read the wrong way round exactly once.

import { Extent } from '../grid/aggregate.ts';

/** Whose book the exposure belongs to. No default: the two answers are the
 *  negative of each other. */
export const Perspective = {
  Unspecified: 0,
  /** The market maker's book -- what a "dealer gamma" chart means. */
  Dealer: 1,
  /** This account's own book. */
  Own: 2,
} as const;
export type Perspective = (typeof Perspective)[keyof typeof Perspective];

export const SurfaceProblem = {
  /** Fewer than two slices, so there is nothing to interpolate between. */
  TooFewSlices: 'TooFewSlices',
  /** A tenor was zero or negative -- total variance is undefined there. */
  BadTenor: 'BadTenor',
  /** The forward was not supplied, so log-moneyness cannot be formed. */
  NoForward: 'NoForward',
  NonPositive: 'NonPositive',
  /** Asked for a tenor outside the quoted range. Refused rather than
   *  extrapolated: the surface says nothing there. */
  OutsideQuotedRange: 'OutsideQuotedRange',
  Unspecified: 'Unspecified',
} as const;
export type SurfaceProblem =
  (typeof SurfaceProblem)[keyof typeof SurfaceProblem];

export type SurfaceResult<T> =
  | { readonly ok: true; readonly value: T }
  | { readonly ok: false; readonly problem: SurfaceProblem };

/** One quoted expiry at one log-moneyness. */
export interface Slice {
  /** Years to expiry. */
  readonly tenorYears: number;
  /** Implied volatility as a decimal, not a percentage. 0.18, never 18. */
  readonly iv: number;
}

/** Total implied variance, w = sigma^2 * T. The quantity the no-arbitrage
 *  condition is actually about. */
export function totalVariance(s: Slice): number {
  return s.iv * s.iv * s.tenorYears;
}

/** sigma back out of w at a tenor. */
export function ivFromTotalVariance(w: number, tenorYears: number): number {
  if (tenorYears <= 0 || w < 0) return 0;
  return Math.sqrt(w / tenorYears);
}

/**
 * Interpolate the term structure at `tenorYears`, in TOTAL VARIANCE.
 *
 * Refuses to extrapolate. Beyond the quoted range the surface has no opinion,
 * and a chart that draws one there is drawing the interpolator rather than the
 * market.
 */
export function interpolateVariance(
  slices: readonly Slice[],
  tenorYears: number,
): SurfaceResult<number> {
  if (slices.length < 2) {
    return { ok: false, problem: SurfaceProblem.TooFewSlices };
  }
  if (tenorYears <= 0) return { ok: false, problem: SurfaceProblem.BadTenor };

  const sorted = [...slices].sort((a, b) => a.tenorYears - b.tenorYears);
  const first = sorted[0];
  const last = sorted[sorted.length - 1];
  if (first === undefined || last === undefined) {
    return { ok: false, problem: SurfaceProblem.TooFewSlices };
  }
  if (tenorYears < first.tenorYears || tenorYears > last.tenorYears) {
    return { ok: false, problem: SurfaceProblem.OutsideQuotedRange };
  }

  for (let i = 0; i + 1 < sorted.length; i++) {
    const a = sorted[i];
    const b = sorted[i + 1];
    if (a === undefined || b === undefined) continue;
    if (tenorYears < a.tenorYears || tenorYears > b.tenorYears) continue;
    const span = b.tenorYears - a.tenorYears;
    if (span <= 0) return { ok: false, problem: SurfaceProblem.BadTenor };
    const t = (tenorYears - a.tenorYears) / span;
    // Linear in w, then square-root back out. This is the whole card.
    const w = totalVariance(a) * (1 - t) + totalVariance(b) * t;
    return { ok: true, value: ivFromTotalVariance(w, tenorYears) };
  }
  return { ok: false, problem: SurfaceProblem.OutsideQuotedRange };
}

/** The version everyone writes first: linear in sigma. Present only so the
 *  test can count the arbitrages it invents. */
export function interpolateIvLinear(
  slices: readonly Slice[],
  tenorYears: number,
): SurfaceResult<number> {
  if (slices.length < 2) {
    return { ok: false, problem: SurfaceProblem.TooFewSlices };
  }
  const sorted = [...slices].sort((a, b) => a.tenorYears - b.tenorYears);
  for (let i = 0; i + 1 < sorted.length; i++) {
    const a = sorted[i];
    const b = sorted[i + 1];
    if (a === undefined || b === undefined) continue;
    if (tenorYears < a.tenorYears || tenorYears > b.tenorYears) continue;
    const span = b.tenorYears - a.tenorYears;
    if (span <= 0) return { ok: false, problem: SurfaceProblem.BadTenor };
    const t = (tenorYears - a.tenorYears) / span;
    return { ok: true, value: a.iv * (1 - t) + b.iv * t };
  }
  return { ok: false, problem: SurfaceProblem.OutsideQuotedRange };
}

/**
 * Count points where total variance FALLS as tenor rises.
 *
 * Each one is a calendar arbitrage: a longer-dated option carrying less total
 * uncertainty than a shorter-dated one. The same Gatheral condition P5-07
 * checks on the strike axis, checked here on the time axis.
 */
export function calendarViolations(
  curve: readonly { readonly tenorYears: number; readonly iv: number }[],
): number {
  let violations = 0;
  let previous: number | null = null;
  for (const p of [...curve].sort((a, b) => a.tenorYears - b.tenorYears)) {
    const w = p.iv * p.iv * p.tenorYears;
    if (previous !== null && w < previous) violations++;
    previous = previous === null ? w : Math.max(previous, w);
  }
  return violations;
}

// ---------------------------------------------------------------------------
// The smile
// ---------------------------------------------------------------------------

export interface SmilePoint {
  /** Log-moneyness, ln(K/F). Zero at the money, on every day and every
   *  underlying, which is what makes two smiles comparable. */
  readonly k: number;
  readonly strikePaise: bigint;
  readonly iv: number;
}

/**
 * Turn quoted strikes into a smile in log-moneyness.
 *
 * `forwardPaise` is required. There is no overload that plots against strike
 * alone, because that version looks right and compares nothing: the curve
 * slides sideways every time spot moves, so this morning's smile cannot be
 * laid over this afternoon's.
 */
export function smilePoints(
  strikes: readonly bigint[],
  ivs: readonly number[],
  forwardPaise: bigint,
): SurfaceResult<SmilePoint[]> {
  if (forwardPaise <= 0n) return { ok: false, problem: SurfaceProblem.NoForward };
  if (strikes.length !== ivs.length || strikes.length === 0) {
    return { ok: false, problem: SurfaceProblem.TooFewSlices };
  }
  const f = Number(forwardPaise);
  const out: SmilePoint[] = [];
  for (let i = 0; i < strikes.length; i++) {
    const K = strikes[i];
    const iv = ivs[i];
    if (K === undefined || iv === undefined) continue;
    if (K <= 0n || !(iv > 0)) {
      return { ok: false, problem: SurfaceProblem.NonPositive };
    }
    out.push({ k: Math.log(Number(K) / f), strikePaise: K, iv });
  }
  return { ok: true, value: out };
}

// ---------------------------------------------------------------------------
// Greeks against strike
// ---------------------------------------------------------------------------

/** Which greek, and -- separately -- what a unit of it means. The two are not
 *  the same question, and P10-08 already paid for conflating them once. */
export const Greek = {
  Unspecified: 0,
  /** Extensive. Change in position value per unit change in the underlying. */
  Delta: 1,
  /** Extensive. Change in DELTA per unit change in the underlying. */
  Gamma: 2,
  /** Extensive. Per vol POINT (0.01 of sigma), matching
   *  analytics/greeks.hpp's `vega_per_vol_point`. */
  Vega: 3,
  /** Extensive. Per calendar day. */
  Theta: 4,
} as const;
export type Greek = (typeof Greek)[keyof typeof Greek];

/** Every greek here is extensive: it scales with position size and adds
 *  across strikes. Implied volatility, which sits beside them on the same
 *  chart, is not -- and P11-05 refuses to sum it. */
export function greekExtent(g: Greek): Extent {
  return g === Greek.Unspecified ? Extent.Unspecified : Extent.Extensive;
}

export interface StrikeExposure {
  readonly strikePaise: bigint;
  /** Signed, in the convention of `perspective`. */
  readonly value: number;
}

export interface GammaProfile {
  /** WHOSE book. No default -- the dealer's answer is the negative of the
   *  customer's, and a chart that cannot say which will be read the wrong way
   *  round exactly once. */
  readonly perspective: Perspective;
  readonly greek: Greek;
  readonly points: readonly StrikeExposure[];
  /** Sum across strikes. Meaningful because the greek is extensive. */
  readonly net: number;
  /** The strike where cumulative exposure crosses zero -- the "flip point"
   *  a gamma chart is usually read for. Null when it never crosses. */
  readonly flipStrikePaise: bigint | null;
}

export function gammaProfile(
  points: readonly StrikeExposure[],
  greek: Greek,
  perspective: Perspective,
): SurfaceResult<GammaProfile> {
  if (perspective === Perspective.Unspecified || greek === Greek.Unspecified) {
    return { ok: false, problem: SurfaceProblem.Unspecified };
  }
  const sorted = [...points].sort((a, b) =>
    a.strikePaise < b.strikePaise ? -1 : a.strikePaise > b.strikePaise ? 1 : 0,
  );
  let net = 0;
  let cumulative = 0;
  let flip: bigint | null = null;
  let previousSign = 0;
  for (const p of sorted) {
    net += p.value;
    cumulative += p.value;
    const sign = Math.sign(cumulative);
    if (previousSign !== 0 && sign !== 0 && sign !== previousSign && flip === null) {
      flip = p.strikePaise;
    }
    if (sign !== 0) previousSign = sign;
  }
  return {
    ok: true,
    value: { perspective, greek, points: sorted, net, flipStrikePaise: flip },
  };
}

/** The same book seen from the other side. Provided so the two charts are
 *  visibly one negation apart rather than two independent calculations that
 *  might drift. */
export function flipPerspective(p: GammaProfile): GammaProfile {
  return {
    perspective:
      p.perspective === Perspective.Dealer ? Perspective.Own : Perspective.Dealer,
    greek: p.greek,
    points: p.points.map((x) => ({ ...x, value: -x.value })),
    net: -p.net,
    flipStrikePaise: p.flipStrikePaise,
  };
}
