// client/src/grid/keymap.ts -- what a keystroke is allowed to do.
//
// P11-08.
//
// NO KEYSTROKE PLACES AN ORDER, BECAUSE THE CLIENT HAS NO WORD FOR IT.
//
// That falls out of P11-01 rather than being enforced here: `ClientMsg` has
// four members and none of them is an order. This file's job is the one
// command that DOES change engine state -- the kill switch -- and everything
// around making it hard to hit and impossible to hit by accident.
//
// A SINGLE KEY MUST NEVER BE DESTRUCTIVE.
//
// This is the card, and the number is the argument. A trading dashboard is
// driven at speed by someone watching prices, not the keyboard. Bind the kill
// switch to one key and the question is not whether it gets pressed by
// accident but how often.
//
// So an `Action` declares its `Severity`, and the binding table is CHECKED at
// construction: a `Destructive` action bound to a chord with no modifier, or
// to a key that sits next to a navigation key, is rejected before the app
// starts rather than discovered in a session. `validateKeymap` returns the
// offences; the test asserts there are none in the shipped map, and asserts
// that a deliberately bad map is caught.
//
// AND A DESTRUCTIVE ACTION IS A REQUEST, NOT AN EFFECT.
//
// `resolve` never performs anything. For a destructive action it returns
// `{ kind: 'confirm' }` carrying the action, and the caller has to come back
// through `confirm` with an explicit acknowledgement. There is no code path
// from a keydown to a kill switch, which is a stronger statement than a
// modal that a fast hand can dismiss.
//
// TYPING IN A FILTER BOX IS NOT A SHORTCUT. The last one, and it is the bug
// every keyboard-driven grid ships once: a global handler that fires on 'k'
// while the user is typing "NIFTY" into a filter. `resolve` takes whether an
// editable field has focus and returns nothing at all when it does -- except
// for Escape, which is how you get out.

export const Severity = {
  Unspecified: 0,
  /** Moves, selects, opens. Reversible by doing it again. */
  Navigation: 1,
  /** Changes what is displayed or saved. Reversible, but worth an undo. */
  Mutating: 2,
  /** Changes the engine. The kill switch, and nothing else so far. */
  Destructive: 3,
} as const;
export type Severity = (typeof Severity)[keyof typeof Severity];

export const Action = {
  Unspecified: 0,
  MoveUp: 1,
  MoveDown: 2,
  PageUp: 3,
  PageDown: 4,
  Home: 5,
  End: 6,
  ExtendUp: 7,
  ExtendDown: 8,
  SelectAll: 9,
  ClearSelection: 10,
  /** The answer to P11-03's scaled-scroll limit: above ~16.7 million rows one
   *  scroll pixel is worth more than a row, and this is how you still land on
   *  one. */
  GoToRow: 11,
  CopySelection: 12,
  ToggleFilterBar: 13,
  SaveView: 14,
  KillSwitch: 15,
} as const;
export type Action = (typeof Action)[keyof typeof Action];

export const SEVERITY_OF: ReadonlyMap<Action, Severity> = new Map([
  [Action.MoveUp, Severity.Navigation],
  [Action.MoveDown, Severity.Navigation],
  [Action.PageUp, Severity.Navigation],
  [Action.PageDown, Severity.Navigation],
  [Action.Home, Severity.Navigation],
  [Action.End, Severity.Navigation],
  [Action.ExtendUp, Severity.Navigation],
  [Action.ExtendDown, Severity.Navigation],
  [Action.SelectAll, Severity.Navigation],
  [Action.ClearSelection, Severity.Navigation],
  [Action.GoToRow, Severity.Navigation],
  [Action.CopySelection, Severity.Navigation],
  [Action.ToggleFilterBar, Severity.Mutating],
  [Action.SaveView, Severity.Mutating],
  [Action.KillSwitch, Severity.Destructive],
]);

export interface Chord {
  readonly key: string;
  readonly ctrl?: boolean;
  readonly shift?: boolean;
  readonly alt?: boolean;
}

export interface Binding {
  readonly chord: Chord;
  readonly action: Action;
}

/** Keys a hand rests on while driving a grid. A destructive action next to
 *  one of these is a destructive action that will be hit. */
const NAVIGATION_KEYS: ReadonlySet<string> = new Set([
  'ArrowUp',
  'ArrowDown',
  'ArrowLeft',
  'ArrowRight',
  'PageUp',
  'PageDown',
  'Home',
  'End',
  'Enter',
  ' ',
  'Escape',
]);

export const DEFAULT_KEYMAP: readonly Binding[] = [
  { chord: { key: 'ArrowUp' }, action: Action.MoveUp },
  { chord: { key: 'ArrowDown' }, action: Action.MoveDown },
  { chord: { key: 'PageUp' }, action: Action.PageUp },
  { chord: { key: 'PageDown' }, action: Action.PageDown },
  { chord: { key: 'Home', ctrl: true }, action: Action.Home },
  { chord: { key: 'End', ctrl: true }, action: Action.End },
  { chord: { key: 'ArrowUp', shift: true }, action: Action.ExtendUp },
  { chord: { key: 'ArrowDown', shift: true }, action: Action.ExtendDown },
  { chord: { key: 'a', ctrl: true }, action: Action.SelectAll },
  { chord: { key: 'Escape' }, action: Action.ClearSelection },
  { chord: { key: 'g', ctrl: true }, action: Action.GoToRow },
  { chord: { key: 'c', ctrl: true }, action: Action.CopySelection },
  { chord: { key: 'f', ctrl: true }, action: Action.ToggleFilterBar },
  { chord: { key: 's', ctrl: true }, action: Action.SaveView },
  // Three modifiers and a letter nowhere near the arrow keys, and it still
  // only produces a confirmation request.
  { chord: { key: 'k', ctrl: true, shift: true, alt: true }, action: Action.KillSwitch },
];

export const KeymapOffence = {
  /** A destructive action reachable without a modifier. */
  DestructiveBare: 'DestructiveBare',
  /** A destructive action on a key the hand already rests on. */
  DestructiveNearNavigation: 'DestructiveNearNavigation',
  /** A destructive action with fewer than two modifiers. */
  DestructiveTooEasy: 'DestructiveTooEasy',
  /** Two actions on one chord. */
  Duplicate: 'Duplicate',
  /** An action with no declared severity. */
  UnknownSeverity: 'UnknownSeverity',
} as const;
export type KeymapOffence =
  (typeof KeymapOffence)[keyof typeof KeymapOffence];

export interface Offence {
  readonly offence: KeymapOffence;
  readonly action: Action;
  readonly chord: Chord;
}

export function chordKey(c: Chord): string {
  return `${c.ctrl ? 'C' : ''}${c.shift ? 'S' : ''}${c.alt ? 'A' : ''}-${c.key}`;
}

/**
 * Check a binding table before the app starts.
 *
 * A destructive binding is a design error, not a runtime condition, so it is
 * caught where design errors belong -- at construction, by a function that
 * returns the list.
 */
export function validateKeymap(map: readonly Binding[]): Offence[] {
  const out: Offence[] = [];
  const seen = new Map<string, Action>();

  for (const b of map) {
    const k = chordKey(b.chord);
    if (seen.has(k)) {
      out.push({ offence: KeymapOffence.Duplicate, action: b.action, chord: b.chord });
    }
    seen.set(k, b.action);

    const sev = SEVERITY_OF.get(b.action);
    if (sev === undefined) {
      out.push({
        offence: KeymapOffence.UnknownSeverity,
        action: b.action,
        chord: b.chord,
      });
      continue;
    }
    if (sev !== Severity.Destructive) continue;

    const mods =
      (b.chord.ctrl ? 1 : 0) + (b.chord.shift ? 1 : 0) + (b.chord.alt ? 1 : 0);
    if (mods === 0) {
      out.push({
        offence: KeymapOffence.DestructiveBare,
        action: b.action,
        chord: b.chord,
      });
    } else if (mods < 2) {
      out.push({
        offence: KeymapOffence.DestructiveTooEasy,
        action: b.action,
        chord: b.chord,
      });
    }
    if (NAVIGATION_KEYS.has(b.chord.key)) {
      out.push({
        offence: KeymapOffence.DestructiveNearNavigation,
        action: b.action,
        chord: b.chord,
      });
    }
  }
  return out;
}

export type Resolution =
  | { readonly kind: 'none' }
  | { readonly kind: 'act'; readonly action: Action }
  /** A destructive action, NOT performed. The caller must come back through
   *  `confirm`. There is no path from a keydown to the kill switch. */
  | { readonly kind: 'confirm'; readonly action: Action };

/**
 * What a keystroke means right now.
 *
 * `editing` is whether an editable field has focus. When it is true, only
 * Escape resolves -- a global handler that fires on 'k' while the user types
 * "NIFTY" into a filter box is the bug every keyboard-driven grid ships once.
 */
export function resolve(
  map: readonly Binding[],
  chord: Chord,
  editing: boolean,
): Resolution {
  const wanted = chordKey(chord);
  for (const b of map) {
    if (chordKey(b.chord) !== wanted) continue;
    if (editing && b.action !== Action.ClearSelection) return { kind: 'none' };
    const sev = SEVERITY_OF.get(b.action);
    if (sev === Severity.Destructive) {
      return { kind: 'confirm', action: b.action };
    }
    return { kind: 'act', action: b.action };
  }
  return { kind: 'none' };
}

/** The second step for a destructive action. `acknowledged` has to be a
 *  deliberate true from a control the user operated -- not a default, and not
 *  a dismissed modal. */
export function confirm(action: Action, acknowledged: boolean): Resolution {
  if (!acknowledged) return { kind: 'none' };
  if (SEVERITY_OF.get(action) !== Severity.Destructive) return { kind: 'none' };
  return { kind: 'act', action };
}
