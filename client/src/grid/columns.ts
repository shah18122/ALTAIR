// client/src/grid/columns.ts -- pin, reorder, resize, column groups.
//
// P11-06.
//
// A PINNED COLUMN IS NOT A POSITION, IT IS A REGION.
//
// The bookkeeping half of the card, and it has one trap worth naming. Pinning
// is usually implemented as "move this column to index 0 and remember that it
// is special". Then a reorder drags something else to index 0, and the pinned
// column is now floating in the scrolling region while still drawn frozen --
// or, worse, two columns claim the same frozen slot and one is painted over
// the other.
//
// So order is stored as three ordered lists, not one list plus a flag.
// `pinnedLeft`, `scrolling`, `pinnedRight` -- a column is in exactly one, and
// moving it between them is the pin operation. There is no state in which a
// column is pinned and also somewhere else, because there is nowhere else for
// it to be.
//
// A COLUMN NARROWER THAN ITS GRIP CANNOT BE WIDENED AGAIN.
//
// The other trap, and the reason `MIN_COLUMN_PX` exists in P11-02b's layout
// schema too. Drag a resize handle past zero and the column has no width, so
// it has no resize handle, so it cannot be dragged back. The layout persists,
// the user reopens the dashboard, and the column is gone for good.
//
// `resize` therefore clamps rather than refusing: a clamp keeps the drag
// feeling continuous while making the unrecoverable state unreachable.

import { MAX_COLUMN_PX, MIN_COLUMN_PX } from '../layout.ts';

export const Pin = {
  Unspecified: 0,
  Left: 1,
  None: 2,
  Right: 3,
} as const;
export type Pin = (typeof Pin)[keyof typeof Pin];

export interface ColumnGroup {
  readonly key: string;
  readonly label: string;
  readonly members: readonly string[];
}

export interface ColumnOrder {
  readonly pinnedLeft: readonly string[];
  readonly scrolling: readonly string[];
  readonly pinnedRight: readonly string[];
}

export function flatten(order: ColumnOrder): string[] {
  return [...order.pinnedLeft, ...order.scrolling, ...order.pinnedRight];
}

/** Which region a column is in. Exactly one, always. */
export function pinOf(order: ColumnOrder, key: string): Pin {
  if (order.pinnedLeft.includes(key)) return Pin.Left;
  if (order.pinnedRight.includes(key)) return Pin.Right;
  if (order.scrolling.includes(key)) return Pin.None;
  return Pin.Unspecified;
}

function without(list: readonly string[], key: string): string[] {
  return list.filter((k) => k !== key);
}

/** Move a column into a region. Removing it from the other two is part of the
 *  same operation, so there is no window in which it is in both. */
export function setPin(
  order: ColumnOrder,
  key: string,
  pin: Pin,
): ColumnOrder {
  if (pin === Pin.Unspecified) return order;
  const left = without(order.pinnedLeft, key);
  const mid = without(order.scrolling, key);
  const right = without(order.pinnedRight, key);
  switch (pin) {
    case Pin.Left:
      return { pinnedLeft: [...left, key], scrolling: mid, pinnedRight: right };
    case Pin.Right:
      return { pinnedLeft: left, scrolling: mid, pinnedRight: [key, ...right] };
    case Pin.None:
    default:
      return { pinnedLeft: left, scrolling: [...mid, key], pinnedRight: right };
  }
}

/**
 * Reorder within a region.
 *
 * `to` is an index in the SAME region as `from`. A drag that crosses into the
 * frozen area is a pin, not a reorder, and is a different call -- collapsing
 * the two is how a column ends up frozen and scrolling at once.
 */
export function reorderWithin(
  order: ColumnOrder,
  key: string,
  to: number,
): ColumnOrder {
  const pin = pinOf(order, key);
  const pick = (o: ColumnOrder): readonly string[] =>
    pin === Pin.Left ? o.pinnedLeft : pin === Pin.Right ? o.pinnedRight : o.scrolling;
  const list = [...pick(order)];
  const from = list.indexOf(key);
  if (from < 0) return order;
  list.splice(from, 1);
  list.splice(Math.max(0, Math.min(to, list.length)), 0, key);
  switch (pin) {
    case Pin.Left:
      return { ...order, pinnedLeft: list };
    case Pin.Right:
      return { ...order, pinnedRight: list };
    default:
      return { ...order, scrolling: list };
  }
}

/** Clamp, do not refuse. A refusal makes the drag feel broken; a clamp keeps
 *  it continuous and makes the width from which there is no return
 *  unreachable. */
export function resize(widthPx: number): number {
  if (!Number.isFinite(widthPx)) return MIN_COLUMN_PX;
  return Math.max(MIN_COLUMN_PX, Math.min(MAX_COLUMN_PX, Math.round(widthPx)));
}

/**
 * Column groups, resolved against the current order.
 *
 * A group's members may have been pinned apart, and a header spanning two
 * regions cannot be drawn. Rather than drawing it wrong, `resolveGroups`
 * reports the split so the caller can render the group header once per region
 * -- which is what a user who pinned one member of a group actually meant.
 */
export interface ResolvedGroup {
  readonly key: string;
  readonly label: string;
  readonly pin: Pin;
  /** Members in this region, in display order. Contiguous by construction. */
  readonly members: readonly string[];
  /** True when this group also has members in another region. */
  readonly split: boolean;
}

export function resolveGroups(
  order: ColumnOrder,
  groups: readonly ColumnGroup[],
): ResolvedGroup[] {
  const out: ResolvedGroup[] = [];
  for (const g of groups) {
    const byPin = new Map<Pin, string[]>();
    for (const region of [Pin.Left, Pin.None, Pin.Right] as const) {
      const list =
        region === Pin.Left
          ? order.pinnedLeft
          : region === Pin.Right
            ? order.pinnedRight
            : order.scrolling;
      const here = list.filter((k) => g.members.includes(k));
      if (here.length > 0) byPin.set(region, here);
    }
    const split = byPin.size > 1;
    for (const [pin, members] of byPin) {
      out.push({ key: g.key, label: g.label, pin, members, split });
    }
  }
  return out;
}
