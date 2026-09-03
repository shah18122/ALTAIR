// P11-13 acceptance tests.
//
// Test 1 is the card: a centred overlay is correlated with a move that has
// not happened yet, because it contains it. Look-ahead from the outside looks
// like a signal that turns just before the market does.
//
// Test 2: the window offers no way to reach past the cursor.
//
// Test 3: a flag renders at the time it happened, not when it arrived.
//
// Test 4: the cone widens as sqrt(h); a linear cone is wrong at both ends,
// and wrong in the dangerous direction at the near end.
//
// Test 5: the cone says how many bars it rests on.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
  FlagKind,
  ReplayWindow,
  coneFrom,
  coneLinearWidth,
  deliveryLagNs,
  visibleFlags,
} from '../src/chart/replay.ts';
import type { Bar, FlagMarker } from '../src/chart/replay.ts';

const T0 = 1_788_393_600_000_000_000n;
const STEP = 60_000_000_000n; // one minute

function session(n: number): Bar[] {
  const bars: Bar[] = [];
  let s = 0x3ec0ded1;
  let v = 24_000;
  for (let i = 0; i < n; i++) {
    s = (s * 1664525 + 1013904223) >>> 0;
    // A random walk with alternating volatility regimes -- clustering, which
    // is what volatility actually does and what makes a centred window so
    // effective at "predicting" the next bar.
    const scale = Math.floor(i / 50) % 2 === 0 ? 3 : 12;
    v += (((s % 2001) - 1000) / 1000) * scale;
    bars.push({ timeNs: T0 + BigInt(i) * STEP, value: v });
  }
  return bars;
}

/** Trailing average: only bars at or before i. What a replay may draw. */
function trailingMean(values: readonly number[], i: number, w: number): number {
  let sum = 0;
  let n = 0;
  for (let j = Math.max(0, i - w + 1); j <= i; j++) {
    const v = values[j];
    if (v === undefined) continue;
    sum += v;
    n++;
  }
  return n === 0 ? 0 : sum / n;
}

/** CENTRED average: bars on both sides of i. The single most common chart
 *  look-ahead, because it is what "smooth this series" means in every
 *  plotting library, and because it looks better. */
function centredMean(values: readonly number[], i: number, w: number): number {
  const half = Math.floor(w / 2);
  let sum = 0;
  let n = 0;
  for (let j = Math.max(0, i - half); j <= Math.min(values.length - 1, i + half); j++) {
    const v = values[j];
    if (v === undefined) continue;
    sum += v;
    n++;
  }
  return n === 0 ? 0 : sum / n;
}

function correlation(xs: readonly number[], ys: readonly number[]): number {
  const n = Math.min(xs.length, ys.length);
  if (n < 2) return 0;
  let sx = 0;
  let sy = 0;
  for (let i = 0; i < n; i++) {
    sx += xs[i] ?? 0;
    sy += ys[i] ?? 0;
  }
  const mx = sx / n;
  const my = sy / n;
  let cov = 0;
  let vx = 0;
  let vy = 0;
  for (let i = 0; i < n; i++) {
    const dx = (xs[i] ?? 0) - mx;
    const dy = (ys[i] ?? 0) - my;
    cov += dx * dy;
    vx += dx * dx;
    vy += dy * dy;
  }
  return vx > 0 && vy > 0 ? cov / Math.sqrt(vx * vy) : 0;
}

test('[1] a centred overlay knows the next bar, and it looks better for it', () => {
  const bars = session(4000);
  const values = bars.map((b) => b.value);
  const W = 21;

  // At each bar, how far the overlay sits above the price -- and what the
  // price does NEXT. An overlay that contains no future information cannot
  // be correlated with a return that has not happened yet.
  const trailingSignal: number[] = [];
  const centredSignal: number[] = [];
  const nextReturn: number[] = [];
  for (let i = W; i < values.length - W - 1; i++) {
    const price = values[i] ?? 0;
    trailingSignal.push(trailingMean(values, i, W) - price);
    centredSignal.push(centredMean(values, i, W) - price);
    nextReturn.push((values[i + 1] ?? 0) - price);
  }

  const trailingCorr = correlation(trailingSignal, nextReturn);
  const centredCorr = correlation(centredSignal, nextReturn);

  console.log(
    `    correlation of "overlay minus price" with the NEXT bar's move, over ${nextReturn.length} bars:`,
  );
  console.log(`      trailing ${W}-bar mean: ${trailingCorr.toFixed(3)}`);
  console.log(`      centred  ${W}-bar mean: ${centredCorr.toFixed(3)}`);

  assert.ok(
    Math.abs(trailingCorr) < 0.15,
    'a trailing overlay carries no information about a move that has not happened -- which is the correct and unexciting answer',
  );
  assert.ok(
    centredCorr > 0.3,
    'while a CENTRED overlay is strongly correlated with the next bar, because it literally contains it: it is the most common chart look-ahead there is, since "smooth this series" means a centred window in every plotting library, and the smoothed line looks better precisely because it leads the price',
  );
  console.log(
    '    on a replay this reads as an overlay that turns just before the market does -- which is what a genuinely good signal looks like from the outside',
  );

  // And the window makes the centred version impossible to compute, because
  // there is nothing after the cursor to average over.
  const cursorIdx = 2000;
  const w = new ReplayWindow(bars, bars[cursorIdx]!.timeNs);
  const visible = w.visible().map((b) => b.value);
  assert.equal(
    visible.length,
    cursorIdx + 1,
    'the replay window hands over the past and nothing else',
  );
  assert.equal(
    centredMean(visible, visible.length - 1, W),
    trailingMean(visible, visible.length - 1, Math.ceil(W / 2)),
    'so a centred mean computed at the cursor degenerates to a trailing one over the half-window that exists -- the look-ahead is not forbidden by a rule, there is simply no data on that side',
  );
});

test('[2] the window offers no way to reach past the cursor', () => {
  const bars = session(500);
  const cursor = bars[199]!.timeNs;
  const w = new ReplayWindow(bars, cursor);

  const visible = w.visible();
  assert.equal(visible.length, 200, 'exactly the bars at or before the cursor');
  assert.equal(
    visible[visible.length - 1]?.timeNs,
    cursor,
    'and a bar stamped exactly at the cursor is included, because it HAS happened -- the other choice is equally defensible and silently shifts every overlay by one bar, so it is stated rather than left to the reader',
  );
  assert.equal(
    visible.every((b) => b.timeNs <= cursor),
    true,
    'nothing after the cursor appears',
  );

  // The guarantee is the missing accessor.
  const surface = Object.getOwnPropertyNames(
    Object.getPrototypeOf(w) as object,
  );
  console.log(`    ReplayWindow exposes: ${surface.join(', ')}`);
  assert.equal(
    surface.some((m) => /^(bars|all|series|data)$/.test(m)),
    false,
    'there is no accessor returning the underlying series -- a caller cannot compute an overlay over the whole day even by accident, which is the same move as P11-08 having no selectAllInStore and P11-03 having no patchCellByIndex',
  );

  assert.equal(
    w.sessionLength,
    500,
    'the session LENGTH is exposed, because a count leaks nothing and a scrubber needs it',
  );

  w.seek(bars[0]!.timeNs);
  assert.equal(w.visible().length, 1, 'seeking back narrows the window again');
  w.seek(T0 - STEP);
  assert.equal(
    w.visible().length,
    0,
    'and a cursor before the session shows nothing rather than the first bar',
  );
  assert.equal(w.current(), null);
});

test('[3] a flag renders when it happened, not when it arrived', () => {
  const flags: FlagMarker[] = [
    {
      kind: FlagKind.DriftAlarm,
      occurredNs: T0 + 4n * STEP,
      deliveredNs: T0 + 7n * STEP, // queued behind a snapshot
      label: 'drift on model v3',
    },
    {
      kind: FlagKind.KillSwitchTripped,
      occurredNs: T0 + 20n * STEP,
      deliveredNs: T0 + 20n * STEP,
      label: 'kill switch',
    },
  ];

  const atFive = visibleFlags(flags, T0 + 5n * STEP);
  console.log(
    `    at T+5min the drift alarm (raised T+4, delivered T+7) is ${atFive.length === 1 ? 'shown' : 'hidden'}`,
  );
  assert.equal(
    atFive.length,
    1,
    'the alarm is visible at T+5 because it was RAISED at T+4 -- filtering on delivery would hide it until T+7, and the replay would then disagree with the audit trail about when the system knew',
  );

  const lag = deliveryLagNs(flags[0]!);
  console.log(`    its delivery lag was ${Number(lag) / 1e9} s`);
  assert.equal(
    lag,
    3n * STEP,
    'the lag is carried and measurable, because a consistently large one is itself a finding about the server -- it is just never what the marker is positioned by',
  );

  assert.equal(visibleFlags(flags, T0 + 3n * STEP).length, 0);
  assert.equal(visibleFlags(flags, T0 + 30n * STEP).length, 2);
});

test('[4] the cone widens as sqrt(h), not linearly', () => {
  const bars = session(600);
  const w = new ReplayWindow(bars, bars[499]!.timeNs);
  const visible = w.visible();

  const root = coneFrom(visible, 16);
  const linear = coneLinearWidth(visible, 16);
  assert.notEqual(root, null);
  assert.notEqual(linear, null);
  if (root === null || linear === null) return;

  const rootWidth = (h: number): number => {
    const p = root.points[h - 1];
    return p === undefined ? 0 : p.upper - p.lower;
  };
  const linWidth = (h: number): number => {
    const p = linear.points[h - 1];
    return p === undefined ? 0 : p.upper - p.lower;
  };

  console.log(
    `    horizon  1: sqrt cone ${rootWidth(1).toFixed(2)}, linear cone ${linWidth(1).toFixed(2)}`,
  );
  console.log(
    `    horizon 16: sqrt cone ${rootWidth(16).toFixed(2)}, linear cone ${linWidth(16).toFixed(2)}`,
  );

  assert.ok(
    Math.abs(rootWidth(4) / rootWidth(1) - 2) < 1e-9,
    'four steps out the band is exactly twice as wide, which is how uncertainty accumulates under independent increments',
  );
  assert.ok(
    linWidth(16) > rootWidth(16) * 3,
    'the linear cone is enormously too wide at the far end, where it looks merely conservative',
  );
  assert.equal(
    linWidth(1),
    rootWidth(1),
    'and identical at one step -- so the two agree exactly where a reviewer would check, and disagree everywhere a stop actually sits',
  );
});

test('[5] the cone says how many bars it rests on', () => {
  const bars = session(400);
  const early = new ReplayWindow(bars, bars[11]!.timeNs);
  const late = new ReplayWindow(bars, bars[399]!.timeNs);

  const a = coneFrom(early.visible(), 4);
  const b = coneFrom(late.visible(), 4);
  assert.notEqual(a, null);
  assert.notEqual(b, null);
  if (a === null || b === null) return;

  console.log(
    `    a cone from ${a.sampleSize} bars and one from ${b.sampleSize} bars are different claims, and both are drawn the same way`,
  );
  assert.equal(a.sampleSize, 11);
  assert.equal(b.sampleSize, 399);
  assert.ok(
    a.sampleSize < b.sampleSize,
    'the sample size is carried so a cone drawn twelve bars into a session is not read with the confidence of one drawn at the close',
  );

  assert.equal(coneFrom(early.visible().slice(0, 2), 4), null, 'and too few bars gives no cone at all rather than a cone from nothing');
  assert.equal(coneFrom(late.visible(), 0), null);
});
