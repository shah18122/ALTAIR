# CX-03 implementation plan — canonical paper-only engine

Status: planning only. No engine dispatcher, intent consumer, or broker order
path is implemented by this document. Live execution is explicitly out of scope.

## Goal

Build one deterministic, owner-thread decision path that consumes validated
canonical market events and (only in paper mode) validated order intents. Live
feed adapters and recorded-tick replay must call the same event admission and
decision function. UI widgets remain read-only with respect to OMS execution.

The first integration proves safety and parity; it does not claim a profitable
strategy. Default policy is no-trade. Any later paper strategy is a separate,
disabled-by-default card requiring causal/net-cost evidence.

## Entry gates

Before code starts, the following must have focused tests and frozen interfaces:

- CX02-B1 strict intent parser, B2 queue cursor/TTL/dedupe, B6 order lifecycle,
  B7 unknown-order/reconciliation contract.
- CX02-E1 independent conservation reconciliation and refused-fill latch.
- CX02-E2 fail-closed unverified charge schedule behavior.
- CX02-E3 checked pre-trade arithmetic; `check_order` remains uncalled until the
  dispatcher card wires it immediately before the paper adapter.
- Canonical instrument mapping from point-in-time `SpecStore` entries; no
  placeholder token/lot/tick registrations.

Status snapshot, 2026-09-21: selected implementations and focused unit-test gates
for B1, B2, B6/B7 and E1/E2/E3 are present; the current configured Windows suite
passes 141/141. This establishes component-level behavior only. It does NOT
establish authenticated/fresh broker-snapshot correlation to a timed-out order,
point-in-time instrument mapping at the decision boundary, or an application
caller for these components. Those integration contracts and the CX03-001
freshness/admission proof remain open, so this document is still planning-only
and no dispatcher implementation is claimed.

## Card sequence and initial manifests

Manifests below are proposals for later cards, not current edit authorization.
Each card must re-check `git status --short <file>` and freeze its manifest before
any edits. Keep each card limited to one component directory where practical.

### CX03-A — canonical admission and event identity

Candidate manifest: `feed/`, `instruments/`, focused tests. Define the event
identity/session epoch, accepted source, event-time vs receive-time rules,
duplicate/out-of-order/gap behavior, stale-depth policy, and point-in-time
instrument mapping. A rejected event must not update tradable book state; counts
and reason codes remain observable. No display socket or Qt work in admission.

### CX03-B — single-owner decision state

Candidate manifest: `engine/` (new directory) and its tests, with only narrowly
approved interfaces to `feed/`, `book/`, `features/`, `models/`, `risk/`, and
`oms/`. One owner serially processes every admitted event: normalise → update
book/required state → construct freshness-stamped features → obtain a coherent
model record → propose an optional intent → synchronous hard risk gate. Do not
add threads or lock-free queues until measured ownership/capacity requires them.

### CX03-C — paper execution and UI-intent adapter

Candidate manifest: `oms/` and `engine/` tests. The dispatcher is the only caller
of `check_order`; it must consume the live risk/kill/account snapshot, point-in-
time contract and fresh quote used to build `ProposedOrder`. A fake paper adapter
captures `place/cancel/replace` calls and returns delayed/partial/rejected/unknown
events through the ordinary order-state machine. Invalid, stale, over-limit,
unverified-cost, duplicate, or halted intents cause zero adapter calls. One
approved id submits exactly once. Persist-before-act ordering, idempotency,
uncertain-send reconciliation and restart behavior need forced-interleaving tests.
No Kite/FYERS transport is allowed in this card.

### CX03-D — recorded-event parity and evidence

Candidate manifest: `feed/` replay adapter plus `engine/` tests. Feed the same
captured, versioned event sequence through the live adapter boundary and replay
adapter; compare admitted-event sequence, features, coherent model generation,
risk verdict, serialized order request and reason codes. Add malformed/stale/gap,
reconnect, dedupe, backpressure, queue-full, kill-switch and restart cases.
Paper results are evidence of software behavior only, never execution quality.

## Architectural constraints

- Only the engine/OMS boundary may invoke a broker adapter; `desktop/` never gets
  a trading handle. The first adapter is in-memory paper-only, not connected to
  accounts or real venues.
- Risk rejection is synchronous and fail-closed. Unknown contract, stale quote,
  incomplete depth, missing/unverified cost, exhausted capacity, or uncertain
  account/order state prevents submission.
- A confirmed exchange/broker fill is the only input to `ConservationLedger::on_fill`.
  Independent position/cash reconciliation uses a fresh same-scope snapshot;
  do not compare a per-instrument ledger with account-level buying power.
- Feed/replay events are lossless at the decision boundary or explicitly rejected
  with a gap state. UI display updates may coalesce; decisions and audit events may not.
- No placeholder model promotion. Existing live tick forecasts lack historical
  tick-phase calibration and cannot be used as a validated strategy signal.
- Keep order events, price/quantity units, timestamps, venue and schema versions
  explicit. Never retry an uncertain order at another broker.

## CX-03 acceptance gates

1. Same captured ingress produces byte/equality-equivalent paper decisions in
   live-adapter and replay modes.
2. Malformed, stale, out-of-order, gapped and backpressured inputs cannot create
   a tradable state update or order submission.
3. Every attempted order passes strict intent parsing, point-in-time contract
   checks, cost policy, `check_order`, kill switch and OMS state before the paper
   adapter can observe it.
4. Cancel/fill/reject/timeout/reconcile interleavings preserve position truth;
   restart and duplicate replay do not submit the same id twice.
5. No GUI callback, logging, allocation-heavy training, file I/O, or network I/O
   runs on the decision owner path without a separately reviewed bounded contract.
6. Record reproducibility metadata and internal latency distributions (p50/p95/
   p99/p99.9, sample count, compiler/CPU/load, queue depth/drops). Missing Linux
   TSan or latency harness remains an open release gate, not a pass.

## Explicit non-goals

This phase does not enable a live strategy, promise P&L, fetch current market data,
log into Kite/FYERS, place/cancel live orders, move positions between accounts,
automate 2FA, or certify the UI as a broker terminal. CX-04 model validation,
CX-05 FYERS, CX-07 live option data, and CX-08 release/go-no-go remain separate.
