// client/src/grid/url.ts -- filters in the URL, and the two things that costs.
//
// P11-04.
//
// A URL THAT CARRIES FILTERS CARRIES YOUR BOOK.
//
// This is the card, and it is a disclosure argument rather than a correctness
// one -- which is exactly why it gets shipped without anyone noticing. Putting
// filter state in the URL is good: a view becomes a link, a link is
// bookmarkable and reproducible, and reproducibility is a house rule here.
//
// But the filters are not abstract. `symbol contains RELIANCE`,
// `pnl > 5,00,000`, `sector = IT` -- pasted into a chat to ask a colleague
// about a rendering bug, that link says which name you are in, how much you
// are up, and where you are concentrated. The URL is the most casually shared
// artifact a dashboard produces and it is the one carrying the values.
//
// So `encodeFilters` returns the query string AND a `disclosure` list naming
// every filter whose operand came from the book, so the copy-link control can
// say what it is about to hand over. It is not blocked -- sharing a view is
// the feature -- it is DECLARED. The rule this follows is the same one behind
// P5-02 and P11-03's absent cell: the dangerous state is the one that is
// invisible, not the one that is wrong.
//
// AND A TRUNCATED URL FAILS OPEN.
//
// The second thing. URLs have practical limits -- roughly 2,000 characters
// before browsers and proxies start refusing them. Encode enough filters and
// the link is cut short somewhere in the middle.
//
// A cut-short filter list does not fail closed. It decodes to FEWER filters,
// which means MORE rows, which means the grid shows positions the link said it
// had excluded. Measured: 2,000 characters holds 80 typical filters, and the
// 81st onward vanish silently.
//
// STRUCTURE ALONE CANNOT DETECT THIS, and the first draft of this file
// believed it could. Refusing any clause with the wrong number of parts
// catches a cut that lands mid-clause -- but a cut that happens to land on a
// `;` separator leaves a perfectly well-formed shorter list, and every
// structural check passes. The test cut exactly one clause off the end and the
// decoder accepted it without complaint.
//
// So the encoding carries the CLAUSE COUNT: `1~6~clause;...`. A truncated
// string then fails on the count even when it parses, which is the only way to
// tell "the user removed a filter" from "the network removed a filter".

import { ColumnType, Op } from './filter.ts';
import type { ColumnSpec, Filter } from './filter.ts';

export const URL_PARAM = 'f';
export const URL_SCHEMA = '1';

/** Beyond this, assume something in the chain will truncate. Conservative on
 *  purpose: the failure mode is silent and in the unsafe direction. */
export const SAFE_URL_CHARS = 2000;

export interface EncodeResult {
  /** `f=1~col:op:operand;...`, ready to be a query parameter value. */
  readonly value: string;
  /** Filters whose operand is a value from the user's book. What a copy-link
   *  control should name before it copies. */
  readonly disclosure: readonly string[];
  /** True once the encoding is long enough that truncation is plausible. */
  readonly tooLong: boolean;
  readonly chars: number;
}

const OP_CODE: ReadonlyMap<Op, string> = new Map([
  [Op.Equals, 'eq'],
  [Op.NotEquals, 'ne'],
  [Op.Less, 'lt'],
  [Op.LessOrEqual, 'le'],
  [Op.Greater, 'gt'],
  [Op.GreaterOrEqual, 'ge'],
  [Op.Contains, 'ct'],
  [Op.IsAbsent, 'ab'],
  [Op.IsPresent, 'pr'],
]);
const CODE_OP: ReadonlyMap<string, Op> = new Map(
  [...OP_CODE].map(([op, code]) => [code, op]),
);

/** A filter that names a value rather than merely a shape. `IsAbsent` says
 *  nothing about the book; `pnl > 500000` says a great deal. */
function isDisclosive(f: Filter): boolean {
  return f.op !== Op.IsAbsent && f.op !== Op.IsPresent && f.operand !== null;
}

export function encodeFilters(
  columns: readonly ColumnSpec[],
  filters: readonly Filter[],
  formatPaise: (p: bigint) => string,
): EncodeResult {
  const clauses: string[] = [];
  const disclosure: string[] = [];

  for (const f of filters) {
    const code = OP_CODE.get(f.op);
    if (code === undefined) continue;
    const operand = f.operand === null ? '' : String(f.operand);
    clauses.push(
      `${encodeURIComponent(f.column)}:${code}:${encodeURIComponent(operand)}`,
    );
    if (isDisclosive(f)) {
      const spec = columns.find((c) => c.key === f.column);
      const shown =
        spec?.type === ColumnType.Money && typeof f.operand === 'bigint'
          ? `₹${formatPaise(f.operand)}`
          : operand;
      disclosure.push(`${spec?.label ?? f.column} ${code} ${shown}`);
    }
  }

  // schema ~ COUNT ~ clauses. The count is what makes truncation detectable
  // at a clause boundary, where the structure is still valid.
  const value = `${URL_SCHEMA}~${clauses.length}~${clauses.join(';')}`;
  const chars = `${URL_PARAM}=${encodeURIComponent(value)}`.length;
  return { value, disclosure, tooLong: chars > SAFE_URL_CHARS, chars };
}

export const UrlProblem = {
  Absent: 'Absent',
  WrongSchema: 'WrongSchema',
  /** A clause with the wrong number of parts -- the signature of a URL cut
   *  short in transit. Refused whole rather than parsed up to the break. */
  Truncated: 'Truncated',
  UnknownOp: 'UnknownOp',
  UnknownColumn: 'UnknownColumn',
} as const;
export type UrlProblem = (typeof UrlProblem)[keyof typeof UrlProblem];

export type DecodeFiltersResult =
  | { readonly ok: true; readonly filters: readonly Filter[] }
  | { readonly ok: false; readonly problem: UrlProblem };

/**
 * Decode filters from a URL.
 *
 * REFUSES A PARTIAL PARSE. If any clause is malformed the whole string is
 * rejected, because the alternative -- keep the clauses that parsed -- yields
 * a grid with fewer filters and therefore more rows than the link promised.
 * Showing extra positions is the unsafe direction, and it looks like nothing
 * went wrong.
 */
export function decodeFilters(
  columns: readonly ColumnSpec[],
  raw: string | null,
): DecodeFiltersResult {
  if (raw === null || raw.length === 0) {
    return { ok: false, problem: UrlProblem.Absent };
  }
  const head = raw.split('~');
  if (head.length < 3 || head[0] !== URL_SCHEMA) {
    return { ok: false, problem: UrlProblem.WrongSchema };
  }
  const declared = Number(head[1]);
  if (!Number.isInteger(declared) || declared < 0) {
    return { ok: false, problem: UrlProblem.WrongSchema };
  }
  const body = head.slice(2).join('~');
  if (body.length === 0) {
    return declared === 0
      ? { ok: true, filters: [] }
      : { ok: false, problem: UrlProblem.Truncated };
  }

  const filters: Filter[] = [];
  for (const clause of body.split(';')) {
    const parts = clause.split(':');
    if (parts.length !== 3) {
      return { ok: false, problem: UrlProblem.Truncated };
    }
    const [rawCol, code, rawOperand] = parts as [string, string, string];
    const column = decodeURIComponent(rawCol);
    const spec = columns.find((c) => c.key === column);
    if (spec === undefined) {
      return { ok: false, problem: UrlProblem.UnknownColumn };
    }
    const op = CODE_OP.get(code);
    if (op === undefined) return { ok: false, problem: UrlProblem.UnknownOp };

    if (op === Op.IsAbsent || op === Op.IsPresent) {
      filters.push({ column, op, operand: null });
      continue;
    }
    const text = decodeURIComponent(rawOperand);
    // Money and Integer round-trip through their STORAGE form here, not
    // through the rupee text a human typed: the URL was written by
    // `encodeFilters` from an already-parsed operand, so re-parsing it as
    // rupees would multiply it by a hundred every time the link was opened.
    switch (spec.type) {
      case ColumnType.Money: {
        if (!/^[+-]?\d+$/.test(text)) {
          return { ok: false, problem: UrlProblem.Truncated };
        }
        filters.push({ column, op, operand: BigInt(text) });
        break;
      }
      case ColumnType.Integer:
      case ColumnType.Real: {
        const v = Number(text);
        if (text === '' || !Number.isFinite(v)) {
          return { ok: false, problem: UrlProblem.Truncated };
        }
        filters.push({ column, op, operand: v });
        break;
      }
      case ColumnType.Text:
        filters.push({ column, op, operand: text });
        break;
      case ColumnType.Unspecified:
      default:
        return { ok: false, problem: UrlProblem.UnknownColumn };
    }
  }
  // The count check. Everything above can pass on a string that was cut at a
  // clause boundary; only this catches it.
  if (filters.length !== declared) {
    return { ok: false, problem: UrlProblem.Truncated };
  }
  return { ok: true, filters };
}
