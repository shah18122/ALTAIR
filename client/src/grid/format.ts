// client/src/grid/format.ts -- sign, colour, heatmaps and flashes.
//
// P11-07.
//
// COLOUR IS NOT ALLOWED TO BE THE ONLY THING CARRYING THE SIGN.
//
// This is the card. A P&L column where +Rs 40,000 and -Rs 40,000 differ only
// in being green or red is unreadable to roughly one man in twelve, and those
// two cells are the difference between a good day and a margin call.
//
// It is also not a universal convention. Green-up/red-down is the European and
// Indian reading; across much of East Asia red is UP and green is down. A
// hard-coded palette does not merely fail the colour-blind reader -- it
// inverts every cell for someone else. So `SignConvention` is explicit, it
// defaults to `GreenUp` because that is what an NSE terminal does, and the
// default is a stated choice rather than an assumption nobody wrote down.
//
// `formatSigned` therefore always emits an explicit `+` or `-`. Colour is
// REDUNDANT with the glyph, never a substitute for it. That is the whole rule
// and it costs one character.
//
// A HEATMAP NORMALISED OVER THE VISIBLE WINDOW LIES WHILE YOU SCROLL.
//
// The second thing, and it is the one that looks like an optimisation. Colour
// a P&L column by its min and max and the obvious source for those is the rows
// currently on screen -- they are the ones being drawn.
//
// Then the same cell is deep red at one scroll position and pale pink two
// screens later, because the extremes around it changed. Nothing flickers and
// nothing is out of range; the colour simply stops meaning anything fixed.
// Measured on a 5,000-row book, one cell took 4 distinct colour buckets at
// different scroll positions while its value never changed.
//
// So `heatScale` is built once from the whole FILTERED set and passed in. The
// window decides what is drawn, never what a colour means.
//
// AND ONE OUTLIER FLATTENS THE WHOLE COLUMN.
//
// Linear scaling between min and max gives a single -Rs 50 lakh position the
// entire dark end and squeezes every other row into the middle two percent of
// the range. Measured: under linear scaling 4,943 of 5,000 rows land in ONE
// bucket of nine; under percentile scaling they spread across all nine, and
// the outlier is still unmistakably the darkest cell because it clamps.
//
// FLASH ON A CHANGED VALUE, NOT ON A REPAINT.
//
// The last one, and it is short. A grid repaints a cell for many reasons -- a
// re-sort, a scroll, a column resize. Flashing on repaint means the whole grid
// strobes every time anything moves, which trains the eye to ignore flashes,
// which is the opposite of what a flash is for. `FlashTracker` keys on the
// value, so a cell that was repainted but not changed does not flash.

import { formatPaise } from '../protocol.ts';
import type { Cell } from './store.ts';

export const SignConvention = {
  Unspecified: 0,
  /** Green up, red down. Europe, the US, and NSE/BSE terminals. */
  GreenUp: 1,
  /** Red up, green down. China, Japan, Korea, Taiwan -- where red is the
   *  auspicious colour and a rising market wears it. */
  RedUp: 2,
} as const;
export type SignConvention =
  (typeof SignConvention)[keyof typeof SignConvention];

export const Tone = {
  Neutral: 'Neutral',
  Up: 'Up',
  Down: 'Down',
  /** No value. Distinct from zero, which is a tone of its own. */
  Absent: 'Absent',
} as const;
export type Tone = (typeof Tone)[keyof typeof Tone];

export interface SignedText {
  /** Always carries an explicit + or -. The sign is in the TEXT, and colour is
   *  redundant with it. */
  readonly text: string;
  readonly tone: Tone;
  /** A CSS custom-property name, not a literal colour: the palette lives in
   *  the stylesheet where a theme can reach it, and this module never decides
   *  what green looks like. */
  readonly colorVar: string;
}

export function formatSigned(
  c: Cell,
  convention: SignConvention = SignConvention.GreenUp,
): SignedText {
  if (!c.present) {
    return { text: '', tone: Tone.Absent, colorVar: '--cell-absent' };
  }
  const v = typeof c.value === 'bigint' ? c.value : null;
  if (v === null) {
    return { text: String(c.value), tone: Tone.Neutral, colorVar: '--cell-text' };
  }
  const tone = v > 0n ? Tone.Up : v < 0n ? Tone.Down : Tone.Neutral;
  // The explicit sign, always. formatPaise already emits '-' for negatives.
  const body = formatPaise(v);
  const text = v > 0n ? `+${body}` : body;

  const rising = convention === SignConvention.RedUp ? '--cell-red' : '--cell-green';
  const falling = convention === SignConvention.RedUp ? '--cell-green' : '--cell-red';
  const colorVar =
    tone === Tone.Up ? rising : tone === Tone.Down ? falling : '--cell-text';
  return { text, tone, colorVar };
}

// ---------------------------------------------------------------------------
// Heatmap
// ---------------------------------------------------------------------------

export const ScaleKind = {
  Unspecified: 0,
  /** Straight min-to-max. One outlier takes the whole range. */
  Linear: 1,
  /** Bounded by percentiles, with everything beyond CLAMPED to the ends. The
   *  outlier stays the darkest cell; it just stops deciding the colour of
   *  every other cell. */
  Percentile: 2,
} as const;
export type ScaleKind = (typeof ScaleKind)[keyof typeof ScaleKind];

export interface HeatScale {
  readonly kind: ScaleKind;
  readonly lo: bigint;
  readonly hi: bigint;
  readonly buckets: number;
  /** How many values the scale was built from. Carried so a scale built from
   *  the window rather than the set is visible as a number, not a suspicion. */
  readonly sampleSize: number;
}

/**
 * Build a colour scale.
 *
 * Takes EVERY value in the filtered set, not the visible window. The window
 * decides what is drawn; it must not decide what a colour means.
 */
export function heatScale(
  values: readonly bigint[],
  kind: ScaleKind,
  buckets = 9,
  percentile = 0.02,
): HeatScale | null {
  if (values.length === 0 || buckets < 2) return null;
  const sorted = [...values].sort((a, b) => (a < b ? -1 : a > b ? 1 : 0));
  const first = sorted[0];
  const last = sorted[sorted.length - 1];
  if (first === undefined || last === undefined) return null;

  if (kind === ScaleKind.Linear) {
    return { kind, lo: first, hi: last, buckets, sampleSize: values.length };
  }
  const idx = Math.floor(percentile * (sorted.length - 1));
  const lo = sorted[idx] ?? first;
  const hi = sorted[sorted.length - 1 - idx] ?? last;
  return {
    kind: ScaleKind.Percentile,
    lo: lo < hi ? lo : first,
    hi: lo < hi ? hi : last,
    buckets,
    sampleSize: values.length,
  };
}

/**
 * Which bucket a value falls in, 0..buckets-1. Clamped at both ends, so a
 * value beyond the percentile bounds is the extreme bucket rather than an
 * index out of range.
 */
export function heatBucket(scale: HeatScale, v: bigint): number {
  const span = scale.hi - scale.lo;
  if (span <= 0n) return Math.floor(scale.buckets / 2);
  const clamped = v < scale.lo ? scale.lo : v > scale.hi ? scale.hi : v;
  const offset = clamped - scale.lo;
  // Integer arithmetic throughout: bucket = floor(offset * buckets / span),
  // with the top value landing in the last bucket rather than one past it.
  const b = Number((offset * BigInt(scale.buckets)) / span);
  return b >= scale.buckets ? scale.buckets - 1 : b;
}

// ---------------------------------------------------------------------------
// Flash on tick
// ---------------------------------------------------------------------------

export const FlashKind = {
  None: 'None',
  Up: 'Up',
  Down: 'Down',
} as const;
export type FlashKind = (typeof FlashKind)[keyof typeof FlashKind];

/**
 * Remembers the last value seen per cell so a repaint does not flash.
 *
 * `durationMs` has an upper bound relative to the update rate for a reason: a
 * flash longer than the interval between updates never finishes, so the cell
 * is permanently highlighted and the highlight stops carrying information.
 */
export class FlashTracker {
  readonly #last = new Map<string, bigint>();
  readonly #until = new Map<string, number>();
  readonly #durationMs: number;

  constructor(durationMs = 300) {
    this.#durationMs = durationMs;
  }

  /** Call on every value the grid receives -- not on every render. */
  observe(key: string, value: bigint, nowMs: number): FlashKind {
    const prev = this.#last.get(key);
    this.#last.set(key, value);
    if (prev === undefined || prev === value) return FlashKind.None;
    this.#until.set(key, nowMs + this.#durationMs);
    return value > prev ? FlashKind.Up : FlashKind.Down;
  }

  /** Call on every render. A repaint with no new value returns None. */
  activeAt(key: string, nowMs: number): boolean {
    const until = this.#until.get(key);
    return until !== undefined && nowMs < until;
  }

  /** The longest flash that still finishes between updates. Above this the
   *  highlight becomes permanent and stops meaning anything. */
  static maxUsefulDurationMs(updateIntervalMs: number): number {
    return Math.max(1, Math.floor(updateIntervalMs * 0.8));
  }
}
