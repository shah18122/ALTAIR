# Prompt execution queue

Updated: 2026-09-25

This is the compact, current register for prompt execution and cleanup. Completed
historical prompt bodies are removed after their implementation and focused tests
are verified. Their source remains recoverable from the commit recorded in
`final.md` §21 and from the immutable CX-01 audit hashes. `final.md` §18 remains
the authoritative product backlog; this file records prompt-file disposition.

## Rules

1. Process cards in dependency order.
2. Read the card, implementation, tests, CMake registration, ledger entry, and
   later correction records before declaring it complete.
3. Run the focused target and test before removing a completed card.
4. Do not remove an in-flight correction, blocked specification, or card owned by
   another concurrent agent.
5. Historical audit manifests are snapshots. A removed path in those manifests
   means the file existed when the audit was taken, not that it must still exist.

## Verified and removed

| Card | Deliverable | Completion evidence | Verification on 2026-09-24 |
|---|---|---|---|
| P0-01 + c1 | Strong dimensional units, exact paise arithmetic and tick rounding | `final.md` §21, commit `eeab54b`; later overflow correction in `prompts/cx02/CORRECTIONS.md` | `types` passed |
| P0-02 | Affine timestamps, durations and IST helpers | `final.md` §21, commit `03c3062` | `time` passed |
| P0-03 | Invariant-TSC clock, calibration and uncertainty | `final.md` §21, commit `6d9c8f1` | `tsc_clock` passed |
| P0-04 | Exchange timestamp normalisation and plausibility gate | `final.md` §21, commit `016c390` | `exchange_ts` passed |
| P0-05a | OS page allocation and honest huge-page reporting | `final.md` §21, commit `bac0264` | `page_alloc` passed |
| P0-05b | Arena and pool allocators | `final.md` §21, commit `066a679`; double-release/copy correction in `prompts/cx02/CORRECTIONS.md` | `arena_pool` passed |
| P0-06a | Bounded SPSC ring | `final.md` §21, commit `f75717e` | `spsc_ring` passed; ordering still requires supported Linux/ARM race evidence |
| P0-06b | Bounded MPSC ring plus snapshot publication | `final.md` §21, commit `0e2fb84`; original racy seqlock superseded by `core/lockfree/snapshot_slots.hpp` under `prompts/cx02/CORRECTIONS.md` | `mpsc_ring` and `snapshot_slots` passed; TSan evidence remains open |

Removed source prompt paths:

- `prompts/P0-01_core_types.md`
- `prompts/P0-01c1_correction.md`
- `prompts/P0-02_core_time_timestamp.md`
- `prompts/P0-03_core_time_tsc_clock.md`
- `prompts/P0-04_core_time_exchange_ts.md`
- `prompts/P0-05a_core_mem_page_alloc.md`
- `prompts/P0-05b_core_mem_arena_pool.md`
- `prompts/P0-06a_core_lockfree_spsc_ring.md`
- `prompts/P0-06b_core_lockfree_mpsc_seqlock.md`

## In flight — preserve

- `P6-01_ATLAS_RECONCILIATION.md` — retained as the Atlas evidence document;
  P6-02 stable-ID routing is now complete, but later model cards still consume
  the reconciliation evidence.

**Closed and removed 2026-09-24, records in `final.md`:**

| Removed path | Passed how | Record |
|---|---|---|
| `P0-02c1_correction.md` | applied; gates 7 and 5b now PASS; 148/148, zero warnings | §18.12 |
| `DEBT-01_format_paise_card.md` | four exact test names green; manifest exactly three files; zero warnings | §18.12 |
| `P6-01c1_correction.md` | §7–§11 appended; 85 IDs / 14 splits / 31 page corrections | §18.13 |

**Closed and removed 2026-09-25, records in `final.md`:** the P0-02 broker contract
was closed and P2-01..P2-03 landed, so their frozen cards were deleted. Each
deliverable exists on disk, is registered in CMake, and its focused test was run
directly from `build/default` (0 failures) before removal.

| Removed path | Deliverable verified | Focused test (green) |
|---|---|---|
| `P0-02_BROKER_CONTRACT.md` | P0-02 closed on 2026-09-25 (funds v2, log events, publication, OMS permit) | `broker_state`, `broker_log` |
| `P0-02c_broker_positions.md` | `core/types/broker_positions.hpp` (contract of record) | `broker_positions` |
| `P0-02d_FUNDS_PAYLOAD.md` | `core/types/broker_state.hpp` `kSchemaVersion = 2`, four optional paise fields | `broker_state` |
| `P0-02e_BROKER_LOG_EVENT.md` | `core/types/broker_log.hpp` + `core/types/tests/test_broker_log.cpp` | `broker_log` |
| `P0-02f_OMS_BROKER_GATE.md` | `oms/broker_dispatch_gate.hpp` + `oms/tests/test_broker_dispatch_gate.cpp` | `broker_dispatch_gate` |
| `P0-02g_BROKER_PUBLICATION.md` | `broker/state_store.hpp` + `broker/tests/test_state_store.cpp` | `broker_state_store` |
| `P2-01_SECURE_CREDENTIAL_STORE.md` | `broker/credential_store.hpp/.cpp` + test | `credential_store` |
| `P2-02_ASYNC_HELPER_COMMANDS.md` | `desktop/helper_process.hpp` + child + test | `desktop_helper_process` |
| `P2-03_CREDENTIAL_SETUP_UI.md` | `app/credential_helper_main.cpp`, `desktop/credential_setup.hpp` + test | `desktop_credential_setup` |


These files are active contracts/corrections and may be owned by concurrent
agents. Do not edit or remove them during historical-card cleanup.

## Completed cards awaiting sequential verification and removal

**Empty as of 2026-09-24 — the 35 cards listed here are removed.** One method
for all of them: the §21 ledger row says **DONE** with a commit hash; every path
in the card's file manifest exists on disk (module-relative entries resolved
against the module); and the module's test target is **Passed** in the 148/148
run recorded in `final.md` §20.1. Where a card had a later correction, the
correction was checked for *application* first, not assumed. Bodies remain
recoverable from the commit that added each one:
`git log --diff-filter=A -- prompts/<file>`.

| Card | Module | Test (all PASS) |
|---|---|---|
| P0-07 | `core/log` | `binlog` |
| P0-08a | `core/config` | `config` |
| P0-09a | `core/invariant` | `conservation` |
| P0-09b | `feed/replay` | `replay` |
| P0-10 | `app/` | `app_selftest`, `app_instruments` |
| P1-01 | `instruments/spec_store` | `spec_store` |
| P1-04 | `instruments/kite_dump` | `kite_dump` |
| P1-06 | `instruments/reconcile` | `reconcile` |
| P1-08a | `instruments/snapshot` | `snapshot` |
| P1-09 | `instruments/universe` | `universe` |
| P2-01 | `feed/tick` | `tick` |
| P2-02 | `feed/kite_decoder` | `kite_decoder` |
| P2-04 | `feed/normaliser` | `normaliser` |
| P2-05 | `feed/failover` | `failover` |
| P2-06 | `feed/tick_store` | `tick_store` |
| P2-08 | `book/l2_book` | `l2_book` |
| P2-09a | `book/microstructure` | `microstructure` |
| P2-09b | `book/flow` | `flow` |
| P3-01 | `analytics/greeks` | `greeks` |
| P3-02 | `analytics/greeks2` | `greeks2` |
| P3-03 | `analytics/american` | `american` |
| P3-04 | `analytics/iv` | `iv` |
| P3-05a | `analytics/svi` | `svi` |
| P3-05b | `analytics/svi_fit` | `svi_fit` |
| P3-06a | `analytics/rolling` | `rolling` |
| P3-06b | `analytics/ewma` | `ewma` |
| P3-06c | `analytics/hurst` | `hurst` |
| P3-07 | `analytics/derivatives` | `derivatives` |
| P3-08 | `analytics/vix` | `vix` |
| P3-09 | `risk/cost` | `cost` |
| P3-10 | `risk/slippage` | `slippage` |
| P4-01 | `risk/sizing` | `sizing` |
| P4-02 | `risk/limits` | `limits` |
| P4-04 | `oms/order_state` | `order_state` |
| P7-00 | `research/tools/ingest_bars.py` | no test — a once-a-day Python tool; `research_pipeline` covers the pipeline |

**P0-07**, the item the previous "Next" named, is in this batch: its four
manifest files exist, `binlog` passed, and its ledger row is DONE at `78545fa`.

**Two things the sweep found, recorded rather than waived.**

1. **The `cx02/CORRECTIONS.md` card for P0-06b/P0-08a names five acceptance
   tests that "must be written exactly". They are not there by those names.**
   The behaviour is: `writer_never_writes_a_held_slot`,
   `contended_read_is_refused_not_torn`,
   `a_free_slot_exists_with_every_hold_pinned`,
   `reader_pins_between_the_writers_scan_and_its_write` and
   `a_reader_rechecks_after_its_unheld_slot_is_recycled` in
   `core/lockfree/tests/test_snapshot_slots.cpp`, with the non-rising-version
   case asserted inside `test_config_store_publish_and_refresh`
   (`core/config/tests/test_config.cpp:255-261`, comment `CX02-C1 (C02-004)`).
   The correction is applied and green; its *naming* requirement was not
   followed. A later reader searching for
   `test_publish_refuses_non_rising_version` finds nothing and could wrongly
   conclude the correction never landed.
2. **Audit findings outlive the cards they name.** They live in
   `prompts/audit/cx01c/`, not in the prompts, so removing a prompt neither
   resolves nor hides one. Open for this batch: C22-001 (four blocked cards'
   stale status lines), C22-002 (`P1-06`'s contract says `Verdict`, the shipped
   enum is named differently), C22-003 (**no C++ code reads any key from
   `config/altair.toml`**), C22-005 and C22-006.

## Blocked or partial — preserve until resolved

- `P1-02a_nse_fo_master.md` — sample-file blocker
- `P1-02b_nse_mktlots.md` — sample-file blocker
- `P1-02c_nse_equity_master.md` — sample-file blocker
- `P1-03a_bse_masters.md` — sample-file blocker
- `P1-08b_master_fetch.md` — dependency blocker stated by card
- `P2-10_kite_auth.md` — a/b complete, c blocked according to its header
- `PHASE13_PLAN.md` — contains the explicitly blocked live VRP card

## Coordination, evidence and planning documents — preserve

- `CODEX_COMPLETION_PLAN.md`
- `CODEX_PROJECT_MEMORY.md`
- `CX03_IMPLEMENTATION_PLAN.md`
- `P0-01_BASELINE.md`
- `P0-03_NAV_TERMINAL_LAYOUT.md`
- `P0-P4_RESUME_CHECKPOINT.md`
- `P4-04_GETS_TABLE_SPEC.md`
- `ROADMAP_GAP.md`
- `P7-06_MTBT_card.md` and `P7-06_MTBT_FEED_SPEC.md` — the card and its delivered
  specification (2026-09-25). The spec is the deliverable of record; keep both.
- `P2_P3_COMPLETION.md` — Phase 2/3 implementation and operational-gate record
- `DEBT-01_format_paise_card.md` — completed and passed 2026-09-24, **removed**;
  record in `final.md` §18.12 (`desktop/format.hpp`, `desktop/tests/test_format.cpp`,
  one `desktop/CMakeLists.txt` block)
- `audit/`, `cx02/`, and `cx06/` — evidence/correction directories, not executable card queue entries

## Next

**The register is empty: no completed card is awaiting verification.** The
2026-09-24 items are closed — P0-02c1 and DEBT-01 implemented and green, P6-01's
correction applied with its one recorded defect (`final.md` §18.13), and the 35
historical prompts verified and removed in the batch above. `desktop_position_table`
was re-run against the post-P0-02c1 header, closing §18.10's action item.

The 2026-09-25 batch above is also closed: P0-02 was completed and its contract,
position, funds, log-event, dispatch-gate and publication cards removed, and
P2-01..P2-03 (secure storage, async helper commands, in-app credential setup)
removed after their focused tests re-ran green.

What is left in `prompts/` is exactly what cannot be processed yet: the seven
blocked cards above, the retained Atlas evidence document
(`P6-01_ATLAS_RECONCILIATION.md`), the coordination/evidence documents, and the
P7-06 pair (card + delivered spec; no decoder is written and the spec's own
recommendation is NO-GO until a licence and a captured reference stream exist).
Nothing in this directory is executable work.


The next engineering work is in `final.md` §18, not here:

1. **P7-03 / P7-04** — complete and verified in `strategies/basis.hpp`. P7-03
   gates available size, measured impact/latency allowance, inventory, permitted
   short capacity and explicit settlement eligibility. P7-04 uses direction-specific
   bid/ask touches, explicit premium/discount classification and priced reverse-
   carry borrow cost. `strategies_basis` and its consumers pass. No further
   Phase 7 strategy card is pending here.
2. **P2-09 / P2-11** — wire bounded polling/publication into the service, then drive verified
   pills from evidence. P2-07 now includes the dry-run-by-default FYERS account
   helper and a visible Brokers-page account tab; P2-08 now gates the existing
   Kite snapshot helper through the same typed contract. The bounded shared contract and no-transport parser plus the
   P2-09 publisher now binds session epochs, immutable snapshots and stale/
   revoked evidence. P2-01
   through P2-06 now provide OS-vault storage, bounded asynchronous stdin IPC,
   in-app Save/Replace/Delete/Connect forms, exact OAuth/profile verification,
   and the Pair Trading UI foundation; focused OAuth verification is 5/5
   green. Continuous provider polling still needs the networked service adapter.
3. **P3-02 / P3-03 / P3-05** — local implementation is complete and focused
   verification is green. Remaining operational gates are explicit and cannot be
   closed on this host: official FYERS SDK + entitled callback transport (P3-02),
   app-level live supervisor/publication (P3-03), and an authenticated entitled
   account's real 1m/5m discrepancy report (P3-05). No live claim is inferred.
4. **FYERS local facilities** — auth helpers, all five read-only account GETs,
   typed snapshots, history parser/fetcher, decoder, bounded adapter, headless
   router, desktop handoff and dry-run helpers are inventoried and exercised.
   `altair_fyers_login --help` was fixed to work without credentials. No local
   FYERS facility is silently omitted; only the official live SDK/account paths
   remain operationally gated.
5. Open audit findings with no card yet: **C22-003 (no C++ code reads any key
   from `config/altair.toml`)**, C22-002 (`P1-06`'s `Verdict` naming drift),
   C22-005 and C22-006.
6. **Phase 8 stays untouched** — wholly blocked until Phases 2, 3 and 7 are live.

7. **FYERS webhook** — local receiver and offline validation are complete in
   `app/fyers_webhook_main.cpp` and `broker/fyers_webhook.hpp`; production
   activation remains gated on verified FYERS signature/header documentation,
   TLS reverse-proxy deployment, DNS/IP ownership and a generated secret.

P0-02d/e/f plus producer publication and desktop consumption were completed on
2026-09-25. Focused tests passed: broker_state, broker_log, broker_state_store,
broker_dispatch_gate and desktop_broker_status. See `final.md`'s
post-consolidation execution record. Live submission remains disabled.
