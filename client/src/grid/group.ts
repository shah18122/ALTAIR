// client/src/grid/group.ts -- grouping and pivot.
//
// P11-05.
//
// A GROUP TOTAL AND THE GRAND TOTAL MUST AGREE, AND FOR AN INTENSIVE COLUMN
// THEY CANNOT.
//
// The reconciliation invariant from CLAUDE.md -- sum of the parts equals the
// whole -- is checkable on a pivot table, and checking it is the fastest way
// to find a meaningless aggregate. For an extensive column the group totals
// add to the grand total exactly, in bigint, every time. For an intensive one
// they do not add to anything, which is `aggregate` refusing to sum it rather
// than a discrepancy to chase.
//
// `checkAdditive` performs that reconciliation and is meant to be run, not
// admired: a pivot whose parts do not sum to its whole has a bug in the
// grouping, the filter, or the aggregate, and it is cheaper to assert than to
// notice.
//
// GROUPING IS BY A KEY, NOT BY A RENDERED LABEL.
//
// The second thing, and it is the quiet one. Group by expiry and the obvious
// implementation groups by the formatted date -- "26 Sep 2026". Two expiries
// that format identically merge; one expiry formatted two ways (a locale
// change mid-session, a timezone) splits. Neither shows on screen as anything
// but a group with the wrong number of rows in it.
//
// So `groupBy` takes a key function returning a STRING KEY and, separately, a
// label function for display. They are allowed to differ, and usually should.

import type { RowId, RowStore } from './store.ts';
import { Agg, Extent, aggregate } from './aggregate.ts';
import type { AggColumnSpec, AggResult } from './aggregate.ts';

export interface Group {
  readonly key: string;
  readonly label: string;
  readonly rows: readonly RowId[];
}

/**
 * Partition rows into groups.
 *
 * Insertion-ordered: the first row of each group fixes that group's position,
 * so a grid that regroups every frame does not reshuffle its sections under
 * the cursor.
 */
export function groupBy(
  rows: readonly RowId[],
  keyOf: (id: RowId) => string,
  labelOf: (id: RowId) => string,
): Group[] {
  const byKey = new Map<string, { label: string; rows: RowId[] }>();
  for (const id of rows) {
    const k = keyOf(id);
    const existing = byKey.get(k);
    if (existing === undefined) {
      byKey.set(k, { label: labelOf(id), rows: [id] });
    } else {
      existing.rows.push(id);
    }
  }
  return [...byKey].map(([key, v]) => ({ key, label: v.label, rows: v.rows }));
}

export interface PivotCell {
  readonly groupKey: string;
  readonly columnKey: string;
  readonly result: AggResult;
}

export interface Pivot {
  readonly groups: readonly Group[];
  readonly cells: readonly PivotCell[];
  /** The same aggregate over every row, for the total line. */
  readonly grand: readonly PivotCell[];
}

export function pivot(
  store: RowStore,
  rows: readonly RowId[],
  groups: readonly Group[],
  columns: readonly AggColumnSpec[],
  aggFor: (spec: AggColumnSpec) => Agg,
  weightFor: (spec: AggColumnSpec) => string | undefined,
): Pivot {
  const cells: PivotCell[] = [];
  const grand: PivotCell[] = [];
  for (const spec of columns) {
    const agg = aggFor(spec);
    const weight = weightFor(spec);
    for (const g of groups) {
      cells.push({
        groupKey: g.key,
        columnKey: spec.key,
        result: aggregate(store, g.rows, spec, agg, weight),
      });
    }
    grand.push({
      groupKey: '',
      columnKey: spec.key,
      result: aggregate(store, rows, spec, agg, weight),
    });
  }
  return { groups, cells, grand };
}

export interface AdditiveCheck {
  readonly columnKey: string;
  /** Sum of the group totals. */
  readonly parts: bigint;
  /** The aggregate computed over every row at once. */
  readonly whole: bigint;
  readonly agrees: boolean;
}

/**
 * Reconcile the parts against the whole, for the columns where that is a
 * meaningful thing to ask.
 *
 * Extensive columns only, and deliberately so: an intensive column has no
 * whole for the parts to add up to, and a pivot that reported one would be
 * inventing it.
 */
export function checkAdditive(
  p: Pivot,
  columns: readonly AggColumnSpec[],
): AdditiveCheck[] {
  const out: AdditiveCheck[] = [];
  for (const spec of columns) {
    if (spec.extent !== Extent.Extensive) continue;
    const grandCell = p.grand.find((c) => c.columnKey === spec.key);
    if (grandCell === undefined || !grandCell.result.ok) continue;
    const whole = grandCell.result.value.exact;
    if (whole === null) continue;
    if (grandCell.result.value.agg !== Agg.Sum) continue;

    let parts = 0n;
    for (const c of p.cells) {
      if (c.columnKey !== spec.key || !c.result.ok) continue;
      const v = c.result.value.exact;
      if (v !== null) parts += v;
    }
    out.push({ columnKey: spec.key, parts, whole, agrees: parts === whole });
  }
  return out;
}
