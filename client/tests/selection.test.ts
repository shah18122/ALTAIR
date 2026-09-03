// P11-08 acceptance tests.
//
// Test 1 is the card: "select all" over a filtered grid must mean the rows on
// screen, and the cost of the other reading is measured in rows and rupees.
//
// Test 2: a selection is a set of ids, so it survives a re-sort -- and an
// index range does not, measured in rows retained.
//
// Test 3: the status bar totals the selection through the same aggregate
// rules, and says how many of the selected rows had a value.
//
// Test 4: the shipped keymap has no destructive binding a hand can reach, and
// a deliberately bad map is caught.
//
// Test 5: a keystroke cannot reach the kill switch -- it can only ask.
//
// Test 6: typing in a filter box is not a shortcut.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { RowStore } from '../src/grid/store.ts';
import { ColumnType, Op, applyFilters } from '../src/grid/filter.ts';
import { Agg, Extent } from '../src/grid/aggregate.ts';
import type { AggColumnSpec } from '../src/grid/aggregate.ts';
import { formatPaise } from '../src/protocol.ts';
import { Selection, statusBar } from '../src/grid/selection.ts';
import {
  Action,
  DEFAULT_KEYMAP,
  KeymapOffence,
  SEVERITY_OF,
  Severity,
  chordKey,
  confirm,
  resolve,
  validateKeymap,
} from '../src/grid/keymap.ts';
import type { Binding } from '../src/grid/keymap.ts';

const PNL: AggColumnSpec = {
  key: 'pnl',
  label: 'P&L',
  type: ColumnType.Money,
  extent: Extent.Extensive,
};
const NOTIONAL: AggColumnSpec = {
  key: 'notional',
  label: 'Notional',
  type: ColumnType.Money,
  extent: Extent.Extensive,
};
const IV: AggColumnSpec = {
  key: 'iv',
  label: 'IV',
  type: ColumnType.Real,
  extent: Extent.Intensive,
};
const KEYS = ['pnl', 'notional', 'iv', 'qty'];

function book(n: number): RowStore {
  const store = new RowStore(KEYS);
  let s = 0x7ea51234;
  for (let i = 0; i < n; i++) {
    s = (s * 1664525 + 1013904223) >>> 0;
    store.upsert(i, {
      pnl: BigInt((s % 2_000_000) - 1_000_000),
      notional: BigInt((s % 4_000_000) + 100_000),
      iv: (s % 300) / 1000 + 0.08,
      qty: (s % 200) + 1,
    });
  }
  return store;
}

test('[1] select-all means the rows you can see', () => {
  const N = 5000;
  const store = book(N);
  // An ordinary filter: the positions that are meaningfully up.
  const view = applyFilters(store, [
    { column: 'pnl', op: Op.Greater, operand: 985_000n },
  ]);
  assert.ok(view.length > 0 && view.length < 100, `filtered to ${view.length} rows`);

  const sel = new Selection();
  sel.selectAll(view);

  const unseen = N - view.length;
  let unseenNotional = 0n;
  const visible = new Set(view);
  for (const id of store.view) {
    if (visible.has(id)) continue;
    const c = store.read(id, 'notional');
    if (c.present) unseenNotional += c.value as bigint;
  }

  console.log(
    `    filtered to ${view.length} of ${N}: select-all selects ${sel.size}`,
  );
  console.log(
    `    reaching past the filter would add ${unseen} rows (${(unseen / view.length).toFixed(0)}x) carrying Rs ${formatPaise(unseenNotional)} of notional the user never looked at`,
  );

  assert.equal(
    sel.size,
    view.length,
    'select-all selects exactly the filtered view -- if you cannot see it, you have not selected it',
  );
  assert.ok(
    unseen > view.length * 10,
    'while selecting the underlying set would pick up orders of magnitude more rows, and the only warning would have been a count in the status bar, in a number nobody reads before pressing a key they have pressed a thousand times',
  );

  // A narrowed filter drops selected rows that are no longer visible.
  const narrower = applyFilters(store, [
    { column: 'pnl', op: Op.Greater, operand: 995_000n },
  ]);
  const dropped = sel.retainVisible(narrower);
  console.log(`    narrowing the filter dropped ${dropped} rows from the selection`);
  assert.ok(dropped > 0);
  assert.equal(
    sel.size,
    narrower.length,
    'a selection that outlived the filter it was made under is a selection of invisible rows -- the same hazard, arrived at from the other direction',
  );
});

test('[2] a selection survives a re-sort; an index range does not', () => {
  const store = book(4096);
  store.sortBy(store.columnComparator('pnl', false));
  const view0 = [...store.view];

  const sel = new Selection();
  const anchorId = view0[1000];
  const endId = view0[1099];
  assert.ok(anchorId !== undefined && endId !== undefined);
  sel.set(anchorId);
  sel.extendTo(endId, view0);
  assert.equal(sel.size, 100, 'a shift-click selects a hundred rows');
  const selectedIds = new Set(sel.ids());

  // The index range the naive implementation would have stored.
  const indexRange: readonly [number, number] = [1000, 1099];

  // Ordinary churn, then a re-sort.
  for (let i = 0; i < 4096; i += 3) {
    store.patchCell(i, 'pnl', BigInt(((i * 41) % 2_000_000) - 1_000_000));
  }
  store.sortBy(store.columnComparator('pnl', false));
  const view1 = [...store.view];

  const stillById = sel.ids().filter((id) => selectedIds.has(id)).length;
  const nowInRange = view1.slice(indexRange[0], indexRange[1] + 1);
  const retainedByIndex = nowInRange.filter((id) => selectedIds.has(id)).length;

  console.log(
    `    after one re-sort: by id ${stillById} of 100 original rows still selected; by index range ${retainedByIndex} of 100`,
  );

  assert.equal(
    stillById,
    100,
    'ids are unaffected by a re-sort, so the selection is still the hundred instruments the user picked',
  );
  assert.ok(
    retainedByIndex < 20,
    'while the same index range now covers almost entirely different instruments -- the highlight does not move, so nothing on screen changes; the rows underneath it do',
  );

  // The anchor is an id too, so a second shift-click extends from the right
  // place rather than from whatever now sits at that index.
  assert.equal(sel.anchor, anchorId);
  const further = view1[view1.indexOf(anchorId) + 5];
  if (further !== undefined) {
    sel.extendTo(further, view1);
    assert.equal(sel.size, 6, 'and a second shift-click extends from the anchor row, not from an index');
  }

  // An anchor filtered away starts a new range rather than selecting a span
  // whose ends the user cannot see.
  const tiny = view1.slice(0, 3);
  const fresh = new Selection();
  fresh.set(view1[2000]!);
  fresh.extendTo(tiny[0]!, tiny);
  assert.equal(fresh.size, 1);
});

test('[3] the status bar totals the selection, under the same rules', () => {
  const store = book(500);
  const view = [...store.view];
  const sel = new Selection();
  sel.selectAll(view.slice(0, 25));

  const bar = statusBar(store, sel, view, store.size, [PNL, NOTIONAL, IV], (spec) =>
    spec === IV ? Agg.WeightedMean : Agg.Sum,
  );

  assert.equal(bar.selectedRows, 25);
  assert.equal(bar.visibleRows, 500);
  assert.equal(
    bar.totalRows,
    500,
    'selected, visible and total are all reported -- "25 selected" means something different when the grid holds 25 rows than when it holds 5,000',
  );

  const pnlEntry = bar.entries.find((e) => e.columnKey === 'pnl');
  assert.ok(pnlEntry !== undefined);
  assert.equal(pnlEntry.result.ok, true);
  if (pnlEntry.result.ok) {
    console.log(
      `    ${bar.selectedRows} of ${bar.visibleRows} rows selected, P&L Rs ${formatPaise(pnlEntry.result.value.exact ?? 0n)} over ${pnlEntry.result.value.present} rows with a value`,
    );
    assert.equal(pnlEntry.result.value.present, 25);
  }

  // The status bar is a pivot with one group, so it inherits P11-05's rules.
  const ivSum = statusBar(store, sel, view, store.size, [IV], () => Agg.Sum);
  assert.equal(
    ivSum.entries[0]?.result.ok,
    false,
    'and it cannot sum an implied volatility either -- the status bar is the most-read number on the screen, so it is the last place to make an exception',
  );
});

test('[4] no destructive binding is within reach', () => {
  const offences = validateKeymap(DEFAULT_KEYMAP);
  console.log(
    `    shipped keymap: ${DEFAULT_KEYMAP.length} bindings, ${offences.length} offences`,
  );
  assert.deepEqual(
    offences,
    [],
    'the shipped map has no destructive binding without two modifiers, none on a navigation key, and no duplicated chord -- checked before the app starts, because a destructive binding is a design error rather than a runtime condition',
  );

  const kill = DEFAULT_KEYMAP.find((b) => b.action === Action.KillSwitch);
  assert.ok(kill !== undefined);
  console.log(`    kill switch is bound to ${chordKey(kill.chord)}`);

  // Deliberately bad maps, each caught by name.
  const bad: ReadonlyArray<readonly [Binding, KeymapOffence]> = [
    [{ chord: { key: 'k' }, action: Action.KillSwitch }, KeymapOffence.DestructiveBare],
    [
      { chord: { key: 'k', ctrl: true }, action: Action.KillSwitch },
      KeymapOffence.DestructiveTooEasy,
    ],
    [
      { chord: { key: 'Escape', ctrl: true, shift: true }, action: Action.KillSwitch },
      KeymapOffence.DestructiveNearNavigation,
    ],
  ];
  for (const [binding, expected] of bad) {
    const found = validateKeymap([binding]);
    assert.ok(
      found.some((o) => o.offence === expected),
      `${chordKey(binding.chord)} is rejected as ${expected}`,
    );
  }

  const dup = validateKeymap([
    { chord: { key: 'a', ctrl: true }, action: Action.SelectAll },
    { chord: { key: 'a', ctrl: true }, action: Action.CopySelection },
  ]);
  assert.ok(dup.some((o) => o.offence === KeymapOffence.Duplicate));
});

test('[5] a keystroke can ask for the kill switch, never perform it', () => {
  const kill = DEFAULT_KEYMAP.find((b) => b.action === Action.KillSwitch);
  assert.ok(kill !== undefined);

  const r = resolve(DEFAULT_KEYMAP, kill.chord, false);
  assert.equal(
    r.kind,
    'confirm',
    'the destructive chord resolves to a CONFIRMATION REQUEST, not to an action -- there is no code path from a keydown to the kill switch, which is a stronger statement than a modal a fast hand can dismiss',
  );
  if (r.kind === 'confirm') assert.equal(r.action, Action.KillSwitch);

  assert.equal(
    confirm(Action.KillSwitch, false).kind,
    'none',
    'and a dismissed confirmation does nothing',
  );
  assert.equal(confirm(Action.KillSwitch, true).kind, 'act');
  assert.equal(
    confirm(Action.SelectAll, true).kind,
    'none',
    'while confirm is not a general-purpose way to run an action -- it only completes a destructive one',
  );

  // Everything else acts directly.
  const nav = resolve(DEFAULT_KEYMAP, { key: 'ArrowDown' }, false);
  assert.equal(nav.kind, 'act');

  // And the client has no vocabulary for placing an order at all -- there is
  // no Action for it, and P11-01's ClientMsg has no message for it. Read off
  // the exported table rather than a copy of it.
  const destructive = [...SEVERITY_OF.entries()].filter(
    ([, sev]) => sev === Severity.Destructive,
  );
  console.log(
    `    ${SEVERITY_OF.size} declared actions, ${destructive.length} destructive: the kill switch, which can only flatten`,
  );
  assert.equal(destructive.length, 1);
  assert.equal(destructive[0]?.[0], Action.KillSwitch);
});

test('[6] typing in a filter box is not a shortcut', () => {
  const selectAll = { key: 'a', ctrl: true };
  assert.equal(resolve(DEFAULT_KEYMAP, selectAll, false).kind, 'act');
  assert.equal(
    resolve(DEFAULT_KEYMAP, selectAll, true).kind,
    'none',
    'with an editable field focused, a global shortcut does not fire -- a handler that acts on Ctrl+A while the user is selecting text in a filter box is the bug every keyboard-driven grid ships once',
  );

  const kill = DEFAULT_KEYMAP.find((b) => b.action === Action.KillSwitch);
  assert.ok(kill !== undefined);
  assert.equal(
    resolve(DEFAULT_KEYMAP, kill.chord, true).kind,
    'none',
    'and that includes the destructive one, which is the case that matters',
  );

  assert.equal(
    resolve(DEFAULT_KEYMAP, { key: 'Escape' }, true).kind,
    'act',
    'Escape still resolves while editing, because it is how you get out of the box',
  );
});
