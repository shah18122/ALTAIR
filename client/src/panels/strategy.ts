// client/src/panels/strategy.ts -- the strategy builder's payoff, and the two
// numbers it must refuse to print.
//
// P11-14.
//
// "MAX LOSS" EVALUATED ON A GRID IS NOT MAX LOSS.
//
// This is the card, and it is the single most dangerous number in retail
// options software. A strategy builder draws the payoff over a range of spot
// -- typically a few strikes either side -- takes the minimum of what it drew,
// and prints it in a box labelled "Max Loss".
//
// For a short call that is a lie with a rupee sign on it. The payoff at the
// edge of the grid is not the worst case; it is the worst case IN THE WINDOW.
// Measured on a naked short 24,000 call at Rs 120, one NIFTY lot:
//
//     grid 22,000-26,000   "max loss Rs -47,185.80"
//     grid 22,000-40,000   "max loss Rs -3,97,185.80"
//
// Same position, same afternoon, two answers an order of magnitude apart. The
// number in the box is a property of the drawing range, not of the position --
// and a position was sized against it.
//
// So `payoffProfile` computes the SLOPE at the top end rather than the value.
// If the net delta as spot rises without limit is non-zero, the strategy is
// unbounded in that direction and the corresponding bound returns `null`. A
// null renders as "unlimited"; a number renders as a number. There is no path
// that returns a grid extremum dressed as a bound.
//
// AND THE LOW END IS NOT A GRID QUESTION EITHER -- IT IS EXACT.
//
// Spot cannot go below zero. So the worst case at the bottom is simply the
// payoff AT zero, computed exactly, and a short put's max loss is the strike
// less the premium -- a finite number. Calling that "unlimited" is the same
// mistake as calling a short call's grid extremum a bound, made in the other
// direction, and it is the one that makes a builder useless rather than
// dangerous. The test caught the first version doing exactly that.
//
// EVERY NUMBER HERE IS NET OF FULL COST, BEFORE IT EXISTS.
//
// CLAUDE.md rule 5. A break-even drawn gross of charges is wrong by the round
// trip, and for a four-leg iron condor on NIFTY the round trip is not small
// relative to the credit. The payoff is shifted by the total cost at
// construction, so there is no window in which an uncosted number is available
// to be displayed.
//
// AND THE CLIENT DOES NOT COMPUTE THE COST.
//
// The third thing, and it is an architecture decision rather than an
// arithmetic one. `risk/cost.hpp` (P3-09) owns the charge schedule: STT and
// which side it applies to, exchange charges, GST, stamp duty, the
// effective-dated tables in `config/charges.toml`, and the fact that STT rose
// on 2026-04-01.
//
// Reimplementing any of that here would create a SECOND source of truth for
// the number that decides whether a trade is worth taking -- and the two would
// drift, quietly, in the direction of whichever one was updated last. So
// `CostBreakdown` is a value the server sends and this module only ever
// subtracts it. `payoffProfile` requires one; there is no overload that
// computes costs, and none that defaults them to zero.

export const LegKind = {
  Unspecified: 0,
  Call: 1,
  Put: 2,
  Future: 3,
} as const;
export type LegKind = (typeof LegKind)[keyof typeof LegKind];

export interface Leg {
  readonly kind: LegKind;
  /** Positive for long, negative for short. In LOTS. */
  readonly lots: number;
  /** From the spec store, never a literal (rule 1). */
  readonly lotSize: number;
  /** Paise. Ignored for a future. */
  readonly strikePaise: bigint;
  /** Premium paid (long) or received (short), per unit, in paise. For a
   *  future this is the entry price. */
  readonly pricePaise: bigint;
}

/**
 * What the server computed. Not what this module computed, because this module
 * does not know the charge schedule and must not learn it.
 */
export interface CostBreakdown {
  readonly brokeragePaise: bigint;
  /** Sell side only for options -- on PREMIUM, not notional. Which side it
   *  applies to is exactly the thing gate 7 exists to catch, and exactly the
   *  thing this module is not allowed to decide. */
  readonly sttPaise: bigint;
  readonly exchangePaise: bigint;
  readonly gstPaise: bigint;
  readonly stampDutyPaise: bigint;
  /** The effective-dated schedule version this was priced under, so a
   *  displayed cost can be traced to the table that produced it. */
  readonly scheduleVersion: string;
}

export function totalCostPaise(c: CostBreakdown): bigint {
  return (
    c.brokeragePaise +
    c.sttPaise +
    c.exchangePaise +
    c.gstPaise +
    c.stampDutyPaise
  );
}

export const StrategyProblem = {
  NoLegs: 'NoLegs',
  UnspecifiedLeg: 'UnspecifiedLeg',
  /** A lot size of zero -- the spec store did not answer. */
  NoLotSize: 'NoLotSize',
  BadRange: 'BadRange',
} as const;
export type StrategyProblem =
  (typeof StrategyProblem)[keyof typeof StrategyProblem];

/** Payoff of one leg at expiry, in paise, per unit of the underlying. */
function legPayoffPerUnit(leg: Leg, spotPaise: bigint): bigint {
  switch (leg.kind) {
    case LegKind.Call: {
      const intrinsic =
        spotPaise > leg.strikePaise ? spotPaise - leg.strikePaise : 0n;
      return intrinsic - leg.pricePaise;
    }
    case LegKind.Put: {
      const intrinsic =
        leg.strikePaise > spotPaise ? leg.strikePaise - spotPaise : 0n;
      return intrinsic - leg.pricePaise;
    }
    case LegKind.Future:
      return spotPaise - leg.pricePaise;
    case LegKind.Unspecified:
    default:
      return 0n;
  }
}

/** Net payoff across every leg, in paise, before cost. */
export function payoffAt(legs: readonly Leg[], spotPaise: bigint): bigint {
  let total = 0n;
  for (const leg of legs) {
    const units = BigInt(leg.lots) * BigInt(leg.lotSize);
    total += units * legPayoffPerUnit(leg, spotPaise);
  }
  return total;
}

/**
 * Net delta as spot goes to +infinity and to zero, in units.
 *
 * This is what decides boundedness, and it is exact arithmetic on the legs
 * rather than a numerical slope taken off the grid -- a slope estimated from
 * two grid points inherits the grid's opinion about where the world ends.
 */
export interface EndSlopes {
  /** Units of underlying exposure as spot rises without limit. */
  readonly upside: bigint;
  /** As spot falls toward zero. */
  readonly downside: bigint;
}

export function endSlopes(legs: readonly Leg[]): EndSlopes {
  let up = 0n;
  let down = 0n;
  for (const leg of legs) {
    const units = BigInt(leg.lots) * BigInt(leg.lotSize);
    switch (leg.kind) {
      case LegKind.Call:
        up += units; // deep ITM call moves one for one
        break;
      case LegKind.Put:
        down -= units; // deep ITM put gains as spot falls
        break;
      case LegKind.Future:
        up += units;
        down += units;
        break;
      case LegKind.Unspecified:
      default:
        break;
    }
  }
  return { upside: up, downside: down };
}

export interface PayoffPoint {
  readonly spotPaise: bigint;
  /** NET of cost. There is no gross field, because a gross number in this
   *  struct is a gross number somebody will render. */
  readonly netPaise: bigint;
}

export interface PayoffProfile {
  readonly points: readonly PayoffPoint[];
  /** Null when the strategy is unbounded on the upside. Renders as
   *  "unlimited", never as the best point on the grid. */
  readonly maxProfitPaise: bigint | null;
  /** Null when unbounded on the downside. */
  readonly maxLossPaise: bigint | null;
  readonly unboundedUpside: boolean;
  readonly unboundedDownside: boolean;
  /** Spot levels where the net payoff crosses zero, on the grid. Net of cost,
   *  so these are the levels that actually matter. */
  readonly breakEvenPaise: readonly bigint[];
  readonly totalCostPaise: bigint;
  readonly scheduleVersion: string;
}

export type ProfileResult =
  | { readonly ok: true; readonly profile: PayoffProfile }
  | { readonly ok: false; readonly problem: StrategyProblem };

/**
 * The payoff, net of cost, with honest bounds.
 *
 * `cost` is required and comes from the server. `loPaise`/`hiPaise` bound the
 * DRAWING only -- they have no influence on `maxLoss`, which is decided by the
 * legs' end slopes.
 */
export function payoffProfile(
  legs: readonly Leg[],
  cost: CostBreakdown,
  loPaise: bigint,
  hiPaise: bigint,
  steps: number,
): ProfileResult {
  if (legs.length === 0) return { ok: false, problem: StrategyProblem.NoLegs };
  for (const l of legs) {
    if (l.kind === LegKind.Unspecified) {
      return { ok: false, problem: StrategyProblem.UnspecifiedLeg };
    }
    if (l.lotSize <= 0) return { ok: false, problem: StrategyProblem.NoLotSize };
  }
  if (hiPaise <= loPaise || steps < 2) {
    return { ok: false, problem: StrategyProblem.BadRange };
  }

  const total = totalCostPaise(cost);
  const span = hiPaise - loPaise;
  const points: PayoffPoint[] = [];
  for (let i = 0; i < steps; i++) {
    const spot = loPaise + (span * BigInt(i)) / BigInt(steps - 1);
    // Cost is subtracted HERE, once, at construction. There is no moment at
    // which an uncosted payoff exists to be rendered by mistake (rule 5).
    points.push({ spotPaise: spot, netPaise: payoffAt(legs, spot) - total });
  }

  const slopes = endSlopes(legs);
  // Unbounded means unbounded AS SPOT RISES, in either direction. There is no
  // unbounded low end: spot cannot go below zero.
  const unboundedUpside = slopes.upside > 0n;
  const unboundedDownside = slopes.upside < 0n;

  let best: bigint | null = null;
  let worst: bigint | null = null;
  for (const p of points) {
    if (best === null || p.netPaise > best) best = p.netPaise;
    if (worst === null || p.netPaise < worst) worst = p.netPaise;
  }

  // SPOT CANNOT GO BELOW ZERO, so the low end is not a grid question -- it is
  // exactly computable. A short put's worst case is the strike less the
  // premium, a finite number, and reporting it as "unlimited" is the same
  // mistake as reporting a short call's as a grid extremum, made in the other
  // direction. Evaluate at zero and fold it in.
  const atZero = payoffAt(legs, 0n) - total;
  if (best === null || atZero > best) best = atZero;
  if (worst === null || atZero < worst) worst = atZero;

  const breakEven: bigint[] = [];
  for (let i = 1; i < points.length; i++) {
    const a = points[i - 1];
    const b = points[i];
    if (a === undefined || b === undefined) continue;
    if ((a.netPaise < 0n && b.netPaise >= 0n) || (a.netPaise > 0n && b.netPaise <= 0n)) {
      breakEven.push(b.spotPaise);
    }
  }

  return {
    ok: true,
    profile: {
      points,
      // The whole point: a grid extremum is only reported when the strategy is
      // actually bounded in that direction.
      maxProfitPaise: unboundedUpside ? null : best,
      maxLossPaise: unboundedDownside ? null : worst,
      unboundedUpside,
      unboundedDownside,
      breakEvenPaise: breakEven,
      totalCostPaise: total,
      scheduleVersion: cost.scheduleVersion,
    },
  };
}

/** What the grid would have claimed, ignoring boundedness. Present only so
 *  the test can print the number a naive builder puts in the box. */
export function gridExtremes(
  legs: readonly Leg[],
  cost: CostBreakdown,
  loPaise: bigint,
  hiPaise: bigint,
  steps: number,
): { readonly best: bigint; readonly worst: bigint } {
  const total = totalCostPaise(cost);
  const span = hiPaise - loPaise;
  let best: bigint | null = null;
  let worst: bigint | null = null;
  for (let i = 0; i < steps; i++) {
    const spot = loPaise + (span * BigInt(i)) / BigInt(steps - 1);
    const v = payoffAt(legs, spot) - total;
    if (best === null || v > best) best = v;
    if (worst === null || v < worst) worst = v;
  }
  return { best: best ?? 0n, worst: worst ?? 0n };
}
