# CX-02 checkpoint log (append-only)

## 2026-09-16 00:20–00:45 IST — evidence re-established, plan written, Tier A started

**Evidence.**
- HEAD `a34af5c` is unchanged. No tracked file was modified before CX-02; only `change_by_codex.txt` and `prompts/CODEX_PROJECT_MEMORY.md` differ from the audit inventory (CX-01 appends).
- Codex processes are active (codex PIDs 15200/14224, node children), hence the private build directories and a `git status` check before each edit.
- `data/kite_session.json`: metadata only (mtime 2026-09-15 10:26). Not read.

**Toolchain.** MSVC 19.51.36256.0. That is a newer build than CX-01's 19.51.36231, so it is recorded here rather than assumed identical. CMake 4.3.1-msvc1.

**Private build `build/cx-net`** (preset `net`, `-B build/cx-net`):
- configure: exit 0, 90 s
- full build: 621/621, exit 0, **0 warnings** (`build_logs/build_cx-net_baseline.log`)
- ctest: **135/135 passed, 218.68 s** (`build_logs/ctest_cx-net_baseline.log`)

This is the CX-02 pre-change baseline.

**Disposition ledger.** `DISPOSITION.csv` generated from `CX01C_FINDINGS_INDEX.md`: 321 rows (P1 16 · P2 107 · P3 198), all `UNTRIAGED`.

**Card adjustments versus README (declared before landing):**
- A1b is now `app/kite_fetch_main.cpp` only. kite_update's precision fix lands inside A2, which replaces the `%g` line with `append_prices`.
- A2b (new) is `app/kite_fetch_main.cpp`:
  - month files are built in memory and replaced via `replace_file_checked`
  - `--force` refuses unless the window covers the whole month (C14-007)
  - token parsing is strict
  - the non-200 body echo is bounded to 512 bytes
  - HTTP status is printed as `%lld` (C14-016, part)
- `kPriceTextMax` is 48 with a stated proof. The format is `chars_format::fixed`: plain shortest produced `1e+05` for Rs 1,00,000.

## 2026-09-16 00:45–01:10 IST — Tier A, B1, B2, B6, B8 written; project audits caught two of my mistakes

**Tier A build and tests.** Target build in `build/cx-net`: exit 0, 0 warnings.
- `dataset_merge`: PASS on the first run. That includes `std::filesystem::rename` over an existing file on MSVC (it replaces) and a rename onto a non-empty directory (refused, temp removed).
- `order_intent` (B1): PASS.
- `price_text`: 1 FAIL, a true positive. `-0.0` came out as `-0` because MSVC `/fp:precise` folded `v == 0.0 ? 0.0 : v` back to `v`. Fixed with `std::fabs`.

**Project configure audits refused my code twice.** Both were fixed properly, not suppressed.
- `BoundsAudit`: `dataset_merge.hpp` `last < today ? last : today` has the clamp shape and no RULE 11 statement. It is a proof (no stored row post-dates today), and it is now annotated as one.
- `NamespaceAudit`: `altair::oms::kFnvPrime` collided with `features/registry.hpp`. The audit keys on the bare name. Renamed `kIntentFnvBasis`/`kIntentFnvPrime`, the more specific user taking the qualified name as that audit's rule says.

**Planted-violation evidence** (`prompts/cx02/evidence/`, compiled with `cl` into the scratchpad against `git show HEAD:` copies of the headers; no tracked file touched):

| Harness | Pre-change header (HEAD a34af5c) | Working-tree header |
|---|---|---|
| `replay_old_order_state` (C13-001, C13-002, late ack counted as refusal, crossed fill reopening a cancelled order) | 4/4 REPRODUCED | 0/4 |
| `replay_old_exit_ladder` (C13-010) | REPRODUCED: level 2,400,000 books 0 paise; the tick gives -22,500,000 | not run (not meaningful, see note) |
| `test_order_intent` [5] (C13-005) | carries a copy of the old first-occurrence reader and shows it reading lots 3 at 5,711,490 paise from the torn line | the strict grammar refuses it |
| `test_price_text` [2] (C14-001) | the old `%g` formatter fails the round-trip property on >1,000,000 of the swept prices | — |

Note on the exit-ladder harness: on the new header `level` still means the trigger level by design, so the defect-versus-fix comparison there is `fill_ref`, which exists only on the new header.

**Card changes.**
- B1 removes `drain_intents`/`DrainResult` from `order_intent.hpp` (moved to B2's `IntentDrainer`) instead of leaving a second drain API with the C13-008 defects.
- `oms/CMakeLists.txt` gains `intent_queue`.

**Desktop manifests, declared before any desktop edit** (per the brief; UI redesign, Models screens and `atlas*.hpp` stay out of scope):
- B3/B4 — `desktop/order_ticket.hpp`, `desktop/tests/test_order_ticket.cpp`
- B5 — `desktop/kill_switch.hpp`, `desktop/tests/test_kill_switch.cpp` (new), `desktop/CMakeLists.txt` (one test target only; no link change to `altair_desktop`, gate-3 allow-list untouched)
- B4b (C17-021) — `desktop/panels.hpp` (`emit_pick` plus a `block_instrument` passthrough to `Watchlist::block`), `desktop/tests/test_terminal.cpp`

## 2026-09-16 01:07–01:20 IST — full suite after A/B, desktop B3–B5, replay of the old drain

**Full rebuild and ctest (`build/cx-net`) after A, B1, B2, B6, B8:** 177 build steps, exit 0, **0 warnings**; **138/138 passed** in 225.73 s (the 135-test baseline plus `price_text`, `dataset_merge`, `intent_queue`). Logs: `build_logs/build_cx-net_AB2.log`, `ctest_cx-net_AB2.log`.

**Desktop B3/B4/B5** — targets `altair_order_ticket_test`, `altair_kill_switch_test`, `altair_terminal_test`, `altair_desktop`: exit 0, **0 warnings**. `desktop_order_ticket`, `desktop_kill_switch` and `desktop_terminal` all PASS. `altair_desktop` still links no `altair_oms`/`altair_broker`: the gate-3 audit ran at configure and passed.

**`replay_old_intent_drain`** against HEAD `oms/order_intent.hpp`: **4/4 REPRODUCED**.
- a missing file drained as an empty batch with no error
- intents from 2026-09-08 accepted with no expiry
- a re-drain returned the same ids
- an offset past a truncated file returned nothing with no error

Log `evidence/replay_old_intent_drain.log`. One slip: `Push-Location` to the scratchpad failed on the 8.3 path, so the replay ran in the repo root. It created and removed `cx02_replay_drain.jsonl` there; checked, nothing was left behind.

**Ledger:** batch 1 applied (11 FIXED_TESTED, including P1s C14-001, C13-001, C13-002, C13-010).

**C14-016 remainder:** `kite_quote_main.cpp` `%ld` changed to `%lld` with a cast, and `kite_ticker_main.cpp` token parsing is now strict with the uint32 bound refused. Awaiting a net build before disposition.

**Independent reviewer agent** launched on the Tier A/B diffs. It is read-only, runs no builds, and writes to `prompts/cx02/review/REVIEW_AB.md`.

## 2026-09-16 01:20–05:15 IST — Tier C1, B4b, and the first review

**Usage limit.** The session hit its limit at ~01:30 and reset at 05:10. The reviewer agent was killed mid-run by the same limit; it had already written 18 findings covering the four oms files, and a second reviewer was started to continue with `app/`, `desktop/` and `core/`.

**CX02-C1 (C02-002, C02-003, C02-004, correction C21-003).**
- `core/lockfree/snapshot_slots.hpp` (new): slot publication. kSlots = kReaders + 2; the writer never writes the published slot or one a reader holds; a reader pins, re-checks and copies, or retries at most `kSlotReadRetries` and is refused with `Contended`. The Dekker argument and the free-slot proof are in the header.
- `core/lockfree/tests/test_snapshot_slots.cpp` (new): interleavings forced deterministically through a `Hooks` template parameter — a reader's hook runs the writer (or a second reader) between "hold stored" and "published re-checked".
- `core/config/store.hpp`: `ConfigStore` now publishes through `SnapshotSlots`, refuses a version that does not rise (`ConfigStoreError::StaleVersion`, counted), and returns `ConfigRefresh{Contended, Unchanged, Updated}` — ordinal 0 is `Contended`, so a zeroed result never reads as "your copy is current".
- Call sites updated: `core/config/tests/test_config.cpp` (plus new C02-004 checks) and `app/main.cpp` (which publishes `store.version() + 1`, because `run_session` runs more than once per process and a fixed version 1 would now be refused as stale).
- Name clash avoided: `StoreError` is already `feed/tick_store.hpp`, so the config error type is `ConfigStoreError`.

**CX02-B4b (C17-021).** `desktop/panels.hpp`: a BLOCKED watchlist row sends an EMPTY spec (lot 0, tick 0, no exchange) instead of the master's, and `OrderTicket::set_contract` treats that as REVOKING any spec it holds — a row picked while resolvable and blocked afterwards was otherwise still requestable. A row the master resolves is marked Resolved before its spec is sent, so the Spec column stops saying "watch only" beside an orderable contract. `desktop/tests/test_terminal.cpp` gained section [4b].

**Gate 2 disclosure, which the reviewer was right to raise (R-AB-007).** CX02-B2 shipped an `IntentBatch`/`Quarantined` shape different from the one frozen in this directory's README: `QuarantineKind` replaced the per-kind counters, and `save_drainer_state`/`load_drainer_state`/`NoState`/`CorruptState`/`StateWriteFailed` are new API. That change was not recorded when it landed. It is recorded here, and CX02-B2b restores the per-kind counts the frozen contract required.

**Disposition corrected (R-AB-005).** C13-003 moves from FIXED_TESTED to DEFERRED: the change is documentation only, `ts < o.last_event` is byte-identical to HEAD, and test 12 passes on HEAD for an unrelated reason. Dependency: the CX-03 adapter that actually stamps receipt time.

**Full rebuild and ctest after C1 and B4b:** 195 build steps, exit 0, **0 warnings**; **140/140 passed** in 220.08 s (`build_logs/build_cx-net_C1.log`, `ctest_cx-net_C1.log`). `app_selftest` passes with the new publish/refresh contract.

## 2026-09-16 05:20 IST — answering the review

**Landed (pending the next build):**
- **B6b** (`oms/order_state.hpp`, its test): a late fill out of `Expired` is kept exactly as one out of `Cancelled` (R-AB-001 — a DAY order whose lapse overtakes its fill report used to lose 75 units); answers to cancel/amend requests are duplicates from any state that could have sent one, and refused only from `Unset`/`PendingNew` (R-AB-002, R-AB-003); `CancelReject` with no recorded resume state is refused rather than guessing (R-AB-004). CancelAck is deliberately NOT in the absorbed set: unsolicited, it is news about the order and must escalate. One existing assertion was inverted by this change and updated with the reason.
- **B2b** (`oms/intent_queue.hpp`, its test): per-kind refusal counts beside the capped list (R-AB-007); `rollback` for a failed state save (R-AB-008); `Unreadable` when `exists()` itself errors (R-AB-009); `IntentDrainer::create` refuses a livelocking policy (R-AB-010); a trailing `\r` no longer counts toward the length bound (R-AB-011); the dedupe identity and eviction rule stated (R-AB-012); the state file's close checked and the durability and single-drainer limits recorded (R-AB-013); test gaps closed (R-AB-014).
- **B8b** (`oms/exit_ladder.hpp`): `evaluate_exit` takes the `Touch`; a crossing exit is valued at the side that gets hit — bid for a long, ask for a short — and falls back to the tick only as a stated assumption (R-AB-015). `square_off_limit` prices a protective exit THROUGH the touch by a band, because a limit at `fill_ref` repeats C13-010 one print later (R-AB-016). A target touched exactly carries `fill_certain = false` (R-AB-017).

**Dispositions.** C02-002 and C02-003 are DEFERRED, not fixed: `ConfigStore` no longer uses the seqlock, but `SeqlockSnapshot` and its P0-06b test still exercise the racy copy and the unbounded spin. Card **CX02-C1b** removes or restricts the primitive; until then the racy code is still in the tree and the finding is not closed.

## 2026-09-16 05:55 IST — the ASan configure caught an ODR collision a green build could not

Configuring `build/cx-asan` (a private directory, `--preset asan`) failed the project's own namespace audit:

```
NAMESPACE COLLISION: altair::Touch
    oms/exit_ladder.hpp
    strategies/parity.hpp
```

CX02-B8b added `struct Touch` at namespace scope; `strategies/parity.hpp:80` has had one since P5. Everything is in `namespace altair`, so that is an ODR violation waiting for the first translation unit to include both.

**Why the passing build did not catch it.** The audit runs at CONFIGURE time. `build/cx-net` last reconfigured when a `CMakeLists.txt` changed (C1's `core/lockfree/CMakeLists.txt`); every build since has been header and source edits only, so the audit never re-ran while B8b, B6b, B2b and Tier D landed. A green `ctest` says nothing about it. Worth remembering: after adding any namespace-scope name, a fresh configure is the check, not the build.

Renamed to `ExitTouch` — the audit's own rule is that the more specific user takes the qualified name, and the exit ladder is more specific than parity scanning. `oms/exit_ladder.hpp` (5 sites) and `oms/tests/test_exit_ladder.cpp` (11 sites); it has no other users.

**After the rename, `build/cx-asan` configures clean** (`build_logs/configure_cx-asan_2.log`):

```
-- altair: namespace audit clean -- 641 names, no clashes
-- altair: bounds audit clean -- 9 clamp site(s), all stated
=== END configure exit=0
```

The ASan BUILD is deliberately not started yet: four review fix cards (B4c, B4d, B3b, A2c, C1c) are still to land, and a full ASan build of this tree took 2,434 s in CX-01. It runs once the code is final, and its result — pass or fail — goes in the Phase-2 closure. ASan is memory evidence, never race evidence; TSan still needs a Linux box this project does not have.

## 2026-09-16 05:50 IST — Tier D landed, and the two ways its first run failed

**CX02-D1** (`core/types/units.hpp`, `test_units.cpp`) — `round_to_tick` checks the final multiply BEFORE it happens, so rounding near `Price::max()`/`min()` refuses with `Overflow` instead of wrapping. P0-01's item 9 never required it (C21-001, correction card in `CORRECTIONS.md`); the code defect is C01-001.

**CX02-D2** (`core/mem/pool.hpp`, `core/mem/arena.hpp`, `test_arena_pool.cpp`) — releasing an OWNED block that is already free is refused and counted in `double_releases()` instead of self-looping the free list and underflowing `in_use_`; `Pool` and `Arena` are move-only, because a copy hands the same blocks out twice (C21-002, C01-004).

**The first run of these two failed, and both failures were mine.**

1. `types` FAILED on my own assertion, not on the code. I asserted that a Nearest rounding of `int64 max` on a 5-tick overflows. It does not: max is 2 (mod 5), so Nearest rounds DOWN. Nearest can only step past the top when the remainder is at least half the tick — max is 3 (mod 4), so the tick-4 case is the real one. The test now asserts both: tick 4 overflows, tick 5 answers.
2. `arena_pool` TIMED OUT at 600 s — a real defect in my tag design, caught by the existing benchmark rather than by a new test. `acquire()` left the free tag in the block, so a block that had been round-tripped once still looked free; every release then matched the tag and paid for a free-list walk to disprove it. Against the 1,000,000-pair benchmark over 131,072 blocks that is on the order of 10^11 pointer chases. `acquire()` now clears the tag in one store, which is what keeps `release()` O(1).

The lesson is the one PROTOCOL.md already states: the benchmark is part of the evidence, not decoration. A correctness fix that silently made a hot path O(n) passed every functional check.

## 2026-09-16 05:40 IST — the second review, and the manifests it caught me not declaring

**Result: 50 findings, 0 P1, 19 P2, 31 P3** (`review/REVIEW_AB.md`). The reviewer re-derived the planted-violation evidence against the code rather than trusting this log, and confirms the four audit P1s CX-02 targeted — C14-001, C13-001, C13-002, C13-010, C17-010 — are genuinely closed. It also confirmed the `SnapshotSlots` publication proof by trying to break it, and recorded why the ABA case is harmless.

**Three P2s are defects this phase introduced, and they are the first fixes queued:**
- R-AB-038 — the halt panel's CLEAR path asks for NO reason (`reason_out == nullptr`), inverting what the file's header and its own on-screen text both state. Pre-existing in P11Q-05c, and B5 touched that function without fixing it.
- R-AB-039 — B5's own 2-second poll overwrites the "could not write the request" notice within two seconds. C17-014 was "the panel never refreshes"; the fix made it refresh over the error.
- R-AB-042 — B4 stamps `at` before the modal confirmation, so B2's TTL would quarantine a confirmed request as `Expired` while the ticket logs PENDING.

**And one of my closures used the wrong mechanism:** R-AB-041 — B4b closed C17-021 by calling `Watchlist::resolve` from `emit_pick`, promoting a row to `Resolved` on the strength of the UI's own `data/instruments.csv`. `watchlist.hpp` says an unresolved row is "NEVER SILENTLY PROMOTED", and `Resolved` means the point-in-time spec store, which `desktop/` cannot even link. CX02-B4d replaces it with an honest `SpecState::FromMaster`.

**Manifest and contract drift the reviewer caught, declared now rather than argued away:**
- CX02-C1's declared manifest was `core/lockfree/snapshot_slots.hpp`, its test, `core/config/store.hpp`, `core/CMakeLists.txt`. It ALSO changed `core/config/tests/test_config.cpp` (+74 lines) and `core/lockfree/CMakeLists.txt`, and the test file appears in no manifest (R-AB-024).
- `app/main.cpp` was changed because of C1 — a card whose manifest is `core/` only, and a manifest may not span two component directories (CLAUDE.md). It needed its own card; it did not get one (R-AB-037).
- `app/kite_quote_main.cpp` and `app/kite_ticker_main.cpp` were edited as "the C14-016 remainder" with no card declared (R-AB-037).
- `PriceTextError` gained `OutOfRange` in the MIDDLE of the enum, shifting `BufferTooSmall`'s ordinal, against this phase's own stated convention of appending (R-AB-034).
- Named acceptance tests that exist only as unnamed `check()` lines, so `ctest -R <name>` finds nothing: four in A1a/A2, one in C1, and B4's `unset_limit_refused`, which does not exist at all (R-AB-024, R-AB-034, R-AB-044).

The fix cards — B5b, B4c, B4d, A2c, C1c, B3b — are in `README.md` with their manifests and named tests.

**CX02-A3 — the dataset precision inventory is done** (`DATASET_PRECISION_INVENTORY.md`). Read-only scan of 4,208,576 rows in 1,737 files under `dataset/`; nothing was written there and no refetch was made.

- 2,467,150 rows are *at risk* (a price at or above 10,000, where six significant digits cannot carry paise). 1,237,797 prove themselves exact; 1,229,353 are consistent with rounding.
- **BANKNIFTY spot shows no evidence of damage.** A two-decimal source yields a value with a zero in the hundredths about 10% of the time, and that is written with one decimal by an exact writer too. BANKNIFTY 1m is 11.8% — the chance rate.
- **NIFTY is not chance.** All 810,434 at-risk rows in `spot/nifty/1m` — and every at-risk row in 5m, 15m, 60m and all `fut/nifty` series — carry at most one decimal, with not one proving itself exact. Either those series went through the lossy writer, or Kite reported NIFTY with one decimal. The bytes are identical either way, so the scan cannot decide it. `spot/nifty/1d` (2,079 exact against 103) shows the daily series did not come through the same path.
- **Settling it is a user action:** after a login, fetch one recent NIFTY minute candle and compare the raw API body against the stored row. No refetch should be run before that answer, and none is run by anything here.

## Resume checkpoint — 2026-09-21

The user's latest request explicitly authorized using Cline. A plan-only CLI run
was attempted; the normal local state database was read-only, and a temp-state
retry then hit `ConnectionRefused` from the configured provider with 0 model
tokens. Cline produced no analysis or code patch. Do not claim Cline reviewed
anything.

Since the 2026-09-19 checkpoint, focused additions include B7 order timeout /
snapshot-resolution rules and safer reconciliation arithmetic/input validation;
D4 partial-TCP-frame preservation; E4 early, overflow-safe one-shot canary loss
handling and honest monitor severity; and E5 explicit `ALTAIR_STRICT_INVARIANTS=0`
definition. The full `build/codex-cx06` build exits 0 and CTest passes 141/141
in 234.35 s. `build/cx-net` price_bus test passes 1/1. The strict-off Release
negative build fails at the intended conservation guard. Production cost config
remains UNVERIFIED.

One failed full-suite attempt set `QT_QPA_PLATFORM=offscreen` globally: five
Windows GUI tests timed out. All five pass when rerun with the correct/default
Windows platform, and the full 141-test suite passes with no global override.

Still open: Linux TSan/UBSan, hot-path latency/allocation proof, full current-tree
line coverage, canonical paper engine integration, FYERS, calibrated tick-level
model evidence, live option data, integrated terminal acceptance and release
go/no-go. No broker operation or reference-directory access was performed.
