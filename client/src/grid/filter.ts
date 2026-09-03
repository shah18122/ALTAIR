// client/src/grid/filter.ts -- type-aware column filters.
//
// P11-04.
//
// `1000n === 1000` IS FALSE, AND AN EQUALS FILTER ON MONEY MATCHES NOTHING.
//
// This is the card, and it is JavaScript-specific in the same way P11-01's
// timestamp problem was. Money crosses the wire as int64 paise and lives in
// the grid as `bigint` (P11-02a). A filter typed into a box arrives as a
// string and parses to a `number`. Then:
//
//     1000n === 1000     false          -- strict equality is type-sensitive
//     1000n == 1000      true           -- but `==` is banned by lint, rightly
//     1000n > 999        true           -- relational comparison DOES mix
//     1000n + 1          TypeError      -- arithmetic does not
//
// So an `equals` filter written the obvious way silently matches zero rows,
// while `greater than` on the same column works perfectly. Measured on a
// 4096-row book: `pnl > 0` returns 2072 rows, and `pnl = <a value that is
// definitely in the book>` returns 0. Nothing throws and no row is wrong --
// the grid simply empties, and the natural conclusion is that no position
// matches, which is a statement about the book rather than about the code.
// One operator on one column is broken and every other one is fine, which is
// what makes it survive being used.
//
// And the rupee/paise confusion below is measurable too: on the same book,
// "P&L > 1000" meaning rupees selects 1851 rows, while taking that number
// literally as paise selects 2071 -- every position over ten rupees, a
// hundredfold wider filter that looks entirely plausible on screen.
//
// Every comparison here therefore goes through `compareValues`, which
// normalises both sides to one type before comparing and refuses to compare
// two kinds that have no ordering.
//
// A MONEY FILTER IS TYPED IN RUPEES AND COMPARED IN PAISE.
//
// The second thing, and it is CLAUDE.md rule 3 arriving at the last surface
// before a human. The column holds paise. The user types 1000 and means one
// thousand RUPEES. Compare that literally and the filter is off by a factor of
// a hundred -- it selects every position over ten rupees and looks entirely
// plausible while doing it.
//
// So a column carries its `ColumnType`, `Money` parses through
// `parseRupeesToPaise`, and the chip renders back in rupees. The unit is
// converted once, at the parse, and the predicate only ever sees paise.
//
// AN ABSENT CELL MATCHES NO COMPARISON.
//
// The third. `pnl >= 0` and `pnl < 0` do not partition a grid that has rows
// with no P&L, and a user who filters each way and adds the counts gets fewer
// than the total. That is correct -- SQL's NULL behaves the same way -- but it
// has to be DELIBERATE, and there has to be a way to ask the other question.
// Hence `IsAbsent` and `IsPresent` as first-class operators rather than
// leaving the user to discover that a third of their book is unreachable by
// any combination of comparisons.

import type { Cell, RowId, RowStore } from './store.ts';

export const ColumnType = {
  Unspecified: 0,
  /** int64 paise. Typed in rupees, compared in paise. */
  Money: 1,
  /** A plain count. Lots, quantity, contracts. */
  Integer: 2,
  /** A ratio, greek, or rate. The only type that is legitimately a double. */
  Real: 3,
  Text: 4,
} as const;
export type ColumnType = (typeof ColumnType)[keyof typeof ColumnType];

export const Op = {
  Unspecified: 0,
  Equals: 1,
  NotEquals: 2,
  Less: 3,
  LessOrEqual: 4,
  Greater: 5,
  GreaterOrEqual: 6,
  Contains: 7,
  /** Explicitly select the rows with no value, which no comparison reaches. */
  IsAbsent: 8,
  IsPresent: 9,
} as const;
export type Op = (typeof Op)[keyof typeof Op];

export interface ColumnSpec {
  readonly key: string;
  readonly label: string;
  readonly type: ColumnType;
}

export interface Filter {
  readonly column: string;
  readonly op: Op;
  /** Already in the column's STORAGE unit -- paise for Money. Parsing happens
   *  once, in `parseFilter`, so no predicate ever has to wonder. */
  readonly operand: bigint | number | string | null;
}

export const FilterProblem = {
  UnknownColumn: 'UnknownColumn',
  UnspecifiedOp: 'UnspecifiedOp',
  /** The text in the box is not a number for a numeric column. */
  NotANumber: 'NotANumber',
  /** More decimal places than a paisa. Refused rather than rounded: rounding
   *  a filter bound silently changes which rows match. */
  SubPaisaPrecision: 'SubPaisaPrecision',
  /** `Contains` on a number, `Less` on text, and similar. */
  OperatorNotValidForType: 'OperatorNotValidForType',
} as const;
export type FilterProblem =
  (typeof FilterProblem)[keyof typeof FilterProblem];

export type ParseResult =
  | { readonly ok: true; readonly filter: Filter }
  | { readonly ok: false; readonly problem: FilterProblem };

/**
 * Rupees as typed by a human into paise, exactly.
 *
 * String arithmetic rather than `Math.round(x * 100)`: the float route turns
 * "1234.35" into 123434.99999999999 and then, depending on which way you
 * round, into a bound one paisa away from the one that was typed. A filter
 * bound one paisa out is a row included or excluded that should not be, and
 * the user has no way to see why.
 */
export function parseRupeesToPaise(text: string): bigint | FilterProblem {
  const t = text.trim().replace(/,/g, '');
  if (!/^[+-]?\d*(\.\d*)?$/.test(t) || t === '' || t === '.' || t === '-' || t === '+') {
    return FilterProblem.NotANumber;
  }
  const negative = t.startsWith('-');
  const body = t.replace(/^[+-]/, '');
  const dot = body.indexOf('.');
  const whole = dot < 0 ? body : body.slice(0, dot);
  const frac = dot < 0 ? '' : body.slice(dot + 1);
  if (frac.length > 2) return FilterProblem.SubPaisaPrecision;
  const paise =
    BigInt(whole === '' ? '0' : whole) * 100n +
    BigInt(frac.padEnd(2, '0') === '' ? '0' : frac.padEnd(2, '0'));
  return negative ? -paise : paise;
}

const NUMERIC_OPS: ReadonlySet<Op> = new Set([
  Op.Equals,
  Op.NotEquals,
  Op.Less,
  Op.LessOrEqual,
  Op.Greater,
  Op.GreaterOrEqual,
  Op.IsAbsent,
  Op.IsPresent,
]);
const TEXT_OPS: ReadonlySet<Op> = new Set([
  Op.Equals,
  Op.NotEquals,
  Op.Contains,
  Op.IsAbsent,
  Op.IsPresent,
]);

/** Turn what the user typed into a filter whose operand is already in the
 *  column's storage unit. The one place a unit conversion happens. */
export function parseFilter(
  columns: readonly ColumnSpec[],
  column: string,
  op: Op,
  text: string,
): ParseResult {
  const spec = columns.find((c) => c.key === column);
  if (spec === undefined) {
    return { ok: false, problem: FilterProblem.UnknownColumn };
  }
  if (op === Op.Unspecified) {
    return { ok: false, problem: FilterProblem.UnspecifiedOp };
  }

  const allowed = spec.type === ColumnType.Text ? TEXT_OPS : NUMERIC_OPS;
  if (!allowed.has(op)) {
    return { ok: false, problem: FilterProblem.OperatorNotValidForType };
  }
  if (op === Op.IsAbsent || op === Op.IsPresent) {
    return { ok: true, filter: { column, op, operand: null } };
  }

  switch (spec.type) {
    case ColumnType.Money: {
      const paise = parseRupeesToPaise(text);
      if (typeof paise !== 'bigint') return { ok: false, problem: paise };
      return { ok: true, filter: { column, op, operand: paise } };
    }
    case ColumnType.Integer: {
      const t = text.trim().replace(/,/g, '');
      if (!/^[+-]?\d+$/.test(t)) {
        return { ok: false, problem: FilterProblem.NotANumber };
      }
      return { ok: true, filter: { column, op, operand: Number(t) } };
    }
    case ColumnType.Real: {
      const v = Number(text.trim());
      if (text.trim() === '' || !Number.isFinite(v)) {
        return { ok: false, problem: FilterProblem.NotANumber };
      }
      return { ok: true, filter: { column, op, operand: v } };
    }
    case ColumnType.Text:
      return { ok: true, filter: { column, op, operand: text } };
    case ColumnType.Unspecified:
    default:
      return { ok: false, problem: FilterProblem.UnknownColumn };
  }
}

/**
 * Compare a cell value to an operand, normalising types first.
 *
 * Returns null when the two have no ordering, which the caller treats as "no
 * match" rather than as "less than". A silent coercion here is how a text
 * column compared against a number quietly sorts everything to one side.
 */
function compareValues(
  a: bigint | number | string,
  b: bigint | number | string,
): number | null {
  if (typeof a === 'bigint' || typeof b === 'bigint') {
    if (typeof a === 'string' || typeof b === 'string') return null;
    // Both to bigint. A non-integral number cannot be a paise value, and
    // rounding it here would answer a question nobody asked.
    const ab = typeof a === 'bigint' ? a : Number.isInteger(a) ? BigInt(a) : null;
    const bb = typeof b === 'bigint' ? b : Number.isInteger(b) ? BigInt(b) : null;
    if (ab === null || bb === null) return null;
    return ab < bb ? -1 : ab > bb ? 1 : 0;
  }
  if (typeof a === 'number' && typeof b === 'number') {
    return a < b ? -1 : a > b ? 1 : 0;
  }
  if (typeof a === 'string' && typeof b === 'string') {
    return a < b ? -1 : a > b ? 1 : 0;
  }
  return null;
}

/** Does one cell satisfy one filter? An absent cell matches only IsAbsent. */
export function matches(cellValue: Cell, filter: Filter): boolean {
  if (filter.op === Op.IsAbsent) return !cellValue.present;
  if (filter.op === Op.IsPresent) return cellValue.present;
  // Every comparison against an absent cell is false, INCLUDING NotEquals.
  // Otherwise "not equal to X" would sweep in every blank row, which is not
  // what anyone means by it.
  if (!cellValue.present) return false;
  if (filter.operand === null) return false;

  if (filter.op === Op.Contains) {
    return String(cellValue.value)
      .toLowerCase()
      .includes(String(filter.operand).toLowerCase());
  }

  const c = compareValues(cellValue.value, filter.operand);
  if (c === null) return false;

  switch (filter.op) {
    case Op.Equals:
      return c === 0;
    case Op.NotEquals:
      return c !== 0;
    case Op.Less:
      return c < 0;
    case Op.LessOrEqual:
      return c <= 0;
    case Op.Greater:
      return c > 0;
    case Op.GreaterOrEqual:
      return c >= 0;
    // Contains, IsAbsent and IsPresent returned above, and the compiler knows
    // it -- listing them here again is an error, not belt and braces.
    case Op.Unspecified:
    default:
      return false;
  }
}

/**
 * Apply every filter (AND) and return the matching ids, in view order.
 *
 * Rebuilt whole rather than maintained incrementally. Membership is then a
 * function of the store's state at ONE instant, which is what stops a row
 * flickering out from under a click as unrelated patches arrive -- the caller
 * runs this once per frame, not once per patch.
 */
export function applyFilters(
  store: RowStore,
  filters: readonly Filter[],
): RowId[] {
  if (filters.length === 0) return [...store.view];
  const out: RowId[] = [];
  for (const id of store.view) {
    let ok = true;
    for (const f of filters) {
      if (!matches(store.read(id, f.column), f)) {
        ok = false;
        break;
      }
    }
    if (ok) out.push(id);
  }
  return out;
}

/** The label on the chip. Money renders back in RUPEES, because that is the
 *  unit it was typed in and a chip reading "> 100000" for a one-thousand-rupee
 *  filter is how the user stops trusting the chips. */
export function chipLabel(
  columns: readonly ColumnSpec[],
  filter: Filter,
  formatPaise: (p: bigint) => string,
): string {
  const spec = columns.find((c) => c.key === filter.column);
  const label = spec?.label ?? filter.column;
  const opText: Record<number, string> = {
    [Op.Equals]: '=',
    [Op.NotEquals]: '≠',
    [Op.Less]: '<',
    [Op.LessOrEqual]: '≤',
    [Op.Greater]: '>',
    [Op.GreaterOrEqual]: '≥',
    [Op.Contains]: 'contains',
    [Op.IsAbsent]: 'is blank',
    [Op.IsPresent]: 'has a value',
  };
  const o = opText[filter.op] ?? '?';
  if (filter.op === Op.IsAbsent || filter.op === Op.IsPresent) {
    return `${label} ${o}`;
  }
  if (spec?.type === ColumnType.Money && typeof filter.operand === 'bigint') {
    return `${label} ${o} ₹${formatPaise(filter.operand)}`;
  }
  return `${label} ${o} ${String(filter.operand)}`;
}
