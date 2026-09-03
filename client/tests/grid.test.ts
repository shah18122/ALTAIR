// P11-03 acceptance tests.
//
// Test 1 is the card: a patch addressed by index lands on the wrong
// instrument once anything sorts. Measured, not asserted -- the count of
// wrong landings is the finding, and so is the count of accidentally-right
// ones, because those are what make it survive a spot check.
//
// Test 2: a million rows is taller than a browser will scroll, and the naive
// spacer makes a measured fraction of the grid unreachable.
//
// Test 3: the scaled mapping is still finer than one row here, and the test
// finds the row count where it stops being.
//
// Test 4: the window is O(visible) -- its cost does not move when the row
// count goes up by three orders of magnitude.
//
// Test 5: an absent cell is not a zero, in reading and in sorting.
//
// Test 6: the window clamps at both ends and under overscroll.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { ABSENT, PatchOutcome, RowStore } from '../src/grid/store.ts';
import type { RowId } from '../src/grid/store.ts';
import {
  SAFE_MAX_SPACER_PX,
  computeWindow,
  rowsPerScrollPixel,
  scrollTopForRow,
  unreachableRows,
  wantedHeightPx,
} from '../src/grid/viewport.ts';

const COLUMNS = ['symbol', 'qty', 'pnl'] as const;

function seededBook(n: number): RowStore {
  const store = new RowStore([...COLUMNS]);
  let s = 0x9e3779b9;
  for (let i = 0; i < n; i++) {
    s = (s * 1664525 + 1013904223) >>> 0;
    store.upsert(i, {
      symbol: `SYM${String(i).padStart(6, '0')}`,
      qty: (s % 500) + 1,
      pnl: BigInt((s % 2000000) - 1000000),
    });
  }
  return store;
}

test('[1] an index-addressed patch lands on the wrong instrument', () => {
  const N = 4096;
  const store = seededBook(N);

  // The trader's sort: by P&L, descending.
  store.sortBy(store.columnComparator('pnl', false));

  // The server picks 512 rows to patch and names them BOTH ways: by the id,
  // and by where they sat in the view at that moment.
  const targets: Array<{ id: RowId; index: number; newPnl: bigint }> = [];
  for (let k = 0; k < 512; k++) {
    const index = (k * 7) % N;
    const id = store.view[index];
    assert.ok(id !== undefined);
    targets.push({ id, index, newPnl: BigInt(k) * 1000n });
  }

  // Between the server choosing and the client applying, P&L moved and the
  // grid re-sorted. This is not a rare interleaving; it is every second in a
  // grid sorted by a live number.
  for (let i = 0; i < N; i += 3) {
    store.patchCell(i, 'pnl', BigInt(((i * 37) % 2000000) - 1000000));
  }
  store.sortBy(store.columnComparator('pnl', false));

  let wrongRow = 0;
  let accidentallyRight = 0;
  for (const t of targets) {
    const nowAtIndex = store.view[t.index];
    if (nowAtIndex === t.id) accidentallyRight++;
    else wrongRow++;
  }

  const pct = (100 * wrongRow) / targets.length;
  console.log(
    `    ${targets.length} patches applied by INDEX after one re-sort: ${wrongRow} land on the wrong row (${pct.toFixed(1)}%), ${accidentallyRight} happen to be right`,
  );

  assert.ok(
    wrongRow > targets.length * 0.9,
    'addressing a patch by its position in the view puts almost every value on an instrument that never traded at it -- nothing throws, the cell simply updates',
  );
  assert.ok(
    accidentallyRight > 0,
    'and a few land correctly by chance, which is why spot-checking a handful of cells does not find this',
  );

  // The same patches, addressed by identity.
  let applied = 0;
  for (const t of targets) {
    if (store.patchCell(t.id, 'pnl', t.newPnl) === PatchOutcome.Applied) {
      applied++;
    }
  }
  assert.equal(applied, targets.length);
  for (const t of targets) {
    const c = store.read(t.id, 'pnl');
    assert.equal(c.present && c.value === t.newPnl, true);
  }
  console.log(
    '    the same patches addressed by RowId: all 512 on the row they named, re-sorts notwithstanding',
  );

  // And a patch for a row the client has never seen is reported, not invented.
  assert.equal(
    store.patchCell(999_999, 'pnl', 1n),
    PatchOutcome.UnknownRow,
    'a patch for an unknown row means the snapshot is stale -- that is a gap, not a row to create',
  );
  assert.equal(store.patchCell(0, 'nonesuch', 1n), PatchOutcome.UnknownColumn);
});

test('[2] a million rows is taller than a browser will scroll', () => {
  const ROWS = 1_000_000;
  const ROW_PX = 24;

  const wanted = wantedHeightPx(ROWS, ROW_PX);
  const lost = unreachableRows(ROWS, ROW_PX);
  const lostPx = wanted - SAFE_MAX_SPACER_PX;

  console.log(`    wanted spacer height   ${wanted.toLocaleString()} px`);
  console.log(`    usable (2^24 cap)      ${SAFE_MAX_SPACER_PX.toLocaleString()} px`);
  console.log(
    `    unreachable            ${lostPx.toLocaleString()} px = ${lost.toLocaleString()} rows = ${((100 * lost) / ROWS).toFixed(1)}% of the grid`,
  );

  assert.ok(
    wanted > SAFE_MAX_SPACER_PX,
    'the naive spacer wants to be taller than any browser will make an element',
  );
  assert.ok(
    lost > 0,
    'so with a naive spacer the tail of the grid cannot be reached by scrolling at all -- nothing errors, the scrollbar just stops',
  );

  // With scaling, every row is reachable.
  const w = computeWindow({
    scrollTopPx: SAFE_MAX_SPACER_PX,
    viewportHeightPx: 900,
    rowHeightPx: ROW_PX,
    totalRows: ROWS,
    overscanRows: 4,
  });
  assert.equal(w.scaled, true, 'the mapping reports that it had to scale');
  assert.equal(
    w.spacerHeightPx,
    SAFE_MAX_SPACER_PX,
    'and the spacer is set to the cap rather than to a height the browser will silently refuse',
  );
  assert.equal(
    w.lastRow,
    ROWS,
    'scrolling to the bottom of the scaled spacer reaches the LAST row -- the obvious scrollTop*(wanted/cap) multiply stops short of it by almost a full viewport, which is the same unreachable-tail bug in miniature',
  );

  // And the go-to-row control lands where it says.
  const target = 987_654;
  const back = computeWindow({
    scrollTopPx: scrollTopForRow(target, ROW_PX, ROWS, 900),
    viewportHeightPx: 900,
    rowHeightPx: ROW_PX,
    totalRows: ROWS,
    overscanRows: 0,
  });
  assert.ok(
    Math.abs(back.firstRow - target) <= 1,
    'and scrollTopForRow puts the requested row at the top, within a row',
  );
});

test('[3] the scaled mapping is finer than a row here -- and here is where it stops', () => {
  const ROW_PX = 24;
  const atMillion = rowsPerScrollPixel(1_000_000, ROW_PX);
  console.log(
    `    at 1,000,000 rows: ${atMillion.toFixed(4)} rows per scroll pixel`,
  );
  assert.ok(
    atMillion < 1,
    'one scroll pixel is worth less than one row, so scrolling can still land on any row it wants',
  );

  // Find where it stops. Scaling is coarser than a row once
  // rows * rowHeight / cap > rowHeight, i.e. rows > cap.
  let breaks = 0;
  for (let rows = 1_000_000; rows <= 40_000_000; rows += 250_000) {
    if (rowsPerScrollPixel(rows, ROW_PX) >= 1) {
      breaks = rows;
      break;
    }
  }
  console.log(
    `    one scroll pixel first exceeds one row at ${breaks.toLocaleString()} rows`,
  );
  assert.ok(
    breaks > 16_000_000,
    'which is above 16 million rows -- so the resolution cost of scaling is not a real cost at this grid size, and saying so is worth more than a hedge',
  );
  assert.ok(
    breaks > 0,
    'but it is a real limit and it has a number, so a busy year of audit rows meets a known wall rather than a surprise -- the answer there is P11-08 go-to-row, not a taller spacer',
  );
});

test('[4] the window is O(visible), not O(total)', () => {
  const ROW_PX = 24;
  const REPS = 200_000;

  const timeAt = (totalRows: number): number => {
    const t0 = performance.now();
    let acc = 0;
    for (let i = 0; i < REPS; i++) {
      const w = computeWindow({
        scrollTopPx: (i * 97) % 100000,
        viewportHeightPx: 900,
        rowHeightPx: ROW_PX,
        totalRows,
        overscanRows: 4,
      });
      acc += w.lastRow - w.firstRow;
    }
    assert.ok(acc > 0);
    return performance.now() - t0;
  };

  const small = timeAt(1_000);
  const large = timeAt(1_000_000);
  console.log(
    `    ${REPS.toLocaleString()} window computations: ${small.toFixed(1)} ms at 1e3 rows, ${large.toFixed(1)} ms at 1e6 rows`,
  );
  assert.ok(
    large < small * 4,
    'a thousandfold more rows does not cost meaningfully more per frame, because the window is arithmetic on five numbers and touches no row',
  );

  // And the slice is bounded regardless of row count.
  const w = computeWindow({
    scrollTopPx: 1000,
    viewportHeightPx: 900,
    rowHeightPx: ROW_PX,
    totalRows: 1_000_000,
    overscanRows: 4,
  });
  assert.ok(
    w.lastRow - w.firstRow < 60,
    'the rendered slice stays under sixty rows at a million, which is what fits in a 16.7 ms frame',
  );
});

test('[5] an absent cell is not a zero', () => {
  const store = new RowStore([...COLUMNS]);
  store.upsert(1, { symbol: 'RELIANCE', qty: 100 });
  store.upsert(2, { symbol: 'TCS', qty: 50, pnl: 0n });
  store.upsert(3, { symbol: 'INFY', qty: 25, pnl: -500000n });

  const noPnl = store.read(1, 'pnl');
  assert.equal(
    noPnl.present,
    false,
    'a cell that was never set reads as explicitly absent, so there is no undefined for a caller to turn into 0 with ?? ',
  );
  assert.deepEqual(noPnl, ABSENT);

  const flat = store.read(2, 'pnl');
  assert.equal(
    flat.present && flat.value === 0n,
    true,
    'while a genuinely flat position reads as the value zero -- these are different statements about the world and the type keeps them apart',
  );

  // Sorting: absent goes last in BOTH directions.
  store.sortBy(store.columnComparator('pnl', true));
  assert.equal(
    store.view[store.view.length - 1],
    1,
    'ascending, the row with no P&L sorts last',
  );
  store.sortBy(store.columnComparator('pnl', false));
  assert.equal(
    store.view[store.view.length - 1],
    1,
    'and descending it sorts last too -- treating absent as zero would file it among the genuinely flat positions, which is where nobody would look for it',
  );

  store.clearCell(3, 'pnl');
  assert.equal(
    store.read(3, 'pnl').present,
    false,
    'and a cell can be cleared back to absent, which a numeric column with no null could not express',
  );
});

test('[6] the window clamps at both ends and under overscroll', () => {
  const base = {
    viewportHeightPx: 480,
    rowHeightPx: 24,
    totalRows: 100,
    overscanRows: 3,
  };

  const top = computeWindow({ ...base, scrollTopPx: 0 });
  assert.equal(top.firstRow, 0, 'no negative first row from overscan at the top');

  const bottom = computeWindow({ ...base, scrollTopPx: 1_000_000 });
  assert.equal(
    bottom.lastRow,
    100,
    'and no last row past the end from a scrollTop the platform reported during rubber-band overscroll',
  );
  assert.ok(bottom.firstRow < bottom.lastRow);

  const empty = computeWindow({ ...base, totalRows: 0, scrollTopPx: 0 });
  assert.deepEqual(
    [empty.firstRow, empty.lastRow, empty.spacerHeightPx],
    [0, 0, 0],
    'an empty grid produces an empty window rather than a one-row window of nothing',
  );

  const store = seededBook(10);
  assert.deepEqual(store.window(-5, 3), store.window(0, 3));
  assert.equal(store.window(8, 500).length, 2);
  assert.deepEqual(store.window(5, 5), []);
});
