# CX-02 — correctness, concurrency and trust boundaries

Started 2026-09-16 00:20 IST. Baseline HEAD `a34af5c5c9fa8c7498e8cc4df00907548a41e1a5`.
Input: the frozen Phase-1 audit `prompts/audit/cx01c/` (read-only; never edited).

This directory holds CX-02 plans and evidence only:

| File | Contents |
|---|---|
| `README.md` | Plan, conventions, card list with manifests, frozen interfaces and named tests |
| `CORRECTIONS.md` | Correction cards amending P0-01, P0-05b, P0-06b/P0-08a and P0-09a (C21-001…004) |
| `DISPOSITION.csv` | All 321 finding IDs, each with exactly one disposition |
| `CHECKPOINT.md` | Append-only progress log (what landed, what ran, what failed) |
| `build_logs/` | Raw build and test logs from the private build directories |
| `CLOSURE.md` | Phase-2 closure (written last) |

## Conventions

- **Dispositions.** Every finding gets exactly one of:
  - `FIXED_TESTED` — changed, with a named regression test that fails on the old code (planted or reverted and observed)
  - `DUPLICATE` — names the surviving ID
  - `NOT_REPRODUCED` — gives the evidence
  - `DEFERRED` — names its dependency or blocker
  - `UNRESOLVED`
  - `UNTRIAGED` — may not survive into the closure
- **Planted violations.** A regression test counts as evidence only after it has been seen to fail against the defect. Each card records how (re-planting the old line, or the test run before the fix).
- **Evidence limits.** ASan is not race evidence. A skipped or unavailable test is not a PASS. TSan and UBSan need Linux; this box is Windows/MSVC only, and nothing here installs WSL.
- **Build directories are private** (`build/cx-net`, `build/cx-asan`), so these builds never share a directory with Codex or the user's `build/net` and `build/default`. The user's running `altair_desktop` is never closed.
- **Concurrent writers.** Codex sessions are active (Phase-6 design/navigation). Before editing any tracked file, `git status --short <file>` must show it unmodified. Desktop manifests are declared below, before editing.
- **Out of scope for CX-02.**
  - UI redesign and Models screens.
  - `desktop/atlas.hpp`, `desktop/atlas_data.hpp`.
  - Any live order transport, broker login, credential handling, dataset refetch or replacement, WSL, commits and pushes.
- **Credentials.** No card reads `data/kite_session.json`, `.env`, or account files.

## Card list (dependency order)

Each card has at most 4 files, and a manifest never spans two component directories. "Frozen" means that signature is the contract the next card compiles against.

### Tier A — dataset precision and safe replacement

**CX02-A1a · exact price text** — `app/price_text.hpp` (new), `app/tests/test_price_text.cpp` (new), `app/CMakeLists.txt`
- Frozen: `namespace altair::dataset { enum class PriceTextError : std::uint8_t { NotFinite, Negative, BufferTooSmall }; [[nodiscard]] std::expected<std::size_t, PriceTextError> format_price(double v, char* out, std::size_t cap) noexcept; }`
- Output is the shortest decimal text that parses back to the identical double (`std::to_chars`, general, shortest). So a Kite price `24123.45` is written as `24123.45`, never `24123.5`.
- Named tests:
  - `price_text_round_trips_every_paise_value` — every paise 0.01 … 1,00,000.00 at stride 7, plus the edges
  - `price_text_never_rounds_to_six_significant_digits` — planted `%g` must fail
  - `price_text_refuses_nan_inf_negative`
  - `price_text_buffer_bound_refuses`

**CX02-A1b · use it in both writers** — `app/kite_update_main.cpp`, `app/kite_fetch_main.cpp`. Fixes C14-001. No behaviour change except the precision.

**CX02-A2 · checked, atomic dataset rewrite + coverage refusal** — `app/dataset_merge.hpp` (new), `app/tests/test_dataset_merge.cpp` (new), `app/CMakeLists.txt`, `app/kite_update_main.cpp`
- Frozen:
  - `enum class ReplaceError : std::uint8_t { OpenTemp, WriteTemp, CloseTemp, Rename };`
  - `std::expected<void, ReplaceError> replace_file_checked(const std::filesystem::path&, std::string_view bytes);`
  - `struct LoadedFile { std::string header; std::vector<std::string> rows; std::size_t headerless_rows; std::size_t short_rows; };`
  - `LoadedFile load_csv_rows(std::istream&);`
- Behaviour:
  - Write the temp file, check write, flush and close, then one `std::filesystem::rename` over the target. No remove-first window.
  - Only files actually changed are rewritten.
  - A headerless first row is kept and counted.
  - The per-file header is preserved.
  - A failed rewrite exits non-zero and names the file.
  - `verify_coverage` runs on every fetched window before merge; incomplete coverage merges nothing for that series.
- Fixes C14-005, C14-006, C14-016 (substring `--symbols` match, printf formats).
- Named tests:
  - `replace_checked_overwrites_existing_file`
  - `replace_checked_leaves_target_intact_when_temp_cannot_open`
  - `load_keeps_headerless_first_row`
  - `load_counts_short_rows`
  - `symbols_list_is_exact_not_substring`

**CX02-A3 · read-only inventory of affected series** — `prompts/cx02/DATASET_PRECISION_INVENTORY.md` (report only; no dataset file written)
- Counts rows whose price text has ≥6 significant digits with 0 decimals kept, i.e. rows that are plausibly rounded.
- Recovery is a refetch that needs a user login and a separately confirmed replacement scope, so it is not done here.

### Tier B — order lifecycle and the UI→OMS trust boundary

**CX02-B1 · strict intent grammar** — `oms/order_intent.hpp`, `oms/tests/test_order_intent.cpp`
- Grammar: exactly one flat JSON object per line, then nothing but whitespace.
  - The 13 known keys each appear exactly once; unknown or duplicate keys are refused.
  - Strings allow only the `\"` and `\\` escapes. Control characters are refused.
  - Integers are JSON integers (no `+`, no leading zeros, no fraction or exponent, no quotes), checked against int64 overflow.
- Bounds (rule 11 — refuse):
  - line ≤ `kMaxIntentLineBytes = 1024`
  - `token` 1 … 4,294,967,295
  - `lots` 1 … `kMaxIntentLots = 1000` (the ticket allows 100; risk limits are tighter again)
  - `limit_paise` 0 … `kMaxIntentPricePaise = 100,000,000,000`
  - `id` `[A-Za-z0-9_-]{1,64}`
  - `by` `[A-Za-z0-9._@-]{1,64}`
  - `symbol` printable ASCII 1–64
  - `exchange` `[A-Z]{2,8}`
  - `product` ∈ {NRML, MIS, CNC}; `validity` ∈ {DAY, IOC}
  - `at` = `YYYY-MM-DDTHH:MM:SS` followed by `Z` or `±HH:MM`, a real calendar date, parsed to `at_ns` (UTC ns)
- Frozen:
  - `IntentError` gains `DuplicateField, UnknownField, TooLong, OutOfRange` (appended)
  - `OrderIntent` gains `std::int64_t at_ns`
- Fixes C13-005 (reader half), C13-006, C13-007.
- Named tests:
  - `torn_append_then_next_append_is_refused` (C13-005 trigger)
  - `token_above_uint32_is_refused_not_narrowed`
  - `lots_overflow_is_refused`
  - `duplicate_key_is_refused`
  - `nested_object_is_refused`
  - `decimal_exponent_and_quoted_integers_are_refused`
  - `unknown_escape_is_refused`
  - `unenumerated_product_validity_are_refused`
  - `impossible_date_is_refused`
  - the vectors still parse byte-identically

**CX02-B2 · queue hygiene** — `oms/intent_queue.hpp` (new), `oms/tests/test_intent_queue.cpp` (new), `oms/CMakeLists.txt`
- Frozen:
  - `struct IntentCursor { std::uint64_t offset; std::uint64_t tail_fnv; };`
  - `enum class QueueError : std::uint8_t { Unreadable, Truncated, Replaced };`
  - `struct Quarantined { std::uint64_t offset; IntentError why; };`
  - `struct IntentBatch { std::vector<OrderIntent> accepted; std::vector<Quarantined> quarantined; std::size_t quarantined_dropped; std::size_t expired, future_dated, duplicate_ids; IntentCursor next; bool more; };`
  - `class IntentDrainer { explicit IntentDrainer(DrainPolicy); std::expected<IntentBatch, QueueError> drain(const std::string& path, IntentCursor from, std::int64_t now_ns); };`
- Decided policy: an invalid line is quarantined and counted, with its offset surfaced. Valid independent lines continue, and every accepted intent still passes mandatory risk (CX-03). The queue does not jam on a torn tail.
- Other behaviour:
  - A missing file is `Unreadable`, not "empty".
  - Size < offset is `Truncated`.
  - A tail hash mismatch is `Replaced`.
  - Age > `ttl_ns` is expired.
  - `at` later than now + skew is future-dated.
  - Duplicate ids inside the TTL window are refused. The dedupe table is bounded and refuses when full.
  - At most `max_batch` accepted per call; `more` is set and the cursor stops at the first unconsumed line.
  - Oversized lines are consumed without unbounded allocation and quarantined as `TooLong`.
- Fixes C13-008 and the queue half of C13-009 (expiry).
- Named tests:
  - `missing_file_is_an_error_not_an_empty_queue`
  - `truncated_file_is_refused`
  - `replaced_file_is_refused`
  - `stale_intent_expires`
  - `future_dated_intent_is_refused`
  - `redrain_after_crash_does_not_duplicate`
  - `batch_bound_leaves_the_rest_for_next_call`
  - `oversized_line_is_quarantined_without_allocation_growth`
  - `torn_tail_then_valid_line_quarantines_one_accepts_one`

**CX02-B3 · desktop writer** — `desktop/order_ticket.hpp`, `desktop/tests/test_order_ticket.cpp` (declared desktop manifest)
- One write of `[\n if the file does not end in \n] + line + \n`.
- `QLockFile` (`<path>.lock`, 2 s) serialises writers.
- Checks write, flush and close.
- `at` is written with an explicit UTC offset.
- Returns `enum class AppendResult { Ok, LockTimeout, OpenFailed, WriteFailed }`.
- Fixes C17-011 and C13-005 (writer half).
- Named tests: `append_after_torn_tail_starts_a_new_line`, `append_reports_write_failure`, `at_carries_offset`, `vectors_still_byte_identical`.

**CX02-B4 · confirmation and stale limit** — `desktop/order_ticket.hpp`, `desktop/tests/test_order_ticket.cpp`
- The confirmation text comes from a pure `confirmation_text(const IntentDraft&, const QString& phrase)`. It shows side, lots, symbol, exchange, type, the limit in rupees and paise, product and validity.
- The limit is cleared (set invalid) on contract change and must be re-entered or clicked. A zero or unset limit on a LIMIT order is refused.
- Rows without a resolved spec stay refused.
- Fixes C17-010, C17-021 (ticket side), C17-013 (int narrowing and sell snapping).
- Named tests: `confirmation_names_price_type_product_validity`, `limit_cleared_on_contract_change`, `unset_limit_refused`, `set_price_does_not_narrow`.

**CX02-B5 · halt panel fail-safe** — `desktop/kill_switch.hpp`, `desktop/tests/test_kill_switch.cpp` (new), `desktop/CMakeLists.txt`
- `read_kill_request()` returns a tri-state `{Absent, Present, Unreadable}`. Malformed or unreadable shows UNKNOWN, which is treated as halted.
- The write is checked and atomic (`QSaveFile`).
- Fixes C17-014.
- Named tests: `malformed_request_reads_unknown_not_absent`, `unreadable_request_reads_unknown`, `write_is_checked`.

**CX02-B6 · order-state transitions** — `oms/order_state.hpp`, `oms/tests/test_order_state.cpp`
- Frozen: `OrderEvent` gains `CancelReject, ReplaceReject` (appended); `Order` gains `OrderState resume_to`.
- Transitions:
  - A partial fill keeps PendingCancel/PendingReplace; a completing fill goes to Filled.
  - A late fill after Cancelled stays terminal Cancelled with `cum_qty` raised (it is not reopened as live); a late fill reaching the order quantity goes to Filled.
  - A late Ack after Filled or Cancelled, or during a pending request, is absorbed as a duplicate.
  - CancelReject/ReplaceReject return to `resume_to`. `Reject` means the NEW order was refused, and is legal only while the order was never acknowledged.
  - `ts` is defined as OMS receipt order. Equal stamps are legal, and the adapter stamps with the OMS clock.
- Fixes C13-001, C13-002, C13-003.
- Named tests:
  - `partial_fill_during_pending_cancel_keeps_the_cancel`
  - `partial_fill_during_pending_replace_keeps_the_replace`
  - `rejected_cancel_returns_order_to_live`
  - `rejected_replace_returns_order_to_live`
  - `fill_after_rejected_cancel_is_accepted`
  - `late_fill_after_cancel_stays_terminal`
  - `late_ack_after_fill_is_a_duplicate`

**CX02-B7 · uncertain orders and idempotency** — `oms/order_state.hpp`, `oms/tests/test_order_state.cpp`, `oms/reconcile.hpp`, `oms/tests/test_reconcile.cpp`
- Frozen:
  - `OrderState::Unknown` (appended) and `OrderEvent::SendTimeout` (appended)
  - `std::expected<Applied, OrderError> apply_reconciled(Order&, OrderState broker, Qty cum, Price avg, Timestamp)`
  - `ClientOrderId make_client_order_id(std::uint32_t session, std::uint32_t seq)` (20 chars `[A-Z0-9]`)
- Unknown refuses every broker event except `apply_reconciled`.
- `enforce_reconciliation` trips the kill switch on any quantity mismatch and on sign flips. The reconciliation key includes the product.
- Fixes C13-004, C13-011.
- Named tests: `send_timeout_makes_order_unknown`, `unknown_refuses_ack_and_fill`, `reconciled_snapshot_resolves_unknown`, `reconcile_qty_mismatch_trips_kill`, `mis_and_nrml_same_token_do_not_collide`.

**CX02-B8 · exit ladder: trigger vs attainable price** — `oms/exit_ladder.hpp`, `oms/tests/test_exit_ladder.cpp`
- Frozen: `ExitDecision` gains `Price fill_ref` (conservative attainable execution reference); `level` stays the trigger level.
- `fill_ref` by exit type:
  - stops: the worse of stop and tick (min for a long, max for a short)
  - target: the target
  - kill and time: the tick price
- Rewrite test 1 so the ordering is shown to change attribution, not a fictional Rs 1.5 lakh.
- Fixes C13-010.
- Named tests: `gap_stop_fill_ref_is_the_gap_price_not_the_stop`, `both_orderings_realise_the_same_gap_pnl`, `ordering_changes_attribution`.

### Tier B fix cards, from the independent review (`review/REVIEW_AB.md`)

The reviewer found 18 issues in Tier B. These cards answer them; each names the review IDs it closes, and anything argued down is recorded with its reasoning rather than quietly dropped.

**CX02-B6b · terminal states and request answers** — `oms/order_state.hpp`, `oms/tests/test_order_state.cpp`
- A late fill out of `Expired` is accepted exactly as one out of `Cancelled` is: the position is recorded, the order stays terminal (R-AB-001). The "believes it is flat while holding a position" argument does not stop at one state.
- An answer to a cancel or amend request (`CancelAck`, `CancelReject`, `ReplaceAck`, `ReplaceReject`) arriving when no such request is outstanding is a DUPLICATE from any state that could have sent one, and refused only from `Unset` and `PendingNew` (R-AB-002, R-AB-003). Brokers resend, and an alarm on `refused > 0` must not fire on healthy traffic.
- `CancelReject` with `resume_to == Unset` is refused instead of guessing a live state (R-AB-004(2)); the `resume_to` doc says what it means while an amend is pending (R-AB-004(1)).
- Named tests: `late_fill_after_expire_is_kept`, `duplicate_request_answers_are_duplicates`, `terminal_states_absorb_answers_to_overtaken_requests`, `cancel_reject_without_a_resume_state_is_refused`.

**CX02-B2b · queue: per-kind counts, staged ids, policy validation** — `oms/intent_queue.hpp`, `oms/tests/test_intent_queue.cpp`
- `IntentBatch` regains per-kind refusal counts (`expired`, `future_dated`, `duplicate_ids`, `dedupe_full`, `unparseable`, `too_long`) beside the capped list and the total, so rule-11 visibility survives more than 64 refusals (R-AB-007).
- `rollback(const IntentBatch&)` un-stages accepted ids, so a caller whose state save FAILED can retry without its own requests coming back as duplicates (R-AB-008). The header states the order: drain → save → act, and rollback on a failed save.
- `load_drainer_state` reports `Unreadable` when `exists()` itself errored; `NoState` means the file is genuinely absent (R-AB-009).
- `IntentDrainer::create(DrainPolicy)` refuses a zero bound or a negative duration (`BadPolicy`), rather than livelocking on `max_batch = 0` (R-AB-010).
- A trailing `\r` no longer counts toward `kMaxIntentLineBytes`, so a 1,024-byte line has the same verdict with LF and CRLF (R-AB-011).
- The dedupe identity and its eviction rule are stated: an id is remembered until it ages past the TTL, and a NEW line reusing an evicted id is a new request (R-AB-012).
- `save_drainer_state` checks its close explicitly and records the durability limit and the single-drainer assumption (R-AB-013).
- Test gaps closed: size-checked indexing, `more` on `max_lines`, the exact-1,024-byte line with both endings, and the failed-save case (R-AB-014).

**CX02-B8b · an exit is priced at the touch, not the last print** — `oms/exit_ladder.hpp`, `oms/tests/test_exit_ladder.cpp`
- `evaluate_exit` takes the `Touch` (bid/ask) as well as the tick. `fill_ref` for a long stop is the BID, for a short stop the ASK: a market exit crosses the spread, and valuing it at the last print is better than attainable (R-AB-015, gate 7). With no touch supplied it falls back to the tick, and the header states that as an assumption rather than a fact.
- `square_off_limit(const ExitDecision&, Touch, Bps band)`: a protective exit is priced THROUGH the touch by a band, never at `fill_ref` — a SELL LIMIT at the gap print rests unfilled if the next print is lower, which is C13-010's live failure moved one print (R-AB-016).
- A target touched exactly is marked as an uncertain fill rather than assumed filled at the target (R-AB-017).
- `fill_ref` asserted on the kill, clock, initial-stop and short-target paths (R-AB-018).

**CX02-B4c · the writer refuses what the reader will refuse** — `desktop/order_ticket.hpp`, `desktop/tests/test_order_ticket.cpp`
- `submit()` refuses a `by` or `symbol` outside the classes `oms/order_intent.hpp` enforces, instead of logging PENDING for a request that will be quarantined on arrival (R-AB-006). Starts after the second reviewer finishes with `desktop/`.

### Second-review fix cards (`review/REVIEW_AB.md`, R-AB-019 … R-AB-050)

The continuation review found 0 P1 and confirmed the four audit P1s CX-02 targeted are closed. It also found 19 P2s, three of them defects THIS PHASE INTRODUCED. Those three come first.

**CX02-B5b · the halt panel's own contradictions** — `desktop/kill_switch.hpp`, `desktop/tests/test_kill_switch.cpp`
- The CLEAR path passes `reason_out == nullptr`, so clearing a halt asks for no reason at all — the inverse of what the file's header and its own on-screen text both promise (R-AB-038). `do_clear` demands a reason like `do_request` does, and the clear is RECORDED (who, when, why) instead of `QFile::remove` deleting the only account of the halt.
- The 2-second poll B5 added overwrites the "could not write the request" notice within two seconds (R-AB-039). A failed action latches into `last_action_error_`, and `refresh()` renders it until the operator acts again.
- A `data/` that cannot be stat'ed reads as `Absent`, so the fail-safe direction shows green (R-AB-045).
- `confirm_phrase`'s enablement becomes a pure `confirm_enabled(bool phrase_matches, bool reason_required, QString reason)` so the clear-without-a-reason case is testable; plus a test for a request file that exists and cannot be opened.
- Named tests: `clear_refuses_without_a_reason`, `a_failed_write_notice_survives_the_poll`, `an_unstattable_path_is_unreadable`, `unreadable_request_reads_unknown`.

**CX02-B4c · the ticket refuses what the reader will refuse, and stamps at the write** — `desktop/order_ticket.hpp`, `desktop/tests/test_order_ticket.cpp`
- `at` and `id` are stamped AFTER the confirmation, immediately before the append. Stamped before it, a request the operator read for longer than the queue's TTL is quarantined as `Expired` while the ticket logs PENDING (R-AB-042).
- `submit()` refuses a `by` or `symbol` outside the classes B1 accepts — including control characters, which split one record into two quarantined lines (R-AB-043, R-AB-006) — and refuses an invented `token N` label as a tradingsymbol (R-AB-049).
- The four refusals move into a pure `refuse_request(...)` that `submit()` calls, so `unset_limit_refused` and the rest exist as tests rather than as unreachable branches (R-AB-044).
- Named tests: `unset_limit_refused`, `no_spec_refused`, `off_tick_limit_refused`, `aggressive_snap_refused`, `a_field_the_reader_would_refuse_is_refused_here`, `at_is_stamped_at_the_write_not_at_the_dialog`.

**CX02-B4d · a pick must not promote a row** — `desktop/watchlist.hpp`, `desktop/panels.hpp`, `desktop/tests/test_terminal.cpp`
- B4b closed C17-021 by calling `Watchlist::resolve` from `emit_pick`, which promotes a row to `SpecState::Resolved` on the strength of the UI's own `data/instruments.csv` — the silent promotion `watchlist.hpp` forbids in its header, and a claim that the point-in-time spec store agreed when it was never asked (R-AB-041, rule 1).
- The row instead takes a new, honest state: `SpecState::FromMaster` — orderable, labelled "master lot/tick", NOT `Resolved`, and `tradeable()` stays false because that means the spec store. The label then matches the behaviour without overstating where the numbers came from.
- Named tests: `a_pick_does_not_report_the_spec_store`, `a_blocked_row_sends_no_usable_spec`.

**CX02-A2c · the dataset merge's silent equalities** — `app/dataset_merge.hpp`, `app/kite_update_main.cpp`, `app/kite_fetch_main.cpp`, `app/tests/test_dataset_merge.cpp`
- A zero price is not a price: `format_price` refuses it, and `merge_bar` never answers `Same` because one side was unusable (R-AB-026). The fetcher already refuses an all-zero VOLUME for exactly this reason.
- `close_of` has no failure channel — an unparseable close reads as 0.0 and the fresh bar is dropped as `Same`. An unreadable close blocks the rewrite like an unkeyable stamp does (R-AB-027).
- `NothingNew` must mean "no session in the window", not "the answer was empty"; consecutive empty answers are counted and printed, and a `last_stamp` in the future REFUSES instead of being clamped into a zero-width window that reports success for ever (R-AB-028, rule 11).
- `--dump-instruments` writes the instrument master through `replace_file_checked` instead of remove-then-rename (R-AB-029).
- Named tests: `zero_price_is_refused`, `unparseable_close_blocks_rather_than_matching`, `empty_answer_over_trading_days_is_not_nothing_new`, `future_last_stamp_refuses_rather_than_clamping`.

**CX02-C1c · the store's third answer, and a writer-side test seam** — `core/config/store.hpp`, `core/config/tests/test_config.cpp`, `core/lockfree/snapshot_slots.hpp`, `core/lockfree/tests/test_snapshot_slots.cpp`
- `refresh()` gains `ConfigRefresh::NoConfig` for a store that has never published, so an empty snapshot cannot read as current — the conflation C02-004 fixed on the writer's side only (R-AB-019), with the dead `*g == 0` branch resolved with it (R-AB-020).
- `SnapshotSlots` gains `Hooks::before_write`, and a test in which a reader pins between the writer's busy scan and its slot write — the half of the publication proof no forced interleaving currently exercises (R-AB-022).
- The stress payload becomes large enough for a partial write to be visible, with an evidence floor in the thousands (R-AB-023).
- Named tests: `empty_store_is_not_reported_as_current`, `reader_pins_between_the_writers_scan_and_its_write`.

**CX02-B3b · the append gives the kernel back the guarantee** — `desktop/order_ticket.hpp`
- B3 replaced an atomic `Append` with a remembered size and a seek, so a writer that appends in between is overwritten, and `QLockFile`'s 10-second stale break can hand two writers the same offset (R-AB-046). The tail is read under the lock, then the record is written through `QIODevice::Append`, so the kernel guarantees the append even when the advisory lock is lost.

**Contract and manifest drift, recorded rather than quietly fixed** (R-AB-024, R-AB-034, R-AB-037): `PriceTextError` gained `OutOfRange` in the middle of the enum instead of appended; C1 changed `core/config/tests/test_config.cpp` and `app/main.cpp` outside its declared manifest; `app/kite_quote_main.cpp` and `app/kite_ticker_main.cpp` were edited under no card. All are now declared in `CHECKPOINT.md`, and the enum is re-ordered to append.

### Tier C — publication and model serving (after B)

**CX02-C1 · SnapshotSlots replaces the seqlock payload race in ConfigStore** — see `CORRECTIONS.md` (P0-06b/P0-08a correction, C21-003).
- Manifest: `core/lockfree/snapshot_slots.hpp` (new), `core/tests/test_snapshot_slots.cpp` (new), `core/config/store.hpp`, `core/CMakeLists.txt`.
- Fixes C02-002, C02-003, C02-004.

**CX02-C1b · the racy primitive itself** — `core/lockfree/seqlock.hpp`, `core/lockfree/tests/test_mpsc_seqlock.cpp`, `core/lockfree/CMakeLists.txt`
- C1 moved the only production user (`ConfigStore`) off `SeqlockSnapshot`; this card removed the still-racy/unbounded primitive and its tests while retaining the MPSC half.
- **Completed 2026-09-19:** removed the primitive and its seqlock tests/benchmark; retained and renamed the MPSC ring target, with bounds-safe stress-test indexing. `mpsc_ring` and `snapshot_slots` pass in `build/codex-cx06`; a source scan found no remaining type/header references. This closes the stale deferred status in the disposition ledger; it is not TSan evidence.

**CX02-C2 · ModelServer coherent record** — models/ manifest declared when it starts. Fixes C08-001, C08-002, C08-023.

### Tiers D–F — bounds, safety/config, research correctness

**CX02-D3 · malformed book depth counts** — `book/l2_book.hpp`, `book/microstructure.hpp`, `book/tests/test_l2_book.cpp`, `book/tests/test_microstructure.cpp`
- A `DepthUpdate` whose bid or ask count exceeds `kDepthLevels` is refused before it becomes stored book state. Existing market state stays unchanged and malformed snapshots are counted.
- Directly supplied `BookState` values with an impossible count are non-tradable; best-level, spread/mid and microstructure accessors refuse them instead of trusting the count. The summation primitive has its own capacity guard.
- Named tests: `malformed_depth_counts_are_rejected`, `malformed_book_counts_are_unusable`.
- Implemented 2026-09-19. Raw `DepthUpdate` best-level accessors outside the book still validate only the level-0 access (which is memory-safe); they do not certify the count. Normaliser-level malformed-depth rejection is not claimed.

Each card is declared with its manifest when it starts, in the order below. The findings they target:

| Tier | Findings |
|---|---|
| **D (bounds)** | C01-001 (P0-01 correction), C01-004 (P0-05b correction), C05-001/002, C08-010/014/015, C03-001/005/010, C15-001, C18-013, C01-006, C02-007 |
| **E (safety/config)** | C02-001 + C12-002 (P0-09a correction), C12-003, C12-001, C15-014, C05-012, C02-011, C02-006, C22-003 |
| **F (research)** | C11-001, C11-002, C11-003, C08-007/009, C10-003/004/013 |

### Declared next safety cards (manifest frozen before edits)

**CX02-E1 · external reconciliation and refused-fill latching** —
`core/invariant/conservation.hpp`, `core/invariant/tests/test_conservation.cpp`.
Append `Breach::ExternalMismatch` and `Breach::RefusedFill` to preserve ordinals.
Add exact independent cash/position reconciliation that latches mismatch; a
post-execution fill rejected for invalid cost/overflow keeps its original return
error, latches `RefusedFill`, and mutates no balances. Add both mismatch
directions, successful reconciliation, negative-cost and overflow tests. Do not
wire app/OMS here; `on_fill` means a confirmed execution only.

**CX02-E2 · make unverified charge schedules unusable** — `risk/cost.hpp`,
`risk/charges_toml.hpp`, `risk/charges_toml.cpp`, `risk/tests/test_cost.cpp`,
`risk/tests/test_charges_toml.cpp`, `desktop/cost_panel.hpp`, `ops/go-live.md`.
Append `CostError::UnverifiedSchedule`; reject pricing when `ChargeSchedule` is
unverified; require the real `[safety].block_on_unverified_schedule` value to be
a boolean `true` and reject missing/wrong/false policy. Test the real
UNVERIFIED file refusing computation while explicitly verified test schedules
still price. This closes the schedule-verification enforcement slice only;
`block_on_unset_broker_slab`, `edge_over_cost_factor` and `[implicit]` remain
separate unresolved settings and must not be claimed enforced by this card.

**CX02-E3 · overflow-safe pre-trade limits** — `risk/limits.hpp`,
`risk/tests/test_limits.cpp`. Append an explicit arithmetic-overflow violation;
refuse `INT64_MIN` quantity magnitudes and checked position/quote-age/gross
notional calculations without signed UB or wrapped approvals. Add boundary
regressions. This hardens the checker but does NOT create its missing OMS caller.

The paper-only CX-03 engine can start only after B1, B2, B6, B7 and E1/E2/E3
pass their focused evidence gates. The *C12-001 caller* is not a precondition;
it is an explicit CX-03 integration acceptance owned by the paper dispatcher.
The planning-only outline is `prompts/CX03_IMPLEMENTATION_PLAN.md`; implementation
card manifests remain to be frozen after the entry gates pass. These are
dependencies, not a claim that CX-02 or CX-03 is closed.

## Verification handoff — 2026-09-19

Current working tree contains tested slices of CX02-B1/B2/B3, B5, B6/B8,
CX02-A1/A2, CX02-C1/C1b/C2, CX02-D3, and related desktop/forecast checks.
This is NOT a CX-02 closure: disposition entries remain authoritative per finding;
unimplemented cards and post-edit coverage still need separate evidence.

- Final incremental full-target build: `build/codex-cx06`, exit 0 (13 rebuilt
  steps after final corrections).
- Configured CTest suite: **141/141 passed** in 229.98 s.
- Focused parser/queue/order-state/exit-ladder/kill-switch selection: 5/5 passed.
- The intent queue's clock and persisted-id timestamps now share the parser's
  bounded instant range; invalid clocks return `BadClock`, and invalid restored
  timestamps make state corrupt without partial restore.
- The final kill-switch helper uses `kill_switch_detail` to avoid shadowing
  `altair::detail` in desktop include order. The Qt desktop target and
  `desktop_kill_switch` pass.

Still unverified: Linux TSan/UBSan; post-change whole-tree race review;
allocation/tail-latency evidence; the canonical paper/live event-to-OMS pipeline;
FYERS adapter/fixtures; and live broker/account behavior. No data fetch or live
order was run. See `prompts/CODEX_PROJECT_MEMORY.md` and the append-only root
`change_by_codex.txt` for the cross-phase handoff and Cline approval blocker.

## CX continuation — 2026-09-21 selected follow-ups

These focused fixes are tested, but do not close CX-02 as a whole.

- **CX02-B7 follow-up:** order submission and cancel/amend response timeouts
  enter `Unknown`; only an authoritative snapshot may resolve them. The caller
  must verify snapshot authentication, freshness and exact order/account/
  product/instrument identity before invoking the value-level reconciliation
  function. OMS/reconcile average-price gaps are overflow-safe; malformed
  input pointers/presence/counts and negative tolerances are refused.
- **CX02-D4:** price-bus cap coalescing never removes a partially sent TCP head;
  it drops complete unsent frames only. `build/cx-net` `price_bus` passed 1/1.
- **CX02-E4:** hard canary loss is checked before the sample-size floor,
  accumulation handles `INT64_MIN`, and each controller is one-shot. Monitor
  summaries now rank `Unobserved` above healthy readings; NaN and invalid
  thresholds are Critical. Histogram bucket wrap/bounds still need a proof.
- **CX02-E5:** CMake defines the strict-invariants OFF state as `=0`. A Release
  build with it OFF was verified to stop at the intended compile-time guard.
- The synthetic note-reconciliation schedule fixture is marked verified only for
  that deterministic test scenario. The real `config/charges.toml` remains
  UNVERIFIED and pricing remains fail-closed.

Verification: full `build/codex-cx06` build exit 0; full configured CTest 141/141
passed in 234.35 s with no global Qt offscreen override; selected OMS/monitor/
note/flagging tests 5/5; Boost-enabled price-bus test 1/1; strict-off Release
negative build rejected as intended. No Linux TSan/UBSan, latency proof, paper
end-to-end path, FYERS integration, live option data or live-readiness decision.
