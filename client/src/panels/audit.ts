// client/src/panels/audit.ts -- the audit row and the kill-switch panel.
//
// P11-14.
//
// AN AUDIT ROW THAT CANNOT REPRODUCE THE DECISION IS A LOG LINE.
//
// CLAUDE.md rule 10: "Every live decision is reproducible from
// {model_hash, feature_version, config_hash, spec_version, tick_seqno}."
// That is five fields, and an audit trail missing any one of them cannot
// answer the only question anyone asks of it -- why did it do that -- because
// the answer requires re-running the decision, and re-running it requires
// knowing exactly which model, which features, which config, which contract
// specs and which tick.
//
// `AuditRow` therefore has all five as required fields, and `reproducible()`
// checks them at the boundary rather than trusting the server. An
// incompletely-stamped row is shown AS incomplete rather than shown as a row
// with some blank columns, because a blank column reads as "nothing happened
// there" and what it means is "this decision cannot be re-run".
//
// THE KILL SWITCH PANEL SHOWS WHAT IT IS ABOUT TO DO.
//
// P11-08 established that no keystroke reaches the kill switch: it resolves to
// a confirmation request. This is what that confirmation has to contain.
//
// "Are you sure?" is not a confirmation. The person pressing it already
// believes they are sure; that is why their hand is on the key. A confirmation
// that changes an outcome has to state the CONSEQUENCE -- how many positions
// will be flattened, what notional that is, and what the estimated cost of
// flattening is -- because those are the numbers that occasionally stop
// somebody, and "are you sure" never has.
//
// The estimated cost comes from the server, for the same reason the strategy
// builder's does: the charge schedule lives in `risk/cost.hpp` and having a
// second copy here would give two answers to the question of what an action
// costs.
//
// AND THE CLIENT STILL CANNOT FLATTEN ANYTHING. `confirmKill` returns a
// COMMAND to send, not an effect. `oms/` is the only thing that places or
// cancels an order; this returns the one `ClientMsg` the protocol has for it.

import { ClientMsg } from '../protocol.ts';

export interface AuditRow {
  readonly tickSeqno: bigint;
  readonly engineTimeNs: bigint;
  readonly modelHash: string;
  readonly featureVersion: string;
  readonly configHash: string;
  readonly specVersion: string;
  readonly action: string;
}

export const AuditProblem = {
  MissingModelHash: 'MissingModelHash',
  MissingFeatureVersion: 'MissingFeatureVersion',
  MissingConfigHash: 'MissingConfigHash',
  MissingSpecVersion: 'MissingSpecVersion',
  MissingTickSeqno: 'MissingTickSeqno',
} as const;
export type AuditProblem = (typeof AuditProblem)[keyof typeof AuditProblem];

/**
 * Which of the five stamps are missing.
 *
 * Returns the list rather than a boolean so the panel can name what is absent.
 * A row rendered with blank columns reads as "nothing happened there"; a row
 * rendered as NOT REPRODUCIBLE, missing the spec version, says the thing that
 * is actually true.
 */
export function reproducibilityGaps(row: AuditRow): AuditProblem[] {
  const out: AuditProblem[] = [];
  if (row.modelHash.length === 0) out.push(AuditProblem.MissingModelHash);
  if (row.featureVersion.length === 0) {
    out.push(AuditProblem.MissingFeatureVersion);
  }
  if (row.configHash.length === 0) out.push(AuditProblem.MissingConfigHash);
  if (row.specVersion.length === 0) out.push(AuditProblem.MissingSpecVersion);
  if (row.tickSeqno <= 0n) out.push(AuditProblem.MissingTickSeqno);
  return out;
}

export function reproducible(row: AuditRow): boolean {
  return reproducibilityGaps(row).length === 0;
}

/** The five fields, joined, for a copy-to-clipboard control -- so re-running a
 *  decision starts with pasting one string rather than transcribing five. */
export function reproductionKey(row: AuditRow): string {
  return [
    `tick=${row.tickSeqno.toString()}`,
    `model=${row.modelHash}`,
    `features=${row.featureVersion}`,
    `config=${row.configHash}`,
    `spec=${row.specVersion}`,
  ].join(' ');
}

// ---------------------------------------------------------------------------
// Kill switch
// ---------------------------------------------------------------------------

/**
 * What the confirmation must show.
 *
 * Every field is required. A confirmation dialog that could be built without
 * knowing the consequence is a dialog that will be built without it.
 */
export interface KillImpact {
  readonly openPositions: number;
  readonly grossNotionalPaise: bigint;
  /** Estimated, from the SERVER. The charge schedule lives in risk/cost.hpp
   *  and a second copy here would give two answers to what an action costs. */
  readonly estimatedCostPaise: bigint;
  /** Positions the kill switch cannot flatten by itself -- an illiquid strike,
   *  a halted symbol. Named, because "flatten everything" that silently
   *  flattens most things is the worst of both. */
  readonly cannotFlatten: readonly string[];
}

export const KillProblem = {
  /** The impact was not supplied, so the consequence cannot be stated. */
  NoImpact: 'NoImpact',
  /** The user did not acknowledge. */
  NotAcknowledged: 'NotAcknowledged',
  /** Nothing is open. Refused so the confirmation is never a habit formed on
   *  no-op presses. */
  NothingToFlatten: 'NothingToFlatten',
} as const;
export type KillProblem = (typeof KillProblem)[keyof typeof KillProblem];

export type KillResult =
  | { readonly ok: true; readonly message: ClientMsg }
  | { readonly ok: false; readonly problem: KillProblem };

/**
 * Turn an acknowledged confirmation into the one message the protocol has.
 *
 * Returns a COMMAND, not an effect. oms/ is the only thing that places or
 * cancels an order; the client can ask, and that is all the protocol lets it
 * say (P11-01).
 */
export function confirmKill(
  impact: KillImpact | null,
  acknowledged: boolean,
): KillResult {
  if (impact === null) return { ok: false, problem: KillProblem.NoImpact };
  if (impact.openPositions === 0) {
    return { ok: false, problem: KillProblem.NothingToFlatten };
  }
  if (!acknowledged) {
    return { ok: false, problem: KillProblem.NotAcknowledged };
  }
  return { ok: true, message: ClientMsg.KillSwitch };
}

/**
 * The sentence the dialog shows.
 *
 * Deliberately not "Are you sure?". The person pressing it already believes
 * they are sure -- that is why their hand is on the key. What occasionally
 * stops somebody is a number.
 */
export function killPrompt(
  impact: KillImpact,
  formatPaise: (p: bigint) => string,
): string {
  const head =
    `Flatten ${impact.openPositions} position${impact.openPositions === 1 ? '' : 's'}` +
    `, Rs ${formatPaise(impact.grossNotionalPaise)} gross,` +
    ` at an estimated cost of Rs ${formatPaise(impact.estimatedCostPaise)}.`;
  if (impact.cannotFlatten.length === 0) return head;
  return `${head} ${impact.cannotFlatten.length} position${impact.cannotFlatten.length === 1 ? '' : 's'} CANNOT be flattened automatically: ${impact.cannotFlatten.join(', ')}.`;
}
