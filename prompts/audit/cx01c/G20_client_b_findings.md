# G20_client_b -- findings (client chart, layout, panels, protocol, session)

Reader: claude-subagent(sonnet):G20_client_b
Date: 2026-09-15
Baseline HEAD: a34af5c5c9fa8c7498e8cc4df00907548a41e1a5
Status: COMPLETE -- all 18 assigned files fully read.

## 1. Scope & coverage

18 files, 4,993 lines assigned (`client/src/chart/{candles,footprint,ladder,
replay,scale,surface}.ts`, `client/src/layout.ts`,
`client/src/panels/{audit,strategy}.ts`, `client/src/protocol.ts`,
`client/src/session.ts`, and their 7 test files). All 18 hashes matched the
`CX01C_PARTITION.json` inventory exactly before reading, and again after --
no drift, and this review made no edits (audit is read-only by rule).

Every file was read in full with the Read tool, each read confirmed to reach
the file's own last line (no file here exceeds 375 lines, so each was one
read). Nothing was marked read from a grep hit or a summary.

`client/` is RETIRED per `CLAUDE.md`: not built, not shipped, kept as a
record. Severity below is P3 by default; it is raised only where a defect
also exists in live `desktop/` or `server/` code, cited explicitly.

**Overall impression**: this is unusually well-instrumented code for an
audit target -- essentially every file's own header comment documents a
specific bug class, a measured (not asserted) magnitude of the mistake, and
the design choice that prevents it, and the matching test file measures the
same finding rather than merely exercising the happy path. The two findings
below are the residue after that: places where the stated invariant is not
actually enforced by the function that would need to enforce it, rather than
a wrong number the code currently produces.

## 2. Findings table

| ID | Sev | Class | File:line | Summary |
|---|---|---|---|---|
| C20-001 | P3 | DESIGN GAP | client/src/chart/ladder.ts:143-199 | `ladderRows` does not itself refuse a crossed snapshot; the refusal lives only in a separate `crossed()` function nothing in this module calls, and even this group's own test file bypasses `fromFrame` and calls `ladderRows` directly in every case. |
| C20-002 | P3 | HYPOTHESIS | client/src/panels/strategy.ts:159-188, 255-256 | `EndSlopes.downside` is computed, documented ("As spot falls toward zero"), and exposed, but no bound decision anywhere in the module reads it, and no test asserts on it -- current output is correct, but the field is a trap for a future "obvious" edit. |

No P0/P1/P2 findings. No numerical or protocol-conformance defect was found.

## 3. Finding details

### C20-001 -- `ladderRows` trusts an already-consistent snapshot; it does not defend against a crossed one itself

- Severity: P3 (retired code; the intended guard exists, just one call frame away and not wired in)
- Classification: DESIGN GAP
- Evidence: `client/src/chart/ladder.ts:143-199` (`ladderRows`), compare
  `client/src/chart/ladder.ts:116-121` (`crossed`):
  > `export function crossed(s: LadderSnapshot): boolean {`
  > `  return bid.pricePaise >= ask.pricePaise;`

  and the module comment at line 28: `crossed()` is "kept as a runtime
  assertion for the debug build" -- but nothing in `ladder.ts` calls it.
- Impact: `ladderRows` builds two row ranges independently -- descending
  from `top` (above best ask) down to `bestAsk.pricePaise`, and descending
  from `bestBid.pricePaise` down to `bottom` -- and never checks
  `bestBid.pricePaise >= bestAsk.pricePaise` before doing so. If a
  `LadderSnapshot` that is internally crossed is passed in, the two price
  ranges overlap and the function emits two rows for the same `pricePaise`
  (one carrying only a bid, one only an ask) instead of refusing, rather
  than surfacing `LadderProblem`. `fromFrame` only checks that the two
  sides share a sequence number (P11-11's actual card: the consistency unit
  is the frame); it never checks that the paired result is uncrossed. So
  the crossed-book guard the file's own commentary describes exists as an
  unused, orphaned function.
  This is retired code, so there is no runtime path exercising it today.
  The finding matters for two reasons: (1) it is directly checkable, not
  speculative -- see the reproducer; and (2) if `ladder.ts` were ever mined
  for a revival or a port, "there's a `crossed()` check already" is the
  natural (wrong) assumption to make about `ladderRows` specifically,
  because the module comment describes the invariant as handled.
- Reproducer: `ladderRows({ seq: 1n, bids: [{pricePaise: 100n, quantity: 1n, orders: 1}], asks: [{pricePaise: 90n, quantity: 1n, orders: 1}] }, 5n, 20)`
  -- a bid at 100 paise and an ask at 90 paise (a crossed book, bid > ask)
  -- returns `{ ok: true, rows: [...] }` with overlapping/duplicate
  `pricePaise` rows between 90 and 100, rather than `{ ok: false, problem:
  LadderProblem.Crossed }` (no such problem variant currently exists in
  `LadderProblem`).
- Confirming detail: `client/tests/ladder.test.ts` (this group's own test
  file) never calls `ladderRows` via `fromFrame`; every one of its four
  invocations (lines 120, 139, 140, 144) constructs a plain `LadderSnapshot`
  object literal directly. Test [1] in that file does demonstrate
  `crossed()` and `fromFrame`'s seq check separately, but never demonstrates
  that a crossed-but-seq-consistent snapshot fed to `ladderRows` is refused,
  because it is not.
- Suggested regression test: add a case to `client/tests/ladder.test.ts`
  that constructs a `LadderSnapshot` with `bids[0].pricePaise >=
  asks[0].pricePaise` and asserts `ladderRows(...).ok === false`; or, as the
  fix, have `ladderRows` call `crossed()` internally as its first check and
  return a new `LadderProblem.Crossed`.

### C20-002 -- `EndSlopes.downside` looks load-bearing but is not read by `payoffProfile`, and no test exercises it

- Severity: P3 (retired code; current behaviour is verified correct -- this is a code-hygiene / future-edit hazard, not a wrong number today)
- Classification: HYPOTHESIS
- Evidence: `client/src/panels/strategy.ts:255-256`
  > `const unboundedUpside = slopes.upside > 0n;`
  > `const unboundedDownside = slopes.upside < 0n;`

  compare the `EndSlopes` interface and its doc-comment at
  `client/src/panels/strategy.ts:159-164` ("As spot falls toward zero") and
  `endSlopes()` at `client/src/panels/strategy.ts:166-188`, which computes
  `down` from put and future legs.
- Impact: Both `unboundedUpside` and `unboundedDownside` are derived solely
  from `slopes.upside`; `slopes.downside` is computed and returned on
  `EndSlopes` but is not read anywhere in `strategy.ts`, and a grep of all of
  `client/` for `.downside` (outside the interface field and its own
  computation) finds nothing. Re-deriving the logic by hand shows the
  current code is actually correct given the file's own stated invariant --
  spot cannot go below zero, so the low end is always exactly computable at
  `spot = 0` (folded in separately at lines 270-272) and can never be
  "unbounded"; the only direction that can be unbounded is as spot rises
  without limit, which is exactly what `slopes.upside`'s sign captures for
  *both* the profit and the loss case. So `unboundedDownside` correctly
  means "loss grows without bound as spot rises," not "as spot falls," and
  `slopes.downside` genuinely has no bound-decision to make.
  The risk is purely for a future maintainer: the field's name and its own
  doc-comment ("As spot falls toward zero") read as though they should
  drive `unboundedDownside`, and the natural-looking "fix" --
  `unboundedDownside = slopes.downside < 0n` -- would be a silent
  regression of the exact invariant this card exists to protect (max loss on
  a short put would then read "unlimited" instead of "strike less premium,
  exact," reintroducing the mistake `client/tests/panels.test.ts` test [2]
  specifically measures against). Nothing currently in the test suite would
  catch that regression, because nothing asserts on `slopes.downside`'s
  value or its non-role at all -- `client/tests/panels.test.ts` test [1]
  only asserts `endSlopes(SHORT_CALL).upside === -LOT` (line ~141) and never
  references `downside`.
- Suggested regression test: add an assertion in `panels.test.ts` that
  constructs an `EndSlopes`-producing leg set where `downside < 0n` while
  `upside === 0n` (e.g. a lone short put) and asserts `unboundedDownside` is
  `false` in that case specifically, with a comment stating that
  `downside`'s sign must never flip the bound decision -- turning the
  currently-implicit invariant into something a future refactor cannot
  silently break.

## 4. Protocol conformance vs server/ (match / mismatch)

`client/src/protocol.ts` was checked byte-offset-for-byte-offset against
`server/protocol.hpp` (`FrameHeader`, `wire::put_*`/`get_*`, `SeqTracker`,
`ClientMsg`, `Channel`, `FrameKind`) and independently re-decoded all 8
vectors in `server/tests/vectors/frames.txt` with a standalone Python
`struct` decode -- not the client's own code, not Node/tsc, per the
no-JS-tooling audit rule.

Field layout agrees exactly: magic `u32@0` (0x414c5452 / `'ALTR'`), version
`u16@4`, kind `u8@6`, channel `u8@7`, seq `u64@8`, topic `u32@16`,
payload_len `u32@20`, engine_time_ns `i64@24`, server_time_ns `i64@32`;
`FRAME_HEADER_BYTES` (client) and `kFrameHeaderBytes` (server) both 48.
Validation order agrees: magic, then version, then
kind/channel-Unspecified -- which is what makes `wrong_version` and
`zero_frame_kind` resolve to exactly one refusal reason each, on both sides.

All 8 vectors (`js_number_loss`, `negative_i64`, `high_bit_u32`,
`seq_u64_max`, `seq_above_2_53`, `all_zero`, `wrong_version`,
`zero_frame_kind`) independently decoded (Python `struct`, little-endian, per
the client's declared `DataView` calls and the server's field offsets) to
the exact values and refusal reasons their `expect=` column states.
**No mismatch found.** `ClientMsg` in `protocol.ts`
(`Unspecified/Subscribe/Unsubscribe/SnapshotRequest/KillSwitch`) matches
`server/protocol.hpp`'s `ClientMsg` enum member-for-member, including the
one-way kill switch (no order-placing member on either side).

`client/tests/conformance.test.ts` was then read and confirmed to exercise
all 8 vectors from `frames.txt` (`assert.ok(vectors.length >= 8)` followed
by a loop over every parsed vector, not a fixed subset), and separately
demonstrates the hand-rolled-decoder hazard is real by running a
deliberately naive 32-bit-signed-shift decoder against the same vectors and
asserting it is wrong on at least 3 of them (`high_bit_u32`,
`js_number_loss`, `negative_i64` are each asserted by name). This is the
right shape for a conformance test: it does not merely assert the two
implementations agree, it first demonstrates that agreeing was a real
result and not a construction artifact.

## 5. Survivals into desktop (verified / unverified)

Checked by Grep only, per instructions -- `desktop/chart/candles.hpp`,
`desktop/chart/chart_widget.hpp`, `desktop/depth_ladder.hpp`,
`desktop/quant_pages.hpp` were never read in full for this review.

- **Half-open candle bucketing (candles.ts) -- VERIFIED, survived
  correctly.** `desktop/chart/candles.hpp:35` carries the identical
  reasoning in its own header comment: "Half-open [start, end). Closed at
  both ends counts a boundary tick twice," and line 100 gates `c.complete =
  c.end_ns <= now_ns` exactly as `candles.ts`'s `complete: end <= nowNs`
  does. The lesson survived even though the TypeScript file implementing it
  was never linked into anything.
- **float32 GPU-vertex rebasing (scale.ts) -- VERIFIED, survived
  correctly.** `desktop/chart/chart_widget.hpp` carries a `Domain
  domain_` member with an `origin_ns` field (line 475, 178) and a comment at
  line 329, "THROUGH THE float32 STAGE, rebased in int64 first" -- the same
  origin-subtract-before-the-boundary pattern as `scale.ts`'s
  `toFloat32Offsets`.
- **Crossed-book handling (ladder.ts) -- VERIFIED DIFFERENT, and correctly
  so.** `desktop/depth_ladder.hpp:15-21` documents a *different, deliberate,
  live* decision under carried debt P2-08: "A CROSSED BOOK IS NOT AN ERROR
  AND MUST STILL DRAW... Bid above ask is legitimate during the auction and
  a fault at any other time, and the ladder cannot tell which -- so it
  RENDERS the book and marks it, rather than refusing to draw." This is the
  opposite policy from `ladder.ts`'s comment that "a real crossed book is
  arbitraged away in microseconds and is not something a 60 fps dashboard
  observes" (line 113-114). This is not a shared bug and not a regression:
  it is a case where the retired client's simplifying assumption
  (crossed-always-means-renderer-bug) is one the live desktop team already
  found to be wrong for the pre-open auction and fixed with a documented,
  opposite design. Worth recording so nobody "fixes" `depth_ladder.hpp` by
  porting `ladder.ts`'s refusal logic into it.
- **Footprint / aggressor inference (footprint.ts) -- grepped for, not
  found.** No match for `aggressor`, `Lee-Ready`, `tick rule`, `quote rule`,
  or `footprint` anywhere under `desktop/`. This feature has not been ported
  live; C20's footprint-classification design (quote rule declines rather
  than guessing) has no live counterpart to check for regression in.
- **Strategy payoff end-slope bounds (strategy.ts) -- grepped for, not
  found as a port.** `desktop/quant_pages.hpp` has one comment line
  ("the shape of the payoff") near line 1248 but no `end_slope`,
  `unbounded`, `max_loss`, or `payoff` bound-decision logic matching this
  module's pattern. Not confirmed as absent entirely (only grepped, per
  instructions, not read in full), but no evidence of the specific
  grid-extremum defect this card fixes being reintroduced live.

## 6. Revival hazards

- `client/src/session.ts`: the lock/unlock design is sound in isolation --
  `Teardown` obliges the caller (by convention, not by any compiler check)
  to unsubscribe everything, discard decoded state, and close the socket,
  rather than merely painting an overlay. PBKDF2-SHA256 at 600k iterations
  with a constant-time compare (`equalConstantTime`,
  `client/src/session.ts:140-145`) is a reasonable implementation, and the
  file is honest in its own comments about what a 6-digit PIN buys (hours,
  against someone who already has the machine -- not real secrecy). If ever
  revived, the one thing to re-verify is that whatever code *calls*
  `SessionLock.lock()` actually performs all three `Teardown` obligations;
  the type only documents them, it cannot enforce that the caller acts on
  them.
- `client/src/panels/audit.ts` / `client/src/panels/strategy.ts`: both
  correctly refuse to compute cost or place an order client-side, and
  `KillImpact`/`CostBreakdown` are required, server-supplied values with no
  zero-defaulting overload. A revival would need to re-verify the server
  side still only ever sends a `CostBreakdown` computed by `risk/cost.hpp`
  and never lets a client construct one, since nothing in this client code
  can catch that regression from its own side.
- `client/src/layout.ts`: `parseLayout`'s field-by-field validation with
  named refusal (never silent repair) is a solid localStorage-hardening
  template; flagging as a positive pattern rather than a hazard, in case a
  live surface (`desktop/`, which does not currently appear to persist UI
  state this way) wants one later.
- `client/src/chart/ladder.ts`: see C20-001 -- the specific hazard if
  revived is that a caller assumes `ladderRows` alone is safe against a
  crossed snapshot, because the module's own commentary describes the
  invariant as handled.

## 7. Open questions

- Whether `LadderProblem` should gain a `Crossed` variant as part of fixing
  C20-001, or whether the intended fix is only to call the existing
  `crossed()` from within `ladderRows` and reuse an existing problem code --
  this is a design call for whoever owns `client/` (or its port), not
  something this review resolves.
- Whether the strategy-builder payoff/bound logic in `strategy.ts` has a
  live desktop equivalent under different naming that this review's grep
  patterns missed (only `payoff|end_slope|unbounded|max_loss|vwap` were
  tried against `desktop/quant_pages.hpp`); if `desktop/` does have an
  options strategy builder, it is worth a direct check that it independently
  reproduces the exact-at-zero / end-slope logic rather than a grid
  extremum, since that is exactly the class of "confident, plausible, wrong
  finance" CLAUDE.md's gate 7 exists to catch.
