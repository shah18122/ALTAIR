# CX-01 → Phase 2 — dependency-ordered task recommendations

Reader: claude-lead · Revision: 2026-09-15 ~12:45 IST (**FINAL for Phase 1**)
- All 22 review groups have reported, and the later groups added findings without changing the tier order below. Tier 0 (CX01-R) is done.
- Additional rule from G21: every fix to a defect that originates in a task card (C21-001…004: `round_to_tick`, Pool double release, seqlock, conservation) needs a correction card that **amends the original P0 card's contract**, so a re-implementation cannot reintroduce it. · Baseline HEAD `a34af5c5c9fa8c7498e8cc4df00907548a41e1a5`

**Nothing here has been started.** Each card follows `prompts/PROTOCOL.md`: at most 4 files, frozen interface contract, named acceptance tests, all 8 gates. Finding IDs refer to `CX01C_FINDINGS.md` and the group files. Order is by dependency. A later tier must not start before the tiers it depends on.

## Tier 0 — finish Phase 1 (blocking the Phase 1 sign-off)

| Card | Work | Depends on |
|---|---|---|
| CX01-R | Complete the remaining review groups (see `CX01C_STATUS.md`), re-run coverage merge and index, finalise FINDINGS, ARCHITECTURE and Atlas/training sections | — |

## Tier A — research data integrity (independent; also unblocks the user's data request)

| Card | Files | Change | Acceptance tests | Findings |
|---|---|---|---|---|
| CX02-A1 | `app/kite_update_main.cpp`, `app/kite_fetch_main.cpp` | Write prices exactly (integer paise to a fixed 2-decimal rupee string; no `%g`, no default stream precision). | `test_update_writes_23398_15_exactly`, `test_fetch_writes_bankifty_56606_65_exactly` | C14-001 ✔ |
| CX02-A2 | `app/kite_update_main.cpp` | Checked temp→rename; per-series coverage check before replace. | `test_update_failed_rename_leaves_original`, `test_update_refuses_short_coverage` | C14-005/006 |
| CX02-A3 | *(operational, after A1–A2)* | Smit logs in to Kite, runs `altair_kite_update`, re-fetches the series stored at 6-digit precision (record which), verifies round-trip. **Not a code card.** | row check against a Kite historical response | user request "fetch data till yesterday" |

## Tier B — order trust boundary and lifecycle (must precede any intent drainer or OMS assembly)

| Card | Files | Change | Acceptance tests | Findings |
|---|---|---|---|---|
| CX02-B1 | `oms/order_intent.hpp`, `oms/tests/test_order_intent.cpp` | Strict single-object line grammar: no duplicate or nested keys, checked int64 parse, token range `≤ UINT32_MAX`, refuse a line containing a second object. | `test_torn_prefix_plus_next_record_is_rejected`, `test_huge_digits_refused`, `test_token_above_u32_refused`, `test_duplicate_key_refused` | C13-005 ✔, C13-006, C13-007 |
| CX02-B2 | `desktop/order_ticket.hpp`, `desktop/tests/test_order_ticket.cpp` | Before appending, ensure the file ends in `\n` (or refuse); check write+flush; report failure. | `test_append_after_torn_tail_starts_new_line`, `test_failed_flush_not_reported_pending` | C13-005, C17-011 |
| CX02-B3 | `desktop/order_ticket.hpp`, `desktop/tests/test_order_ticket.cpp` | Confirmation shows side, lots, symbol, order type, limit price (₹), product, validity, exchange. Limit is cleared on contract change and must be entered. Watch-only rows are not requestable. | `test_confirmation_text_contains_price_type_product`, `test_contract_change_clears_limit`, `test_watch_only_row_refused` | C17-010 ✔, C17-021 |
| CX02-B4 | `oms/order_state.hpp`, `oms/tests/test_order_state.cpp` | Separate `CancelReject`/`ReplaceReject` from order `Reject`; a partial fill keeps PendingCancel/PendingReplace. | `test_partial_fill_in_pending_cancel_then_cancel_ack`, `test_cancel_reject_then_fill` | C13-001 ✔, C13-002 ✔ |
| CX02-B5 | `oms/order_state.hpp` (+ contract design by lead) | Add an `Unknown` (outcome-uncertain) state with a mandatory client order id; define reconciliation-only exits from it; **no retry path to another broker**. | `test_placement_timeout_enters_unknown`, `test_unknown_only_resolved_by_reconcile` | C13-004 |
| CX02-B6 | `oms/exit_ladder.hpp`, `oms/tests/test_exit_ladder.cpp` | Gap exit level = `min(stop, price)` (long) / `max` (short); correct the test's fictional ₹1.5 lakh figure. | `test_gap_below_both_stops_level_at_or_below_price` | C13-010 ✔ |
| CX02-B7 | `oms/order_intent.hpp` | Queue hygiene: intent expiry (`at` + TTL), id dedupe, file identity with offset, bounded batch. | `test_expired_intent_refused`, `test_duplicate_id_refused`, `test_offset_on_replaced_file_refused` | C13-008, C13-009 |
| CX02-B8 | `desktop/kill_switch.hpp` | A malformed or unreadable halt file shows UNKNOWN (not "no halt"); check the write; refresh from disk. | `test_malformed_halt_file_reported_unknown`, `test_halt_write_failure_reported` | C17-014 |

## Tier C — shared state and concurrency (must precede engine assembly across threads)

| Card | Files | Change | Acceptance tests | Findings |
|---|---|---|---|---|
| CX02-C1 | `core/lockfree/seqlock.hpp` (+ tests) | Replace plain-payload copy with a standards-correct publication (immutable preallocated snapshots behind an atomic index, owner-thread install, or annotated byte copy). Bounded retry with an explicit result. | `test_reader_paused_across_two_publishes_is_coherent`, `test_load_bounded_attempts` | C02-002 ✔, C02-003 ✔ |
| CX02-C2 | `core/config/store.hpp`, `core/config/config.hpp` | Refuse non-increasing version publish; handle carries key hash or generation and is verified. | `test_same_version_different_content_refused`, `test_stale_handle_after_refresh_refused` | C02-004, C02-005 |
| CX02-C3 | `models/serving.hpp`, `models/tests/test_serving.cpp` | Reader-lifetime protection (writer defers or refuses reuse while a reader holds the slot); one coherent `{prediction, digest, feature_version, generation}` record; remove SKIP→PASS. | `test_slot_not_reused_while_reader_inferring` (latched), `test_forecast_record_coherent` | C08-001, C08-002, C08-023 |
| CX02-C4 | *(environment decision — Smit)* | Provide a Linux GCC/Clang environment (WSL needs admin + reboot per LEDGER) to run TSan and the `-Wall -Wextra` gate; first check V-03 portability. | TSan run of `core/lockfree`, `core/config`, `models/serving` tests | V-03, B6 |

## Tier D — bounds that must refuse (rule 11)

| Card | Files | Findings |
|---|---|---|
| CX02-D1 | `features/vector.hpp`, `features/book_flow.hpp` (+ tests) — cap `n` or refuse; clear and age-gate slots | C05-001, C05-002 |
| CX02-D2 | `book/l2_book.hpp`, `feed/normaliser.hpp` (+ tests) — refuse level counts > 5; a stale-flagged depth never replaces a fresher book | C03-005, C03-001 |
| CX02-D3 | `models/horizon_eval.hpp`, `models/labels.hpp`, `models/registry.hpp` — `lags ≥ 16` refused; uniqueness cap refused or counted; names refused not truncated | C08-010, C08-014, C08-015 |
| CX02-D4 | `server/price_bus.hpp` (+ test) — never drop a partly written head frame | C15-001 ✔ |
| CX02-D5 | `book/flow.hpp` — VPIN clamp counted/refused | C03-010 |

## Tier E — safety nets that cannot fire

| Card | Files | Findings |
|---|---|---|
| CX02-E1 | design card (lead) + `core/invariant/conservation.hpp` — conservation against an **independent** source (broker fills or OMS record); refused fills latch or escalate | C02-001 ✔, C12-002 |
| CX02-E2 | `risk/charges_toml.cpp`, `risk/cost.hpp` — enforce `[safety] block_on_unverified_schedule`, or delete the setting so the file stops promising it | C12-003 ✔ |
| CX02-E3 | new bench target + CMake — latency budget misses fail a dedicated benchmark gate (best-of-N) rather than printing in unit tests | C02-006 |
| CX02-E4 | `app/monitor.hpp`, `flagging/deploy.hpp` — "never observed" ranks worse than Ok; canary loss limit fires regardless of `min_observations`; `open()` not re-callable | C15-014, C05-012 |
| CX02-E5 | `core/invariant/conservation.hpp`, root `CMakeLists.txt` — `ALTAIR_STRICT_INVARIANTS` guard that can actually fire | C02-011 |

## Tier F — statistical honesty (before any positive result is believed)

| Card | Files | Findings |
|---|---|---|
| CX02-F1 | `models/forecast_scorecard.hpp`, `models/spot_forecast.hpp` — HAC/Newey-West SE for overlapping horizons; band width fit out of sample | C08-007, C08-009, C08-012 |
| CX02-F2 | `flagging/drift.hpp` — ADWIN range normalisation and multiple-testing control | C05-011 |

## Tier G — desktop responsiveness and the user's Models GUI request (parallel with B–F; integrated acceptance needs CX-03)

| Card | Files | Change | Findings / request |
|---|---|---|---|
| CX06-G1 | `desktop/main_window.hpp`, `desktop/quant_pages.hpp` (split if >4 files) | Run fits, reports and subprocess waits on a worker with cancellation; disable re-entry; Halt tab always responsive | C17-001 |
| CX06-G2 | `desktop/option_chain.hpp` | Incremental model/view updates; coalesce repaint; no per-frame item recreation | C17-007 |
| CX06-G3 | `desktop/terminal.hpp`, `desktop/option_chain.hpp` | LIVE/REPLAY/STALE per token; disconnect clears LIVE | C17-008 |
| **CX06-G4** | **Models page (user request "i want gui in models")** — new page files only; **`desktop/atlas.hpp` and `desktop/atlas_data.hpp` unchanged** (user: "dont change model atlas") | Per-model panel: what it is, where it is trained (refit per run, nothing persisted), data used, last verdict vs random walk **net of cost**, uncertainty; runs on the CX06-G1 worker. **Scope to confirm with Smit before the card is written**: which models, and whether "GUI in models" means inspecting models or training them from the UI | user request 2026-09-14 |

## Tier H — canonical engine assembly (CX-03; only after B, C, D, E)

1. Replace placeholder specs in live decode with reconciled point-in-time specs (G03/G15 leads).
2. One admitted-event path: decoder → normaliser → book → features, shared by live and recorded ticks. A tick-store reader cursor and a depth recorder (C03-003/004) make recorded replay possible.
3. Synchronous pre-trade risk (`check_order`, sizing, cost) in front of any intent drain (C12-001).
4. Paper execution adapter only. No live broker transport without a separate user decision.

## Tier I — FYERS backup (after H contracts are stable; see `CX01C_FYERS_PREREQUISITES.md`)

FY-01 identity and persistence migration (`FeedSource::Fyers = 2`, tick-store version) · FY-02 symbol master → Reconciler · FY-03 auth/session, user-performed daily 2FA · FY-04 data-socket feasibility spike with recorded fixtures · FY-05 order-socket decoder with Unknown state · FY-06 data-only failover policy. Execution backup only after a live Kite OMS loop exists, plus static IP and app activation.

## User-owned blockers (not cards)

- Rotate the exposed Kite API key and secret.
- Kite login on the day any fetch or live test runs.
- A Linux environment for TSan (C4).
- FYERS account, app activation, static IP.
- Capital, rate-limit and prod-OS decisions (ROADMAP §14).
- Confirm the Models GUI scope (G4).
