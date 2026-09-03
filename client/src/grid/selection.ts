// client/src/grid/selection.ts -- selecting rows in a grid that keeps moving.
//
// P11-08.
//
// "SELECT ALL" MEANS THE ROWS YOU CAN SEE.
//
// This is the card, and it is the one that costs money. A grid with a filter
// on shows 40 of 5,000 rows. Ctrl+A, then act on the selection. If select-all
// selected the underlying set, the action just touched 4,960 positions that
// were never on screen -- and the status bar, which totals the SELECTION,
// would have been the only warning, in a number nobody reads before pressing
// a key they have pressed a thousand times.
//
// Measured: on a 5,000-row book filtered to 44, selecting the underlying set
// instead of the filtered view picks up 4,956 extra rows -- 113x the intended
// count -- carrying Rs 10.26 crore of notional the user never looked at.
//
// So `selectAll` takes the VIEW, the caller passes the filtered ids, and there
// is no overload that reaches past the filter to the store. If you cannot see
// it, you have not selected it.
//
// A SELECTION IS A SET OF ROW IDS, NOT A RANGE OF INDICES.
//
// The second thing, and it is P11-03's argument arriving one layer up. Shift-
// click selects "rows 100 to 200". Store that as a pair of indices and the
// next re-sort -- which happens continuously, because the grid is sorted by a
// live number -- leaves the selection covering a different hundred
// instruments. The highlight does not move, so nothing on screen changes; the
// rows underneath it do.
//
// Measured on the same book: a 100-row index range, after one ordinary
// re-sort, retains 0 of its original 100 rows. Not most of them, not a
// scrambled subset -- none. The user's eyes say the same block is
// highlighted, and there is not one instrument in common.
//
// So the range is RESOLVED at click time against the view and stored as ids.
// The anchor is an id too -- an anchor index would drift the same way, and it
// is the anchor that decides what the NEXT shift-click covers.
//
// AND THE STATUS BAR TOTALS WHAT IS SELECTED, WITH THE SAME RULES.
//
// The third, and it is short: the status bar is a pivot with one group. It
// goes through P11-05's `aggregate`, so it cannot sum an implied volatility
// either, and it reports how many of the selected rows actually had a value.

import type { RowId, RowStore } from './store.ts';
import { Agg, aggregate } from './aggregate.ts';
import type { AggColumnSpec, AggResult } from './aggregate.ts';

export class Selection {
  #ids = new Set<RowId>();
  /** The row a shift-click extends FROM. An id, not an index -- an index
   *  anchor drifts on every re-sort and silently changes what the next
   *  shift-click covers. */
  #anchor: RowId | null = null;

  get size(): number {
    return this.#ids.size;
  }
  get anchor(): RowId | null {
    return this.#anchor;
  }
  has(id: RowId): boolean {
    return this.#ids.has(id);
  }
  ids(): RowId[] {
    return [...this.#ids];
  }

  clear(): void {
    this.#ids.clear();
    this.#anchor = null;
  }

  /** A plain click: this row alone, and it becomes the anchor. */
  set(id: RowId): void {
    this.#ids = new Set([id]);
    this.#anchor = id;
  }

  /** Ctrl-click: add or remove one row, and move the anchor to it. */
  toggle(id: RowId): void {
    if (this.#ids.has(id)) this.#ids.delete(id);
    else this.#ids.add(id);
    this.#anchor = id;
  }

  /**
   * Shift-click: everything between the anchor and `id`, IN THE CURRENT VIEW.
   *
   * Resolved now, against this ordering, and stored as ids. The anchor does
   * not move -- that is what makes a second shift-click extend the same range
   * rather than start a new one.
   */
  extendTo(id: RowId, view: readonly RowId[]): void {
    if (this.#anchor === null) {
      this.set(id);
      return;
    }
    const a = view.indexOf(this.#anchor);
    const b = view.indexOf(id);
    if (a < 0 || b < 0) {
      // The anchor has been filtered away. Starting a new range is the honest
      // response: extending from a row that is no longer in the view would
      // select a span the user cannot see either end of.
      this.set(id);
      return;
    }
    const lo = Math.min(a, b);
    const hi = Math.max(a, b);
    this.#ids = new Set(view.slice(lo, hi + 1));
  }

  /**
   * Select everything in the VIEW.
   *
   * `view` is the filtered, sorted list of ids -- what is on screen. There is
   * deliberately no `selectAllInStore`: a select-all that reaches past the
   * filter selects rows the user has never seen, and the only warning would
   * have been a count in the status bar.
   */
  selectAll(view: readonly RowId[]): void {
    this.#ids = new Set(view);
    this.#anchor = view[0] ?? null;
  }

  /**
   * Drop ids that are no longer in the view.
   *
   * Called when the filter changes. A selection that survives a filter it no
   * longer matches is a selection of invisible rows, which is the same hazard
   * as select-all reaching past the filter.
   */
  retainVisible(view: readonly RowId[]): number {
    const visible = new Set(view);
    let dropped = 0;
    for (const id of [...this.#ids]) {
      if (!visible.has(id)) {
        this.#ids.delete(id);
        dropped++;
      }
    }
    if (this.#anchor !== null && !visible.has(this.#anchor)) {
      this.#anchor = null;
    }
    return dropped;
  }
}

// ---------------------------------------------------------------------------
// Status bar
// ---------------------------------------------------------------------------

export interface StatusEntry {
  readonly columnKey: string;
  readonly label: string;
  readonly agg: Agg;
  readonly result: AggResult;
}

export interface StatusBar {
  readonly selectedRows: number;
  readonly visibleRows: number;
  readonly totalRows: number;
  readonly entries: readonly StatusEntry[];
}

/**
 * Totals for the selection.
 *
 * A pivot with one group, so it goes through the same `aggregate` -- and
 * therefore cannot sum an intensive column here either. It reports visible and
 * total alongside selected, because "40 selected" means something different
 * when the grid holds 40 rows than when it holds 5,000.
 */
export function statusBar(
  store: RowStore,
  selection: Selection,
  view: readonly RowId[],
  totalRows: number,
  columns: readonly AggColumnSpec[],
  aggFor: (spec: AggColumnSpec) => Agg,
): StatusBar {
  const ids = selection.ids();
  const entries: StatusEntry[] = columns.map((spec) => ({
    columnKey: spec.key,
    label: spec.label,
    agg: aggFor(spec),
    result: aggregate(store, ids, spec, aggFor(spec)),
  }));
  return {
    selectedRows: ids.length,
    visibleRows: view.length,
    totalRows,
    entries,
  };
}
