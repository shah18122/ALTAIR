// P11-12 acceptance tests.
//
// Test 1 is the card: interpolating in sigma across expiries manufactures
// calendar arbitrage, counted; interpolating in total variance produces none,
// by construction.
//
// Test 2: the surface refuses to extrapolate beyond the quoted tenors.
//
// Test 3: a smile is in log-moneyness, so two smiles taken at different spots
// are comparable -- measured as the shift the strike-axis version suffers.
//
// Test 4: "net gamma" has no sign until somebody says whose book.
//
// Test 5: greeks are extensive and IV is not, on the same chart.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { Extent } from '../src/grid/aggregate.ts';
import {
  Greek,
  Perspective,
  SurfaceProblem,
  calendarViolations,
  flipPerspective,
  gammaProfile,
  greekExtent,
  interpolateIvLinear,
  interpolateVariance,
  ivFromTotalVariance,
  smilePoints,
  totalVariance,
} from '../src/chart/surface.ts';
import type { Slice, StrikeExposure } from '../src/chart/surface.ts';

// An ordinary post-event term structure: elevated front, decaying back.
const TERM: Slice[] = [
  { tenorYears: 30 / 365, iv: 0.18 },
  { tenorYears: 60 / 365, iv: 0.155 },
  { tenorYears: 90 / 365, iv: 0.14 },
  { tenorYears: 180 / 365, iv: 0.12 },
];

// The demonstration needs a WIDE gap between quoted slices, and that is not a
// contrivance -- it is the ordinary case. A chart interpolating between the
// weekly and the six-month has exactly this gap, because nothing trades in
// between. With intermediate knots at 60 and 90 days each segment is too short
// for the effect to appear, which is why the first draft of this test measured
// zero violations and had to be rebuilt around the gap it was actually about.
const WIDE: Slice[] = [
  { tenorYears: 30 / 365, iv: 0.18 },
  { tenorYears: 180 / 365, iv: 0.12 },
];

test('[1] interpolating in sigma manufactures calendar arbitrage', () => {
  const lo = WIDE[0]!.tenorYears;
  const hi = WIDE[WIDE.length - 1]!.tenorYears;

  // The QUOTED endpoints are themselves perfectly well behaved.
  assert.ok(
    totalVariance(WIDE[1]!) > totalVariance(WIDE[0]!),
    'the two quoted slices satisfy the calendar condition -- so anything wrong between them was introduced by the interpolation, not inherited from the market',
  );

  const sigmaCurve: { tenorYears: number; iv: number }[] = [];
  const varianceCurve: { tenorYears: number; iv: number }[] = [];
  for (let i = 0; i <= 200; i++) {
    const t = lo + ((hi - lo) * i) / 200;
    const a = interpolateIvLinear(WIDE, t);
    const b = interpolateVariance(WIDE, t);
    if (a.ok) sigmaCurve.push({ tenorYears: t, iv: a.value });
    if (b.ok) varianceCurve.push({ tenorYears: t, iv: b.value });
  }

  const sigmaBad = calendarViolations(sigmaCurve);
  const varianceBad = calendarViolations(varianceCurve);

  console.log(
    `    ${sigmaCurve.length} interpolated tenors: linear-in-sigma produces ${sigmaBad} points whose total variance FALLS with time; linear-in-variance produces ${varianceBad}`,
  );

  assert.ok(
    sigmaBad > 0,
    'a longer-dated option carrying less total uncertainty than a shorter-dated one is a calendar arbitrage, and the interpolation invented it -- drawn as a perfectly smooth surface, which is what a trader reads a calendar spread off',
  );
  assert.equal(
    varianceBad,
    0,
    'interpolating total variance cannot produce one, because w is linear in t between two non-decreasing endpoints -- the same Gatheral condition P5-07 checks on the strike axis, checked here on the time axis',
  );

  // The two disagree in the middle and agree at the knots, which is what
  // makes this survive review: spot-check an expiry that trades and it looks
  // perfect.
  for (const s of TERM) {
    const a = interpolateIvLinear(TERM, s.tenorYears);
    const b = interpolateVariance(TERM, s.tenorYears);
    if (a.ok && b.ok) {
      assert.ok(
        Math.abs(a.value - b.value) < 1e-9,
        'at a quoted expiry both methods return the quoted value',
      );
    }
  }
  const mid = (WIDE[0]!.tenorYears + WIDE[1]!.tenorYears) / 2;
  const ma = interpolateIvLinear(WIDE, mid);
  const mb = interpolateVariance(WIDE, mid);
  assert.ok(ma.ok && mb.ok);
  if (ma.ok && mb.ok) {
    console.log(
      `    halfway between the 30d and 180d slices: sigma-linear ${(100 * ma.value).toFixed(3)}%, variance-linear ${(100 * mb.value).toFixed(3)}%`,
    );
    assert.notEqual(
      ma.value,
      mb.value,
      'and they differ between the knots, where nobody spot-checks',
    );
  }

  // Round trip.
  const w = totalVariance(TERM[0]!);
  assert.ok(
    Math.abs(ivFromTotalVariance(w, TERM[0]!.tenorYears) - TERM[0]!.iv) < 1e-12,
  );
});

test('[2] the surface refuses to extrapolate', () => {
  const tooShort = interpolateVariance(TERM, 1 / 365);
  assert.equal(tooShort.ok, false);
  if (!tooShort.ok) {
    assert.equal(
      tooShort.problem,
      SurfaceProblem.OutsideQuotedRange,
      'beyond the quoted tenors the surface has no opinion, and a chart that draws one there is drawing the interpolator rather than the market',
    );
  }
  const tooLong = interpolateVariance(TERM, 2);
  assert.equal(tooLong.ok, false);

  assert.equal(interpolateVariance([TERM[0]!], 0.1).ok, false);
  const bad = interpolateVariance(TERM, -1);
  assert.equal(bad.ok, false);
  if (!bad.ok) assert.equal(bad.problem, SurfaceProblem.BadTenor);
});

test('[3] a smile is in log-moneyness, so two smiles are comparable', () => {
  const strikes = [
    2_300_000n, 2_350_000n, 2_400_000n, 2_450_000n, 2_500_000n,
  ];
  const ivs = [0.21, 0.19, 0.18, 0.185, 0.2];

  // The same smile, seen at two different spots on the same day.
  const morning = smilePoints(strikes, ivs, 2_400_000n);
  const afternoon = smilePoints(strikes, ivs, 2_450_000n);
  assert.equal(morning.ok, true);
  assert.equal(afternoon.ok, true);
  if (!morning.ok || !afternoon.ok) return;

  const atmMorning = morning.value.find((p) => p.strikePaise === 2_400_000n);
  const atmAfternoon = afternoon.value.find((p) => p.strikePaise === 2_450_000n);
  console.log(
    `    ATM sits at k = ${atmMorning?.k.toFixed(6)} in the morning and k = ${atmAfternoon?.k.toFixed(6)} in the afternoon`,
  );
  assert.ok(Math.abs(atmMorning?.k ?? 1) < 1e-12);
  assert.ok(
    Math.abs(atmAfternoon?.k ?? 1) < 1e-12,
    'the at-the-money point is k = 0 on both, so the two curves lie on top of one another and the SHAPE is what differs',
  );

  // On a strike axis, the same two curves are a translation apart.
  const shiftPaise = 2_450_000n - 2_400_000n;
  console.log(
    `    on a strike axis the same curve is displaced by ${shiftPaise} paise, and the shape is buried under the translation`,
  );
  assert.ok(shiftPaise > 0n);

  // The forward is required.
  assert.equal(smilePoints(strikes, ivs, 0n).ok, false);
  const noFwd = smilePoints(strikes, ivs, 0n);
  if (!noFwd.ok) assert.equal(noFwd.problem, SurfaceProblem.NoForward);

  // A zero or negative IV has no place on a smile.
  assert.equal(smilePoints([2_400_000n], [0], 2_400_000n).ok, false);
});

test('[4] net gamma has no sign until you say whose book', () => {
  const points: StrikeExposure[] = [
    { strikePaise: 2_300_000n, value: -1200 },
    { strikePaise: 2_350_000n, value: -800 },
    { strikePaise: 2_400_000n, value: 900 },
    { strikePaise: 2_450_000n, value: 1500 },
  ];

  const unspecified = gammaProfile(points, Greek.Gamma, Perspective.Unspecified);
  assert.equal(unspecified.ok, false);
  if (!unspecified.ok) {
    assert.equal(
      unspecified.problem,
      SurfaceProblem.Unspecified,
      'a profile with no perspective is refused -- the dealer answer is the negative of the customer answer, and a chart that cannot say which will be read the wrong way round exactly once',
    );
  }

  const dealer = gammaProfile(points, Greek.Gamma, Perspective.Dealer);
  assert.equal(dealer.ok, true);
  if (!dealer.ok) return;

  console.log(
    `    dealer net ${dealer.value.net}, flip at ${dealer.value.flipStrikePaise}`,
  );
  assert.equal(dealer.value.net, 400);
  assert.equal(
    dealer.value.flipStrikePaise,
    2_450_000n,
    'and the flip point -- where cumulative exposure crosses zero -- is what the chart is usually read for',
  );

  const own = flipPerspective(dealer.value);
  assert.equal(
    own.net,
    -dealer.value.net,
    'the other side of the same book is exactly one negation, computed from the first rather than derived independently, so the two charts cannot drift apart',
  );
  assert.equal(own.perspective, Perspective.Own);
  assert.equal(flipPerspective(own).net, dealer.value.net);

  assert.equal(gammaProfile(points, Greek.Unspecified, Perspective.Dealer).ok, false);
});

test('[5] greeks are extensive; the IV beside them is not', () => {
  for (const g of [Greek.Delta, Greek.Gamma, Greek.Vega, Greek.Theta]) {
    assert.equal(
      greekExtent(g),
      Extent.Extensive,
      'every greek scales with position size and adds across strikes, which is why a net figure means something',
    );
  }
  assert.equal(greekExtent(Greek.Unspecified), Extent.Unspecified);

  // The chart shows both on one panel, and only one of them may be totalled.
  console.log(
    '    greeks-vs-strike and the smile share an axis; the greeks total and the IV does not, on the same panel',
  );
  assert.notEqual(
    greekExtent(Greek.Vega),
    Extent.Intensive,
    'vega adds -- per vol POINT, the unit analytics/greeks.hpp produces, which P10-08 already paid for conflating once',
  );
});
