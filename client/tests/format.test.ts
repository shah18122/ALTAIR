// P11-07 acceptance tests.
//
// Test 1 is the card: a sparkline that decimates aliases, and at some panel
// widths draws a flat line through a series that never stopped moving. The
// Nyquist argument the engine is built on, at the width of a table cell.
//
// Test 2: colour is never the only thing carrying the sign, and the up/down
// convention is not universal.
//
// Test 3: a heatmap normalised over the visible window changes a cell's colour
// as you scroll, while the value does not move.
//
// Test 4: one outlier flattens a linear scale, measured in buckets occupied.
//
// Test 5: a flash keys on a changed value, not on a repaint.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { ABSENT, cell } from '../src/grid/store.ts';
import {
  FlashKind,
  FlashTracker,
  ScaleKind,
  SignConvention,
  Tone,
  formatSigned,
  heatBucket,
  heatScale,
} from '../src/grid/format.ts';
import {
  decimate,
  drawnRange,
  envelope,
  rangeOf,
} from '../src/grid/sparkline.ts';

test('[1] a decimating sparkline aliases, and can draw a flat line', () => {
  // An ordinary bid-ask bounce: 300 ticks alternating by 50 paise. This is
  // precisely the signal two-scale realised variance exists to see past, so
  // it is not a contrived waveform -- it is the most common one there is.
  const N = 300;
  const series: number[] = [];
  for (let i = 0; i < N; i++) series.push(i % 2 === 0 ? 18000 : 18050);

  const trueRange = rangeOf(series);
  assert.equal(trueRange, 50, 'the series moves 50 paise, in every single tick');

  // WHICH WIDTH MATTERS, and that is worth stating rather than picking one
  // and hoping. Decimation at width w uses stride n/w. An ODD stride walks
  // both phases of a period-2 signal and survives; an EVEN stride lands on
  // the same phase every time and collapses. So 60 px (stride 5) happens to
  // look fine and 50 px (stride 6) draws a flat line -- which is worse than
  // if it always failed, because it works until it doesn't.
  const WIDTH = 50;
  const sampled = decimate(series, WIDTH);
  const sampledRange = rangeOf(sampled);
  const luckyWidth = rangeOf(decimate(series, 60));

  const env = envelope(series, WIDTH);
  assert.notEqual(env, null);
  if (env === null) return;
  const envRange = drawnRange(env);

  console.log(
    `    ${N} ticks, true range ${trueRange} paise:  decimated at ${WIDTH} px (stride 6) draws ${sampledRange};  at 60 px (stride 5) it draws ${luckyWidth};  envelope draws ${envRange}`,
  );

  assert.equal(
    sampledRange,
    0,
    'an even stride hits the SAME phase of a period-2 oscillation every time, so the sparkline is perfectly flat through a series that never stopped moving -- nothing errors and the chart looks calm',
  );
  assert.equal(
    luckyWidth,
    trueRange,
    'while an odd stride walks both phases and looks correct -- so the bug is not reliably reproducible by resizing a panel, which is the worst way for a bug to behave',
  );
  assert.equal(
    envRange,
    trueRange,
    'the envelope keeps every tick, because each pixel column takes the min and max of what fell in it rather than one representative -- nothing is sampled, so nothing can alias',
  );

  // Every tick is accounted for. That is what makes it a summary rather than
  // a subset.
  const counted = env.columns.reduce((a, c) => a + c.ticks, 0);
  assert.equal(counted, N, 'every one of the 300 ticks lands in exactly one column');
  assert.ok(sampled.length < N, `while decimation kept only ${sampled.length}`);

  // Sweep the width to show this is not one unlucky number.
  let flatWidths = 0;
  for (let w = 2; w <= 150; w++) {
    if (rangeOf(decimate(series, w)) === 0) flatWidths++;
  }
  console.log(
    `    across widths 2..150 px, decimation draws a completely flat line at ${flatWidths} of them`,
  );
  assert.ok(
    flatWidths >= 10,
    'it collapses at 11 widths in that range, not one unlucky pixel count -- exactly the widths that divide 300 with an even quotient, which is a property of the panel size and the tick count and nothing a reviewer would think to vary',
  );

  // The envelope never collapses.
  let envFlat = 0;
  for (let w = 2; w <= 150; w++) {
    const e = envelope(series, w);
    if (e !== null && drawnRange(e) === 0) envFlat++;
  }
  assert.equal(envFlat, 0, 'the envelope collapses at no width at all');
});

test('[2] colour is never the only thing carrying the sign', () => {
  const up = formatSigned(cell(4000000n));
  const down = formatSigned(cell(-4000000n));
  const flat = formatSigned(cell(0n));
  const none = formatSigned(ABSENT);

  console.log(
    `    up "${up.text}"  down "${down.text}"  flat "${flat.text}"  absent "${none.text}"`,
  );

  assert.equal(up.text, '+40,000.00', 'a gain carries an explicit +');
  assert.equal(down.text, '-40,000.00', 'and a loss an explicit -');
  assert.notEqual(
    up.text,
    down.text,
    'so the two are distinguishable with no colour at all -- which matters to roughly one man in twelve, and those two cells are the difference between a good day and a margin call',
  );
  assert.equal(
    up.text.replace('+', '').length,
    down.text.replace('-', '').length,
    'and they differ ONLY in the sign character, so the columns still align',
  );
  assert.equal(flat.tone, Tone.Neutral);
  assert.equal(
    none.text,
    '',
    'and an absent cell is blank rather than +0.00, which would be a claim that the position is flat',
  );
  assert.equal(none.tone, Tone.Absent);

  // The convention is not universal, and swapping it swaps the colours
  // without touching the text.
  const upRed = formatSigned(cell(4000000n), SignConvention.RedUp);
  assert.equal(
    upRed.text,
    up.text,
    'switching to the East Asian convention leaves the TEXT identical',
  );
  assert.notEqual(
    upRed.colorVar,
    up.colorVar,
    'and swaps only the colour -- a hard-coded palette does not merely fail a colour-blind reader, it inverts every cell for a reader in Shanghai or Tokyo',
  );
  assert.equal(upRed.colorVar, '--cell-red');
  assert.equal(up.colorVar, '--cell-green');
});

test('[3] a window-normalised heatmap changes colour as you scroll', () => {
  const N = 5000;
  const values: bigint[] = [];
  let s = 0x51ed7a1c;
  for (let i = 0; i < N; i++) {
    s = (s * 1664525 + 1013904223) >>> 0;
    values.push(BigInt((s % 400_000) - 200_000));
  }
  // The cell we watch. Its value never changes.
  const watched = values[2500] ?? 0n;

  // A window is small, so its extremes move a lot as it slides. Every scroll
  // position from which the watched row is visible at all:
  const WINDOW = 20;
  const seen = new Set<number>();
  for (let start = 2500 - WINDOW + 1; start <= 2500; start++) {
    const windowValues = values.slice(start, start + WINDOW);
    const wScale = heatScale(windowValues, ScaleKind.Linear);
    if (wScale === null) continue;
    seen.add(heatBucket(wScale, watched));
  }
  console.log(
    `    one cell, value fixed at ${watched}: ${seen.size} different colour buckets across scroll positions when the scale is built from the visible window`,
  );
  assert.ok(
    seen.size > 1,
    'the same value takes different colours at different scroll positions, because the extremes around it changed -- nothing flickers, nothing is out of range, the colour just stops meaning anything fixed',
  );

  // Built from the whole filtered set instead.
  const full = heatScale(values, ScaleKind.Linear);
  assert.notEqual(full, null);
  if (full === null) return;
  const fixed = heatBucket(full, watched);
  for (let start = 0; start + WINDOW <= N; start += 373) {
    assert.equal(
      heatBucket(full, watched),
      fixed,
      'while a scale built once from the whole filtered set gives the same bucket at every scroll position -- the window decides what is DRAWN, never what a colour MEANS',
    );
  }
  assert.equal(
    full.sampleSize,
    N,
    'and the scale carries how many values it was built from, so a window-built scale is visible as a number rather than a suspicion',
  );
});

test('[4] one outlier flattens a linear scale', () => {
  const N = 5000;
  const values: bigint[] = [];
  let s = 0x0badc0de;
  for (let i = 0; i < N; i++) {
    s = (s * 1664525 + 1013904223) >>> 0;
    values.push(BigInt((s % 100_000) - 50_000));
  }
  // One position down Rs 50 lakh. Entirely ordinary, and it owns the range.
  values[0] = -500_000_000n;

  const linear = heatScale(values, ScaleKind.Linear, 9);
  const robust = heatScale(values, ScaleKind.Percentile, 9, 0.02);
  assert.notEqual(linear, null);
  assert.notEqual(robust, null);
  if (linear === null || robust === null) return;

  const countBuckets = (scale: typeof linear): Map<number, number> => {
    const m = new Map<number, number>();
    for (const v of values) {
      const b = heatBucket(scale, v);
      m.set(b, (m.get(b) ?? 0) + 1);
    }
    return m;
  };
  const lin = countBuckets(linear);
  const rob = countBuckets(robust);
  const linBiggest = Math.max(...lin.values());

  console.log(
    `    linear: ${lin.size} of 9 buckets used, ${linBiggest} of ${N} rows in one of them`,
  );
  console.log(`    percentile: ${rob.size} of 9 buckets used`);

  assert.ok(
    linBiggest > N * 0.9,
    'a single outlier takes the whole dark end and squeezes every other row into one bucket -- the column is a solid block of colour that distinguishes nothing',
  );
  assert.ok(
    rob.size > lin.size,
    'percentile bounds spread the ordinary rows across the range',
  );
  assert.equal(
    heatBucket(robust, -500_000_000n),
    0,
    'and the outlier is STILL the darkest cell, because it clamps rather than being excluded -- it just stops deciding the colour of everything else',
  );
});

test('[5] a flash keys on a changed value, not on a repaint', () => {
  const f = new FlashTracker(300);

  assert.equal(
    f.observe('r1/pnl', 100n, 1000),
    FlashKind.None,
    'the first value seen is not a change -- otherwise the whole grid flashes on the first snapshot',
  );
  assert.equal(f.observe('r1/pnl', 150n, 1010), FlashKind.Up);
  assert.equal(f.observe('r1/pnl', 120n, 1020), FlashKind.Down);
  assert.equal(
    f.observe('r1/pnl', 120n, 1030),
    FlashKind.None,
    'and the same value arriving again is not a change: a grid repaints for a re-sort, a scroll, a resize, and flashing on repaint trains the eye to ignore flashes -- the opposite of what a flash is for',
  );

  assert.equal(f.activeAt('r1/pnl', 1100), true, 'the flash is live for its duration');
  assert.equal(f.activeAt('r1/pnl', 1400), false, 'and then it is over');
  assert.equal(f.activeAt('never-seen', 1000), false);

  // A flash longer than the update interval never finishes.
  const interval = 250;
  const maxUseful = FlashTracker.maxUsefulDurationMs(interval);
  console.log(
    `    at a ${interval} ms update interval, the longest flash that still finishes is ${maxUseful} ms`,
  );
  assert.ok(
    maxUseful < interval,
    'a flash longer than the gap between updates never ends, so the cell is permanently highlighted and the highlight stops carrying information',
  );

  const tooLong = new FlashTracker(interval * 2);
  tooLong.observe('r/pnl', 1n, 0);
  tooLong.observe('r/pnl', 2n, 0);
  assert.equal(
    tooLong.activeAt('r/pnl', interval),
    true,
    'which is exactly what an over-long duration produces: still lit when the next update lands',
  );
});
