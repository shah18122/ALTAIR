// client/src/grid/aggregate.ts -- what a column may be aggregated BY.
//
// P11-05.
//
// YOU CANNOT SUM AN IMPLIED VOLATILITY.
//
// This is the card, and it is CLAUDE.md's physics discipline -- "dimensional
// analysis -> the type system" -- arriving at the pivot menu. Every grid
// library offers Sum, Avg, Min, Max on every numeric column, and half of those
// offers are meaningless.
//
// The distinction is EXTENSIVE versus INTENSIVE. An extensive quantity scales
// with the size of the system and adds across its parts: P&L, quantity,
// notional, delta, gamma, vega, charges. An intensive quantity does not:
// implied volatility, a price, a ratio, a beta, a half-life. Two positions
// with 18% IV do not make a position with 36% IV, and the sum of the two
// strikes' prices is not a price of anything.
//
// A grid that offers Sum on an IV column produces a number with no referent,
// formatted to two decimal places, sitting in a total row that looks exactly
// like the P&L total next to it. Nothing is out of range, nothing throws, and
// the number is not even wrong -- it is not about anything.
//
// So a column declares its `Extent`, `allowedAggregations` returns what is
// meaningful for it, and `aggregate` REFUSES the rest rather than computing
// it. Min, Max and Count stay available on intensive columns because those are
// order statistics and a count, which remain meaningful; Sum and Mean do not,
// and a weighted mean is what an intensive column actually wants.
//
// AN UNWEIGHTED AVERAGE PRICE IS THE WRONG NUMBER.
//
// The second thing, and it is the one that will actually appear on screen. Ask
// a grid for the average price of a group and it takes the mean of the price
// column. That is right only if every row carries the same quantity.
//
// Measured on a two-row group -- 1 lot at Rs 100, then 1000 lots at Rs 200 --
// the unweighted mean is Rs 150.00 and the quantity-weighted mean is Rs 199.90.
// The unweighted number is 25% low, on the figure a trader reads as "what I
// paid", in a group that is not remotely contrived: adding to a winner is the
// ordinary case.
//
// So `weightedMean` exists, takes the weight column explicitly, and there is
// no default weight of 1 to fall back on.
//
// AND MONEY IS SUMMED IN BIGINT.
//
// The third, and it is short. Rule 3 says all money is integer paise. A sum
// reduced through `Number` is exact for each term and stops being exact for
// the total somewhere above 2^53 paise. More to the point, there is no reason
// to find out where: `bigint` addition is exact at every magnitude and the
// column already holds bigint. A mean is analytics and may be rounded, but it
// is rounded ONCE, at the end, from an exact numerator and denominator.

import type { Cell, RowId, RowStore } from './store.ts';
import { ColumnType } from './filter.ts';

/**
 * Does this quantity add across rows?
 *
 * The single most useful thing a column can declare, and the one no grid
 * library asks for.
 */
export const Extent = {
  Unspecified: 0,
  /** Scales with the size of the book and adds across its parts: P&L,
   *  quantity, notional, delta, gamma, vega, charges, turnover. */
  Extensive: 1,
  /** Does not add: implied volatility, price, ratio, beta, half-life,
   *  probability. Summing one produces a number with no referent. */
  Intensive: 2,
  /** A label. Nothing numeric applies at all. */
  Categorical: 3,
} as const;
export type Extent = (typeof Extent)[keyof typeof Extent];

export const Agg = {
  Unspecified: 0,
  Sum: 1,
  Mean: 2,
  /** The only mean that means anything for an intensive quantity. Requires a
   *  weight column; there is no implicit weight of 1. */
  WeightedMean: 3,
  Min: 4,
  Max: 5,
  /** Rows in the group. Always meaningful. */
  Count: 6,
  /** Rows in the group with a value in this column. Distinct from Count, and
   *  the difference is what a mean actually divided by. */
  CountPresent: 7,
} as const;
export type Agg = (typeof Agg)[keyof typeof Agg];

export interface AggColumnSpec {
  readonly key: string;
  readonly label: string;
  readonly type: ColumnType;
  readonly extent: Extent;
}

const ORDER_STATS: readonly Agg[] = [Agg.Min, Agg.Max, Agg.Count, Agg.CountPresent];

/**
 * What may meaningfully be computed for this column.
 *
 * Note what is missing from the intensive case: Sum and Mean. An unweighted
 * mean of an intensive quantity is not as wrong as a sum, but it is wrong in
 * the same direction and for the same reason -- it treats rows as
 * interchangeable when they are not.
 */
export function allowedAggregations(spec: AggColumnSpec): readonly Agg[] {
  switch (spec.extent) {
    case Extent.Extensive:
      return [Agg.Sum, Agg.Mean, Agg.WeightedMean, ...ORDER_STATS];
    case Extent.Intensive:
      return [Agg.WeightedMean, ...ORDER_STATS];
    case Extent.Categorical:
      return [Agg.Count, Agg.CountPresent];
    case Extent.Unspecified:
    default:
      return [];
  }
}

export const AggProblem = {
  /** Sum or Mean asked of an intensive quantity. */
  NotAdditive: 'NotAdditive',
  /** A weighted mean with no weight column, or a weight column that is not a
   *  count. */
  NoWeightColumn: 'NoWeightColumn',
  /** Every weight in the group was zero or absent, so the mean has no
   *  denominator. Reported, never quietly returned as zero. */
  NoWeight: 'NoWeight',
  /** No row in the group had a value in this column. Distinct from a total of
   *  zero, which is a claim. */
  Empty: 'Empty',
  UnknownColumn: 'UnknownColumn',
  MixedTypes: 'MixedTypes',
} as const;
export type AggProblem = (typeof AggProblem)[keyof typeof AggProblem];

/**
 * An aggregate, with its provenance attached.
 *
 * `numerator` and `denominator` are carried so a mean can be rendered at any
 * precision without a second rounding, and so `rows` and `present` can be
 * compared -- a mean over 4 of 100 rows is a different claim from a mean over
 * 100 of 100, and only one number on screen cannot say which it is.
 */
export interface AggValue {
  readonly agg: Agg;
  /** Exact, for money and counts. Null when the aggregate is a real. */
  readonly exact: bigint | null;
  /** For Mean and WeightedMean, the exact ratio before rounding. */
  readonly numerator: bigint | null;
  readonly denominator: bigint | null;
  /** Present when the column is Real and exactness does not apply. */
  readonly real: number | null;
  readonly rows: number;
  readonly present: number;
}

export type AggResult =
  | { readonly ok: true; readonly value: AggValue }
  | { readonly ok: false; readonly problem: AggProblem };

function asBigint(c: Cell): bigint | null {
  if (!c.present) return null;
  if (typeof c.value === 'bigint') return c.value;
  if (typeof c.value === 'number' && Number.isInteger(c.value)) {
    return BigInt(c.value);
  }
  return null;
}

function asNumber(c: Cell): number | null {
  if (!c.present) return null;
  if (typeof c.value === 'number') return c.value;
  if (typeof c.value === 'bigint') return Number(c.value);
  return null;
}

/** Round an exact ratio to whole units, half away from zero. The one place a
 *  money aggregate loses exactness, and it loses it once. */
export function roundRatio(numerator: bigint, denominator: bigint): bigint {
  if (denominator === 0n) return 0n;
  const neg = numerator < 0n !== denominator < 0n;
  const n = numerator < 0n ? -numerator : numerator;
  const d = denominator < 0n ? -denominator : denominator;
  const q = n / d;
  const r = n % d;
  const rounded = r * 2n >= d ? q + 1n : q;
  return neg ? -rounded : rounded;
}

/**
 * Aggregate one column over a set of rows.
 *
 * `weightColumn` is required for WeightedMean and rejected for everything
 * else, so a caller cannot pass one and quietly have it ignored.
 */
export function aggregate(
  store: RowStore,
  rows: readonly RowId[],
  spec: AggColumnSpec,
  agg: Agg,
  weightColumn?: string,
): AggResult {
  if (!allowedAggregations(spec).includes(agg)) {
    return {
      ok: false,
      problem:
        agg === Agg.Sum || agg === Agg.Mean
          ? AggProblem.NotAdditive
          : AggProblem.UnknownColumn,
    };
  }
  if (agg === Agg.WeightedMean && weightColumn === undefined) {
    return { ok: false, problem: AggProblem.NoWeightColumn };
  }

  const base = { agg, rows: rows.length } as const;
  let present = 0;
  for (const id of rows) if (store.read(id, spec.key).present) present++;

  if (agg === Agg.Count) {
    return {
      ok: true,
      value: {
        ...base,
        exact: BigInt(rows.length),
        numerator: null,
        denominator: null,
        real: null,
        present,
      },
    };
  }
  if (agg === Agg.CountPresent) {
    return {
      ok: true,
      value: {
        ...base,
        exact: BigInt(present),
        numerator: null,
        denominator: null,
        real: null,
        present,
      },
    };
  }
  if (present === 0) return { ok: false, problem: AggProblem.Empty };

  // Real columns are the one place a double is the right representation --
  // they were never exact to begin with.
  if (spec.type === ColumnType.Real) {
    let acc = 0;
    let lo = Number.POSITIVE_INFINITY;
    let hi = Number.NEGATIVE_INFINITY;
    let wsum = 0;
    let wacc = 0;
    for (const id of rows) {
      const v = asNumber(store.read(id, spec.key));
      if (v === null) continue;
      acc += v;
      if (v < lo) lo = v;
      if (v > hi) hi = v;
      if (weightColumn !== undefined) {
        const w = asNumber(store.read(id, weightColumn));
        if (w !== null) {
          wsum += w;
          wacc += v * w;
        }
      }
    }
    const real =
      agg === Agg.Sum
        ? acc
        : agg === Agg.Mean
          ? acc / present
          : agg === Agg.Min
            ? lo
            : agg === Agg.Max
              ? hi
              : wsum !== 0
                ? wacc / wsum
                : null;
    if (real === null) return { ok: false, problem: AggProblem.NoWeight };
    return {
      ok: true,
      value: {
        ...base,
        exact: null,
        numerator: null,
        denominator: null,
        real,
        present,
      },
    };
  }

  // Money and Integer: exact, in bigint, at every magnitude.
  let sum = 0n;
  let lo: bigint | null = null;
  let hi: bigint | null = null;
  let wsum = 0n;
  let wacc = 0n;
  for (const id of rows) {
    const v = asBigint(store.read(id, spec.key));
    if (v === null) {
      if (store.read(id, spec.key).present) {
        return { ok: false, problem: AggProblem.MixedTypes };
      }
      continue;
    }
    sum += v;
    if (lo === null || v < lo) lo = v;
    if (hi === null || v > hi) hi = v;
    if (weightColumn !== undefined) {
      const w = asBigint(store.read(id, weightColumn));
      if (w !== null) {
        wsum += w;
        wacc += v * w;
      }
    }
  }

  switch (agg) {
    case Agg.Sum:
      return {
        ok: true,
        value: { ...base, exact: sum, numerator: null, denominator: null, real: null, present },
      };
    case Agg.Mean: {
      const d = BigInt(present);
      return {
        ok: true,
        value: {
          ...base,
          exact: roundRatio(sum, d),
          numerator: sum,
          denominator: d,
          real: null,
          present,
        },
      };
    }
    case Agg.WeightedMean: {
      if (wsum === 0n) return { ok: false, problem: AggProblem.NoWeight };
      return {
        ok: true,
        value: {
          ...base,
          exact: roundRatio(wacc, wsum),
          numerator: wacc,
          denominator: wsum,
          real: null,
          present,
        },
      };
    }
    case Agg.Min:
    case Agg.Max: {
      const v = agg === Agg.Min ? lo : hi;
      if (v === null) return { ok: false, problem: AggProblem.Empty };
      return {
        ok: true,
        value: { ...base, exact: v, numerator: null, denominator: null, real: null, present },
      };
    }
    // Count and CountPresent returned above, before the emptiness check.
    case Agg.Unspecified:
    default:
      return { ok: false, problem: AggProblem.UnknownColumn };
  }
}
