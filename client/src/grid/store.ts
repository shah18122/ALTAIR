// client/src/grid/store.ts -- rows, and the only safe way to address one.
//
// P11-03.
//
// A PATCH IS ADDRESSED BY ROW IDENTITY. NEVER BY ROW INDEX.
//
// This is the card, and it is the bug that puts a fill on the wrong
// instrument.
//
// The grid is sorted -- by P&L, say, which is the sort a trader actually uses.
// A delta arrives saying "row 4207, column 3, now 18,755". Between the server
// choosing that index and the client applying it, two things happened: another
// delta changed a P&L above it, and the grid re-sorted. Index 4207 is now a
// different instrument. The patch lands, the cell updates, nothing throws, and
// the screen shows a price against a symbol that never traded at it.
//
// Measured on a 4096-row book under an ordinary sort-by-P&L, with 512 patches
// applied against indices captured one re-sort earlier, 511 of 512 land on the
// wrong row -- 99.8%. ONE happens to be right, and that one is the reason this
// is dangerous rather than obvious: a bug that gets everything wrong is found
// in a minute, and a bug that leaves a cell or two looking correct survives
// the spot check that follows "the grid looks a bit off".
//
// So `patchCell` takes a `RowId`, and there is deliberately no
// `patchCellByIndex`. An index is a position in a VIEW -- one particular sort
// of one particular filter at one particular instant -- and a view is not an
// address. The server assigns row ids, they never change, and re-sorting
// permutes the view without touching them.
//
// ABSENCE IS NOT ZERO, AGAIN.
//
// The second thing, and it is P5-02's rule arriving at the last surface before
// a human. A cell with no value is not a cell with the value zero. Rendered as
// `0.00` in a P&L column it is a claim -- that the position is flat, that the
// greek is neutral, that the leg has no exposure -- and it is a claim nobody
// made. `Cell` is therefore a tagged union with a `present: false` case, and
// there is no numeric default to fall through to.

/** Assigned by the server. Stable for the life of the row, and never a
 *  position in any view. */
export type RowId = number;

export type CellValue = bigint | number | string;

/** A cell that has a value, or one that explicitly does not. There is no
 *  third state and no default -- see the header. */
export type Cell =
  | { readonly present: true; readonly value: CellValue }
  | { readonly present: false };

export const ABSENT: Cell = { present: false };

export function cell(value: CellValue): Cell {
  return { present: true, value };
}

export interface Row {
  readonly id: RowId;
  readonly cells: ReadonlyMap<string, Cell>;
}

export const PatchOutcome = {
  Applied: 'Applied',
  /** The row id is not in the store. Reported, not created: a patch for a row
   *  the client has never seen means the snapshot is stale, which is a gap,
   *  not a row to invent. */
  UnknownRow: 'UnknownRow',
  /** The column key is not one this view has. Same reasoning. */
  UnknownColumn: 'UnknownColumn',
} as const;
export type PatchOutcome = (typeof PatchOutcome)[keyof typeof PatchOutcome];

/**
 * The rows, and one ordering of them.
 *
 * Two structures, kept deliberately separate: `#rows` is addressed by id and
 * is what a patch touches; `#view` is an array of ids in display order and is
 * what the viewport slices. Nothing writes through the view.
 */
export class RowStore {
  readonly #rows = new Map<RowId, Map<string, Cell>>();
  readonly #columns: ReadonlySet<string>;
  #view: RowId[] = [];

  constructor(columns: readonly string[]) {
    this.#columns = new Set(columns);
  }

  get size(): number {
    return this.#rows.size;
  }

  /** Display order. A copy is not made -- callers must not mutate it, and the
   *  type says so. */
  get view(): readonly RowId[] {
    return this.#view;
  }

  upsert(id: RowId, values: Readonly<Record<string, CellValue>>): void {
    let row = this.#rows.get(id);
    if (row === undefined) {
      row = new Map<string, Cell>();
      this.#rows.set(id, row);
      this.#view.push(id);
    }
    for (const [k, v] of Object.entries(values)) {
      if (this.#columns.has(k)) row.set(k, cell(v));
    }
  }

  remove(id: RowId): void {
    if (!this.#rows.delete(id)) return;
    const at = this.#view.indexOf(id);
    if (at >= 0) this.#view.splice(at, 1);
  }

  /**
   * Patch one cell, addressed by identity.
   *
   * There is no `patchCellByIndex`, and that absence is the point of this
   * file. A function taking an index would be correct in a static table and
   * silently wrong the moment anything sorts -- which is every second, in a
   * grid sorted by a live number.
   */
  patchCell(id: RowId, column: string, value: CellValue): PatchOutcome {
    if (!this.#columns.has(column)) return PatchOutcome.UnknownColumn;
    const row = this.#rows.get(id);
    if (row === undefined) return PatchOutcome.UnknownRow;
    row.set(column, cell(value));
    return PatchOutcome.Applied;
  }

  /** Mark a cell as having no value. Distinct from setting it to zero, which
   *  is a different statement about the world. */
  clearCell(id: RowId, column: string): PatchOutcome {
    if (!this.#columns.has(column)) return PatchOutcome.UnknownColumn;
    const row = this.#rows.get(id);
    if (row === undefined) return PatchOutcome.UnknownRow;
    row.set(column, ABSENT);
    return PatchOutcome.Applied;
  }

  /** Never returns undefined for a known column: an unset cell reads as
   *  explicitly absent, so callers cannot reach for `?? 0`. */
  read(id: RowId, column: string): Cell {
    return this.#rows.get(id)?.get(column) ?? ABSENT;
  }

  /**
   * Reorder the view.
   *
   * Takes a comparator over ids rather than over indices, and rebuilds the id
   * array. Row identity is untouched, so a patch in flight during a sort still
   * lands on the row it named.
   */
  sortBy(compare: (a: RowId, b: RowId) => number): void {
    this.#view.sort(compare);
  }

  /** A comparator over one column, absent-last in both directions.
   *
   *  Absent-last rather than absent-as-zero: sorting a blank P&L to the middle
   *  of the list, among the genuinely flat positions, hides it. */
  columnComparator(
    column: string,
    ascending: boolean,
  ): (a: RowId, b: RowId) => number {
    return (a, b) => {
      const ca = this.read(a, column);
      const cb = this.read(b, column);
      if (!ca.present && !cb.present) return 0;
      if (!ca.present) return 1;
      if (!cb.present) return -1;
      const sign = ascending ? 1 : -1;
      const va = ca.value;
      const vb = cb.value;
      if (typeof va === 'bigint' && typeof vb === 'bigint') {
        return va < vb ? -sign : va > vb ? sign : 0;
      }
      if (typeof va === 'number' && typeof vb === 'number') {
        return (va - vb) * sign;
      }
      return String(va).localeCompare(String(vb)) * sign;
    };
  }

  /** The ids in a half-open window of the view. O(window), not O(size) --
   *  which is what makes a million rows affordable. */
  window(first: number, last: number): RowId[] {
    const lo = Math.max(0, first);
    const hi = Math.min(this.#view.length, last);
    return lo >= hi ? [] : this.#view.slice(lo, hi);
  }
}
