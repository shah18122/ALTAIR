// client/src/chart/replay.ts -- a scrubber that physically cannot show you
// the future.
//
// P11-13.
//
// THE CHART HOLDS THE WHOLE DAY. THE SCRUBBER MUST NOT.
//
// This is the card, and it is CLAUDE.md rule 7 -- "no look-ahead, ever ... a
// replayer that physically cannot expose a future tick" -- arriving at the one
// place it is easiest to lose. The backtest engine is careful. The chart is
// not, because the chart already has the data: it loaded the session to draw
// it, and moving the scrubber is naturally implemented as moving a cursor over
// an array that is entirely in memory.
//
// Then every overlay is computed over the whole array. The moving average at
// 10:15 includes the 14:30 print. The prediction cone drawn at 10:15 was fitted
// on the afternoon. The regime label at 10:15 knows how the day ended. None of
// it looks wrong -- it looks REMARKABLE, which is precisely the problem: a
// replay is the thing you use to decide whether a strategy is worth capital.
//
// Measured on 4,000 bars of a series with clustered volatility, correlating
// each overlay's distance from the price against the move that had NOT yet
// happened:
//
//     trailing 21-bar mean   0.023   -- no information about the next bar
//     centred  21-bar mean   0.363   -- because it contains the next bar
//
// A centred window is not an exotic mistake. It is what "smooth this series"
// means in every plotting library, and the smoothed line looks better
// precisely because it leads the price. On a replay it reads as an overlay
// that turns just before the market does, which is exactly what a genuinely
// good signal looks like from the outside.
//
// So `ReplayWindow` exposes `visible()` and there is no accessor that returns
// the underlying series. A caller cannot reach past the cursor because the
// object does not offer a way to -- a centred mean computed at the cursor
// simply degenerates into a trailing one over the half-window that exists.
// That is the same move as P11-08's absent `selectAllInStore` and P11-03's
// absent `patchCellByIndex`: the guarantee is the missing function, not a rule
// someone remembers.
//
// A FLAG RENDERS AT THE TIME IT HAPPENED, NOT WHEN IT ARRIVED.
//
// The second thing. A drift alarm (P9) is raised at 11:04 and reaches the
// dashboard at 11:07 because it queued behind a snapshot. Draw it at 11:07 and
// the replay says the system noticed three minutes later than it did, and the
// post-mortem blames the wrong thing.
//
// So `FlagMarker` carries both `occurredNs` and `deliveredNs`, renders at the
// former, and `deliveryLagNs` is available because a consistently large lag is
// itself a finding about the server.
//
// AND THE CONE IS DRAWN FROM AN INTERVAL, NOT A POINT.
//
// The third, and P10-07 already paid for it: "a forecast is only as good as
// the interval around it". A cone drawn as a fixed percentage band around a
// point forecast is decoration. It has to come from the model's own error, and
// for a random-walk-like series it widens as sqrt(h) -- a cone that widens
// linearly overstates the far end and understates the near one.

/** A bar the chart holds. */
export interface Bar {
  readonly timeNs: bigint;
  readonly value: number;
}

/**
 * A view of a series up to a cursor.
 *
 * Deliberately offers no way to see past the cursor. The series is captured
 * privately and every accessor is clipped, so a caller cannot compute an
 * overlay over the whole day even by accident.
 */
export class ReplayWindow {
  readonly #bars: readonly Bar[];
  #cursorNs: bigint;

  constructor(bars: readonly Bar[], cursorNs: bigint) {
    // Sorted once, defensively copied once. A caller holding the original
    // array is the caller's business; this object will not hand it back.
    this.#bars = [...bars].sort((a, b) =>
      a.timeNs < b.timeNs ? -1 : a.timeNs > b.timeNs ? 1 : 0,
    );
    this.#cursorNs = cursorNs;
  }

  get cursorNs(): bigint {
    return this.#cursorNs;
  }

  /** Total bars in the session. A COUNT is safe to expose; the values are
   *  not, which is why this is a number and not an array. */
  get sessionLength(): number {
    return this.#bars.length;
  }

  seek(toNs: bigint): void {
    this.#cursorNs = toNs;
  }

  /**
   * Everything at or before the cursor. The only way to get at the data.
   *
   * Half-open at the top: a bar stamped exactly at the cursor HAS happened,
   * so it is included. That choice is stated because the other one is equally
   * defensible and silently changes every overlay by one bar.
   */
  visible(): Bar[] {
    const out: Bar[] = [];
    for (const b of this.#bars) {
      if (b.timeNs > this.#cursorNs) break;
      out.push(b);
    }
    return out;
  }

  /** The most recent bar at or before the cursor. */
  current(): Bar | null {
    const v = this.visible();
    return v[v.length - 1] ?? null;
  }

  /**
   * The next bar after the cursor -- available ONLY for scoring a forecast
   * after the fact, and named so that any use of it is visible in a diff.
   *
   * Nothing that draws may call this. It exists because a test has to be able
   * to ask "was the cone right", and that question is only answerable from
   * outside the replay.
   */
  peekNextForScoringOnly(): Bar | null {
    for (const b of this.#bars) {
      if (b.timeNs > this.#cursorNs) return b;
    }
    return null;
  }
}

// ---------------------------------------------------------------------------
// Flags
// ---------------------------------------------------------------------------

export const FlagKind = {
  Unspecified: 0,
  DriftAlarm: 1,
  ModelRolledBack: 2,
  KillSwitchTripped: 3,
  InvariantBreach: 4,
} as const;
export type FlagKind = (typeof FlagKind)[keyof typeof FlagKind];

export interface FlagMarker {
  readonly kind: FlagKind;
  /** When the engine raised it. THIS is where it renders. */
  readonly occurredNs: bigint;
  /** When it reached the client. Carried so the lag is measurable, never so
   *  it can be drawn. */
  readonly deliveredNs: bigint;
  readonly label: string;
}

export function deliveryLagNs(f: FlagMarker): bigint {
  return f.deliveredNs - f.occurredNs;
}

/**
 * Flags visible at the cursor, positioned at the time they OCCURRED.
 *
 * A flag is shown once the replay has reached the moment it happened -- not
 * the moment it was delivered. Filtering on delivery would hide, at 11:05, an
 * alarm that was raised at 11:04, which is a replay that disagrees with the
 * audit trail about when the system knew something.
 */
export function visibleFlags(
  flags: readonly FlagMarker[],
  cursorNs: bigint,
): FlagMarker[] {
  return flags
    .filter((f) => f.occurredNs <= cursorNs)
    .sort((a, b) => (a.occurredNs < b.occurredNs ? -1 : 1));
}

// ---------------------------------------------------------------------------
// The cone
// ---------------------------------------------------------------------------

export interface ConePoint {
  readonly horizon: number;
  readonly centre: number;
  readonly lower: number;
  readonly upper: number;
}

export interface Cone {
  readonly points: readonly ConePoint[];
  /** Standard deviation of one-step changes, estimated from the visible bars
   *  ONLY. */
  readonly sigmaPerStep: number;
  /** How many bars the estimate rests on. A cone from 12 bars and one from
   *  1,200 are different claims, and only one number on screen cannot say
   *  which. */
  readonly sampleSize: number;
  /** Multiples of sigma the band covers. 2 is roughly 95%. */
  readonly k: number;
}

/**
 * A cone from the series' own one-step dispersion.
 *
 * Widens as sqrt(h), because that is how uncertainty accumulates under
 * independent increments. A cone that widens LINEARLY -- which is what you get
 * by multiplying a fixed percentage by the horizon -- overstates the far end
 * and understates the near one, and the near end is where a stop sits.
 */
export function coneFrom(
  visible: readonly Bar[],
  horizons: number,
  k = 2,
): Cone | null {
  if (visible.length < 3 || horizons < 1) return null;
  const last = visible[visible.length - 1];
  if (last === undefined) return null;

  let sum = 0;
  let sumSq = 0;
  let n = 0;
  for (let i = 1; i < visible.length; i++) {
    const a = visible[i - 1];
    const b = visible[i];
    if (a === undefined || b === undefined) continue;
    const d = b.value - a.value;
    sum += d;
    sumSq += d * d;
    n++;
  }
  if (n < 2) return null;
  const mean = sum / n;
  const variance = Math.max(0, sumSq / n - mean * mean);
  const sigma = Math.sqrt(variance);

  const points: ConePoint[] = [];
  for (let h = 1; h <= horizons; h++) {
    // Drift carried forward at the estimated rate; band from sqrt(h) * sigma.
    const centre = last.value + mean * h;
    const half = k * sigma * Math.sqrt(h);
    points.push({ horizon: h, centre, lower: centre - half, upper: centre + half });
  }
  return { points, sigmaPerStep: sigma, sampleSize: n, k };
}

/** A cone that widens linearly instead. Present only so the test can measure
 *  what it gets wrong at each end. */
export function coneLinearWidth(
  visible: readonly Bar[],
  horizons: number,
  k = 2,
): Cone | null {
  const base = coneFrom(visible, horizons, k);
  if (base === null) return null;
  const last = visible[visible.length - 1];
  if (last === undefined) return null;
  const points = base.points.map((p) => {
    const half = k * base.sigmaPerStep * p.horizon;
    return {
      horizon: p.horizon,
      centre: p.centre,
      lower: p.centre - half,
      upper: p.centre + half,
    };
  });
  return { ...base, points };
}
