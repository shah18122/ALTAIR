# Altair project memory — Codex handoff

Updated: 2026-09-19  
Baseline HEAD: `a34af5c5c9fa8c7498e8cc4df00907548a41e1a5`  
Current activity: partial implementation and focused verification. Read the
latest dated continuation at the end before continuing; the earlier planning
and initial coverage sections below are historical records, not current status.

Read `CODEX_COMPLETION_PLAN.md` in this directory before continuing, and root
`final.md` — the single merged document that replaced `CLAUDE.md`, `ROADMAP.md`,
`README.md`, `plan.md`, `idea.txt`, `prompts/PROTOCOL.md` and `prompts/LEDGER.md`
(2026-09-23). The active backlog is `final.md` §18; the card ledger is §21.
Record actual changes in root `change_by_codex.txt`. Re-read changed files before relying on
these notes. Memory is a maintained reference, not proof of source coverage.

## User intent

Ultra-low-latency quantitative engine for NSE/BSE. First plan phases, then audit
all first-party code line by line, fix validated bugs/locks/races, implement viable
ideas from the model/fly-brain notes (now `final.md` §16–§17), add FYERS as the
primary broker with Zerodha Kite as the
explicit secondary, and rebuild/improve the Qt terminal with
GETS-inspired workflows, Bloomberg-style density, options and Atlas-based
navigation including hide/show. Keep a concise future-Claude change log.

Reference-only: RXT_trade, RXT_trade 2, Quants and GETSClient. Do not modify or
count them as Altair implementation. GETS inventory suggests a binary installer/
application distribution; original source and permission to redistribute assets
were not established. A clean layout screenshot was requested, not yet received.

## Guidance read during planning

- Root `CLAUDE.md`, `prompts/PROTOCOL.md`, and the entire `idea.txt` — all three
  have since been merged into root `final.md` (§2, §13, §16–§17).
- `.commandcode/taste/` root, coding-style, workflow and tooling guidance.
- `.gitignore`, build preset/manifest context and selected CMake code.
- Targeted ROADMAP latency section and LEDGER status/debt context; neither entire
  document is claimed fully reviewed in this task.
- Selected IDE project metadata was inspected; the large editor profile was
  truncated and is not claimed fully read. Personal workspace history and secrets
  were intentionally not read. No root `.claude/` directory was found.

The redesign-existing-projects skill informed an audit-first improvement of the
existing Qt stack. Its decorative web suggestions are not requirements for this
latency-sensitive terminal; data readability and preserving functionality govern.

## Coverage truth

CX-01 was completed by Claude on 2026-09-14/15: the attested ledger records
481/481 in-scope files and 141,880 lines, with 22 reviewer groups plus lead
verification. Exclusions are the reference-only trees, generated builds,
dependencies, data payloads, IDE state and binary assets. See
`prompts/audit/cx01c/CX01C_STATUS.md` and the appended CX-01 continuation below.

This does not mean every later edit was re-read line by line, nor that the
ledger hashes remain current after implementation. Re-read every changed file
and update or supersede stale coverage evidence as work continues. Do not turn an
inventory, old phase percentage, test name or source comment into a verified
status.

## Current architecture: disk observations

- C++23, mostly header-defined modules. Qt 6 Widgets is the active desktop;
  `client/` is retired TypeScript, not to be revived or installed as a frontend.
- Root docs mention LibTorch/ONNX aspirations; existing models include custom C++
  primitives. An LSTM forward/reservoir implementation is not a fully trained
  recurrent network. Atlas PARTIAL/ABSENT distinctions must remain honest.
- `app/price_service_main.cpp:378` onward: Kite decode feeds PriceBus display
  frames, not assembled normaliser/features/strategy/risk/OMS execution.
- Its historical CSV replay uses bar closes (`:279` onward), not canonical tick
  replay. `app/main.cpp` remains a Phase-0 replay demonstration. Canonical
  `Tick`/`DepthUpdate` persistence exists in `feed/tick_store.hpp` separately.
- No application consumer of `oms::drain_intents` was found in targeted searches;
  its presence as a function/test is not a working order execution loop.
- UI links read-side engine facilities, not broker/OMS targets; it issues intents
  and kill requests and uses broker-related subprocesses/price networking. Audit
  actual wiring against the documented in-process decision before extending it.
- `desktop/main_window.hpp:96` defines a flat page list; the sidebar has a fixed
  190-pixel width at `:600`. Atlas currently routes through integer page positions.
- `desktop/option_chain.hpp` already exists. It models a chain using a flat
  volatility input; it is not a complete live option quote/OI/smile chain. Its
  synchronous repricing recreates table items; optimise UI updates separately
  from engine tick processing.
- `instruments/contract_spec.hpp:38` defines only Kite=0 and Xts=1 with a fixed
  source count. FYERS needs explicit identity/mapping and stored-schema handling,
  not renaming XTS.

## Priority findings: confirmed code patterns, fixes still pending

1. `models/serving.hpp:104–108,137,147,194`: no reader-lifetime protection against
   slot reuse across promotions; generation counter is also shared non-atomically.
   Existing scalar-output stress testing does not establish coherent metadata or
   safe reuse. Force interleavings and choose a bounded ownership/publication design.
2. `core/lockfree/seqlock.hpp:52,71,104`: concurrent payload accesses are plain
   `T`. Atomic sequence counters alone do not provide a race-free payload protocol.
   Also inspect unbounded config-refresh spinning in `core/config/store.hpp`.
3. `oms/order_intent.hpp:127,170,208`: substring JSON parsing, unchecked integer
   accumulation and token narrowing. Add strict schema/bounds handling before
   consuming real intents; preserve queue conformance and incomplete-tail behaviour.
4. Live specs in `app/price_service_main.cpp:344` use placeholder lot/tick/scale
   registrations; replace with verified point-in-time master data at integration.
5. Stale normalised depth can carry a fresh sequence while book tradability does
   not reject stale flags. Define audit-only versus tradable admission explicitly.
6. Failover stability may miss an outage between polls. The Unknown→NoneHealthy
   result is already expected by a test: contradictory comments are not sufficient
   evidence that this transition itself is wrong.

Detailed regression ideas and phase gates are in the completion plan. Other
modules still require audit; these six items are not the full defect inventory.

## Decisions and constraints to preserve

- No hot-path allocations, blocking I/O, unbounded retries or exception flow.
  Establish ownership and bounded reclamation before choosing a lock-free design.
- Keep mandatory risk checks before orders; reject the execute-then-async-risk
  sketch in `final.md` §17. Slow analytics can be supplementary.
- Prefer small event-driven circuits and slow context layers, adopted only after
  causal/net-cost validation. Defer DPDK/RDMA and automatic live retraining until
  platform support, measurements and deployment controls justify them.
- ROADMAP 20/80 us path numbers are targets, not measured achievements. Measure
  internal latency separately from broker transport/execution and Qt paint time.
- FYERS market-data fallback does not transfer positions or permit retrying an
  uncertain Kite order at another account. Reconcile first; preserve OMS ownership.
- User controls phase decisions and live activation. No live financial action,
  broker account change or secret handling was performed by this planning task.
- Retain existing stack/dependency boundaries and targeted protocol cards. User's
  current request makes Codex the intended future implementer; old role names in
  `final.md` §13 do not erase the newly requested implementation scope.

## Verification and environment caveats

- Working tree at start: user-owned untracked `idea.txt` (since merged into
  `final.md`) and GETSClient reference
  directory. Preserve both. No source modifications were made during planning.
- CMake config points to Qt at `D:/Qt/6.8.3/msvc2022_64`; recheck availability at
  build time. Network/config features are optional; default green does not prove
  network targets or the charge-config loader were built.
- Root CMake explicitly refuses TSan under MSVC. A supported Linux TSan gate is
  needed; plain Windows Debug/ASan does not establish race freedom. No current
  cross-platform success or sanitizer success is claimed.
- Existing ledger says live-readiness dependencies remain open. Revalidate
  credentials/permissions, charge schedules, data access and reconciliation
  status without exposing secrets; do not assume historical notes are current.
- Official FYERS support confirms separate data/order sockets and publishes
  2026 API activation/static-IP/2FA/order restrictions. Source links and caveats
  are in the completion plan. The requested docs URL failed research-tool fetch;
  low-level C++ protocol feasibility and account entitlements remain unverified.

## Next step

Continue from the latest dated Codex continuation below and append actual
changes to root `change_by_codex.txt`. CX-01 is complete; CX-02, CX-03, CX-04,
CX-05, CX-07 and CX-08 still have open work. CX-06 navigation/layout cards were
completed earlier, but new forecast/Atlas changes still require their own
acceptance evidence. Never infer a full phase closure from a green targeted test.

## CX-01 Claude continuation — 2026-09-14 → 2026-09-15 (AUDIT COMPLETE; no implementation)

Appended by Claude (lead) after completing Phase 1. It supersedes the "Coverage truth" and "Next step" sections above, which are kept as the historical Codex record. Baseline HEAD is unchanged: `a34af5c5c9fa8c7498e8cc4df00907548a41e1a5`. No tracked file was modified.

**Entry point:** `prompts/audit/cx01c/CX01C_STATUS.md`. Deliverables, all under `prompts/audit/cx01c/`:

| File | Contents |
|---|---|
| `CX01C_COVERAGE_LEDGER.{json,md}` | Per-file path, SHA-256, lines, exact ranges, reader, purpose, integration, findings. Codex inherited coverage is listed separately and not counted |
| `CX01C_FINDINGS.md` | Prioritised findings: leads revalidation, priority P1 table, 16 cross-cutting themes, first regression tests |
| `CX01C_FINDINGS_INDEX.md` | All 321 findings; details and regression tests are in the `G*_findings.md` files |
| `LEAD_VERIFICATION.md` | V-01…V-16 source-verified findings |
| `CX01C_ARCHITECTURE.md` | Actual executables, threads, queues, conventions, live/replay paths |
| `CX01C_BASELINE_REPORT.md` | Checks run, their results, latency measurements |
| `CX01C_IDEA_ATLAS_READINESS.md` | idea-note decisions (`final.md` §16–§17); Atlas × integration × disposition; training status |
| `CX01C_FYERS_PREREQUISITES.md` | FYERS broker prerequisites |
| `CX01C_PHASE2_TASKS.md` | Dependency-ordered cards (tiers A–I) |

**Facts future sessions should start from:**
- **No executable assembles feed → book → features → model → strategy → risk → OMS.** Nothing drains `data/order_intents.jsonl`, no code sends an order to a broker, and `check_order` has no non-test caller. There is no P0 only because nothing can place an order.
- **Coverage:** 481/481 files, 141,880 lines, 22 reviewer groups plus lead, all hash-checked. Reviewer-attested; the lead re-read cited lines only.
- **Confirmed P1s (lead-verified ✔):**
  - intent torn-line merge C13-005 ✔
  - order state: cancel reject → terminal Rejected C13-002 ✔; partial fill drops pending cancel C13-001 ✔
  - gap stop exit valued at stop C13-010 ✔
  - order confirmation hides price and ₹24,000 default limit C17-010 ✔ (partly lead-authored P39 code)
  - dataset prices at 6 significant digits C14-001 ✔
  - charges.toml `[safety]` inert C12-003 ✔
  - pre-trade limits unused C12-001 ✔
  - overnight filter look-ahead C11-001 ✔
  - spot parity formula C11-002 ✔
  - also: no Unknown order state C13-004; exercise STT omitted C11-003; GUI-thread report blocks up to 125 s C16-001
- **Card-origin defects:** four core defects are specified by the P0 task cards themselves (C21-001…004). Fixes must amend the cards.
- **Codex leads:**
  - E001/E002/E003/E006, Q01/Q02/Q04/Q05, UI-001…005: CONFIRMED
  - E004, Q03: PARTIAL
  - E005: CONFIRMED and extended
  - Config spin: CONFIRMED unbounded
  - Stale depth and failover: CONFIRMED; Unknown→NoneHealthy is defensible
- **Baselines (2026-09-14):**
  - `default` 130/130, `net` 135/135, `asan` 129/129 with 0 compiler warnings
  - `tsan` refuses to configure on MSVC
  - No Linux, no UBSan
  - Latency numbers are batch means inside test binaries. The TSC p99 printed OVER budget and still passed; IV at 1.6 µs/solve means a 100-strike chain is far over the ROADMAP chain target.
  - The P39-00 "46 µs vs 50 µs budget" claim compared a mean with a p99 and is withdrawn.
- **Training:** nothing is persisted; every model is refit in memory per run or click. The neural tier is reservoir readouts. No model beats a random walk net of cost; some verdicts are printed, not asserted.
- **`config/altair.toml` has no consumers.**

**User requests received with the audit brief:**
- "fetch data till yesterday": **not done**. The Kite token has expired (session file written 2026-09-13), and C14-001 means a fetch writes lossy prices; fix CX02-A1 first. Daily data already ends Fri 2026-09-11.
- "gui in models" / "dont change model atlas": deferred to Phase 2 card CX06-G4, Atlas unchanged. **Scope needs Smit's answer:** inspect models or train from the UI.
- "is training of all models perfectly done": answered **no** (readiness §5).

**Process notes:**
- Usage limits interrupted reviewers three times. Work resumed from saved reviewer context; the resume point is kept in STATUS.
- One reviewer (G19) ran a single `node -e` command on its own output JSON, against the brief. It was disclosed; no project file was touched.
- Generated side effects outside the source tree: `build/asan` rebuilt; `build/tsan` failed configure.

**Next step (needs Smit):**
1. Rotate the exposed Kite API key and secret.
2. Decide on a Linux/TSan environment.
3. Answer the Models GUI scope question.
4. Give go/no-go for Phase 2. Tier A (dataset precision) and Tier B (intent/order lifecycle) come first.

## CX continuation — 2026-09-19 (partial implementation, not phase closure)

This is the current handoff after the user's request to continue remaining work.
CX-00 planning and the Claude-led CX-01 audit remain complete as recorded above.
Do not treat implementation of selected CX-02/CX-06 cards as proof that every
card or full phase gate is closed.

**Implemented and verified in this continuation:**

- CX-02 selected fixes: safe model-slot publication with reader pins and
  coherent generation/metadata; strict intent grammar and numeric/time bounds;
  bounded durable-queue cursor/dedupe/TTL handling; positive-fill price checks;
  safer exit-price/stop-side validation; checked dataset formatting/merge and
  per-series update locking; kill-request clear/write interleaving protection;
  book/depth bounds; removal of the unsafe seqlock payload API.
- CX-06/forecast slice: grouped expandable/hideable navigation and stable Atlas
  routing are present. Forecast UI separates current tick from the next target,
  has 1m/5m/15m/60m/daily horizons, reanchors after real stream reconnects, and
  explicitly discloses that unfinished-bar live-tick predictions are not
  historically tick-calibrated.
- Latest `build/codex-cx06` incremental full-target build: exit 0 (13 rebuilt
  steps; an earlier full build in this continuation had completed 523/523
  before final corrections). Latest configured CTest suite: **141/141 passed**,
  229.98 s, including intent/queue, order state, exit ladder, kill switch,
  model serving/forecast, desktop navigation/scale, dataset merge and price text.
- Focused OMS/queue/kill-switch checks also passed 5/5. `git diff --check`
  reported no whitespace errors; Git emitted only LF→CRLF notices.

The CX-01 line-coverage ledger is a historical audit snapshot. Every changed
file's current hash differs from that snapshot and must be re-read before anyone
claims current-tree line-by-line coverage. The edits here were scoped and
reviewed/tested; they do not constitute a re-audit of 481 files.

**Explicitly still open:**

- CX-02: no supported Linux TSan/UBSan run, no comprehensive post-edit race
  audit, and no allocation or tail-latency evidence for the actual hot path.
- CX-03: the planning-only outline is `prompts/CX03_IMPLEMENTATION_PLAN.md`,
  but no executable wires live/replay events through canonical mapping,
  book/features/model/strategy, synchronous risk, OMS and paper broker. The UI
  request queue remains without an application drainer/order sender.
- CX-04: no causal walk-forward net-of-cost evaluation, tick-horizon historical
  calibration, shadow/paper promotion proof or accuracy claim. Live tick
  inference is not the same phase as completed-bar training.
- CX-05: FYERS adapter/auth/data/order/reconciliation integration is not built;
  protocol fixtures, account entitlements and credential authorization are open.
- CX-06: navigation/layout cards have evidence, but whole-terminal integrated
  acceptance is not asserted for the latest forecast/Atlas changes.
- CX-07: live option quote/depth/OI/IV/Greeks and linked broker-backed workflows
  are not complete; model values must not be represented as market quotes.
- CX-08: no TSan/replay paper drill/latency percentiles/live go-no-go. No live
  broker session, order, data fetch or secret operation was performed.

Cline CLI attempts produced no review output: the earlier configured provider
returned ConnectionRefused; a no-edit retry was rejected by the workspace
approval layer because it would transmit first-party source to an external
provider. Do not retry or route around that decision. Obtain explicit user
approval for that source-sharing destination before any future Cline review.

Reference boundary remains unchanged: `GETSClient_5.0.191022_64bit_040924/`,
`RXT_trade/`, `RXT_trade 2/`, and `Quants/` were not modified or executed.

## CX continuation — 2026-09-21 (verified selected safety slices)

The 2026-09-19 entries above are historical. This continuation rebuilt after
additional fixes and reran the complete configured suite; it does not complete
the project phases or authorize live use.

**Verified local changes:**

- `oms/order_state.hpp`: new-order send timeout and cancel/amend reply timeout
  move to conservative `Unknown`; incremental events are refused until a
  structurally valid authoritative snapshot resolves state. The caller must
  authenticate, correlate exact order/account/product/instrument identity and
  establish snapshot freshness; the value-level API cannot prove provenance.
  Incremental zero-quantity/nonzero-price reports and invalid request-answer
  enum states are refused.
- `oms/reconcile.hpp`: average-price subtraction/absolute-value overflow is
  removed with full unsigned magnitude and saturated display gap. Negative
  tolerance, null counted pointers, absent counted rows and sizes beyond `int`
  result capacity are rejected. Product remains part of `(product,key)` identity.
- `flagging/deploy.hpp`: the cumulative hard-loss limit is checked before
  `min_observations`; `INT64_MIN` loss is handled without signed negation; one
  controller instance cannot reopen and erase a canary's loss history; invalid
  non-finite observations roll back.
- `app/monitor.hpp`: worst-health ordering now makes `Unobserved` worse than
  `Stale/Critical/Warn/Ok`; non-finite gauge values and invalid thresholds are
  `Critical` rather than silently green. Histogram buckets now use bounded
  `uint64_t` counters; recording stops when the accepted total reaches
  `UINT64_MAX`, and an online mean avoids signed cumulative-sum overflow.
- `server/price_bus.hpp`: cap coalescing retains a partially sent head frame
  and discards only complete unsent frames. Tested in the Boost-enabled
  `build/cx-net`, because `build/codex-cx06` omits that target without Boost.
- `CMakeLists.txt`: strict invariant option now always defines 0 or 1. A Release
  build with the option OFF reaches the intended compile-time rejection.
- `app/tests/test_note_reconcile.cpp`: the deterministic synthetic fixture is
  explicitly marked verified to exercise `compute_cost`; this is not evidence
  that the production TOML rate schedule is verified. `config/charges.toml`
  remains UNVERIFIED and fail-closed.
- `core/invariant/conservation.hpp`: `check()` is documented as internal
  consistency and evaluates all five terms through `ExactSignedSum`; refused
  fills latch `RefusedFill`; `reconcile()` compares independently supplied
  cash and position. Focused conservation CTest passes; live/paper snapshot
  wiring remains in CX-03, not proven by the component API test.

**Verification:**

- Full `build/codex-cx06` build: exit 0.
- Full configured CTest: **141/141 passed** in 234.35 s, with no global
  `QT_QPA_PLATFORM=offscreen`. The previous five GUI timeouts were caused by a
  blanket offscreen override; rerunning those five under their proper/default
  Windows platform passed 5/5.
- Focused order-state, OMS reconciliation, monitor, note reconciliation and
  flagging tests: 5/5 passed. Boost-enabled price-bus test: 1/1 passed in 3.30 s.
- After the full-suite run, the histogram counter/mean hardening rebuilt the
  monitor target and the focused monitor CTest passed 1/1.
- `ALTAIR_STRICT_INVARIANTS=OFF` Release target failed at the intended header
  `#error`; `git diff --check` found no whitespace errors (only line-ending
  notices).

**Continuation verification 2026-09-22:**

- The existing navigation/forecast slice was rechecked without source changes:
  grouped menu/Atlas routing, compact/hidden recovery, 125%/200% GUI scaling,
  1-minute future-target inference and live stream continuity tests passed.
- A complete `build/codex-cx06` rebuild reported no work. The full configured
  CTest run passed **141/141** in 224.46 seconds using the normal Windows GUI
  platform. This is regression evidence only; the open phase gates below are
  unchanged.

**Model and navigation continuation 2026-09-22:**

- `models/classical.hpp` adds deterministic, bounded logistic regression,
  bootstrap/random-feature shallow forests, PCA factor extraction and VAR(1).
  The new classical-model test passes; these are cold research primitives and
  are not connected to order submission.
- `analytics/binomial.hpp` adds a bounded Cox-Ross-Rubinstein lattice for
  European and American contracts. The binomial test passes; the Atlas row is
  `PARTIAL` because trinomial pricing is not included.
- `risk/optimise.hpp` now exposes long-only projected-gradient mean-variance
  weights from explicit expected returns; the existing optimiser test covers
  full investment and no-short constraints.
- Atlas configure audit now reports 63 built, 10 partial and 12 absent entries.
  Remaining absences are not silently relabelled: they require verified data,
  provider contracts or larger research implementations.
- `desktop/workspace_navigation.hpp` keeps stable routing but now presents an
  Altair/control-room header, search entry, page/domain counts, stronger dark
  surfaces and visible active/focus states. Navigation shell tests at 100%,
  125% and 200% plus the registry test pass.
- Expanded verification after the model/UI changes: full `build/codex-cx06`
  build succeeded and configured CTest passed **143/143** in 231.58 seconds.
  The added classical-model, binomial and mean-variance checks pass with the
  existing suite. This remains regression evidence, not live-readiness proof.

**Cline status:** The user explicitly requested Cline. A plan-only invocation
was attempted; default state was read-only, and a temp-state retry could start
the CLI but the configured provider returned `ConnectionRefused` before
producing model output (reported 0 tokens). No Cline edits or review were
received. Do not claim Cline reviewed the changes.

**Still open:** CX-01 current-tree whole-file coverage is not re-established;
CX-02 has no supported Linux TSan/UBSan or actual hot-path allocation/tail
latency proof; CX-03 still lacks the canonical live/replay→mapping→book/features
→model/strategy→synchronous-risk→paper-OMS dispatcher and parity drill; CX-04
has no causal tick-horizon walk-forward net of costs/calibration; CX-05 has no
FYERS adapter/auth/verified fixtures or entitlements; CX-06 visual/paper
integration acceptance is not complete; CX-07 lacks verified real option
quotes/depth/OI/IV/Greeks; CX-08 has no supported sanitizer/performance/paper
release drill or go/no-go. No broker login, order, live data fetch, secret
operation, commit or push was performed. Reference directories remain
untouched.

**Broker priority update (2026-09-22):** `config/altair.toml` now declares
FYERS as primary and Zerodha Kite as secondary for feed/broker preference.
`config/charges.toml` has an explicit, still-unverified `[broker.fyers]`
schedule. `broker/fyers_api.hpp` is credential-boundary scaffolding only:
login URL, SHA-256 `<client_id>:<secret>` app-id hash, and FYERS authorization
header; it does not open sockets, read environment variables, persist tokens or
place orders. `oms/throttle.hpp` now has independent FYERS/Kite limits and an
explicit venue router that refuses unsupported XTS and never performs blind
cross-broker failover. The official FYERS support documentation confirms daily
2FA/static-IP/order-rate requirements and separate market-data/order sockets;
raw FYERS WebSocket frames remain unverified, so live FYERS transport is still
blocked on fixtures and account prerequisites.

The broker update reconfigured and built cleanly. Full configured CTest after
the change passed **144/144** in 226.27 seconds, including `fyers_auth`, the
dual-broker throttle route, existing Kite tests and the real config parser.
