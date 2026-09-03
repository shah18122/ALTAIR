// P11-05 acceptance tests.
//
// Test 1 is the card: an intensive column cannot be summed, and the number a
// grid would have produced is computed here to show what it looks like --
// entirely plausible, and about nothing.
//
// Test 2: an unweighted average price is the wrong number, measured on the
// most ordinary group there is.
//
// Test 3: money aggregates are exact in bigint, and a float reduction is run
// alongside to find where it stops being.
//
// Test 4: the parts add to the whole, exactly, for every extensive column.
//
// Test 5: grouping is by key, not by rendered label.
//
// Test 6: an empty group is refused, not returned as zero, and a mean says
// how many rows it actually divided by.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { RowStore } from '../src/grid/store.ts';
import { ColumnType } from '../src/grid/filter.ts';
import { formatPaise } from '../src/protocol.ts';
import {
  Agg,
  AggProblem,
  Extent,
  aggregate,
  allowedAggregations,
  roundRatio,
} from '../src/grid/aggregate.ts';
import type { AggColumnSpec } from '../src/grid/aggregate.ts';
import { checkAdditive, groupBy, pivot } from '../src/grid/group.ts';

const PNL: AggColumnSpec = {
  key: 'pnl',
  label: 'P&L',
  type: ColumnType.Money,
  extent: Extent.Extensive,
};
const QTY: AggColumnSpec = {
  key: 'qty',
  label: 'Qty',
  type: ColumnType.Integer,
  extent: Extent.Extensive,
};
const PRICE: AggColumnSpec = {
  key: 'price',
  label: 'Price',
  type: ColumnType.Money,
  extent: Extent.Intensive,
};
const IV: AggColumnSpec = {
  key: 'iv',
  label: 'IV',
  type: ColumnType.Real,
  extent: Extent.Intensive,
};
const SECTOR: AggColumnSpec = {
  key: 'sector',
  label: 'Sector',
  type: ColumnType.Text,
  extent: Extent.Categorical,
};

const ALL = [PNL, QTY, PRICE, IV, SECTOR];
const KEYS = ALL.map((c) => c.key);

test('[1] you cannot sum an implied volatility', () => {
  const store = new RowStore(KEYS);
  const ivs = [0.181, 0.174, 0.192, 0.168, 0.203];
  ivs.forEach((iv, i) => {
    store.upsert(i, { iv, qty: (i + 1) * 10, sector: 'IT' });
  });

  // What a grid that offers Sum on every numeric column would print.
  const naiveSum = ivs.reduce((a, b) => a + b, 0);
  console.log(
    `    a grid offering Sum on an IV column would print ${(100 * naiveSum).toFixed(2)}% -- five positions around 18% IV "totalling" ${(100 * naiveSum).toFixed(0)}%`,
  );

  assert.ok(
    !allowedAggregations(IV).includes(Agg.Sum),
    'Sum is not offered for an intensive quantity: two positions at 18% IV do not make one at 36%, and the total row would look exactly like the P&L total beside it',
  );
  assert.ok(
    !allowedAggregations(IV).includes(Agg.Mean),
    'and neither is an unweighted Mean, which is wrong in the same direction -- it treats rows as interchangeable when they are not',
  );
  assert.ok(
    allowedAggregations(IV).includes(Agg.WeightedMean),
    'a weighted mean IS meaningful, and is what an intensive column actually wants',
  );
  assert.deepEqual(
    [Agg.Min, Agg.Max, Agg.Count].map((a) => allowedAggregations(IV).includes(a)),
    [true, true, true],
    'order statistics and counts stay available, because those remain meaningful for any quantity',
  );

  const refused = aggregate(store, [...store.view], IV, Agg.Sum);
  assert.equal(refused.ok, false);
  if (!refused.ok) {
    assert.equal(
      refused.problem,
      AggProblem.NotAdditive,
      'and asking anyway is refused by name rather than computed -- the naive number is not out of range, does not throw, and is not even wrong; it is about nothing',
    );
  }

  assert.ok(
    allowedAggregations(PNL).includes(Agg.Sum),
    'while P&L, which scales with the size of the book, sums perfectly well',
  );
  assert.deepEqual(allowedAggregations(SECTOR), [Agg.Count, Agg.CountPresent]);
});

test('[2] an unweighted average price is the wrong number', () => {
  const store = new RowStore(KEYS);
  // The ordinary case: a small first position, then adding to a winner.
  store.upsert(1, { price: 10000n, qty: 1, sector: 'IT' }); // Rs 100, 1 lot
  store.upsert(2, { price: 20000n, qty: 1000, sector: 'IT' }); // Rs 200, 1000 lots

  const rows = [...store.view];
  const weighted = aggregate(store, rows, PRICE, Agg.WeightedMean, 'qty');
  assert.equal(weighted.ok, true);
  if (!weighted.ok) return;

  const unweighted = (10000n + 20000n) / 2n;

  console.log(
    `    1 lot at Rs 100 and 1000 lots at Rs 200:  unweighted mean Rs ${formatPaise(unweighted)},  quantity-weighted Rs ${formatPaise(weighted.value.exact ?? 0n)}`,
  );

  assert.equal(weighted.value.exact, 19990n, 'the weighted mean is Rs 199.90');
  const errorPct =
    (100 * Number(weighted.value.exact! - unweighted)) /
    Number(weighted.value.exact!);
  console.log(`    the unweighted number is ${errorPct.toFixed(1)}% low`);
  assert.ok(
    Math.abs(errorPct) > 20,
    'a 25% error on the number a trader reads as "what I paid", in a group that is not remotely contrived -- adding to a winner is the ordinary case',
  );

  assert.equal(
    weighted.value.numerator,
    10000n * 1n + 20000n * 1000n,
    'the exact numerator is carried so the mean can be rendered at any precision without a second rounding',
  );
  assert.equal(weighted.value.denominator, 1001n);

  const noWeight = aggregate(store, rows, PRICE, Agg.WeightedMean);
  assert.equal(noWeight.ok, false);
  if (!noWeight.ok) {
    assert.equal(
      noWeight.problem,
      AggProblem.NoWeightColumn,
      'and there is no implicit weight of 1 to fall back on -- that fallback IS the unweighted mean, arrived at by omission',
    );
  }

  // Rounding is stated and happens once.
  assert.equal(roundRatio(5n, 2n), 3n, 'half away from zero');
  assert.equal(roundRatio(-5n, 2n), -3n, 'in both directions');
  assert.equal(roundRatio(1n, 3n), 0n);
});

test('[3] money aggregates are exact in bigint', () => {
  const store = new RowStore(KEYS);
  // Values chosen so the running total climbs past 2^53 paise, which is where
  // a float reduction stops being able to represent every step.
  const big = 900_719_925_474_099n;
  for (let i = 0; i < 40; i++) {
    store.upsert(i, { pnl: big + BigInt(i), qty: 1, sector: 'X' });
  }
  const rows = [...store.view];

  const exact = aggregate(store, rows, PNL, Agg.Sum);
  assert.equal(exact.ok, true);
  if (!exact.ok) return;

  let asFloat = 0;
  for (const id of rows) {
    const c = store.read(id, 'pnl');
    if (c.present) asFloat += Number(c.value as bigint);
  }
  const floatAsBig = BigInt(Math.round(asFloat));
  const drift = floatAsBig - (exact.value.exact ?? 0n);

  console.log(`    exact bigint sum   ${exact.value.exact}`);
  console.log(`    float reduction    ${floatAsBig}`);
  console.log(`    drift              ${drift} paise`);

  assert.notEqual(
    drift,
    0n,
    'a float reduction of paise drifts once the running total passes 2^53 -- each term is exact and the total is not',
  );
  assert.equal(
    exact.value.exact,
    rows.reduce((a, id) => {
      const c = store.read(id, 'pnl');
      return c.present ? a + (c.value as bigint) : a;
    }, 0n),
    'while bigint addition is exact at every magnitude, which is why the column holds bigint in the first place',
  );
});

test('[4] the parts add to the whole, exactly', () => {
  const store = new RowStore(KEYS);
  const sectors = ['IT', 'BANK', 'AUTO', 'PHARMA'];
  let s = 0x1f2e3d4c;
  for (let i = 0; i < 5000; i++) {
    s = (s * 1664525 + 1013904223) >>> 0;
    store.upsert(i, {
      pnl: BigInt((s % 4_000_000) - 2_000_000),
      qty: (s % 900) + 1,
      price: BigInt((s % 500000) + 1000),
      iv: (s % 400) / 1000 + 0.05,
      sector: sectors[s % sectors.length] ?? 'IT',
    });
  }
  const rows = [...store.view];
  const label = (id: number): string => {
    const c = store.read(id, 'sector');
    return c.present ? String(c.value) : '(none)';
  };
  const groups = groupBy(rows, label, label);
  assert.equal(groups.length, sectors.length);

  const p = pivot(
    store,
    rows,
    groups,
    [PNL, QTY],
    () => Agg.Sum,
    () => undefined,
  );
  const checks = checkAdditive(p, [PNL, QTY]);
  for (const c of checks) {
    console.log(
      `    ${c.columnKey}: parts ${c.parts} vs whole ${c.whole} -> ${c.agrees ? 'agree' : 'DISAGREE'}`,
    );
    assert.equal(
      c.agrees,
      true,
      `${c.columnKey}: the group totals reconcile to the grand total exactly, in paise -- the same invariant the engine checks every tick, applied to the pivot`,
    );
  }
  assert.equal(checks.length, 2);

  // And the intensive column has no whole for the parts to add up to.
  const intensiveChecks = checkAdditive(
    pivot(store, rows, groups, [PRICE], () => Agg.Sum, () => undefined),
    [PRICE],
  );
  assert.equal(
    intensiveChecks.length,
    0,
    'nothing is reconciled for an intensive column, because there is no whole -- reporting one would be inventing it',
  );
});

test('[5] grouping is by key, not by rendered label', () => {
  const store = new RowStore([...KEYS, 'expiry']);
  // Two DIFFERENT expiries that a careless formatter renders identically.
  store.upsert(1, { sector: 'IT', qty: 1 });
  store.upsert(2, { sector: 'IT', qty: 1 });
  store.upsert(3, { sector: 'IT', qty: 1 });

  // Three rows, three DISTINCT expiries -- but two of them fall in the same
  // month, so a month-granular label renders them identically.
  const trueKey = new Map<number, string>([
    [1, '2026-09-24'],
    [2, '2026-09-29'],
    [3, '2026-10-29'],
  ]);
  // A formatter that drops the day-of-month, which is exactly the kind of
  // thing a locale or a "Sep 2026" column heading produces.
  const sloppyLabel = (id: number): string =>
    (trueKey.get(id) ?? '').slice(0, 7);

  const byLabel = groupBy([1, 2, 3], sloppyLabel, sloppyLabel);
  const byKey = groupBy(
    [1, 2, 3],
    (id) => trueKey.get(id) ?? '',
    sloppyLabel,
  );

  console.log(
    `    grouped by rendered label: ${byLabel.length} group(s);  grouped by key: ${byKey.length}`,
  );
  assert.equal(
    byLabel.length,
    2,
    'grouping on the formatted string merges the two September expiries into one row -- three positions become two groups and the 24th and the 29th are totalled together',
  );
  assert.equal(
    byKey.length,
    3,
    'grouping on the key keeps them apart, and the label is free to be whatever reads best -- nothing on screen would have shown the merge, only a group with the wrong number of rows in it',
  );

  // Insertion order is stable, so sections do not reshuffle under the cursor.
  assert.deepEqual(
    groupBy([3, 1, 2], (id) => trueKey.get(id) ?? '', sloppyLabel).map((g) => g.key),
    ['2026-10-29', '2026-09-24', '2026-09-29'],
  );
});

test('[6] an empty aggregate is refused, and a mean says what it divided by', () => {
  const store = new RowStore(KEYS);
  store.upsert(1, { qty: 10, sector: 'IT' }); // no pnl
  store.upsert(2, { qty: 20, sector: 'IT' }); // no pnl
  store.upsert(3, { pnl: 30000n, qty: 30, sector: 'IT' });
  store.upsert(4, { pnl: 10000n, qty: 40, sector: 'IT' });
  const rows = [...store.view];

  const mean = aggregate(store, rows, PNL, Agg.Mean);
  assert.equal(mean.ok, true);
  if (!mean.ok) return;
  assert.equal(mean.value.rows, 4);
  assert.equal(
    mean.value.present,
    2,
    'the mean carries how many rows it actually divided by, alongside how many there were',
  );
  assert.equal(
    mean.value.exact,
    20000n,
    'and divides by the PRESENT count, not the row count -- dividing by 4 would treat the two blanks as zeros, which is a claim that those positions are flat',
  );
  console.log(
    `    mean over ${mean.value.present} of ${mean.value.rows} rows = Rs ${formatPaise(mean.value.exact ?? 0n)};  dividing by all 4 would give Rs ${formatPaise(10000n)}`,
  );

  const empty = aggregate(store, [1, 2], PNL, Agg.Sum);
  assert.equal(empty.ok, false);
  if (!empty.ok) {
    assert.equal(
      empty.problem,
      AggProblem.Empty,
      'and a group where no row has a value is refused rather than totalled to zero -- a zero in a P&L total is a claim that the group is flat',
    );
  }

  const noWeightAtAll = aggregate(store, [1, 2], PRICE, Agg.WeightedMean, 'nonesuch');
  assert.equal(noWeightAtAll.ok, false);
});
