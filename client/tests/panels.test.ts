// P11-14 acceptance tests.
//
// Test 1 is the card: "max loss" taken off a grid is a different number for
// every grid, and none of them is the answer.
//
// Test 2: a bounded strategy DOES get real numbers, so the refusal is not
// blanket timidity.
//
// Test 3: every number is net of cost before it exists, and the break-evens
// move because of it.
//
// Test 4: the client does not compute the cost -- the schedule version is
// carried through from the server.
//
// Test 5: an audit row missing any of the five stamps is shown as not
// reproducible, and says which one.
//
// Test 6: the kill switch confirmation states the consequence, and still only
// produces a message.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { ClientMsg, formatPaise } from '../src/protocol.ts';
import {
  LegKind,
  StrategyProblem,
  endSlopes,
  gridExtremes,
  payoffAt,
  payoffProfile,
  totalCostPaise,
} from '../src/panels/strategy.ts';
import type { CostBreakdown, Leg } from '../src/panels/strategy.ts';
import {
  AuditProblem,
  KillProblem,
  confirmKill,
  killPrompt,
  reproducibilityGaps,
  reproducible,
  reproductionKey,
} from '../src/panels/audit.ts';
import type { AuditRow, KillImpact } from '../src/panels/audit.ts';

const LOT = 25; // NIFTY, from the spec store in the real thing

const COST: CostBreakdown = {
  brokeragePaise: 4_000n,
  sttPaise: 12_500n,
  exchangePaise: 900n,
  gstPaise: 880n,
  stampDutyPaise: 300n,
  scheduleVersion: 'charges-2026-04-01',
};

const ZERO_COST: CostBreakdown = {
  brokeragePaise: 0n,
  sttPaise: 0n,
  exchangePaise: 0n,
  gstPaise: 0n,
  stampDutyPaise: 0n,
  scheduleVersion: 'none',
};

/** A naked short 24,000 call at Rs 120. */
const SHORT_CALL: Leg[] = [
  {
    kind: LegKind.Call,
    lots: -1,
    lotSize: LOT,
    strikePaise: 2_400_000n,
    pricePaise: 12_000n,
  },
];

/** A bull call spread: long 24,000 at Rs 120, short 24,500 at Rs 60. */
const SPREAD: Leg[] = [
  {
    kind: LegKind.Call,
    lots: 1,
    lotSize: LOT,
    strikePaise: 2_400_000n,
    pricePaise: 12_000n,
  },
  {
    kind: LegKind.Call,
    lots: -1,
    lotSize: LOT,
    strikePaise: 2_450_000n,
    pricePaise: 6_000n,
  },
];

test('[1] max loss taken off a grid is a different number for every grid', () => {
  const narrow = gridExtremes(SHORT_CALL, COST, 2_200_000n, 2_600_000n, 401);
  const wide = gridExtremes(SHORT_CALL, COST, 2_200_000n, 4_000_000n, 401);

  console.log(
    `    naked short 24,000 call, grid 22,000-26,000: a naive builder prints "max loss Rs ${formatPaise(narrow.worst)}"`,
  );
  console.log(
    `    the same position, grid 22,000-40,000:       it prints "max loss Rs ${formatPaise(wide.worst)}"`,
  );
  assert.notEqual(
    narrow.worst,
    wide.worst,
    'the number in the box is a property of the drawing range, not of the position -- and a position was sized against it',
  );
  assert.ok(wide.worst < narrow.worst);

  const r = payoffProfile(SHORT_CALL, COST, 2_200_000n, 2_600_000n, 401);
  assert.equal(r.ok, true);
  if (!r.ok) return;

  assert.equal(
    r.profile.unboundedDownside,
    true,
    'the end slope is computed from the LEGS, exactly, rather than estimated off two grid points -- a numerical slope inherits the grid opinion about where the world ends',
  );
  assert.equal(
    r.profile.unboundedUpside,
    false,
    'a short call is unbounded in LOSS as spot rises, not in profit -- and the two directions are separate fields precisely so they cannot be conflated',
  );
  assert.equal(
    r.profile.maxLossPaise,
    null,
    'so max loss is null, which renders as "unlimited". There is no path in this module that returns a grid extremum dressed as a bound',
  );
  assert.notEqual(
    r.profile.maxProfitPaise,
    null,
    'while the UPSIDE of a short call is genuinely bounded -- the premium -- so that number is real and is printed',
  );
  console.log(
    `    payoffProfile says: max profit Rs ${formatPaise(r.profile.maxProfitPaise ?? 0n)}, max loss unlimited`,
  );

  const slopes = endSlopes(SHORT_CALL);
  assert.equal(slopes.upside, BigInt(-1 * LOT));
});

test('[2] a bounded strategy gets real numbers', () => {
  const r = payoffProfile(SPREAD, COST, 2_300_000n, 2_600_000n, 601);
  assert.equal(r.ok, true);
  if (!r.ok) return;

  assert.equal(r.profile.unboundedUpside, false);
  assert.equal(r.profile.unboundedDownside, false);
  assert.notEqual(r.profile.maxProfitPaise, null);
  assert.notEqual(r.profile.maxLossPaise, null);

  console.log(
    `    bull call spread: max profit Rs ${formatPaise(r.profile.maxProfitPaise ?? 0n)}, max loss Rs ${formatPaise(r.profile.maxLossPaise ?? 0n)}, break-even at ${r.profile.breakEvenPaise.map((b) => formatPaise(b)).join(', ')}`,
  );

  // The refusal is not blanket timidity: a spread has both ends closed, and
  // both numbers survive widening the grid.
  const wider = payoffProfile(SPREAD, COST, 2_000_000n, 3_000_000n, 601);
  assert.equal(wider.ok, true);
  if (!wider.ok) return;
  assert.equal(
    wider.profile.maxProfitPaise,
    r.profile.maxProfitPaise,
    'and a spread max profit does not move when the grid widens, which is what makes it a real bound rather than a grid artefact',
  );
  assert.equal(wider.profile.maxLossPaise, r.profile.maxLossPaise);

  assert.equal(endSlopes(SPREAD).upside, 0n, 'the two call legs cancel at infinity');

  // A short put: conventionally called "unlimited risk", and it is not.
  const SHORT_PUT: Leg[] = [
    {
      kind: LegKind.Put,
      lots: -1,
      lotSize: LOT,
      strikePaise: 2_400_000n,
      pricePaise: 10_000n,
    },
  ];
  const sp = payoffProfile(SHORT_PUT, ZERO_COST, 2_300_000n, 2_500_000n, 401);
  assert.equal(sp.ok, true);
  if (!sp.ok) return;
  assert.equal(
    sp.profile.unboundedDownside,
    false,
    'spot cannot go below zero, so a short put is NOT unbounded -- calling it unlimited is the same mistake as a grid extremum, made in the other direction',
  );
  const expected = BigInt(-1 * LOT) * (2_400_000n - 10_000n);
  console.log(
    `    short 24,000 put: max loss Rs ${formatPaise(sp.profile.maxLossPaise ?? 0n)} -- strike less premium, exact, and far outside the drawn range`,
  );
  assert.equal(
    sp.profile.maxLossPaise,
    expected,
    'and the number is the strike less the premium, evaluated AT zero rather than at the edge of the drawing window',
  );
});

test('[3] every number is net of cost before it exists', () => {
  const withCost = payoffProfile(SPREAD, COST, 2_300_000n, 2_600_000n, 601);
  const without = payoffProfile(SPREAD, ZERO_COST, 2_300_000n, 2_600_000n, 601);
  assert.equal(withCost.ok, true);
  assert.equal(without.ok, true);
  if (!withCost.ok || !without.ok) return;

  const total = totalCostPaise(COST);
  console.log(
    `    total charges Rs ${formatPaise(total)}; max profit falls from Rs ${formatPaise(without.profile.maxProfitPaise ?? 0n)} to Rs ${formatPaise(withCost.profile.maxProfitPaise ?? 0n)}`,
  );
  assert.equal(
    (without.profile.maxProfitPaise ?? 0n) - (withCost.profile.maxProfitPaise ?? 0n),
    total,
    'the payoff is shifted by the full cost at construction, so there is no moment at which an uncosted number exists to be rendered by mistake (rule 5)',
  );

  const beWith = withCost.profile.breakEvenPaise[0];
  const beWithout = without.profile.breakEvenPaise[0];
  assert.ok(beWith !== undefined && beWithout !== undefined);
  console.log(
    `    and the break-even moves from ${formatPaise(beWithout)} to ${formatPaise(beWith)} -- the level that actually matters`,
  );
  assert.ok(
    beWith > beWithout,
    'a break-even drawn gross of charges is optimistic by the round trip, and it is the number a stop gets placed against',
  );

  // There is no zero-cost default: a caller must pass a breakdown.
  assert.equal(payoffProfile([], COST, 1n, 2n, 10).ok, false);
  const noLegs = payoffProfile([], COST, 1n, 2n, 10);
  if (!noLegs.ok) assert.equal(noLegs.problem, StrategyProblem.NoLegs);

  const badLot = payoffProfile(
    [{ ...SPREAD[0]!, lotSize: 0 }],
    COST,
    1n,
    100n,
    10,
  );
  assert.equal(badLot.ok, false);
  if (!badLot.ok) {
    assert.equal(
      badLot.problem,
      StrategyProblem.NoLotSize,
      'and a lot size of zero means the spec store did not answer, which blocks rather than defaulting to 1 (rule 1)',
    );
  }
});

test('[4] the client does not compute the cost', () => {
  const r = payoffProfile(SPREAD, COST, 2_300_000n, 2_600_000n, 601);
  assert.equal(r.ok, true);
  if (!r.ok) return;

  assert.equal(
    r.profile.scheduleVersion,
    'charges-2026-04-01',
    'the effective-dated schedule version is carried through, so a displayed cost can be traced to the table that produced it -- STT rose on 2026-04-01 and a number priced under the old table is wrong in a way nothing on screen would show',
  );
  console.log(
    `    cost priced under schedule "${r.profile.scheduleVersion}", carried through untouched`,
  );

  // The client's whole cost vocabulary is "add up what the server sent".
  assert.equal(
    totalCostPaise(COST),
    COST.brokeragePaise +
      COST.sttPaise +
      COST.exchangePaise +
      COST.gstPaise +
      COST.stampDutyPaise,
    'this module sums a breakdown and does nothing else with it -- reimplementing which side STT applies to would create a SECOND source of truth for the number that decides whether a trade is worth taking, and the two would drift toward whichever was updated last',
  );

  // Payoff arithmetic is exact in paise.
  assert.equal(
    payoffAt(SHORT_CALL, 2_500_000n),
    BigInt(-1 * LOT) * (100_000n - 12_000n),
    'and the payoff itself is integer paise throughout, never a float (rule 3)',
  );
});

test('[5] an audit row missing a stamp is not reproducible, and says which', () => {
  const complete: AuditRow = {
    tickSeqno: 4_182_991n,
    engineTimeNs: 1_788_393_600_123_456_789n,
    modelHash: 'sha256:9f2c…',
    featureVersion: 'fv-12',
    configHash: 'sha256:41ab…',
    specVersion: 'spec-2026-09-03',
    action: 'ENTER NIFTY26SEP24000CE x1',
  };
  assert.equal(reproducible(complete), true);
  assert.deepEqual(reproducibilityGaps(complete), []);
  console.log(`    reproduction key: ${reproductionKey(complete)}`);

  const cases: ReadonlyArray<readonly [Partial<AuditRow>, AuditProblem]> = [
    [{ modelHash: '' }, AuditProblem.MissingModelHash],
    [{ featureVersion: '' }, AuditProblem.MissingFeatureVersion],
    [{ configHash: '' }, AuditProblem.MissingConfigHash],
    [{ specVersion: '' }, AuditProblem.MissingSpecVersion],
    [{ tickSeqno: 0n }, AuditProblem.MissingTickSeqno],
  ];
  for (const [patch, expected] of cases) {
    const row = { ...complete, ...patch };
    assert.equal(reproducible(row), false);
    assert.deepEqual(
      reproducibilityGaps(row),
      [expected],
      `${expected}: named, so the panel can render the row AS not reproducible rather than as a row with a blank column -- a blank column reads as "nothing happened there", and what it means is "this decision cannot be re-run"`,
    );
  }
  console.log(
    `    all ${cases.length} of rule 10's stamps are required, and each absence is named`,
  );
});

test('[6] the kill switch states the consequence and still only sends a message', () => {
  const impact: KillImpact = {
    openPositions: 7,
    grossNotionalPaise: 84_500_000_00n,
    estimatedCostPaise: 1_842_00n,
    cannotFlatten: ['NIFTY26OCT26000CE'],
  };

  const prompt = killPrompt(impact, formatPaise);
  console.log(`    ${prompt}`);
  assert.ok(prompt.includes('7 positions'), 'the prompt states how many');
  assert.ok(prompt.includes('gross'), 'and how much');
  assert.ok(prompt.includes('estimated cost'), 'and what it will cost');
  assert.ok(
    prompt.includes('CANNOT be flattened'),
    'and names what it cannot flatten -- "flatten everything" that silently flattens most things is the worst of both',
  );
  assert.equal(
    prompt.toLowerCase().includes('are you sure'),
    false,
    'and it is not "are you sure": the person pressing it already believes they are sure, which is why their hand is on the key. What occasionally stops somebody is a number',
  );

  assert.equal(confirmKill(impact, false).ok, false);
  const unack = confirmKill(impact, false);
  if (!unack.ok) assert.equal(unack.problem, KillProblem.NotAcknowledged);

  assert.equal(confirmKill(null, true).ok, false);
  const noImpact = confirmKill(null, true);
  if (!noImpact.ok) {
    assert.equal(
      noImpact.problem,
      KillProblem.NoImpact,
      'a confirmation that could be built without knowing the consequence is one that will be',
    );
  }

  const nothing = confirmKill({ ...impact, openPositions: 0 }, true);
  assert.equal(nothing.ok, false);
  if (!nothing.ok) {
    assert.equal(
      nothing.problem,
      KillProblem.NothingToFlatten,
      'and a no-op press is refused, so the confirmation never becomes a habit formed on presses that did nothing',
    );
  }

  const go = confirmKill(impact, true);
  assert.equal(go.ok, true);
  if (go.ok) {
    assert.equal(
      go.message,
      ClientMsg.KillSwitch,
      'the result is a MESSAGE to send, not an effect -- oms/ is the only thing that places or cancels an order, and this is the one word the protocol gives the client for asking',
    );
  }
});
