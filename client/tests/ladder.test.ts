// P11-11 acceptance tests.
//
// Test 1 is the card: coalescing the two sides independently renders a crossed
// book that never existed, counted over a trending tape.
//
// Test 2: the ladder is a tick grid, so an empty level is an empty row -- and
// a price off the grid is a blocking disagreement, not a rounding job.
//
// Test 3: cost-to-fill is the number a depth ladder is for, and it reports
// what it could not fill rather than pricing a trade that cannot happen.
//
// Test 4: the aggressor is INFERRED. Both rules are scored against a tape
// where the truth is known by construction.
//
// Test 5: the error is concentrated inside the spread, which is the part
// nobody labels.
//
// Test 6: the footprint conserves volume.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { formatPaise } from '../src/protocol.ts';
import {
  LadderProblem,
  costToFill,
  crossed,
  fromFrame,
  ladderRows,
} from '../src/chart/ladder.ts';
import type { Level } from '../src/chart/ladder.ts';
import {
  Rule,
  Side,
  classify,
  footprint,
  footprintTotal,
} from '../src/chart/footprint.ts';
import type { Trade } from '../src/chart/footprint.ts';

const TICK = 5n; // paise, from the spec store in the real thing

test('[1] coalescing the sides independently draws a book that never existed', () => {
  // A trending tape: the market walks up in ticks. Each frame publishes a
  // fresh bid and a fresh ask, but the client coalesces them per FIELD, so a
  // frame can pair a new bid with the previous frame's ask.
  const N = 20_000;
  let crossedFrames = 0;
  let mid = 2_400_000n;

  let staleAsk = mid + TICK;
  for (let i = 0; i < N; i++) {
    // Walk up two ticks out of three, which is what a trend looks like.
    if (i % 3 !== 0) mid += TICK;
    const bid = mid;
    const ask = mid + TICK;

    // The independently-coalesced render: fresh bid, previous ask.
    const bids: Level[] = [{ pricePaise: bid, quantity: 100n, orders: 3 }];
    const asks: Level[] = [{ pricePaise: staleAsk, quantity: 100n, orders: 3 }];
    if (crossed({ seq: BigInt(i), bids, asks })) crossedFrames++;
    staleAsk = ask;
  }

  const pct = (100 * crossedFrames) / N;
  console.log(
    `    ${N.toLocaleString()} frames with the two sides coalesced independently: ${crossedFrames.toLocaleString()} render CROSSED (${pct.toFixed(1)}%)`,
  );
  assert.ok(
    crossedFrames > N / 100,
    'a stale ask below a fresh bid draws a two-paise arbitrage in the middle of the screen that nobody can trade, because it never happened -- and in a trending market it is not a rare interleaving',
  );

  // Pairing by frame refuses the mismatch instead.
  const bad = fromFrame(
    10n,
    [{ pricePaise: 2_400_010n, quantity: 100n, orders: 1 }],
    9n,
    [{ pricePaise: 2_400_005n, quantity: 100n, orders: 1 }],
  );
  assert.equal(bad.ok, false);
  if (!bad.ok) {
    assert.equal(
      bad.problem,
      LadderProblem.Inconsistent,
      'the consistency unit is the FRAME, not the field: two halves with different sequence numbers were never simultaneously true',
    );
  }

  const good = fromFrame(
    10n,
    [{ pricePaise: 2_400_000n, quantity: 100n, orders: 1 }],
    10n,
    [{ pricePaise: 2_400_005n, quantity: 100n, orders: 1 }],
  );
  assert.equal(good.ok, true);
  if (good.ok) {
    assert.equal(
      crossed(good.snapshot),
      false,
      'and a properly paired snapshot is not crossed -- a crossed book is a fact about the renderer, never about the market',
    );
  }
});

test('[2] the ladder is a tick grid, and an empty level is an empty row', () => {
  const snap = {
    seq: 1n,
    // Orders at 2,400,000 and 2,400,015 -- three ticks apart, nothing between.
    bids: [
      { pricePaise: 2_400_000n, quantity: 500n, orders: 4 },
      { pricePaise: 2_399_985n, quantity: 700n, orders: 6 },
    ],
    asks: [
      { pricePaise: 2_400_015n, quantity: 300n, orders: 2 },
      { pricePaise: 2_400_020n, quantity: 400n, orders: 3 },
    ],
  };

  const r = ladderRows(snap, TICK, 4);
  assert.equal(r.ok, true);
  if (!r.ok) return;

  const blanks = r.rows.filter(
    (row) => row.bidQuantity === null && row.askQuantity === null,
  ).length;
  console.log(
    `    a book 3 ticks wide renders as ${r.rows.length} rows, ${blanks} of them empty`,
  );
  assert.ok(
    blanks > 0,
    'the ticks with no orders are PRESENT and blank -- render one row per existing level and a three-tick gap becomes a one-pixel step, and the picture says the book is tight when it is not',
  );

  const gridOk = r.rows.every((row) => row.pricePaise % TICK === 0n);
  assert.equal(gridOk, true, 'every row sits on the instrument tick grid');

  // Tick size is required, never assumed.
  assert.equal(ladderRows(snap, 0n, 4).ok, false);
  const noTick = ladderRows(snap, 0n, 4);
  if (!noTick.ok) assert.equal(noTick.problem, LadderProblem.NoTickSize);

  // A price off the grid means the spec store and the feed disagree.
  const offGrid = ladderRows(
    { ...snap, asks: [{ pricePaise: 2_400_013n, quantity: 1n, orders: 1 }] },
    TICK,
    4,
  );
  assert.equal(offGrid.ok, false);
  if (!offGrid.ok) {
    assert.equal(
      offGrid.problem,
      LadderProblem.OffTickGrid,
      'and a price that is not on the grid blocks rather than being rounded onto it -- rule 9, since a rounded price is a price nobody quoted',
    );
  }
});

test('[3] cost-to-fill reports what it could not fill', () => {
  const asks: Level[] = [
    { pricePaise: 2_400_000n, quantity: 50n, orders: 2 },
    { pricePaise: 2_400_005n, quantity: 30n, orders: 1 },
    { pricePaise: 2_400_010n, quantity: 40n, orders: 3 },
  ];

  const small = costToFill(asks, 60n);
  assert.equal(small.filled, 60n);
  assert.equal(small.unfilled, 0n);
  assert.equal(small.costPaise, 50n * 2_400_000n + 10n * 2_400_005n);
  console.log(
    `    60 lots costs Rs ${formatPaise(small.costPaise)}, VWAP Rs ${formatPaise(small.vwapPaise ?? 0n)}`,
  );
  assert.ok(
    (small.vwapPaise ?? 0n) > 2_400_000n && (small.vwapPaise ?? 0n) < 2_400_005n,
    'the VWAP to depth sits between the levels consumed -- the number a trader wants, unlike the average of the price column, which is an average of an intensive quantity and is a price of nothing',
  );

  const tooBig = costToFill(asks, 500n);
  console.log(
    `    500 lots requested, ${tooBig.filled} available, ${tooBig.unfilled} unfilled`,
  );
  assert.equal(tooBig.filled, 120n);
  assert.equal(
    tooBig.unfilled,
    380n,
    'and a request the book cannot fill reports the shortfall rather than quietly pricing the 120 lots that are there -- a VWAP for 500 lots computed from a book holding 120 is a number about a trade that cannot happen',
  );

  const empty = costToFill([], 10n);
  assert.equal(empty.vwapPaise, null, 'an empty book gives no VWAP rather than zero');
  assert.equal(empty.unfilled, 10n);
});

test('[4] the aggressor is inferred, and the rules are scored honestly', () => {
  // Two tapes where the truth is known by construction. The difference
  // between them is the whole point of the test.
  //
  //   tape A: the mid only moves because of the aggressor.
  //   tape B: the mid also drifts for its own reasons -- other participants,
  //           the index, a quote update between trades.
  //
  // A synthetic tape of the first kind FLATTERS the tick rule enormously,
  // because "price went up" and "a buyer crossed" are the same event by
  // construction. The first draft of this test used only that tape and
  // concluded the tick rule was the better of the two, which is an artefact
  // of the fixture rather than a fact about the rules.
  const N = 20_000;

  const buildTape = (withDrift: boolean): { trades: Trade[]; truth: Side[] } => {
    const trades: Trade[] = [];
    const truth: Side[] = [];
    let s = 0x1a2b3c4d;
    let mid = 2_400_000n;
    for (let i = 0; i < N; i++) {
      s = (s * 1664525 + 1013904223) >>> 0;
      if (withDrift) {
        // Drift unrelated to who crossed the spread.
        if (s % 3 === 0) mid += TICK;
        else if (s % 4 === 0) mid -= TICK;
      }
      const bid = mid;
      const ask = mid + 2n * TICK;
      const aggressorBuys = s % 2 === 0;
      const inside = s % 10 < 3;
      const price = inside ? bid + TICK : aggressorBuys ? ask : bid;
      trades.push({
        timeNs: BigInt(i),
        pricePaise: price,
        quantity: 1n,
        bidPaise: bid,
        askPaise: ask,
      });
      truth.push(aggressorBuys ? Side.Buy : Side.Sell);
    }
    return { trades, truth };
  };

  const score = (
    trades: readonly Trade[],
    truth: readonly Side[],
    rule: Rule,
  ): { classified: number; correct: number; wrong: number } => {
    let classified = 0;
    let correct = 0;
    let wrong = 0;
    let prev: bigint | null = null;
    for (let i = 0; i < trades.length; i++) {
      const t = trades[i]!;
      const got = classify(t, rule, prev);
      prev = t.pricePaise;
      if (got === Side.Unknown) continue;
      classified++;
      if (got === truth[i]) correct++;
      else wrong++;
    }
    return { classified, correct, wrong };
  };

  const report = (label: string, r: ReturnType<typeof score>): number => {
    const acc = r.classified > 0 ? (100 * r.correct) / r.classified : 0;
    console.log(
      `    ${label}: classified ${((100 * r.classified) / N).toFixed(1)}% of trades, and was right on ${acc.toFixed(1)}% of those (${r.wrong} wrong)`,
    );
    return acc;
  };

  const a = buildTape(false);
  const b = buildTape(true);

  console.log('    tape A -- the mid moves only when the aggressor crosses:');
  const qA = report('      quote rule', score(a.trades, a.truth, Rule.Quote));
  const kA = report('      tick rule ', score(a.trades, a.truth, Rule.Tick));

  console.log('    tape B -- the mid also drifts for its own reasons:');
  const qB = report('      quote rule', score(b.trades, b.truth, Rule.Quote));
  const kB = report('      tick rule ', score(b.trades, b.truth, Rule.Tick));

  assert.equal(
    qA,
    100,
    'the quote rule is exact on everything it classifies -- at the bid or the ask there is nothing to infer',
  );
  assert.equal(
    qB,
    100,
    'and that does not depend on the tape: it declines rather than guessing, so it is never wrong, only silent',
  );
  assert.ok(
    kB < kA,
    'while the tick rule degrades sharply once the mid drifts for reasons other than the aggressor -- which is every real tape, and is why it is the fallback rather than the method',
  );
  assert.ok(
    kB < 100,
    'so a footprint built on the tick rule carries real, unlabelled error, and no chart anywhere says which rule produced its colours',
  );
});

test('[5] the error is concentrated inside the spread', () => {
  const bid = 2_400_000n;
  const ask = 2_400_010n;
  const atQuote: Trade = {
    timeNs: 1n,
    pricePaise: ask,
    quantity: 1n,
    bidPaise: bid,
    askPaise: ask,
  };
  const inside: Trade = {
    timeNs: 2n,
    pricePaise: 2_400_005n,
    quantity: 1n,
    bidPaise: bid,
    askPaise: ask,
  };

  assert.equal(
    classify(atQuote, Rule.Quote, null),
    Side.Buy,
    'a trade at the ask is unambiguously a buy',
  );
  assert.equal(
    classify(inside, Rule.Quote, null),
    Side.Unknown,
    'and a trade at the midpoint is genuinely undetermined -- which is where internalised flow and every mid-price fill lands, so a wide-market footprint is materially less reliable than a tight-market one and nothing on the chart says so',
  );

  const f = footprint([atQuote, inside, inside], Rule.Quote);
  console.log(
    `    footprint coverage: ${(100 * f.coverage).toFixed(1)}% of volume could be assigned a side`,
  );
  assert.ok(f.coverage < 1, 'coverage is computed and carried, not assumed');
  const cell = f.cells.find((c) => c.pricePaise === 2_400_005n);
  assert.equal(
    cell?.unknownVolume,
    2n,
    'and the unclassified volume is carried per level, so a level that is 60% unknown is not drawn as demand',
  );
  assert.equal(cell?.buyVolume, 0n);

  // No quote at all: the quote rule declines rather than falling back.
  assert.equal(
    classify({ ...inside, bidPaise: null, askPaise: null }, Rule.Quote, null),
    Side.Unknown,
  );
});

test('[6] the footprint conserves volume', () => {
  const trades: Trade[] = [];
  let s = 0x5f6e7d8c;
  for (let i = 0; i < 5000; i++) {
    s = (s * 1664525 + 1013904223) >>> 0;
    const bid = 2_400_000n + BigInt((s % 40) * 5);
    trades.push({
      timeNs: BigInt(i),
      pricePaise: bid + BigInt((s % 3) * 5),
      quantity: BigInt((s % 20) + 1),
      bidPaise: bid,
      askPaise: bid + 10n,
    });
  }
  const f = footprint(trades, Rule.Quote);
  const tape = trades.reduce((a, t) => a + t.quantity, 0n);

  console.log(
    `    tape ${tape} = buy + sell + unknown ${footprintTotal(f)} across ${f.cells.length} levels`,
  );
  assert.equal(
    footprintTotal(f),
    tape,
    'buy plus sell plus unknown totals the tape exactly -- the same reconciliation as the candle chart, and it is what stops the unknown bucket being quietly dropped to make the colours add up',
  );
  assert.equal(f.totalVolume, tape);
  assert.ok(f.classifiedVolume < tape, 'with some volume genuinely unassigned');
});
