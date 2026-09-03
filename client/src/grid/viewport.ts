// client/src/grid/viewport.ts -- the virtual window, and the wall it hits at
// a million rows.
//
// P11-03.
//
// A MILLION ROWS IS TALLER THAN A BROWSER CAN SCROLL.
//
// This is the card. Virtual scrolling is usually presented as a rendering
// trick: size a spacer to `rows * rowHeight`, absolutely position the visible
// slice, done. It works beautifully at ten thousand rows and it silently
// stops working somewhere below a million, because the spacer is a real DOM
// element and browsers cap how tall one can be.
//
// The caps are engine-specific and undocumented. 2^24 = 16,777,216 px is below
// every one of them and is what this module treats as safe. A million rows at
// 24 px is 24,000,000 px, so:
//
//     wanted height    24,000,000 px
//     usable height    16,777,216 px
//     unreachable       7,222,784 px  =  300,950 rows  =  30.1% of the grid
//
// Nothing errors. The scrollbar simply stops, and the last 300,950 rows of the
// audit trail cannot be reached by scrolling at all. On a smaller monitor or a
// shorter row height the number moves; it does not go away.
//
// So `computeWindow` scales: when the wanted height exceeds the cap the spacer
// is set to the cap and scroll position is mapped through it FRACTIONALLY --
// fraction of the way down the spacer equals fraction of the way down the
// content. The obvious `scrollTop * (wanted / cap)` is wrong at the bottom by
// almost a full viewport, which leaves the last rows unreachable; that is the
// same bug in miniature and the first draft of this file had it. The test
// caught it.
//
// The trade costs resolution, and the honest question is whether it costs
// enough to matter. It does not, here: the mapping is only coarser than one row once
// `rows * rowHeight / cap > rowHeight`, i.e. above 16.7 MILLION rows. At a
// million rows one scroll pixel is 0.06 of a row.
//
// Above that -- and the audit trail of a busy year could get there -- scrolling
// alone can no longer land on a chosen row, and the answer is a "go to row"
// control rather than a bigger spacer. That is P11-08, and it is named here so
// the limit is a known one rather than a surprise.
//
// THE WINDOW IS O(VISIBLE). That is the other half of affording a million
// rows, and it is why `RowStore.window` slices rather than filters: every
// function here is arithmetic on five numbers and touches no row at all.

/** 2^24 px. Comfortably below every major engine's maximum element height,
 *  chosen as a round power of two rather than tuned to one browser's limit --
 *  a number tuned to Chrome is a number that breaks in Firefox. */
export const SAFE_MAX_SPACER_PX = 16_777_216;

export interface ViewportRequest {
  /** Scroll offset in the SPACER's coordinates -- what the DOM reports. */
  readonly scrollTopPx: number;
  readonly viewportHeightPx: number;
  readonly rowHeightPx: number;
  readonly totalRows: number;
  /** Rows rendered beyond each edge, so a fast scroll does not show blank
   *  bands. Costs `2 * overscan` cells of work per frame. */
  readonly overscanRows: number;
}

export interface ViewportWindow {
  /** Half-open [firstRow, lastRow) into the VIEW, already clamped. */
  readonly firstRow: number;
  readonly lastRow: number;
  /** Height to give the scroll spacer, never above the cap. */
  readonly spacerHeightPx: number;
  /** Where to translate the rendered slice to, in spacer coordinates. */
  readonly offsetPx: number;
  /** True when the grid is taller than the cap and scroll had to be scaled. */
  readonly scaled: boolean;
  /** Virtual pixels per spacer pixel. 1 when unscaled. */
  readonly scale: number;
}

/** How tall the grid would like to be, before any cap. */
export function wantedHeightPx(totalRows: number, rowHeightPx: number): number {
  return totalRows * rowHeightPx;
}

/**
 * How many rows are unreachable if the spacer is naively set to
 * `rows * rowHeight` and the cap is respected by the browser rather than by
 * the code.
 *
 * Exists to be measured rather than argued about; nothing in the render path
 * calls it.
 */
export function unreachableRows(
  totalRows: number,
  rowHeightPx: number,
  capPx: number = SAFE_MAX_SPACER_PX,
): number {
  const wanted = wantedHeightPx(totalRows, rowHeightPx);
  if (wanted <= capPx) return 0;
  return totalRows - Math.floor(capPx / rowHeightPx);
}

/**
 * Resolution of the scaled scroll, in ROWS per spacer pixel.
 *
 * Below 1 the mapping can still address every row by scrolling. At or above 1
 * it cannot, and navigation needs a control rather than a scrollbar.
 */
export function rowsPerScrollPixel(
  totalRows: number,
  rowHeightPx: number,
  capPx: number = SAFE_MAX_SPACER_PX,
): number {
  const wanted = wantedHeightPx(totalRows, rowHeightPx);
  if (wanted <= capPx) return 1 / rowHeightPx;
  return totalRows / capPx;
}

/**
 * The visible window.
 *
 * Pure arithmetic: it does not see a row, so its cost does not depend on how
 * many there are.
 */
export function computeWindow(req: ViewportRequest): ViewportWindow {
  const {
    scrollTopPx,
    viewportHeightPx,
    rowHeightPx,
    totalRows,
    overscanRows,
  } = req;

  if (rowHeightPx <= 0 || totalRows <= 0 || viewportHeightPx <= 0) {
    return {
      firstRow: 0,
      lastRow: 0,
      spacerHeightPx: 0,
      offsetPx: 0,
      scaled: false,
      scale: 1,
    };
  }

  const wanted = wantedHeightPx(totalRows, rowHeightPx);
  const scaled = wanted > SAFE_MAX_SPACER_PX;
  const spacerHeightPx = scaled ? SAFE_MAX_SPACER_PX : wanted;

  // THE MAPPING IS FRACTIONAL, NOT A MULTIPLY.
  //
  // The obvious version is `virtualTop = scrollTop * (wanted / cap)`, and it
  // is wrong at the bottom. Scroll can only reach `spacer - viewport`, so that
  // multiply tops out at `(cap - viewport) * wanted / cap`, which is short of
  // `wanted - viewport` by almost a full viewport of content. At a million
  // rows that leaves the last dozen rows unreachable -- a smaller copy of
  // exactly the bug this module exists to fix, and the first draft had it.
  //
  // So map the SCROLLABLE RANGE onto the SCROLLABLE RANGE: fraction of the
  // way down the spacer equals fraction of the way down the content. Then
  // scroll bottom is content bottom by construction, at any scale.
  const spacerRange = Math.max(0, spacerHeightPx - viewportHeightPx);
  const virtualRange = Math.max(0, wanted - viewportHeightPx);

  // Clamp before mapping: platforms report a scrollTop past the end during
  // rubber-band overscroll.
  const clampedScroll = Math.min(Math.max(0, scrollTopPx), spacerRange);
  const fraction = spacerRange > 0 ? clampedScroll / spacerRange : 0;
  const virtualTop = fraction * virtualRange;

  // Reported for callers that want it: virtual pixels per spacer pixel over
  // the scrollable range. 1 when unscaled.
  const scale = spacerRange > 0 ? virtualRange / spacerRange : 1;

  const firstVisible = Math.floor(virtualTop / rowHeightPx);
  const visibleCount = Math.ceil(viewportHeightPx / rowHeightPx);

  const firstRow = Math.max(0, firstVisible - overscanRows);
  const lastRow = Math.min(totalRows, firstVisible + visibleCount + overscanRows);

  // Where the slice sits in SPACER coordinates. The inverse of the mapping
  // above, so the rendered rows stay under the cursor instead of drifting
  // away from it as you scroll.
  const offsetPx =
    virtualRange > 0
      ? (firstRow * rowHeightPx * spacerRange) / virtualRange
      : 0;

  return { firstRow, lastRow, spacerHeightPx, offsetPx, scaled, scale };
}

/** Scroll position that puts `row` at the top -- the "go to row" the scaled
 *  mapping needs once one scroll pixel is worth more than one row. */
export function scrollTopForRow(
  row: number,
  rowHeightPx: number,
  totalRows: number,
  viewportHeightPx: number,
): number {
  const wanted = wantedHeightPx(totalRows, rowHeightPx);
  const spacer = Math.min(wanted, SAFE_MAX_SPACER_PX);
  const spacerRange = Math.max(0, spacer - viewportHeightPx);
  const virtualRange = Math.max(0, wanted - viewportHeightPx);
  if (virtualRange <= 0) return 0;
  const clamped = Math.max(0, Math.min(row, totalRows - 1));
  // Same fractional mapping as computeWindow, inverted. Sharing the shape
  // rather than the code would be how the two drift apart, so the arithmetic
  // is written out and the test checks they agree.
  return (clamped * rowHeightPx * spacerRange) / virtualRange;
}
