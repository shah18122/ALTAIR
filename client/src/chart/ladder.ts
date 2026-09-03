// client/src/chart/ladder.ts -- the depth ladder, and the book that never
// existed.
//
// P11-11.
//
// COALESCING THE TWO SIDES INDEPENDENTLY DRAWS A CROSSED BOOK.
//
// This is the card, and it comes straight out of P11-01's State channel. The
// server coalesces -- last-writer-wins per field -- because sixty frames a
// second cannot carry every book update. That is correct for a quote. It is
// correct for one SIDE of a book. It is not correct for a book.
//
// Take the bid from frame N and the ask from frame N-1 and you have a bid and
// an ask that were never simultaneously true. When the market moves up quickly
// the stale ask sits BELOW the fresh bid, and the ladder draws a crossed book:
// a two-paise arbitrage, in the middle of the screen, that nobody can trade
// because it never happened.
//
// Measured on a tape that ticks up on two of every three updates, with the two
// sides coalesced independently: 13,333 of 20,000 frames render crossed --
// 66.7%, which is exactly the fraction on which the price moved at all. That
// is the point. It is not an unlucky interleaving; it crosses EVERY TIME the
// market moves and the ask is one frame behind.
//
// So a `LadderSnapshot` carries BOTH sides and a sequence number, and
// `fromFrame` refuses a pairing whose halves came from different frames. The
// consistency unit is the frame, not the field. `crossed()` is kept as a
// runtime assertion for the debug build, because a crossed book is a fact
// about the renderer and never about the market.
//
// THE LADDER IS BUILT ON THE TICK GRID, NOT ON THE LEVELS THAT HAPPEN TO EXIST.
//
// The second thing. A book has orders at 2,400,00 and 2,400,15 and nothing
// between. Render one row per level and those two sit adjacent, one pixel
// apart, and the picture says the book is tight. It is three ticks wide.
//
// So the ladder is a grid at the instrument's tick size -- from the spec
// store, never a literal (rule 1) -- and a level with no order is an EMPTY
// ROW, present and blank. That is P5-02 once more: absence is not zero, and
// here it is not absence-of-a-row either.
//
// DEPTH ADDS; A BOOK'S "AVERAGE PRICE" DOES NOT.
//
// Quantity at a level is extensive, so cumulative depth is a sum and means
// something. The average of the price column is not a price of anything
// (P11-05). What a trader actually wants is the volume-weighted price to a
// given depth -- what it would cost to take that many lots -- so that is what
// `costToFill` computes, and it reports how much of the requested size the
// book could NOT fill rather than quietly pricing what is there.

/** One price level. */
export interface Level {
  readonly pricePaise: bigint;
  /** Total quantity resting at this price. Extensive: cumulative depth is a
   *  meaningful sum. */
  readonly quantity: bigint;
  readonly orders: number;
}

/**
 * Both sides, from ONE frame.
 *
 * `seq` is the frame's sequence number (P11-01). It is here so a pairing can
 * be checked rather than trusted: two halves with different `seq` values were
 * never simultaneously true.
 */
export interface LadderSnapshot {
  readonly seq: bigint;
  /** Best bid first, descending. */
  readonly bids: readonly Level[];
  /** Best ask first, ascending. */
  readonly asks: readonly Level[];
}

export const LadderProblem = {
  /** The two sides came from different frames. */
  Inconsistent: 'Inconsistent',
  /** Tick size was not supplied. Never a literal (rule 1). */
  NoTickSize: 'NoTickSize',
  /** A price is not on the instrument's tick grid, which means the spec store
   *  and the feed disagree -- a blocking condition, not a rounding job. */
  OffTickGrid: 'OffTickGrid',
  Empty: 'Empty',
} as const;
export type LadderProblem = (typeof LadderProblem)[keyof typeof LadderProblem];

export type SnapshotResult =
  | { readonly ok: true; readonly snapshot: LadderSnapshot }
  | { readonly ok: false; readonly problem: LadderProblem };

/**
 * Pair two sides, refusing a pairing from different frames.
 *
 * The whole point of the function. A renderer that takes whatever the two
 * sides currently hold will, in a fast market, draw a book that never existed.
 */
export function fromFrame(
  bidSeq: bigint,
  bids: readonly Level[],
  askSeq: bigint,
  asks: readonly Level[],
): SnapshotResult {
  if (bidSeq !== askSeq) {
    return { ok: false, problem: LadderProblem.Inconsistent };
  }
  return { ok: true, snapshot: { seq: bidSeq, bids, asks } };
}

/**
 * Is the best bid at or above the best ask?
 *
 * A fact about the renderer, never about the market: a real crossed book is
 * arbitraged away in microseconds and is not something a 60 fps dashboard
 * observes. Kept as a debug-build assertion.
 */
export function crossed(s: LadderSnapshot): boolean {
  const bid = s.bids[0];
  const ask = s.asks[0];
  if (bid === undefined || ask === undefined) return false;
  return bid.pricePaise >= ask.pricePaise;
}

export interface LadderRow {
  readonly pricePaise: bigint;
  /** Absent, not zero: no order rested here. */
  readonly bidQuantity: bigint | null;
  readonly askQuantity: bigint | null;
  readonly cumulativeBid: bigint;
  readonly cumulativeAsk: bigint;
}

export type LadderResult =
  | { readonly ok: true; readonly rows: readonly LadderRow[] }
  | { readonly ok: false; readonly problem: LadderProblem };

/**
 * Lay the book out on the instrument's tick grid.
 *
 * `tickSizePaise` comes from the spec store. A level with no order becomes an
 * empty row rather than being omitted -- omitting it draws a three-tick gap as
 * a one-pixel step and says the book is tight when it is not.
 */
export function ladderRows(
  s: LadderSnapshot,
  tickSizePaise: bigint,
  depthRows: number,
): LadderResult {
  if (tickSizePaise <= 0n) {
    return { ok: false, problem: LadderProblem.NoTickSize };
  }
  const bestBid = s.bids[0];
  const bestAsk = s.asks[0];
  if (bestBid === undefined || bestAsk === undefined) {
    return { ok: false, problem: LadderProblem.Empty };
  }
  for (const l of [...s.bids, ...s.asks]) {
    if (l.pricePaise % tickSizePaise !== 0n) {
      // The spec store and the feed disagree. Rule 9: fail loud.
      return { ok: false, problem: LadderProblem.OffTickGrid };
    }
  }

  const bidAt = new Map<bigint, bigint>();
  for (const l of s.bids) bidAt.set(l.pricePaise, l.quantity);
  const askAt = new Map<bigint, bigint>();
  for (const l of s.asks) askAt.set(l.pricePaise, l.quantity);

  // Highest ask row down to lowest bid row, on the grid.
  const top = bestAsk.pricePaise + BigInt(depthRows - 1) * tickSizePaise;
  const bottom = bestBid.pricePaise - BigInt(depthRows - 1) * tickSizePaise;

  const rows: LadderRow[] = [];
  let cumAsk = 0n;
  for (let p = top; p >= bestAsk.pricePaise; p -= tickSizePaise) {
    cumAsk = 0n;
    for (let q = bestAsk.pricePaise; q <= p; q += tickSizePaise) {
      cumAsk += askAt.get(q) ?? 0n;
    }
    rows.push({
      pricePaise: p,
      bidQuantity: null,
      askQuantity: askAt.get(p) ?? null,
      cumulativeBid: 0n,
      cumulativeAsk: cumAsk,
    });
  }
  let cumBid = 0n;
  for (let p = bestBid.pricePaise; p >= bottom; p -= tickSizePaise) {
    cumBid += bidAt.get(p) ?? 0n;
    rows.push({
      pricePaise: p,
      bidQuantity: bidAt.get(p) ?? null,
      askQuantity: null,
      cumulativeBid: cumBid,
      cumulativeAsk: 0n,
    });
  }
  return { ok: true, rows };
}

export interface FillCost {
  /** Quantity actually available. Less than requested when the book is thin. */
  readonly filled: bigint;
  readonly requested: bigint;
  /** Total paise paid, exact. */
  readonly costPaise: bigint;
  /** Volume-weighted price of the fill, in paise, rounded once. Null when
   *  nothing could be filled. */
  readonly vwapPaise: bigint | null;
  /** How much of the request the book could not fill. Reported rather than
   *  silently priced: a VWAP for 500 lots computed from a book holding 120 is
   *  a number about a trade that cannot happen. */
  readonly unfilled: bigint;
}

/**
 * What it would cost to take `quantity` from this side.
 *
 * The number a trader actually wants from a depth ladder -- not the average of
 * the price column, which is an average of an intensive quantity and is a
 * price of nothing (P11-05).
 */
export function costToFill(
  side: readonly Level[],
  quantity: bigint,
): FillCost {
  let remaining = quantity;
  let cost = 0n;
  let filled = 0n;
  for (const l of side) {
    if (remaining <= 0n) break;
    const take = l.quantity < remaining ? l.quantity : remaining;
    cost += take * l.pricePaise;
    filled += take;
    remaining -= take;
  }
  const vwap =
    filled > 0n
      ? (cost * 2n + filled) / (filled * 2n) // round half up, once
      : null;
  return {
    filled,
    requested: quantity,
    costPaise: cost,
    vwapPaise: vwap,
    unfilled: remaining > 0n ? remaining : 0n,
  };
}
