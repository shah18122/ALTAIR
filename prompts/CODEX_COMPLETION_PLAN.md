# Altair completion plan — Codex

Date: 2026-09-14  
Baseline commit: `a34af5c5c9fa8c7498e8cc4df00907548a41e1a5`  
Status: PARTIAL EXECUTION, NOT PHASE-COMPLETE. Current implementation and
verification status is maintained in `prompts/CODEX_PROJECT_MEMORY.md` and
`change_by_codex.txt`; the original baseline commit remains historical context.

## Objective and scope

Complete an evidence-backed review of first-party Altair, correct validated
defects, assemble its low-latency trading path, implement viable ideas from
the model inventory and fly-brain notes (now `final.md` §16–§17), add FYERS as
the primary broker with Zerodha Kite as the explicit
secondary, and improve the existing Qt terminal with GETS-inspired workflows
and a restrained Bloomberg-style presentation.

These CX phases are a new delivery sequence, not a renumbering or claim of
completion of the historical P0–P39 cards. Existing ledger statuses are evidence
to inspect, not proof that a feature is integrated, safe, or cleared for live use.

Scope includes source, tests, configuration schemas, build scripts, project
instructions, research/operating documentation, and relevant hidden project
configuration. The project rules that were in `CLAUDE.md` are now `final.md`
(§2 protocol, §13 hard rules); `.commandcode/taste/` exists; no project `.claude/`
directory was found during planning. Retired `client/` is reviewed as historical
first-party code, explicitly labelled inactive and not revived.

Exclude `RXT_trade/`, `RXT_trade 2/`, and `Quants/` from implementation and the
first-party coverage denominator. `GETSClient_5.0.191022_64bit_040924/` is a
read-only UI/workflow reference, not Altair source. Do not run, modify, decompile,
or redistribute its binaries. Its inventory appears to be an installed binary
distribution; original application source was not found. A clean user screenshot
can improve fidelity; absent that, record visual details as unverified.

Exclude generated builds, third-party dependencies, large data payloads, binary
models, `.git` object storage, personal IDE history, credentials, and account/order
state from line-by-line source coverage. Review their interfaces, configuration
requirements and retention/security policies instead. Never copy secrets into
memory, findings, fixtures, or the change log.

## Non-negotiable architecture

- C++23 runtime, existing Qt 6 Widgets desktop; no Python runtime, web rewrite,
  or new ML framework merely to match a name in the idea document.
- Strong units and checked boundary conversions; integer ledger money; contract
  specifications are point-in-time data, never placeholder lot/tick/expiry values.
- Bounded, allocation-free hot paths without blocking I/O or exceptions. A
  worker, recorder, model trainer or UI must not hold up the decision owner.
- Establish ownership before choosing concurrency primitives. Lock-free is not
  synonymous with correct or bounded. Allocation and appropriate locks may remain
  on cold control paths; do not blindly remove them everywhere.
- One canonical admitted-event decision path for live and recorded tick replay.
  Historical bar replay remains a separate, clearly labelled research capability.
- Net-cost signals and mandatory synchronous pre-trade risk checks before order
  submission. Slower risk analytics supplement these checks, never replace them.
- Only OMS places, changes or cancels orders. UI requests/intents do not imply
  broker acceptance; expose distinct queued, validated, submitted and filled states.
- Stale/unknown/ambiguous data, exhausted capacity, invalid model state or an
  uncertain broker order must produce explicit state and a conservative response.
- Retain the documented in-process UI decision unless the user separately approves
  a topology change. Keep display networking, Qt work and training off the critical
  path; re-audit actual process boundaries rather than trusting architecture prose.

## Phase sequence and completion gates

| Phase | Work | Required exit evidence |
|---|---|---|
| CX-00 — Planning and guardrails | Read project guidance and the full idea file; inventory reference/source boundaries; record known gaps, assumptions, phase dependencies and memory/log formats. | This plan, initial memory and an honest documentation-only change log. No implementation claim. |
| CX-01 — Complete audit and baseline | Read every in-scope file in complete bounded chunks; trace callers, tests, threading, data ownership and actual executable assembly. Read ROADMAP, LEDGER, cards and hidden guidance without substituting summaries for source. Establish reproducible build/test/performance baseline. Classify each idea and Atlas entry by implementation and integration readiness. | Per-file hash/physical-line coverage ledger; every source line accounted for; architecture and ownership map; prioritised findings with tests; baseline results, explicit skipped/unavailable gates, and accepted/deferred idea backlog. |
| CX-02 — Correctness, concurrency and boundaries | Fix reproduced/validated defects in snapshot publication, model lifetime/metadata, unbounded waits, intent parsing, integer bounds, feed freshness, queue capacity, order state and ledger invariants. Audit all other modules for analogous errors, deadlocks and races. | Targeted regression tests, deterministic interleaving tests, strict parser conformance, supported sanitizer evidence and hot-path allocation/tail-latency checks. No blanket claim that all possible bugs are eliminated. |
| CX-03 — Canonical engine integration | Assemble live/replay admission, canonical instrument mapping, book, features, model inference, net-cost signals, synchronous risk and OMS. Connect validated control-plane intents and kill requests. Publish UI/recording observations without blocking decisions. | Equivalent captured ingress/replay produces equivalent decisions and reproducibility records; malformed/stale/gap/reconnect/backpressure tests; paper-execution end-to-end tests; UI still has no direct trading handle. |
| CX-04 — Viable idea implementation | Build a tiered event-driven model schedule using existing validated primitives. Start with volatility, then book imbalance, then trade-flow signals; add slower regime/fundamental context where data supports it. Add feature freshness, horizon alignment, calibration and deployment gates. | Out-of-sample walk-forward evaluation net of costs, causal features/labels, null-model comparisons, calibration and regime results, measured latency, and shadow/paper evidence. Unhelpful models may remain disabled with a recorded reason. |
| CX-05 — FYERS backup | Add verified API v3 authentication/session handling, canonical instrument mapping, market/depth adapter, order updates and OMS broker adapter. Implement independent data and execution fallback policies, bounded reconnect/subscription/rate handling and reconciliation. | Versioned fixtures/schema compatibility, stale-feed and reconnect drills, uncertain-order/duplicate prevention tests, capability/freshness matrix, and explicit account/credential/compliance blockers. Live execution stays disabled pending authorisation. |
| CX-06 — Terminal and navigation | Evolve the current Qt terminal into a dense, clean GETS-inspired workspace. Replace flat positional navigation with stable page IDs, grouped Atlas-aware navigation, search/favourites, hide/show/compact modes and saved layout state. | Existing pages remain reachable; Atlas links resolve semantically; navigation and keyboard/persistence tests; visual checks at common laptop sizes and scaling; feed/kill/error status remains visible with navigation hidden. |
| CX-07 — Options and complete workflows | Extend the existing model-price option chain with real contract quotes, depth/OI/volume where available, per-option IV/Greeks, expiry/strike controls and linked watchlist/chart/ticket/depth panels. Complete order/trade/position/risk/audit and primary/backup status views against verified backend capabilities. | No model estimate labelled as a market quote; missing/stale fields remain explicit; subscriptions and quote-age limits respected; pricing boundary tests, linked-selection tests and responsive high-rate UI tests. |
| CX-08 — Release verification and handoff | Rebuild all required configurations and executable targets; run regression, sanitizer, replay, failure/restart and performance suites. Package/rebuild the Altair terminal, not the GETS binaries. Finish operating and developer handoff notes. | Reproducible build instructions, test and benchmark reports, no unresolved release-critical findings, documented residual risk/deferred work, complete memory/change log and written go/no-go. Live use requires a separate explicit decision. |

CX-01 precedes implementation. Correct shared state and input boundaries before
adding integration load. CX-04 and CX-05 can proceed independently after their
CX-02/CX-03 contracts are stable. CX-06 design/static navigation work can run
alongside backend work after the audit; integrated UI acceptance depends on
CX-03. CX-07 depends on the new UI contracts and verified provider data. CX-08
accepts only implemented and measured deliverables, not planned ones.

## Priority findings to resolve first (CX-01 baseline)

1. **Shared-state lifetime.** `models/serving.hpp` has unprotected inactive-slot
   reuse and a non-atomic generation counter accessed across threads. The current
   scalar-weight stress test does not establish prediction/metadata coherence.
   `core/lockfree/seqlock.hpp` also copies a plain payload while another thread may
   write it. Determine real reader/writer ownership, then use a standards-correct
   publication protocol. Prefer owner-thread installation at safe event boundaries
   where appropriate; otherwise prove bounded reader retention and reclamation.
   Merely adding another buffer is not a fix. A slow reader must prevent reuse,
   with deferral/refusal on the writer side rather than hot-reader blocking.
2. **Intent trust boundary.** `oms/order_intent.hpp` uses substring field discovery,
   unchecked signed integer accumulation and unchecked narrowing to a broker token.
   Strict schema/type/escape/duplicate/completeness checks and checked conversions
   must precede any new application consumer. Test integer extremes, huge digit
   strings, token overflow, malformed JSON, partial file tails and restart offsets.
3. **Incomplete executable assembly.** `app/price_service_main.cpp` distributes
   decoded data for display; it is not currently an executing strategy/OMS loop.
   Its CSV bar replay and the old `ReplayTick` harness do not prove canonical tick
   replay parity. Do not describe this as already-live orders bypassing risk.
   Assemble the real pipeline and remove placeholder instrument registrations.
4. **Freshness and limits.** Define what may be logged versus what may update
   tradable state. Cover stale depth with a newer sequence, invalid instrument IDs,
   depth counts, sequence exhaustion, outages missed between health polls, and
   bounded queue saturation. Audit OMS event ordering, cancellation/fill races,
   shutdown/join order, lifetime and financial overflow across the remaining code.

Concurrency tests will force interleavings with latches/barriers, including a
reader paused during inference across multiple attempted publications. Validate
prediction, model digest, feature version and generation as one coherent record.
Keep intentionally racy demonstration tests separate from the clean race-sanitizer
suite; neither reruns nor suppressed races count as fixes.

## What to adopt from the idea notes (now `final.md` §16–§17)

| Direction | Decision for implementation planning |
|---|---|
| Small specialised event-driven circuits | Adopt incrementally using current primitives, with explicit freshness/horizon contracts and benchmarks. |
| Fast inference plus slow context/training | Adopt; publish validated immutable results without making the decision thread wait. |
| Selective model recomputation | Adopt where dependencies permit. Every required market/book/risk state update must still be processed; skipping model evaluation must not mean silently losing ticks. |
| Conformal uncertainty and independent validation | Adopt where assumptions and sample size justify it; evaluate calibration by regime and horizon, not only aggregate fit. |
| Fixed-window training is necessarily look-ahead | Do not accept this assertion. Causal rolling and expanding windows are both candidates; information timing, preprocessing and purged label overlap determine leakage. |
| Execute first, check risk asynchronously | Reject as the primary order gate. Use fast synchronous hard limits before send, plus slower supplementary analytics. |
| DPDK, RDMA, one CPU per circuit | Defer until host/platform/feed support and measured bottlenecks justify the complexity. More threads are not automatically lower latency. |
| Automatically modify live model weights | Defer until versioned updates, reproducibility, validation, shadow/canary and rollback are demonstrated. |
| A quoted 5 ms total or profitable fly-brain analogy | Treat as hypotheses, not acceptance evidence or profit promises. Measure the actual system and strategies. |

## Latency acceptance

Preserve ROADMAP section 11 as target context, not measured achievement: decode
1 us, book update 0.5 us, risk checks 3 us, and approximately 20/80 us for the
non-NN/NN paths in its proposed architecture. First establish exact measurement
boundaries and achievable release-build budgets on the available hardware.

Report internal ingress-to-decision and order-encoding/submission-queue latency
separately from transport, broker acknowledgement, exchange execution and UI
paint latency. A sum of stage p99 values is not an end-to-end p99 measurement.
Record p50/p95/p99/p99.9, sample count, warm/cold conditions, CPU, OS, compiler,
affinity, load, allocations, queue depths and drops. Repeat under burst feeds,
model promotion, recorder pressure and simultaneous UI activity. Any amended
budget must be justified; never loosen it merely to make a gate pass.

MSVC's existing TSan configuration explicitly refuses unsupported use. Schedule
real race/undefined-behaviour checks on a supported Linux toolchain; Windows ASan
is not a substitute. If that environment is unavailable, record the gate as
unverified, not green. Do not install another OS or purchase infrastructure
without separate approval.

## FYERS-specific design and preflight

- Configure FYERS as the requested primary and Kite as the explicit secondary.
  Add a real FYERS source identity in a later transport card; do not
  rename/reinterpret existing XTS source values in stored data.
- Pin API/protocol references and verify the C++ transport/decoder feasibility
  early in CX-01. The requested documentation URL could not be fetched by the
  research tool during planning; use official support and sample repositories,
  with real fixtures and account entitlements before claiming compatibility.
- FYERS documents separate market-data and order-update sockets. Model their
  health, recovery, subscriptions and status independently.
- Automatic data fallback is allowed only to a healthy, correctly mapped source,
  with recorded source changes and deterministic recovery rules. Historical bars
  or stale REST snapshots are not replacements for a fresh depth stream.
- Execution routing is conservative: orders/positions remain associated with
  their broker account. An uncertain FYERS submission is reconciled at FYERS;
  never blindly retry it at Kite. A switch requires reconciled account state,
  available margin, authorised routing policy and fresh risk checks.
- Source OI/volume/Greeks/quote fields from verified capabilities. Use a separate
  bounded cold enrichment path if necessary; do not invent unavailable fields.
- According to FYERS' published 2026 guidance, order integration must account for
  the activated app/static-IP setup, daily 2FA, order-rate restrictions and market
  price protection behaviour. Recheck these before account testing or go-live.
  Do not automate 2FA bypasses or perform live trial orders as a test substitute.

Sources checked 2026-09-14:

- [Requested FYERS API v3 docs](https://myapi.fyers.in/docsv3)
- [Official API examples](https://github.com/FyersDev/fyers-api-sample-code)
- [Separate market and order sockets](https://support.fyers.in/portal/en/kb/articles/can-i-subscribe-to-both-order-updates-and-market-data-simultaneously-through-websocket)
- [WebSocket protocol guidance](https://support.fyers.in/portal/en/kb/articles/can-you-provide-details-about-the-websocket-message-structure-in-fyers-api-v3)
- [FYERS 2026 API requirements](https://support.fyers.in/portal/en/kb/articles/what-are-the-new-sebi-rules-for-retail-algo-trading-from-april-01-2026)

## Terminal / navigation specification

Keep existing functionality and implement original UI code in Qt. Use charcoal
surfaces, restrained amber accents, semantic gain/loss/error colours, tabular
numbers, compact tables, clear separators and visible keyboard focus. Prioritise
usable information density over decoration. Avoid animation or effects that
consume the data/paint budget.

Proposed top-level groups: Market & Terminal; Options & Volatility; Strategies;
Models & Research; Risk & Portfolio; Execution & Accounts; System & Audit.
Under Models & Research retain the Atlas family taxonomy and clearly distinguish
BUILT/PARTIAL/ABSENT from a separate live-readiness state. An Atlas taxonomy entry
is not automatically an approved implementation requirement or tradable model.

Use stable page/model identifiers shared by navigation and Atlas routing, not
integer row positions. Preserve existing named entry points and define migration
for saved layouts/legacy indices. Add searchable navigation, favourites, collapsible
groups, expanded/compact/hidden modes, a visible restore control and a conflict-free
shortcut. Restore last page/layout on startup. Hiding navigation must not hide
emergency controls, connectivity, staleness or live/paper mode.

Reuse and improve the existing terminal and option chain. Quotes and model values
need distinct labels and timestamps. Replace per-tick item reconstruction with
incremental model/view updates and bounded repaint coalescing; this may coalesce
display work, never engine decisions or lossless audit events. Limit background
work for hidden pages. Test linked symbol selection, sorting, scrolling, focus,
keyboard actions, restore state, empty/error/stale states and high-DPI layouts.

## Review tracking, memory and change logging

CX-01 creates a per-file coverage ledger recording path, SHA-256, physical line
count, exact contiguous ranges read, reader, date, module/ownership summary,
findings and status. Distinguish inventoried, partially read, fully read,
reviewed, tested and verified. A changed hash invalidates prior coverage for the
changed content; a search hit is not a full read. Truncated tool output must be
retrieved before its range can be marked complete.

Maintain `prompts/CODEX_PROJECT_MEMORY.md` as the architecture/decision/handoff
entry point, linked to coverage, findings and evidence. It is external continuity,
not a claim of permanent model memory. Append every implementation/documentation
change batch to root `change_by_codex.txt`: date, phase/card, exact files, why,
behavioural effect, tests actually run/results, latency evidence, compatibility,
remaining blockers and next step. Do not log planned changes as completed work.

Use small manifest-limited cards with frozen interface contracts, following
the protocol in `final.md` §2 and its eight review gates (§2.5). Cross-module features use separate
cards with explicit interfaces. Preserve user changes and reference trees.
The user's new request authorises Codex to implement after planning; the old
Claude/DeepSeek actor assignments are historical, while their engineering gates
remain useful. No commits, pushes, live orders, deployment, secret rotation or
broker-account changes are implied by this plan.
