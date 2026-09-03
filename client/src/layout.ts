// client/src/layout.ts -- saved layout, and the two things it must never do.
//
// P11-02b.
//
// A SAVED LAYOUT MUST NOT CONTAIN DATA.
//
// This is the card, and it is the mistake that is genuinely tempting: the
// grid already has the rows in memory when the user closes the tab, and
// writing them alongside the column widths makes the next open instant.
//
// It also makes the next open a LIE. The dashboard paints yesterday's
// positions, at yesterday's marks, before the socket has connected -- and it
// paints them in exactly the layout, colours and sort order the user left, so
// there is nothing on screen to suggest the numbers are hours old. A stale
// position that looks live is worse than an empty grid, because an empty grid
// is unambiguous.
//
// So `SavedLayout` has no field that can hold a value. Not "should not" -- the
// schema has no such field, `parseLayout` drops every key it does not know,
// and the test feeds it a blob with positions and P&L embedded and measures
// that they do not survive. Values arrive over the wire, from a snapshot, with
// a sequence number attached, or they do not arrive.
//
// AND localStorage IS UNTRUSTED INPUT.
//
// The second thing. A layout read back is data from outside the program: the
// user can edit it, another version of the app wrote it, a partial write left
// it truncated, a browser extension touched it. `JSON.parse` and spread into
// state, and a hand-edited entry puts the UI into a configuration it has no
// code path for -- a negative column width, a tab pointing at a panel that no
// longer exists, `activeTab` out of range.
//
// Every field is therefore checked, and a layout that does not check out is
// REPLACED BY THE DEFAULT AND THE FALLBACK IS REPORTED. Silently repairing it
// is how a user loses their layout every session and never finds out why;
// CLAUDE.md rule 9 -- failing loud beats trading wrong -- is a UI rule too.

export const LAYOUT_VERSION = 1;
export const LAYOUT_KEY = 'altair.layout.v1';

export const Theme = {
  Unspecified: 0,
  Light: 1,
  Dark: 2,
  /** Follow the OS. The default, because guessing wrong is a bright white
   *  screen at 09:14 in a dark room. */
  System: 3,
} as const;
export type Theme = (typeof Theme)[keyof typeof Theme];

export const PanelKind = {
  Unspecified: 0,
  Grid: 1,
  Chart: 2,
  DepthLadder: 3,
  OptionChain: 4,
  RiskDashboard: 5,
  AuditTrail: 6,
} as const;
export type PanelKind = (typeof PanelKind)[keyof typeof PanelKind];

const PANEL_KINDS: ReadonlySet<number> = new Set(Object.values(PanelKind));
const THEMES: ReadonlySet<number> = new Set(Object.values(Theme));

/** One tab. Note what is here: which panel, how wide the columns are, what
 *  the sort is. Note what is NOT here: any row, price, quantity or P&L. */
export interface SavedTab {
  readonly title: string;
  readonly panel: PanelKind;
  /** Column widths in CSS pixels. Positive and bounded -- a zero-width column
   *  is invisible and unrecoverable by dragging. */
  readonly columnWidths: readonly number[];
  /** Column key to sort by, or null. A KEY, not a value. */
  readonly sortColumn: string | null;
  readonly sortAscending: boolean;
}

export interface SavedLayout {
  readonly version: number;
  readonly theme: Theme;
  readonly tabs: readonly SavedTab[];
  readonly activeTab: number;
}

export const DEFAULT_LAYOUT: SavedLayout = {
  version: LAYOUT_VERSION,
  theme: Theme.System,
  tabs: [
    {
      title: 'Positions',
      panel: PanelKind.Grid,
      columnWidths: [140, 90, 90, 110],
      sortColumn: null,
      sortAscending: true,
    },
  ],
  activeTab: 0,
};

export const MAX_TABS = 24;
export const MAX_COLUMNS = 256;
export const MIN_COLUMN_PX = 24;
export const MAX_COLUMN_PX = 2000;
export const MAX_TITLE_CHARS = 64;

export const LayoutProblem = {
  /** Nothing stored. Not an error -- a first run. */
  Absent: 'Absent',
  NotJson: 'NotJson',
  NotAnObject: 'NotAnObject',
  WrongVersion: 'WrongVersion',
  BadTheme: 'BadTheme',
  BadTabs: 'BadTabs',
  BadPanel: 'BadPanel',
  BadColumnWidth: 'BadColumnWidth',
  BadActiveTab: 'BadActiveTab',
  BadSort: 'BadSort',
} as const;
export type LayoutProblem =
  (typeof LayoutProblem)[keyof typeof LayoutProblem];

export type LoadResult =
  | { readonly ok: true; readonly layout: SavedLayout }
  | {
      readonly ok: false;
      readonly problem: LayoutProblem;
      /** The default, so the caller always has something to render. It is
       *  handed back rather than substituted silently: the caller is expected
       *  to TELL the user their layout was reset. */
      readonly fallback: SavedLayout;
    };

function isRecord(v: unknown): v is Record<string, unknown> {
  return typeof v === 'object' && v !== null && !Array.isArray(v);
}

function parseTab(raw: unknown): SavedTab | LayoutProblem {
  if (!isRecord(raw)) return LayoutProblem.BadTabs;

  const title = raw['title'];
  if (typeof title !== 'string' || title.length > MAX_TITLE_CHARS) {
    return LayoutProblem.BadTabs;
  }
  const panel = raw['panel'];
  if (
    typeof panel !== 'number' ||
    !PANEL_KINDS.has(panel) ||
    panel === PanelKind.Unspecified
  ) {
    return LayoutProblem.BadPanel;
  }
  const widths = raw['columnWidths'];
  if (!Array.isArray(widths) || widths.length > MAX_COLUMNS) {
    return LayoutProblem.BadColumnWidth;
  }
  const columnWidths: number[] = [];
  for (const w of widths) {
    if (
      typeof w !== 'number' ||
      !Number.isFinite(w) ||
      w < MIN_COLUMN_PX ||
      w > MAX_COLUMN_PX
    ) {
      return LayoutProblem.BadColumnWidth;
    }
    columnWidths.push(w);
  }
  const sortColumn = raw['sortColumn'];
  if (sortColumn !== null && typeof sortColumn !== 'string') {
    return LayoutProblem.BadSort;
  }
  const sortAscending = raw['sortAscending'];
  if (typeof sortAscending !== 'boolean') return LayoutProblem.BadSort;

  // Built field by field from known keys. Anything else the blob carried --
  // rows, marks, a cached P&L -- is simply not copied, because there is
  // nowhere for it to go.
  return {
    title,
    panel: panel as PanelKind,
    columnWidths,
    sortColumn,
    sortAscending,
  };
}

/** Validate a layout that came from storage. Never throws. */
export function parseLayout(raw: string | null): LoadResult {
  const fallback = DEFAULT_LAYOUT;
  if (raw === null || raw.length === 0) {
    return { ok: false, problem: LayoutProblem.Absent, fallback };
  }
  let parsed: unknown;
  try {
    parsed = JSON.parse(raw);
  } catch {
    return { ok: false, problem: LayoutProblem.NotJson, fallback };
  }
  if (!isRecord(parsed)) {
    return { ok: false, problem: LayoutProblem.NotAnObject, fallback };
  }
  if (parsed['version'] !== LAYOUT_VERSION) {
    return { ok: false, problem: LayoutProblem.WrongVersion, fallback };
  }
  const theme = parsed['theme'];
  if (
    typeof theme !== 'number' ||
    !THEMES.has(theme) ||
    theme === Theme.Unspecified
  ) {
    return { ok: false, problem: LayoutProblem.BadTheme, fallback };
  }
  const rawTabs = parsed['tabs'];
  if (
    !Array.isArray(rawTabs) ||
    rawTabs.length === 0 ||
    rawTabs.length > MAX_TABS
  ) {
    return { ok: false, problem: LayoutProblem.BadTabs, fallback };
  }
  const tabs: SavedTab[] = [];
  for (const t of rawTabs) {
    const tab = parseTab(t);
    if (typeof tab === 'string') {
      return { ok: false, problem: tab, fallback };
    }
    tabs.push(tab);
  }
  const activeTab = parsed['activeTab'];
  if (
    typeof activeTab !== 'number' ||
    !Number.isInteger(activeTab) ||
    activeTab < 0 ||
    activeTab >= tabs.length
  ) {
    return { ok: false, problem: LayoutProblem.BadActiveTab, fallback };
  }
  return {
    ok: true,
    layout: { version: LAYOUT_VERSION, theme: theme as Theme, tabs, activeTab },
  };
}

/** Just enough of localStorage to persist a layout. An interface rather than
 *  a direct reference so this is testable without a browser -- and so nothing
 *  in this module can reach the rest of Storage. */
export interface KeyValueStore {
  getItem(key: string): string | null;
  setItem(key: string, value: string): void;
}

/**
 * Serialise a layout.
 *
 * Rebuilt field by field rather than `JSON.stringify(layout)`. If a caller
 * hands in an object that also carries rows -- a spread of component state,
 * say -- stringify would happily write them to disk. This cannot.
 */
export function serialiseLayout(layout: SavedLayout): string {
  return JSON.stringify({
    version: LAYOUT_VERSION,
    theme: layout.theme,
    activeTab: layout.activeTab,
    tabs: layout.tabs.map((t) => ({
      title: t.title,
      panel: t.panel,
      columnWidths: [...t.columnWidths],
      sortColumn: t.sortColumn,
      sortAscending: t.sortAscending,
    })),
  });
}

export function saveLayout(store: KeyValueStore, layout: SavedLayout): void {
  store.setItem(LAYOUT_KEY, serialiseLayout(layout));
}

export function loadLayout(store: KeyValueStore): LoadResult {
  return parseLayout(store.getItem(LAYOUT_KEY));
}
