// P11-04 acceptance tests.
//
// Test 1 is the card: `1000n === 1000` is false, so an equals filter on a
// money column silently matches nothing while `>` on the same column works.
// Measured, and the naive predicate is run alongside so the hazard is shown
// to exist before the fix is credited.
//
// Test 2: a money filter is typed in rupees and compared in paise, and the
// off-by-a-hundred version is measured rather than described.
//
// Test 3: an absent cell matches no comparison, so `>= 0` and `< 0` do not
// partition the grid -- and the deficit is exactly the absent rows.
//
// Test 4: a URL carrying filters carries the book, and says so.
//
// Test 5: a truncated URL fails OPEN unless it is refused whole.
//
// Test 6: round trip, and the money operand does not get re-multiplied.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { RowStore } from '../src/grid/store.ts';
import { formatPaise } from '../src/protocol.ts';
import {
  ColumnType,
  FilterProblem,
  Op,
  applyFilters,
  chipLabel,
  matches,
  parseFilter,
  parseRupeesToPaise,
} from '../src/grid/filter.ts';
import type { ColumnSpec, Filter } from '../src/grid/filter.ts';
import {
  SAFE_URL_CHARS,
  UrlProblem,
  decodeFilters,
  encodeFilters,
} from '../src/grid/url.ts';

const COLUMNS: ColumnSpec[] = [
  { key: 'symbol', label: 'Symbol', type: ColumnType.Text },
  { key: 'qty', label: 'Qty', type: ColumnType.Integer },
  { key: 'pnl', label: 'P&L', type: ColumnType.Money },
  { key: 'delta', label: 'Delta', type: ColumnType.Real },
];

function book(n: number, absentEvery = 0): RowStore {
  const store = new RowStore(COLUMNS.map((c) => c.key));
  let s = 0x2545f491;
  for (let i = 0; i < n; i++) {
    s = (s * 1664525 + 1013904223) >>> 0;
    const pnl = BigInt((s % 2_000_000) - 1_000_000);
    const values: Record<string, bigint | number | string> = {
      symbol: `SYM${String(i).padStart(5, '0')}`,
      qty: (s % 400) + 1,
      delta: ((s % 1000) - 500) / 1000,
    };
    if (absentEvery === 0 || i % absentEvery !== 0) values['pnl'] = pnl;
    store.upsert(i, values);
  }
  return store;
}

test('[1] an equals filter on money matches nothing, unless types are normalised', () => {
  // The language rules this rests on, stated as assertions rather than trusted.
  assert.equal(
    (1000n as unknown) === (1000 as unknown),
    false,
    'strict equality between bigint and number is always false',
  );
  assert.equal(1000n > 999, true, 'while relational comparison happily mixes them');
  assert.throws(
    () => {
      // @ts-expect-error -- mixing in arithmetic is a TypeError at runtime
      const x: unknown = 1000n + 1;
      return x;
    },
    TypeError,
    'and arithmetic between them throws',
  );

  const store = book(4096);
  // A value that genuinely occurs in the book.
  const someId = store.view[17];
  assert.ok(someId !== undefined);
  const existing = store.read(someId, 'pnl');
  assert.equal(existing.present, true);
  if (!existing.present) return;
  const target = existing.value as bigint;

  // The naive predicate: compare the cell to a number operand with ===.
  const naiveEq = [...store.view].filter((id) => {
    const c = store.read(id, 'pnl');
    return c.present && (c.value as unknown) === (Number(target) as unknown);
  }).length;

  const gt = applyFilters(store, [
    { column: 'pnl', op: Op.Greater, operand: 0n },
  ]).length;
  const eq = applyFilters(store, [
    { column: 'pnl', op: Op.Equals, operand: target },
  ]).length;

  console.log(
    `    naive === against a number operand: ${naiveEq} rows;  > 0 works and returns ${gt};  normalised = returns ${eq}`,
  );

  assert.equal(
    naiveEq,
    0,
    'the naive equals filter returns zero rows for a value that is definitely in the book -- nothing throws and no row is wrong, the grid just empties, and the natural reading is that no position matches',
  );
  assert.ok(
    gt > 0,
    'while the SAME column filtered with > works perfectly, which is what makes this hard to spot: one operator on one column is broken and the rest are fine',
  );
  assert.ok(eq >= 1, 'normalising both sides finds the row');
});

test('[2] a money filter is typed in rupees and compared in paise', () => {
  const store = book(4096);

  const parsed = parseFilter(COLUMNS, 'pnl', Op.Greater, '1000');
  assert.equal(parsed.ok, true);
  if (!parsed.ok) return;
  assert.equal(
    parsed.filter.operand,
    100000n,
    'one thousand rupees is a hundred thousand paise -- the conversion happens once, at the parse',
  );

  const correct = applyFilters(store, [parsed.filter]).length;
  const literal = applyFilters(store, [
    { column: 'pnl', op: Op.Greater, operand: 1000n },
  ]).length;

  console.log(
    `    "P&L > 1000" meaning rupees: ${correct} rows;  the same number taken literally as paise: ${literal} rows`,
  );
  assert.ok(
    literal > correct,
    'taking the typed number as paise selects every position over TEN RUPEES -- a hundredfold wider filter that looks entirely plausible on screen',
  );

  assert.equal(
    chipLabel(COLUMNS, parsed.filter, formatPaise),
    'P&L > ₹1,000.00',
    'and the chip renders back in the unit it was typed in -- a chip reading "> 100000" for a thousand-rupee filter is how a user stops trusting the chips',
  );

  // Exact parsing, and a refusal rather than a rounding.
  assert.equal(parseRupeesToPaise('1234.35'), 123435n);
  assert.equal(parseRupeesToPaise('-0.05'), -5n);
  assert.equal(parseRupeesToPaise('1,00,000'), 10000000n);
  assert.equal(
    Math.round(1234.35 * 100),
    123435,
    'the float route happens to round correctly here',
  );
  assert.equal(
    parseRupeesToPaise('1.234'),
    FilterProblem.SubPaisaPrecision,
    'but sub-paisa precision is refused rather than rounded: rounding a filter BOUND silently changes which rows match, and the user cannot see why',
  );
  assert.equal(parseRupeesToPaise('abc'), FilterProblem.NotANumber);
  assert.equal(
    parseFilter(COLUMNS, 'pnl', Op.Contains, 'x').ok,
    false,
    'and "contains" is not a thing you can do to money',
  );
});

test('[3] an absent cell matches no comparison, so the halves do not add up', () => {
  const N = 3000;
  const store = book(N, 7); // every 7th row has no P&L

  const ge = applyFilters(store, [
    { column: 'pnl', op: Op.GreaterOrEqual, operand: 0n },
  ]).length;
  const lt = applyFilters(store, [
    { column: 'pnl', op: Op.Less, operand: 0n },
  ]).length;
  const absent = applyFilters(store, [
    { column: 'pnl', op: Op.IsAbsent, operand: null },
  ]).length;

  console.log(
    `    of ${N} rows: ">= 0" gives ${ge}, "< 0" gives ${lt}, together ${ge + lt} -- ${N - ge - lt} short, and "is blank" finds exactly ${absent}`,
  );

  assert.ok(
    ge + lt < N,
    'the two halves of a comparison do not partition a grid with blank cells -- correct, and the same as SQL NULL, but it has to be deliberate',
  );
  assert.equal(
    ge + lt + absent,
    N,
    'and IsAbsent accounts for exactly the difference, so no row is unreachable by any filter',
  );

  const notEq = applyFilters(store, [
    { column: 'pnl', op: Op.NotEquals, operand: 0n },
  ]).length;
  assert.ok(
    notEq <= N - absent,
    'NotEquals also excludes blanks -- otherwise "not equal to X" would sweep in every blank row, which is not what anyone means by it',
  );

  assert.equal(
    matches({ present: false }, { column: 'pnl', op: Op.IsPresent, operand: null }),
    false,
  );
});

test('[4] a URL carrying filters carries the book, and says so', () => {
  const filters: Filter[] = [
    { column: 'symbol', op: Op.Contains, operand: 'RELIANCE' },
    { column: 'pnl', op: Op.Greater, operand: 50000000n },
    { column: 'qty', op: Op.IsAbsent, operand: null },
  ];
  const enc = encodeFilters(COLUMNS, filters, formatPaise);

  console.log(`    link would disclose: ${enc.disclosure.join(' | ')}`);

  assert.equal(
    enc.disclosure.length,
    2,
    'the two filters carrying values from the book are named; the structural one is not',
  );
  assert.ok(
    enc.disclosure.some((d) => d.includes('RELIANCE')),
    'so a copy-link control can say that the link names which stock you are in',
  );
  assert.ok(
    enc.disclosure.some((d) => d.includes('₹5,00,000.00')),
    'and how much you are up on it, rendered in the unit a human reads',
  );
  assert.ok(
    !enc.tooLong,
    'three filters is nowhere near the length at which truncation starts',
  );
});

test('[5] a truncated URL fails open unless it is refused whole', () => {
  // How many typical filters fit before the length gets risky?
  let fits = 0;
  const many: Filter[] = [];
  for (let i = 0; i < 500; i++) {
    many.push({ column: 'symbol', op: Op.Contains, operand: `SYM${i}XYZ` });
    if (!encodeFilters(COLUMNS, many, formatPaise).tooLong) fits = many.length;
    else break;
  }
  console.log(
    `    ${SAFE_URL_CHARS} characters holds ${fits} typical filters; past that a link may be cut in transit`,
  );
  assert.ok(fits > 0 && fits < 500);

  // Six filters that actually narrow a real book, so dropping one is
  // measurable rather than vacuous. An earlier draft used six mutually
  // exclusive `contains` clauses; they ANDed to zero rows and the comparison
  // compared nothing to nothing.
  const narrowing: Filter[] = [
    { column: 'pnl', op: Op.IsPresent, operand: null },
    { column: 'qty', op: Op.Greater, operand: 20 },
    { column: 'qty', op: Op.Less, operand: 380 },
    { column: 'delta', op: Op.Greater, operand: -0.4 },
    { column: 'symbol', op: Op.Contains, operand: 'SYM' },
    { column: 'pnl', op: Op.Greater, operand: 0n },
  ];
  const good = encodeFilters(COLUMNS, narrowing, formatPaise).value;
  const full = decodeFilters(COLUMNS, good);
  assert.equal(full.ok, true);
  if (!full.ok) return;
  assert.equal(full.filters.length, 6);

  // Cut it mid-clause, the way a proxy would.
  const midClause = decodeFilters(COLUMNS, good.slice(0, good.length - 4));
  assert.equal(midClause.ok, false, 'a cut inside a clause is refused');
  if (!midClause.ok) assert.equal(midClause.problem, UrlProblem.Truncated);

  // AND THE HARD CASE. A cut that happens to land on a `;` leaves a
  // perfectly well-formed shorter list -- every structural check passes. The
  // first draft of the decoder accepted this without complaint, which is why
  // the encoding carries a clause count.
  const lastSemi = good.lastIndexOf(';');
  const atBoundary = good.slice(0, lastSemi);
  const clauseParts = atBoundary.split('~').slice(2).join('~').split(';');
  assert.equal(
    clauseParts.every((c) => c.split(':').length === 3),
    true,
    'the boundary-cut string is STRUCTURALLY perfect -- every clause has its three parts, so nothing about its shape says it was truncated',
  );
  const boundary = decodeFilters(COLUMNS, atBoundary);
  assert.equal(
    boundary.ok,
    false,
    'and it is still refused, because the declared clause count no longer matches -- the only way to tell "the user removed a filter" from "the network removed a filter"',
  );
  if (!boundary.ok) assert.equal(boundary.problem, UrlProblem.Truncated);

  // What refusing prevents: had it parsed the prefix, the grid would show
  // MORE rows than the link described.
  const store = book(2000, 7);
  const asShared = applyFilters(store, full.filters).length;
  const asTruncated = applyFilters(store, full.filters.slice(0, 5)).length;
  const extra = asTruncated - asShared;
  console.log(
    `    had the prefix been kept: ${asTruncated} rows instead of ${asShared} -- ${extra} positions on screen that the link said were filtered out`,
  );
  assert.ok(
    extra > 0,
    'dropping the last filter widens the result, which is why a partial parse fails OPEN: the grid shows positions the link excluded, and nothing looks wrong',
  );

  assert.equal(decodeFilters(COLUMNS, null).ok, false);
  assert.equal(
    decodeFilters(COLUMNS, '9~symbol:ct:X').ok,
    false,
    'and a filter string from a different schema version is refused rather than guessed at',
  );
});

test('[6] the money operand round-trips without being multiplied again', () => {
  const typed = parseFilter(COLUMNS, 'pnl', Op.GreaterOrEqual, '1234.35');
  assert.equal(typed.ok, true);
  if (!typed.ok) return;
  assert.equal(typed.filter.operand, 123435n);

  const enc = encodeFilters(COLUMNS, [typed.filter], formatPaise);
  const dec = decodeFilters(COLUMNS, enc.value);
  assert.equal(dec.ok, true);
  if (!dec.ok) return;

  assert.equal(
    dec.filters[0]?.operand,
    123435n,
    'the URL stores the STORAGE form, so opening a bookmarked link does not re-parse paise as rupees and multiply the bound by a hundred every time',
  );

  // Open it twice more, which is how that class of bug actually shows up.
  const again = decodeFilters(
    COLUMNS,
    encodeFilters(COLUMNS, dec.filters, formatPaise).value,
  );
  assert.equal(again.ok, true);
  if (again.ok) assert.equal(again.filters[0]?.operand, 123435n);
});
