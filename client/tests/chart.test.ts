// P11-10 acceptance tests.
//
// Test 1 is the card: a nanosecond timestamp in a float32 vertex attribute
// collapses a day of ticks into a staircase, and rebasing fixes it. Both
// measured in distinct positions and in nanoseconds of resolution.
//
// Test 2: a price axis has the same problem, and it is less obvious because
// it is fine for the index and not fine for a cheap option.
//
// Test 3: bucket boundaries belong to one candle, and the closed-interval
// version is priced in overstated volume.
//
// Test 4: volume is conserved -- the parts total the tape, exactly.
//
// Test 5: the forming candle is marked, and a gap produces no candle rather
// than a zero.
//
// Test 6: the crosshair reports a candle's own values, not an interpolation.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
  distinctAfterFloat32,
  domainFrom,
  float32Resolution,
  naiveFloat32,
  priceAxis,
  toFloat32Offsets,
  toFloat32Prices,
} from '../src/chart/scale.ts';
import {
  candles,
  candlesClosedInterval,
  checkConservation,
  closedCandles,
  snapCrosshair,
} from '../src/chart/candles.ts';
import type { Tick } from '../src/chart/candles.ts';

const SESSION_START = 1_788_393_600_000_000_000n; // 2026-09-03, in ns
const MINUTE = 60_000_000_000n;

test('[1] a nanosecond timestamp does not survive a float32 attribute', () => {
  // A two-hour window, one tick every 100 ms. Nothing exotic.
  const stamps: bigint[] = [];
  for (let i = 0; i < 72_000; i++) {
    stamps.push(SESSION_START + BigInt(i) * 100_000_000n);
  }

  const resAtEpoch = float32Resolution(Number(SESSION_START));
  console.log(
    `    float32 resolution at 1.79e18 ns: ${(resAtEpoch / 1e9).toFixed(1)} SECONDS per representable step`,
  );

  const naive = naiveFloat32(stamps);
  const naiveDistinct = distinctAfterFloat32(naive);

  const domain = domainFrom(stamps[0]!, stamps[stamps.length - 1]! + 1n);
  assert.notEqual(domain, null);
  if (domain === null) return;
  const rebased = toFloat32Offsets(stamps, domain);
  const rebasedDistinct = distinctAfterFloat32(rebased);

  const spanNs = Number(domain.spanNs);
  const resInWindow = float32Resolution(spanNs);
  console.log(
    `    ${stamps.length.toLocaleString()} ticks over 2 hours: raw float32 keeps ${naiveDistinct} distinct x positions; rebased keeps ${rebasedDistinct.toLocaleString()}`,
  );
  console.log(
    `    within the window float32 resolves to ${(resInWindow / 1e6).toFixed(3)} ms, finer than any pixel at any zoom this chart offers`,
  );

  assert.ok(
    resAtEpoch > 60e9,
    'at today\'s epoch a float32 step is over a minute wide, so every tick in that span lands on one x-coordinate',
  );
  assert.ok(
    naiveDistinct < stamps.length / 100,
    'the raw conversion collapses the session into a handful of positions -- the chart does not error, it draws a staircase, and the staircase looks like low-frequency structure in the market',
  );
  assert.equal(
    rebasedDistinct,
    stamps.length,
    'subtracting the domain origin in BIGINT first keeps every tick distinct -- doing the subtraction after the conversion loses the precision before it can save it, which is the version that looks correct in a diff',
  );
});

test('[2] price survives float32; a NOTIONAL axis does not', () => {
  // FIRST, THE CLAIM THAT TURNED OUT TO BE FALSE. An earlier draft asserted
  // that a cheap option charted on an index-scaled axis becomes a staircase.
  // It does not, and the numbers say why: NIFTY at 24,000 is 2,400,000 paise,
  // where float32 resolves to 0.25 paise against a 5-paise tick size. Twenty
  // times the headroom needed.
  const option: bigint[] = [];
  for (let i = 0; i < 5000; i++) option.push(350n + BigInt(i % 40) * 5n);
  const index: bigint[] = [];
  for (let i = 0; i < 5000; i++) index.push(2_400_000n + BigInt(i % 400) * 5n);

  const optionAxis = priceAxis(option);
  const indexAxis = priceAxis(index);
  assert.notEqual(optionAxis, null);
  assert.notEqual(indexAxis, null);
  if (optionAxis === null || indexAxis === null) return;

  const trueDistinct = new Set(option.map(String)).size;
  const onOwn = distinctAfterFloat32(toFloat32Prices(option, optionAxis));
  const onIndex = distinctAfterFloat32(toFloat32Prices(option, indexAxis));

  console.log(
    `    a Rs 3.50 option with ${trueDistinct} distinct prices: ${onOwn} survive on its own axis, ${onIndex} on the index's -- price in paise is SAFE in float32 at Indian equity magnitudes`,
  );
  console.log(
    `    float32 at 2,400,000 paise resolves to ${float32Resolution(2_400_000)} paise, against a 5-paise tick`,
  );
  assert.equal(
    onIndex,
    trueDistinct,
    'so the tempting claim is simply wrong, and asserting it would have been a scare rather than a finding',
  );

  // WHERE IT IS ACTUALLY EXPOSED: notional and cumulative P&L.
  //
  // Rs 50 crore is 5e10 paise -- and getting THAT wrong by a factor of a
  // hundred is how the first draft of this test ended up demonstrating a
  // Rs 5,000 crore book, which is not a number this system will hold.
  const bookPaise = 50_000_000_000n; // Rs 50 crore, in paise
  const curve: bigint[] = [];
  for (let i = 0; i < 5000; i++) {
    // Moving in Rs 10 steps, which on a book that size is ordinary intraday
    // P&L granularity.
    curve.push(bookPaise + BigInt(i % 500) * 1_000n);
  }
  const resAtBook = float32Resolution(Number(bookPaise));
  console.log(
    `    float32 at Rs 50 crore (5e10 paise) resolves to ${resAtBook} paise = Rs ${(resAtBook / 100).toFixed(0)} -- coarser than the Rs 10 moves being charted`,
  );

  const curveTrue = new Set(curve.map(String)).size;
  const curveRaw = distinctAfterFloat32(
    Float32Array.from(curve.map((v) => Number(v))),
  );
  const curveAxis = priceAxis(curve);
  assert.notEqual(curveAxis, null);
  if (curveAxis === null) return;
  const curveRebased = distinctAfterFloat32(toFloat32Prices(curve, curveAxis));

  console.log(
    `    a P&L curve with ${curveTrue} distinct levels: ${curveRaw} survive un-rebased, ${curveRebased} rebased`,
  );
  assert.ok(
    curveRaw < curveTrue / 2,
    'an un-rebased notional axis loses most of the curve -- Rs 10 moves on a Rs 50 crore book fall inside one float32 step, and the equity curve draws as a staircase',
  );
  assert.equal(
    curveRebased,
    curveTrue,
    'rebasing to the series own range restores every level, which is why the axis is rebased PER SERIES rather than shared',
  );

  const flat = priceAxis([100n, 100n, 100n]);
  assert.notEqual(flat, null);
  assert.ok((flat?.spanPaise ?? 0n) > 0n, 'and a flat series still gets a non-zero span rather than a divide by zero in the shader');
});

test('[3] a bucket boundary belongs to exactly one candle', () => {
  const ticks: Tick[] = [];
  // 60,000 ticks over an hour, with 1,000 landing precisely on a minute
  // boundary -- exchange timestamps are quantised, and a minute boundary is
  // exactly the instant a batch of orders is timed to.
  for (let i = 0; i < 60; i++) {
    const base = SESSION_START + BigInt(i) * MINUTE;
    for (let j = 0; j < 1000; j++) {
      // The first twenty ticks of each minute share the boundary timestamp
      // exactly -- exchange timestamps are quantised and a batch of orders is
      // timed to the boundary, so this is the ordinary case rather than a
      // contrived one.
      ticks.push({
        timeNs: j < 20 ? base : base + BigInt(j) * 60_000_000n,
        pricePaise: 2_400_000n + BigInt((i * 7 + j) % 200),
        quantity: 1n,
      });
    }
  }

  const built = candles(ticks, MINUTE, SESSION_START + 61n * MINUTE);
  const half = built.reduce((a, c) => a + c.volume, 0n);

  const closed = candlesClosedInterval(ticks, MINUTE);
  let closedTotal = 0n;
  for (const v of closed.values()) closedTotal += v;

  const tape = ticks.reduce((a, t) => a + t.quantity, 0n);
  const overstatement =
    (100 * Number(closedTotal - tape)) / Number(tape);

  console.log(
    `    tape volume ${tape};  half-open buckets report ${half};  closed-interval buckets report ${closedTotal} (+${overstatement.toFixed(1)}%)`,
  );

  assert.equal(
    half,
    tape,
    'half-open [start, end) counts every tick exactly once',
  );
  assert.ok(
    closedTotal > tape,
    'while the closed-interval version counts every boundary tick twice -- and it is the number a volume-confirmation rule keys on',
  );
});

test('[4] volume is conserved -- the parts total the tape', () => {
  const ticks: Tick[] = [];
  let s = 0x0c4d1e5f;
  for (let i = 0; i < 40_000; i++) {
    s = (s * 1664525 + 1013904223) >>> 0;
    ticks.push({
      timeNs: SESSION_START + BigInt(i) * 90_000_000n,
      pricePaise: 2_400_000n + BigInt((s % 1000) - 500),
      quantity: BigInt((s % 50) + 1),
    });
  }
  const built = candles(ticks, MINUTE, SESSION_START + 3_600_000_000_000n * 2n);
  const c = checkConservation(ticks, built);

  console.log(
    `    ${c.tapeTicks.toLocaleString()} ticks / ${c.tapeVolume} volume -> ${built.length} candles / ${c.chartTicks.toLocaleString()} ticks / ${c.chartVolume} volume`,
  );
  assert.equal(
    c.agrees,
    true,
    'the candles total the tape exactly, in integers -- the same runtime invariant the engine checks every tick, applied to the chart, and far cheaper to assert than to notice',
  );
  assert.equal(c.chartVolume, c.tapeVolume);
  assert.equal(c.chartTicks, c.tapeTicks);
});

test('[5] the forming candle is marked, and a gap is not a zero', () => {
  const ticks: Tick[] = [];
  // Minute 0 and minute 2 trade. Minute 1 is dead -- lunch, a halt, an
  // illiquid strike.
  for (const m of [0, 2]) {
    for (let j = 0; j < 10; j++) {
      ticks.push({
        timeNs: SESSION_START + BigInt(m) * MINUTE + BigInt(j) * 1_000_000_000n,
        pricePaise: 2_400_000n + BigInt(j),
        quantity: 1n,
      });
    }
  }
  // "Now" is inside minute 2, so minute 2 is still forming.
  const now = SESSION_START + 2n * MINUTE + 30_000_000_000n;
  const built = candles(ticks, MINUTE, now);

  console.log(
    `    two traded minutes with a dead minute between them produced ${built.length} candles`,
  );
  assert.equal(
    built.length,
    2,
    'a minute with no trades produces NO candle -- an open=high=low=close=0 candle draws a spike to the axis, and one carrying the previous close draws a flat line saying trading happened. Neither is true, so the renderer gets a gap',
  );

  assert.equal(built[0]?.complete, true, 'the settled minute is complete');
  assert.equal(
    built[1]?.complete,
    false,
    'and the minute the session is still inside is NOT -- its close is not a close, it is the latest trade, and an overlay that treats it as settled repaints its own last point',
  );

  assert.equal(closedCandles(built).length, 1);
  assert.notEqual(
    built.length,
    closedCandles(built).length,
    'so a moving average over candles and one over closedCandles are different series, and only one of them is something to trade off',
  );

  const first = built[0];
  assert.ok(first !== undefined);
  assert.equal(first.open, 2_400_000n);
  assert.equal(first.close, 2_400_009n);
  assert.equal(first.high, 2_400_009n);
  assert.equal(first.low, 2_400_000n);
  assert.equal(first.volume, 10n);
});

test('[6] the crosshair reports a candle, not an interpolation', () => {
  const ticks: Tick[] = [];
  for (let m = 0; m < 10; m++) {
    ticks.push({
      timeNs: SESSION_START + BigInt(m) * MINUTE + 1_000_000_000n,
      pricePaise: 2_400_000n + BigInt(m) * 100n,
      quantity: 5n,
    });
  }
  const built = candles(ticks, MINUTE, SESSION_START + 20n * MINUTE);
  assert.equal(built.length, 10);

  // Pointer three-quarters of the way through minute 4.
  const at = SESSION_START + 4n * MINUTE + 45_000_000_000n;
  const hit = snapCrosshair(built, at);
  assert.notEqual(hit, null);
  if (hit === null) return;

  console.log(
    `    pointer inside minute 4 -> snapped to candle ${hit.index}, close ${hit.candle.close} paise`,
  );
  assert.equal(hit.index, 4);
  assert.equal(
    hit.candle.close,
    2_400_400n,
    'the readout is that candle\'s own close -- an interpolated readout shows a price at which nothing traded, to the paisa, in the box the user is reading precisely because they want the real number',
  );

  const beyond = snapCrosshair(built, SESSION_START + 50n * MINUTE);
  assert.equal(beyond?.extrapolated, true, 'and a pointer past the last candle says so, rather than silently snapping and looking like a hit');
  assert.equal(snapCrosshair([], SESSION_START), null);
});
