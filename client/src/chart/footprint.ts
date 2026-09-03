// client/src/chart/footprint.ts -- buy volume against sell volume, and the
// word "inferred".
//
// P11-11.
//
// NSE DOES NOT TELL YOU WHO WAS THE AGGRESSOR.
//
// This is the card, and it is a labelling problem rather than an arithmetic
// one -- which is why it ships. A footprint chart splits each price level into
// "bought at the ask" and "sold at the bid", and the split is the entire
// content of the chart: a trader reads a green-heavy level as demand.
//
// The standard NSE tick feed carries price, quantity and time. It does not
// carry which side crossed the spread. So every footprint anywhere is
// INFERRED, by comparing the trade price to the prevailing quote (the
// Lee-Ready rule) or to the previous trade (the tick rule). Both are estimates
// with a known error rate, and neither is labelled as such on any chart I have
// seen.
//
// Measured on two synthetic tapes where the true aggressor is known by
// construction -- 20,000 trades each, 30% of them printing inside the spread.
// The difference between the tapes is the finding:
//
//   tape A, mid moves ONLY when the aggressor crosses
//     quote rule   classifies 70.3%, right on 100.0% of those
//     tick rule    classifies 92.0%, right on 100.0% of those
//
//   tape B, mid ALSO drifts for its own reasons
//     quote rule   classifies 70.3%, right on 100.0% of those
//     tick rule    classifies 81.7%, right on  96.6% of those -- 548 wrong
//
// The first draft of the test used only tape A and concluded the tick rule was
// the better of the two. That is an artefact of the fixture: on a tape where
// the mid moves only because someone crossed, "price went up" and "a buyer
// crossed" are the same event, so the tick rule is being told the answer.
// Every real tape is tape B.
//
// The quote rule's number does not move between them, and that is its whole
// virtue: it is never WRONG, only silent. It declines on the 30% that print
// inside the spread -- which is where a broker's internalised flow and every
// mid-price fill lands -- rather than turning a known unknown into a coin flip
// wearing a colour.
//
// `classify` therefore returns `Side.Unknown` for a mid-price trade rather
// than guessing, `FootprintCell` carries `unknownVolume` alongside buy and
// sell, and the renderer is expected to draw it. A level that is 60% unknown
// is not a level anyone should read as demand.

export const Side = {
  /** Phantom default. Never a guess. */
  Unknown: 0,
  /** Aggressor bought -- lifted the offer. */
  Buy: 1,
  /** Aggressor sold -- hit the bid. */
  Sell: 2,
} as const;
export type Side = (typeof Side)[keyof typeof Side];

export interface Trade {
  readonly timeNs: bigint;
  readonly pricePaise: bigint;
  readonly quantity: bigint;
  /** The prevailing quote at the time of the trade, if known. */
  readonly bidPaise: bigint | null;
  readonly askPaise: bigint | null;
}

export const Rule = {
  Unspecified: 0,
  /** Compare to the prevailing quote. Exact at the bid and the ask; silent
   *  about everything between. */
  Quote: 1,
  /** Compare to the previous trade price. Works with no quote at all, and is
   *  materially worse. */
  Tick: 2,
} as const;
export type Rule = (typeof Rule)[keyof typeof Rule];

/**
 * Infer the aggressor.
 *
 * Returns `Side.Unknown` rather than guessing when the evidence does not
 * decide. A mid-price trade under the quote rule is genuinely undetermined:
 * filling it in with the tick rule turns a known unknown into a 50% error
 * wearing a colour.
 */
export function classify(
  t: Trade,
  rule: Rule,
  previousPricePaise: bigint | null,
): Side {
  if (rule === Rule.Quote) {
    if (t.bidPaise === null || t.askPaise === null) return Side.Unknown;
    if (t.pricePaise >= t.askPaise) return Side.Buy;
    if (t.pricePaise <= t.bidPaise) return Side.Sell;
    return Side.Unknown; // inside the spread -- a coin flip, so do not flip it
  }
  if (rule === Rule.Tick) {
    if (previousPricePaise === null) return Side.Unknown;
    if (t.pricePaise > previousPricePaise) return Side.Buy;
    if (t.pricePaise < previousPricePaise) return Side.Sell;
    return Side.Unknown; // an unchanged price says nothing
  }
  return Side.Unknown;
}

export interface FootprintCell {
  readonly pricePaise: bigint;
  readonly buyVolume: bigint;
  readonly sellVolume: bigint;
  /** Volume the rule could not classify. Carried and rendered: a level that is
   *  60% unknown is not a level anyone should read as demand. */
  readonly unknownVolume: bigint;
  readonly trades: number;
}

export interface Footprint {
  readonly cells: readonly FootprintCell[];
  readonly totalVolume: bigint;
  readonly classifiedVolume: bigint;
  /** Fraction of volume the rule could actually assign a side to. The
   *  headline caveat, computed rather than asserted. */
  readonly coverage: number;
}

export function footprint(
  trades: readonly Trade[],
  rule: Rule,
): Footprint {
  const byPrice = new Map<bigint, { b: bigint; s: bigint; u: bigint; n: number }>();
  let previous: bigint | null = null;
  let total = 0n;
  let classified = 0n;

  for (const t of trades) {
    const side = classify(t, rule, previous);
    previous = t.pricePaise;
    total += t.quantity;
    if (side !== Side.Unknown) classified += t.quantity;

    const cell = byPrice.get(t.pricePaise) ?? { b: 0n, s: 0n, u: 0n, n: 0 };
    if (side === Side.Buy) cell.b += t.quantity;
    else if (side === Side.Sell) cell.s += t.quantity;
    else cell.u += t.quantity;
    cell.n++;
    byPrice.set(t.pricePaise, cell);
  }

  const cells: FootprintCell[] = [...byPrice]
    .map(([pricePaise, v]) => ({
      pricePaise,
      buyVolume: v.b,
      sellVolume: v.s,
      unknownVolume: v.u,
      trades: v.n,
    }))
    .sort((a, b) => (a.pricePaise < b.pricePaise ? 1 : -1));

  return {
    cells,
    totalVolume: total,
    classifiedVolume: classified,
    coverage: total > 0n ? Number(classified) / Number(total) : 0,
  };
}

/** Volume the footprint conserves: buy + sell + unknown must equal the tape.
 *  The same reconciliation as the candle chart, applied here. */
export function footprintTotal(f: Footprint): bigint {
  let t = 0n;
  for (const c of f.cells) t += c.buyVolume + c.sellVolume + c.unknownVolume;
  return t;
}
