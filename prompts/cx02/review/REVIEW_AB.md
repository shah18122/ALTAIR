# CX-02 Tier A + Tier B — adversarial review

Reviewer: independent Claude reviewer · 2026-09-16 · baseline HEAD `a34af5c` · working tree uncommitted.
Mode: READ-ONLY. Nothing was built, run or tested. Every CONFIRMED finding comes from reading the code
at the cited lines, with the event sequence or input traced by hand. HYPOTHESIS marks a trigger that
depends on behaviour outside the files read (Qt internals, Kite behaviour, the contents of dataset/).

Severity follows the audit: P1 = can place, lose or misstate a position/request or silently lose data;
P2 = a correctness or trust-boundary defect with a realistic trigger; P3 = latent, bookkeeping or test weakness.

---

## oms/order_state.hpp (CX02-B6)

State × event was enumerated for all 10 states × 10 events against `detail::legal` (order_state.hpp:221-264),
the `resume_to` conditions (:306-315) and the switch (:317-388), plus `apply_fill` (:404-472).
C13-001 and C13-002 as specified are fixed. What remains:

### R-AB-001 — P2 — CONFIRMED — oms/order_state.hpp:258-261, :380-383
**A fill that crossed an expiry is refused, so the position is lost. This is the C13-001/C13-002 class, left open for `Expired`.**
- **Trigger:** `open_order(Qty{150}, t0)`, then `apply(Ack, 10ms)`, then `apply(Expire, 20ms)`, then `apply_fill({75, px, 25ms})`. For example, a DAY order filled at 15:29:59.9 whose exchange lapse message is delivered before its fill report.
- **Expected:** the fill is recorded. The order stays terminal (`Expired`, or `Filled` if the fill completes it), which is the same rule B6 wrote for `Cancelled`.
- **Actual:** `legal(Expired, Fill)` is false. The result is `IllegalTransition`, `refused = 1` and `cum_qty = 0`. Altair records itself flat while holding 75. The header's own argument ("Refusing it would leave Altair believing it is flat while holding a position", :24-30) applies word for word, yet the exception was granted to `Cancelled` only.
- **Suggested regression test:** `late_fill_after_expire_is_kept`. Run the sequence above and assert `cum_qty == 75`, `!is_live`, `refused == 0`. Do the same for a completing fill, which should end `Filled`.

### R-AB-002 — P3 — CONFIRMED — oms/order_state.hpp:227-231, :306-315
**Duplicate answers to a cancel or amend request count as refusals. The header's rule 1 ("DUPLICATES ARE NORMAL") is broken for the new events.**
- **Triggers:**
  - (a) `Ack@10`, `CancelSent@20`, `CancelReject@30` returns the order to Open. A second `CancelReject@31` (a broker resend) fails `legal(Open, CancelReject)` and is refused.
  - (b) `Ack@10`, `ReplaceSent@20`, `CancelSent@21`, `ReplaceAck@25` sets `resume_to = Open`. A second `ReplaceAck@26` is refused because `resume_to != PendingReplace`.
  - (c) `ReplaceSent`, `ReplaceAck`, `ReplaceAck` from Open is refused. This one predates B6.
- **Expected:** `Applied::Duplicate` with `refused == 0`, the same treatment as a duplicate `CancelAck` (:250) or `CancelSent` (:240).
- **Actual:** `IllegalTransition` with `++refused`. Any alarm keyed on `refused > 0` fires on healthy traffic.
- **Suggested regression test:** `duplicate_cancel_reject_and_replace_ack_are_duplicates`, covering (a) and (b).

### R-AB-003 — P3 — CONFIRMED — oms/order_state.hpp:245-261
**Request answers that arrive after a terminal state are absorbed from `Filled` but refused from `Cancelled`, `Rejected` and `Expired`.**
- **Triggers (each ends in an IllegalTransition refusal):**
  - (a) `Ack`, `ReplaceSent`, `CancelSent`, `CancelAck` (Cancelled), then `ReplaceReject` ("order already cancelled"). This answer is inevitable for an amend overtaken by a cancel.
  - (b) `CancelSent` from PendingNew, `Reject` (Rejected), then `CancelReject`. The cancel chasing a rejected order must be rejected.
  - (c) `Ack`, `CancelSent`, `CancelSent` (resend), `CancelAck`, then `CancelReject` for the second cancel.
  - (d) `Ack`, `ReplaceSent`, `apply_fill(150)` (Filled), then `ReplaceAck`. The amend was accepted and the order then filled, and the ack was delivered after the fill. `Filled` admits `ReplaceReject` but not `ReplaceAck`.
- **Expected:** Duplicate. The comment at :253-254 gives exactly this reasoning for Filled.
- **Actual:** refused and counted. There is no position loss (the state stays terminal), but the "healthy order has no refusals" diagnostic (:182-185) is false.
- **Suggested regression test:** `terminal_states_absorb_answers_to_overtaken_requests`, covering (a) to (d).

### R-AB-004 — P3 — CONFIRMED — oms/order_state.hpp:187-192, :364-373
**`resume_to` does not keep its documented invariant, and one branch guesses.**
- (1) After `ReplaceSent`, `CancelSent`, `CancelReject`, the order is back in `PendingReplace` with `resume_to = Unset` (:372). The field doc says it is Unset only "when nothing is pending", but an amend is pending. It is harmless today because ReplaceAck and ReplaceReject re-derive `live`, but the doc misleads the next editor.
- (2) `CancelReject` with `resume_to == Unset` silently becomes `live` (:367-369). Through the API, PendingCancel always has `resume_to` set, so this is reachable only through a hand-built or deserialised `Order`. That is exactly where a guess is the wrong answer (rule 9): it could return a never-acknowledged order to Open.
- **Expected:** refuse `IllegalTransition` when `resume_to` is Unset in PendingCancel, and document the PendingReplace case.
- **Suggested regression test:** construct `Order{state=PendingCancel, resume_to=Unset}` and assert that `CancelReject` is refused.

### R-AB-005 — P3 — CONFIRMED — oms/tests/test_order_state.cpp:496-508; prompts/cx02/DISPOSITION.csv:199
**C13-003 is marked FIXED_TESTED, but the fix is documentation only and test 12 does not pin it.**
- **Code:** only comments changed. The check `ts < o.last_event` (:288, :409) is byte-identical to HEAD.
- **Test:** test 12 stamps every event at 10 ms. HEAD's time check also accepts equal stamps. On HEAD the test fails only because `CancelAck` is refused from PartiallyFilled, which is C13-001. Nothing in the test concerns time semantics.
- **Adapter:** the property the fix relies on ("the adapter stamps with the OMS clock", README B6) lives in an adapter that the disposition itself defers to CX-03.
- **Expected:** disposition `DEFERRED` (dependency: the CX-03 receipt-stamping adapter), or a test that feeds an exchange-time-ordered sequence through whatever stamps `ts`.

---

## oms/order_intent.hpp (CX02-B1)

The grammar was traced character by character. It covers:
- the object and key loop (:285-350)
- string escapes (:225-252)
- integer overflow (:257-276; the check `v > (max-d)/10` is exact; INT64_MIN is refused, which is conservative)
- the ISO time parser: offsets, day-of-month, the 2100 non-leap year, and ns overflow (2200-12-31T23:59:59-14:00 gives about 7.26e18 < 9.22e18)

Every torn-append concatenation I could construct is refused. After the tear, B's leading `{"v":` always lands where only `,`, `}`, `:` or a closing quote is legal, and escapes are limited to `\"` and `\\`.

The desktop's `id` (`<ms>-<token>`, order_ticket.hpp:608-610) and `at` (`...Z`, :165-168) both match the grammar.

### R-AB-006 — P3 — HYPOTHESIS — oms/order_intent.hpp:429-439, :482-488; desktop/order_ticket.hpp:606
**`by` and `symbol` are validated far more strictly by the reader than anything the writer checks. Drift shows up as "PENDING" in the UI and a quarantine in oms.**
- **Trigger:** a provisioned login name containing a space or a non-ASCII character (for example "Smit Shah"), or an instrument-master name longer than 64 bytes. `UserStore::add` (auth.hpp:159) accepts any QString. `submit()` writes `d.by = user_` unchecked, and the ticket logs PENDING. `parse_intent` refuses the line as BadValue, so every request from that user is dead on arrival.
- **Expected:** the writer refuses what the reader will refuse, or the vectors include a line with the real character classes at their edges.
- **Actual:** only the dev accounts `admin`/`staff` and the vector user `smit` are exercised.
- **Suggested regression test:** in `test_order_ticket`, emit a draft whose `by` and `symbol` sit at the edges of `[A-Za-z0-9._@-]{1,64}` and printable ASCII 1-64. Add those lines to `intents.jsonl` so `test_order_intent` parses the same bytes. Make `submit()` refuse a user name outside the class.

---

## oms/intent_queue.hpp (CX02-B2)

Checked and found correct:
- `TailRing` push order and wrap (:157-164)
- the seed from the pre-cursor tail (:200-215)
- the cursor at the last consumed newline, with `more` stopping BEFORE a line is consumed (:246-263)
- oversized lines bounded at 1,024 stored bytes
- a partial tail not consumed
- blank lines

### R-AB-007 — P2 — CONFIRMED — oms/intent_queue.hpp:107-141 vs prompts/cx02/README.md:114-118
**Gate 2: the frozen `IntentBatch`/`Quarantined` contract was changed without being recorded, and the per-kind refusal counts it froze are gone.**
- **Frozen:**
  - `struct Quarantined { std::uint64_t offset; IntentError why; }`
  - `IntentBatch { accepted; quarantined; std::size_t quarantined_dropped; std::size_t expired, future_dated, duplicate_ids; IntentCursor next; bool more; }`
- **Actual:**
  - `Quarantined { offset; QuarantineKind kind; IntentError parse_error; }`
  - `IntentBatch { accepted; quarantined; quarantined_total; lines; next; more; partial_tail; }`
  - `QueueError` gains `NoState`, `CorruptState` and `StateWriteFailed`.
  - `save_drainer_state` and `load_drainer_state` are new API.
  - CHECKPOINT.md records none of this (it records only the B1 `drain_intents` removal).
- **Consequence (rule 11 visibility):** once more than `max_quarantine_list` (64) lines are refused in one call, how many were Expired, DuplicateId or DedupeFull is unknowable. Only `quarantined_total` survives. A replay after a long outage (thousands of expired intents plus a few duplicates) cannot show that duplicates occurred.
- **Suggested regression test:** 70 expired lines followed by 1 duplicate id. Assert `expired == 70` and `duplicate_ids == 1`, with `quarantined.size() == 64`.

### R-AB-008 — P2 — CONFIRMED — oms/intent_queue.hpp:271, :30-33
**Accepted ids are committed to the dedupe table inside `drain()`, BEFORE the caller persists. A failed or skipped save turns every never-acted request into a "DuplicateId".**
- **Trigger:**
  1. `drain(q, {}, now)` accepts `A1`. `seen_` now contains `A1` (:271).
  2. `save_drainer_state(st, b.next, d)` returns `StateWriteFailed` (temp not creatable, or the rename blocked by a reader holding the state file).
  3. Honouring persist-then-act, the caller does not act, and retries `drain(q, {}, now)` from the cursor it still holds.
- **Expected:** `A1` is accepted again. Nothing was acted on and nothing was persisted.
- **Actual:** `accepted.empty()`, with `quarantined[0].kind == DuplicateId`, and the new cursor consumes the line. The request is never executed, and the audit trail labels it a duplicate. The header's contract (:30-33) covers a crash, not an in-process save failure. There is no API to roll back or to commit after persisting.
- **Suggested regression test:** `failed_save_does_not_burn_ids`. Drain, save to an unwritable path, re-drain from the same cursor, and assert `A1` is accepted. Fix shape: `drain` returns the staged ids, and a separate `commit(const IntentBatch&)` runs after a successful save.

### R-AB-009 — P2 — CONFIRMED — oms/intent_queue.hpp:400-403
**`load_drainer_state` reports `NoState` when it merely could not stat the file. A caller that treats NoState as a fresh start forgets every remembered id, the exact hole CorruptState exists to close.**
- **Trigger:** `std::filesystem::exists(path, ec)` returns false with `ec` set: access denied on the directory, the network path of a `data/` share is unavailable, or a path-too-long error. `ec` is never examined.
- **Expected:** `Unreadable` (or `CorruptState`) whenever `ec` is set. `NoState` only when `ec == std::errc::no_such_file_or_directory` (or `exists` returns false with no error).
- **Actual:** `NoState`, which the header (:87-91) and test 4 present as "the caller can tell apart" and so safe to start over from. That re-accepts in-TTL intents: a duplicate order after restart. This is the same class as C13-008(a) ("a missing file looks the same as an empty queue").
- **Suggested regression test:** point the state path at a location whose stat fails (for example, a path under a file rather than a directory, `file.txt/state`). Assert the error is not `NoState`.

### R-AB-010 — P3 — CONFIRMED — oms/intent_queue.hpp:115-125, :184, :248-253
**`DrainPolicy` is not validated, and some values livelock the drain or refuse everything silently.**
- `max_batch = 0` or `max_lines = 0`: the first `\n` hits the bound before any line is consumed. Every call returns `more = true`, `next == from`, and nothing else, for ever.
- `ttl_ns < 0` expires every intent. `dedupe_capacity = 0` makes every intent `DedupeFull`.
- **Expected:** the constructor refuses (or has a factory returning `std::expected`) any policy with a zero bound or a negative duration.
- **Suggested regression test:** `IntentDrainer(DrainPolicy{.max_batch = 0})` is refused.

### R-AB-011 — P3 — CONFIRMED — oms/intent_queue.hpp:237-241 vs :307-310
**A line exactly at `kMaxIntentLineBytes` (1,024) is accepted with LF and quarantined as TooLong with CRLF.**
- **Cause:** the `\r` is counted toward the 1,024-byte cap while bytes are being stored. Only later does `judge()` strip it, and `parse_object` accepts up to 1,024.
- **Trigger:** a valid intent padded with JSON whitespace to exactly 1,024 bytes, then `\r\n` (the old desktop's text-mode ending, which the comment at :307 says is supported).
- **Expected:** the same verdict for both endings.
- **Suggested regression test:** the 1,024-byte line with each ending; both are accepted. Also a 1,025-byte line with each ending; both are TooLong.

### R-AB-012 — P3 — CONFIRMED (mechanism) — oms/intent_queue.hpp:326-346
**Dedupe keys on `id`, but eviction reasons from the stored `at`. The comment's safety argument ("An id older than the TTL can never be accepted again -- it would be expired", :335-337) is false for a re-used id on a NEWER line.**
- **Trigger:**
  1. With `dedupe_capacity` reached, `X@T0` has been evicted at now > T0 + ttl.
  2. A new line with `id X` and `at = now` is accepted, since it is fresh and no longer in `seen_`.
  3. The same line arriving before the table filled would have been refused as DuplicateId.
- **Effect:** acceptance depends on table fill level. The desktop id is `<ms>-<token>`, so a real collision is unlikely (HYPOTHESIS for impact). A clock step backwards (NTP) combined with a lost cursor can also re-admit an evicted id.
- **Expected:** a stated policy. Either the id alone is identity for the table's lifetime (refuse when full, never evict), or identity is `(id, at)` and the comment says so.
- **Suggested regression test:** capacity 1. Accept `X@T0`, advance now by ttl+1, fill the table, then offer `X@now`. Assert the documented verdict.

### R-AB-013 — P3 — CONFIRMED — oms/intent_queue.hpp:379-393
**`save_drainer_state` writes the temp in a scope whose `ofstream` destructor swallows a close failure, and it uses a fixed `path + ".tmp"`.**
- **Close:** only `flush()` is checked (:385-386). `replace_file_checked` in the same change set checks close as `CloseTemp` (dataset_merge.hpp:113-121). There is no fsync, so a power loss after the rename can leave a short state file. That fails closed as CorruptState, but the queue then stays blocked.
- **Temp name:** two drainer processes, or a stale temp held open by an antivirus scanner, contend for the same temp name.
- **Suggested regression test:** none practical offline. Make close explicit and checked, and record the durability limit the way dataset_merge.hpp:97-99 does.

### R-AB-014 — P3 — CONFIRMED — oms/tests/test_intent_queue.cpp:110-111, :134-136, :174-181, :197-199
**Test weaknesses.**
- `future->quarantined[0]` (:111) and `full->quarantined[0]` (:199) are indexed with no size check. A regression that empties the list is undefined behaviour, not a FAIL.
- "`again` ... sees nothing new" (:134-136) is true with no dedupe at all, because the cursor is at EOF. The `from_zero` check is the real one.
- README's `oversized_line_is_quarantined_without_allocation_growth` measures no allocation. `line.size()` is never inspected and no allocator hook exists.
- There is no test of `more` stopping on `max_lines`, of a line of exactly 1,024 bytes, or of a drain after a save failure (R-AB-008).

---

## oms/exit_ladder.hpp (CX02-B8)

`fill_ref` for stops is `price`. When a stop is breached, `price <= stop` for a long and `price >= stop` for a short, so `min`/`max` of the stop and the tick equals the tick, and the README's formula is honoured. Rule-8 ordering is unchanged and correct.

### R-AB-015 — P2 — HYPOTHESIS — oms/exit_ladder.hpp:36-41, :126-128, :215-275
**`fill_ref` is "the tick" for stop, kill and clock exits. If the tick is a last-traded price, a market exit fills at the bid (long) or ask (short), so `fill_ref` is better than attainable by at least half the spread, plus impact.**
- **Trigger:** long 750 NIFTY FUT, stop breached on an LTP print of 23,700 with the best bid at 23,698. `fill_ref = 2,370,000`, but the attainable fill is at most 2,369,800.
- **Expected (gate 7):** the header says `fill_ref` is "the most a P&L, a simulated fill, or a square-off order may assume" and "Never better than what the tick allows" (:126-128). For an aggressive exit that is the touch on the side being hit, not the LTP. `evaluate_exit` takes one `Price` and cannot express it.
- **Actual:** the attainable side depends on which price a future caller passes, and nothing documents or enforces it. There are no callers (C13-009), hence HYPOTHESIS.
- **Suggested regression test:** make `evaluate_exit` take the bid and ask (or a `Touch`). Assert `fill_ref == bid` for a long stop and `== ask` for a short stop. Assert `fill_ref <= bid` for a long kill or clock exit.

### R-AB-016 — P2 — CONFIRMED — oms/exit_ladder.hpp:43-45, :232-234
**The header instructs that "A square-off order is priced from `fill_ref`". For a protective exit that repeats C13-010's live failure one tick later.**
- **Why:** a SELL LIMIT at the gap print (23,700) rests unfilled if the next print is 23,650, exactly as the header says a limit at the stop does (:43-45). The live consequence C13-010 recorded ("The protective exit rests unfilled while the price falls") is not removed, only moved one print lower.
- **Expected:** stop, kill and clock exits are sent marketable: MARKET, or a LIMIT priced through the touch by a protection band from config. `fill_ref` is a valuation bound, never the limit of a protective order.
- **Suggested regression test:** a pure `square_off_order(const ExitDecision&, Touch, ProtectionBps)`. Assert the long-stop limit is at or below `bid*(1-band)`, and never equal to `fill_ref` when bid < fill_ref.

### R-AB-017 — P3 — HYPOTHESIS — oms/exit_ladder.hpp:264-273
**A target touched exactly (`price == target`) is valued at the target for the full `qty`. A resting limit AT the print may not fill (queue priority), so this is a touch-fill assumption.**
- **Expected (gate 7):** require the tick to trade THROUGH the target (strictly better) before `fill_ref = target` is assumed filled, or flag the fill as uncertain for simulation.
- **Suggested regression test:** `price == target` yields a decision that a backtest fill model does not book as filled at the target.

### R-AB-018 — P3 — CONFIRMED — oms/tests/test_exit_ladder.cpp:62-83, :136-148, :277-297
**The named test `both_orderings_realise_the_same_gap_pnl` compares the real function to a test-local copy that hard-codes `d.fill_ref = price` (:71, :78). The equality adds nothing beyond check :129.**
- **Not asserted:**
  - `fill_ref` on the kill-switch path (test 4 checks `level` only, :293)
  - the clock path
  - an initial-stop-only breach
  - a short target
- A regression setting `fill_ref = level` on the kill or clock path passes every test.
- **Suggested regression test:** assert `fill_ref` in tests 3 and 4, and for a short target and a single initial-stop breach.

---

# Continuation — reviewer B (rate-limit handover). Findings R-AB-019 onward.

Picked up at Tier C and the app/desktop files, which the sections above had not reached.

## core/lockfree/snapshot_slots.hpp (CX02-C1)

**The publication proof holds.** I tried to break it and could not, and the reason is worth recording so
the next reader does not re-derive it. For the dangerous interleaving — writer picks `w` while a reader
copies `w` — the reader must have stored `hold = w` (:170) and then read `published == w` (:172), while
the writer read `published == pub != w` (:119) and read `hold != w` (:123). Under seq_cst those four
form one total order: the writer's hold-read not seeing `w` puts it before the reader's hold store, so
:119 < :123 < :170 < :172, and the reader at :172 must read the most recent store to `published_` in
that order. The only writer is one thread which is *between* :119 and :141 and therefore cannot have
stored `w` in the interval. So no such store exists and :172 cannot return `w`. **This argument consumes
the single-writer premise entirely**, which is the weak point (R-AB-022). The ABA I expected —
`published_` returning to an earlier index with ≥3 slots — is harmless for the same reason: the reader
can only ever pin the *currently published* slot, and `pub` is always excluded at :121.

Also checked and correct: every `read()` exit releases its hold (:167, :175, :179), so no hold leaks;
`kFree`/`kClaimed` (0xFFFF'FFFF / 0xFFFF'FFFE) are both `>= kSlots`, so a claimed-but-unpinned hold is
correctly *not* counted busy and the `p >= kSlots` test at :166 also catches the never-published state;
`kSlots = kReaders + 2` gives at most `kReaders + 1` busy, so `NoFreeSlot` is genuinely unreachable.

### R-AB-019 — P2 — CONFIRMED — core/config/store.hpp:99-112, :104; test_config.cpp:215-218
**`refresh()` cannot tell "your copy is current" from "this store has never published anything", and the fast path trusts `local.version()` as a proxy for content that nothing ever verifies. C02-004's trigger B is fixed on the writer's side only.**
- **Trigger (a):** the loader fails, or hands `publish()` a snapshot left at version 0, which :78 now correctly refuses. A reader then calls `refresh(local)` on its default-constructed `local`: `local.version()` is 0, `version_` is 0, `0 >= 0` is true (:104), so it returns `ConfigRefresh::Unchanged`. The thread runs on an empty snapshot believing it is up to date, and `refresh()` will say `Unchanged` for ever. test_config.cpp:215-218 asserts this as the correct answer ("refresh against an empty store is Unchanged").
- **Trigger (b):** `local` is not a copy this store produced — a snapshot a caller built itself, or one carried over from another `ConfigStore`. `set_version(1'000'000)` and `refresh()` returns `Unchanged` permanently. `content_hash()` is never compared anywhere in the store, so rule 10 ("every live decision reproducible from config_hash") rests on an unchecked precondition (:98).
- **Expected:** a third outcome, e.g. `ConfigRefresh::Never` / `NoConfig`, whenever `version_ == 0`, so a reader must handle "nothing has been published" explicitly (rule 9). The enum authors already reasoned this way once — the comment at :52-53 puts `Contended` at ordinal 0 precisely so "a zeroed result never reads as 'your copy is current'" — and then left the 0-version case reading exactly that way.
- **Actual:** the only signal that no config exists is `store.version() == 0` or `publishes() == 0`, and `refresh()`'s return value actively says otherwise.
- **Suggested regression test:** `empty_store_is_not_reported_as_current` — `ConfigStore fresh; ConfigSnapshot local; assert(fresh.refresh(local) != ConfigRefresh::Unchanged);` and a second case where `local` carries a version the store never published.

### R-AB-020 — P3 — CONFIRMED — core/config/store.hpp:107-111
**The `*g == 0` branch is dead, and if it were ever reached it would mislabel "nothing published" as `Unchanged`.**
- `slots_.read()` returns generation 0 only while `published_ == kFree`, i.e. before any `slots_.publish()`. Control only reaches :107 when `version_ > local.version() >= 0`, so `version_ >= 1`, which at :82-88 implies a `slots_.publish()` already succeeded. The ternary at :111 can therefore never take its left arm.
- It is the same conflation as R-AB-019 written a second time: `Unchanged` is returned for "there is nothing to copy".
- **Suggested regression test:** none reachable; the branch should be `return ConfigRefresh::Contended;` or an assertion, and the reasoning stated next to it.

### R-AB-021 — P3 — CONFIRMED — core/lockfree/snapshot_slots.hpp:141-143, :185-187
**`generation()` is published AFTER `published_`, so it can be older than what `read()` would return. The header offers it as a copy-free change check, which is exactly the use that the ordering breaks.**
- **Trigger:** the writer completes :141 (`published_ = w`) and is preempted before :142 (`generation_ = gen`). A reader that "compares this against the generation it last copied without copying" (:185-186) sees the old value, concludes nothing changed, and skips an update that is already published and copyable.
- **Expected:** either store `generation_` before `published_` (it is only ever a hint that may be stale in the safe direction, i.e. too new), or say in the doc comment that `generation()` may lag `read()` and must not be used to skip a read.
- **Actual:** no caller today — `ConfigStore` keeps its own `version_`, whose equivalent lag at store.hpp:88 is documented and benign because of the `>=` at :104. `generation_` is otherwise unused in the tree.
- **Suggested regression test:** with a `before_generation_store` hook, assert `read()` returns a generation greater than `generation()`.

### R-AB-022 — P2 — CONFIRMED — core/lockfree/tests/test_snapshot_slots.cpp:40-45, :63-72, :97-100, :124-143; snapshot_slots.hpp:117-144
**`publish()` has no test seam, so the half of the proof that depends on the WRITER's window is not exercised by any forced interleaving — and CORRECTIONS.md:129-133 claims deterministic interleavings for the whole protocol.**
- The only hook is `Hooks::after_hold` (:91), fired inside `read()`. Every test drives the writer from inside a reader's hook, so `publish()` always runs as an indivisible unit relative to the reader. The window the proof's second half is about — a reader storing `hold = p` *between* the writer's busy scan (:122-125) and its slot write (:138-139) — cannot be constructed with the current seam, in any of tests 1 to 4.
- Second consequence: because every test calls `publish()` from a reader thread's hook, no test models the production threading at all. `publish()` writes `writer_gen_` and `last_written_` (:136-140) as plain non-atomic members under a "WRITER THREAD ONLY — exactly one" comment that nothing asserts; test 6 is the only place the writer is a separate thread, and it uses no hooks.
- **Expected:** a `Hooks::before_write(std::size_t slot)` called between :125 and :138, plus a test in which that hook runs a reader's pin-and-recheck and asserts the reader did not copy the slot the writer is about to write.
- **Suggested regression test:** `reader_pins_between_the_writers_scan_and_its_write`.

### R-AB-023 — P3 — CONFIRMED — core/lockfree/tests/test_snapshot_slots.cpp:35-38, :219-220, :235
**The stress test's payload is 16 bytes, where tearing is essentially unobservable on x86-64; the production payload is a ~9 KB `ConfigSnapshot`. And its evidence floor is `> 0`.**
- `Pair` is two `uint64_t` (:35-38). A torn copy would have to interleave two 8-byte moves; the `p.b != 2 * p.a` check (:219) would miss almost every real protocol break at that size. The payload the primitive exists for is 128 × 72-byte entries plus 16 bytes of header, about 9,232 bytes.
- `check(reads.load() > 0, ...)` (:235) passes on a single observed read. test_config.cpp:279, :388 deliberately sets an *evidence floor* of 2,000 for the same reason ("terminate on the EVIDENCE, not on a timer, or the test can pass having observed almost nothing"); test 6 abandons that discipline.
- **Suggested regression test:** give the stress an array payload (e.g. `std::array<std::uint64_t, 512>` every element equal to the generation) so a partial write is visible, and assert a read floor in the thousands.

### R-AB-024 — P3 — CONFIRMED — gate 3 / gate 4; README.md:204, CORRECTIONS.md:110, CHECKPOINT.md
**The CX02-C1 manifest does not match the files changed, and the deviation is unrecorded.**
- Declared (README.md:204, repeated in CORRECTIONS.md): `core/lockfree/snapshot_slots.hpp`, `core/tests/test_snapshot_slots.cpp`, `core/config/store.hpp`, `core/CMakeLists.txt`.
- Actual: `core/lockfree/tests/test_snapshot_slots.cpp` and `core/lockfree/CMakeLists.txt` (both defensible relocations), **plus `core/config/tests/test_config.cpp`, which appears in no manifest** (+74 lines, including the C02-004 assertions at :247-271).
- CHECKPOINT.md has no Tier C entry at all; its last entry ends at the reviewer launch, so none of this is recorded. This is the same class as R-AB-007.
- Gate 4: the named acceptance test `test_publish_refuses_non_rising_version` (CORRECTIONS.md:130) does not exist as a named test. It is four unnamed `check()` lines inside `test_config_store_publish_and_refresh` (test_config.cpp:247-271), so `ctest -R` for the named test finds nothing, unlike the four snapshot-slot tests, which do carry their contract names.

### R-AB-025 — P3 — CONFIRMED — core/config/store.hpp:130, :43
**`kConfigReaders = 8` makes `kSlots = 10`, so every `ConfigStore` now embeds ten ~9.2 KB snapshots — about 92 KB where the seqlock held one copy, with no note of the change.**
- `sizeof(ConfigEntry)` is 72 (8 + 8 + 1 + 7 padding + 48), × 128 entries + 16 = 9,232 bytes per slot; `SnapshotSlots<ConfigSnapshot, 8>::kSlots` is 10, and `slots_` is `alignas(kCacheLine)`.
- Not a correctness defect, but it is a 10× growth of an object that tests instantiate on the stack (test_config.cpp:210, :282, :407) and it silently costs L2 residency on the path the header advertises as "0.63 ns". The header's cost discussion (:6-11) still describes the single-copy seqlock.
- **Suggested regression test:** a `static_assert` or printed line pinning `sizeof(ConfigStore)`, so the next change to `kConfigReaders` shows its cost.

---

## app/ — price text, dataset merge and the two writers (CX02-A1a/A1b/A2/A2b)

Checked and correct: `replace_file_checked`'s rename-over-target with no remove-first window and its
WriteTemp/CloseTemp split (dataset_merge.hpp:100-130); `bar_key`'s date-only key for dailies (:140-143);
`bar_is_complete`'s 15:30 IST settle (:159-171); `select_names`' exact match and its `unknown` list, which
`kite_update_main.cpp:177-187` does refuse on, including the empty-selection case; `parse_count`'s
whole-string `from_chars`; the strict token parse in `kite_fetch_main.cpp:310-321` and
`kite_ticker_main.cpp:162-186`; and the 512-byte body caps. The `--force` month check runs over every
month before any is written (`kite_fetch_main.cpp:572-584`), which is the right order.

### R-AB-026 — P2 — CONFIRMED — app/price_text.hpp:76-84; app/dataset_merge.hpp:332-341
**A price of zero is a valid bar to both the formatter and the merge, and a zero close makes the merge return `Same` — so a real stored bar is silently preferred over a fresh one, or a row of zeros is added.**
- `format_price` refuses NaN, infinity, negatives and values at or above the ceiling. It does **not** refuse zero: `std::signbit(v) && v != 0.0` (:79) exempts it deliberately, and `fmt(-0.0) == "0"` is asserted as correct (test_price_text.cpp:152-153). So a candle whose OHLC Kite returned as 0 passes `append_prices` and is written as `,0,0,0,0,`.
- `merge_bar` then guards the log ratio with `(a > 0.0 && b > 0.0) ? ... : 0.0` (:334-335), so a zero on **either** side gives `d = 0.0`, and `return d > 0.0 ? Rounding : Same` (:341) gives **Same**: nothing is marked dirty, and `kite_update_main.cpp:360`'s `case Same: break;` counts it as nothing at all. A fresh, correct bar offered against a stored zero is discarded with no output.
- **Trigger:** any candle Kite returns with a zero price — a halted or newly listed contract, or a malformed row. The fetcher already treats an all-zero **volume** as a refusal requiring an operator decision (`kite_fetch_main.cpp:497-515`, "Kite returns 0 for both cases and cannot distinguish them, so this REFUSES rather than guessing"). The identical argument for a zero **price** is not applied anywhere.
- **Expected (gate 7):** a zero price is not a price. `format_price` should refuse it (`OutOfRange`, or a new `NotAPrice`), and `merge_bar` should not silently answer `Same` when either close is unusable.
- **Suggested regression test:** `zero_price_is_refused` in test_price_text; and in test_dataset_merge, merge a real bar against a stored row whose close is `0` and assert the result is not `Same`.

### R-AB-027 — P2 — CONFIRMED — app/dataset_merge.hpp:175-183, :332-341
**`close_of` returns 0.0 for every row it cannot parse, and that 0.0 is fed straight into the `Same` path. A stored row the reader does not understand always beats the fetched bar, silently.**
- `close_of` returns 0.0 when the row has fewer than five fields (:179) and otherwise hands the text to `std::atof`, which returns 0.0 on anything non-numeric with no error channel.
- **Trigger:** a stored row like `2026-09-08,,,,,` (a bar an older tool wrote with empty prices), a row whose close column holds `nan` or `NA`, or a legacy file whose columns are in a different order. `load_file` keys the row happily — it only requires a stamp of 10 characters or more (:264-267) — so `unkeyed` stays 0 and `writable()` stays true. The bar is not blocked, not replaced, not counted: `MergeResult::Same`.
- **Expected:** an unparseable close belongs with `unkeyed`/`duplicate_keys` — a row this reader cannot fully understand blocks the rewrite, which is the rule the header already states at :28-31 and which dataset_merge applies to the *stamp* column and not to the *close* column.
- **Actual:** the one number the merge compares on has no failure channel at all, and its failure value (0.0) is indistinguishable from a legitimately-zero price (R-AB-026).
- **Suggested regression test:** `unparseable_close_blocks_rather_than_matching` — load `2026-09-08,,,,,` and assert `merge_bar` for that day is not `Same` and the file is not writable.

### R-AB-028 — P2 — CONFIRMED — app/dataset_merge.hpp:378-381; app/kite_update_main.cpp:279-292, :329-338, :390-402
**`NothingNew` cannot tell a holiday from an empty answer, and the unannotated `from_day` clamp guarantees the window is always inside the tolerance. A permanently broken series reports success every run, for ever.**
- `judge_coverage` returns `NothingNew` for `got.empty()` whenever `t - f <= max_gap_days` (:379-380). `kite_update_main.cpp:329` passes 5.
- The window's start is the last **stored** day (`from_day = d`, :287), so on a daily top-up `t - f` is 0 to 3 and never exceeds 5. Every empty answer over that window is `NothingNew`.
- `NothingNew` is not in the refusal set at :330 — only `Incomplete` and `Malformed` are — so the merge loop runs over zero candles, the line prints `+0 ~0`, and `++series_done` at :401 counts the series as **updated**. Exit code 0.
- **The clamp makes it certain.** `if (from_day > today) { from_day = today; }` (:290) is a bare clamp with no rule-11 annotation, in the same change set where the project's own BoundsAudit forced one onto `force_covers_month` (CHECKPOINT.md:38-39). Its trigger is a stored row dated in the future — a mis-stamped vendor row, or a machine whose clock is behind — and the result is a zero-width window, an empty answer, `NothingNew`, and a series that silently stops updating while reporting success on every run.
- **Expected (rule 11 / rule 9):** `NothingNew` should mean "the window contained no trading session", decided against a calendar, not against the answer being empty; and an empty answer over a window that does contain sessions should be counted and printed, with consecutive occurrences surfaced. The clamp should refuse (`last_stamp` is in the future is a corrupt series, not a zero-day window).
- **Actual:** test_dataset_merge.cpp:234-236 pins `judge_coverage({}, "2026-09-12", "2026-09-14", 5) == NothingNew` as correct, which codifies the hole.
- **Suggested regression test:** `empty_answer_over_trading_days_is_not_nothing_new`, plus `future_last_stamp_refuses_rather_than_clamping`.

### R-AB-029 — P2 — CONFIRMED — app/kite_fetch_main.cpp:246-272
**`--dump-instruments` still does remove-then-rename and still ignores the temp's close, in the same file whose new comment says that class of failure is gone. The file it can destroy is the instrument master.**
- :260-267: `rename(tmp, dump, rn); if (rn) { remove(dump, rn); rename(tmp, dump, rn); }`. If the second rename also fails — a reader holding the path, which is the exact case the block above it names — the target is **already deleted** and only `<dump>.tmp` remains. That is C14-012 and C14-005's second trigger, unchanged.
- :247-259: the temp is written with `out << r->body; if (!out) {...}` and the stream is closed by the destructor **after** the check, so a close failure is unchecked; and the write-failure path returns without removing the temp. `replace_file_checked` — in this very translation unit's `#include`, and used 150 lines later at :618 — checks close as `CloseTemp` (dataset_merge.hpp:113-121) and cleans the temp up.
- **Why it matters:** `data/instruments.csv` is where every lot size, tick size, strike step and expiry comes from (rule 1). Losing it takes the spec store with it.
- **Expected:** `replace_file_checked(dump, r->body)`. The comment at :200-203 ("the target is either the old file or the whole new one and never a partial write") is false for the fallback path as written.
- **Suggested regression test:** in test_dataset_merge, `replace_checked_never_removes_the_target_first` already exists (test 1's third case); the fix is to route this call through it.

### R-AB-030 — P3 — CONFIRMED — app/dataset_merge.hpp:93-95, :106, :251, :349-354; app/tests/test_dataset_merge.cpp:41-51, :70
**The temp is opened in TEXT mode with a stated reason that is false on Windows, and the test cannot see the difference because it reads and writes in text mode too.**
- `load_file` strips `\r` from every row (:251) and `render` joins rows with `'\n'` (:349-354). On Windows, `std::ofstream` in text mode translates every `'\n'` to `"\r\n"`, so a dataset file whose rows had LF endings is rewritten with CRLF endings on the first merge that touches it. That is the opposite of the comment's claim at :93-95 ("a rewrite should not change the line endings of the rows it did not touch").
- The test is blind: `spit` (:48-51) and `slurp` (:41-46) both use text-mode streams, which translate in both directions, so `slurp(target) == "time,open\nnew\nrows\n"` (:70) passes whichever ending is on disk.
- **Suggested regression test:** open with `std::ios::binary` in `slurp`, and assert the exact bytes for a file seeded with LF endings.

### R-AB-031 — P3 — CONFIRMED — app/dataset_merge.hpp:407-414
**`force_covers_month`'s rule-11 annotation proves the clamp only for a month that has already started; for a future-dated month it lets `--force` erase a file the window does not intersect at all.**
- `need_to = last < today ? last : today` (:413) clamps to `today` for **every** month whose last day is after today, not just the month in progress. For `month = "2027-05"` and `today = 2026-09-16`, `need_to` is 2026-09-16, so `f <= m0 && t >= need_to` is satisfied by `--from 2020-01-01 --to 2026-09-16 --force`, and `kite_fetch_main.cpp:574-575` then permits `2027-05.csv` to be replaced wholesale — the C14-007 failure, for a month the window never covered.
- The annotation at :410-412 ("a stored month cannot hold a row dated after today") is an assumption about the **data**, not a proof about the **code**; a mis-stamped row is exactly what creates one, and it is the same stamp-form worry C14-006's note raises.
- Secondary: the refusal's strength depends on the machine's wall clock. A clock set back moves `need_to` earlier and widens what `--force` may delete. That dependency is not stated where the clamp is.
- **Suggested regression test:** `force_refuses_a_month_the_window_does_not_reach` — `force_covers_month("2027-05", "2020-01-01", "2026-09-16", today)` must be false.

### R-AB-032 — P3 — CONFIRMED — app/dataset_merge.hpp:102-103, :123-128
**A fixed `path + ".tmp"`, and the merged result is deleted on a rename failure.**
- Two concurrent `altair_kite_update` runs (or an `altair_kite_fetch` into the same directory) contend for one temp name, with no owner in it. Same shape as R-AB-013.
- On `Rename` the temp is removed (:126), so the only copy of the merged month is destroyed. The old defect was the mirror image — C14-005's second trigger left a `.tmp` nobody read — and the message at `kite_update_main.cpp:383-386` now names no recovery path because there is nothing left to name.
- **Suggested:** a unique temp name, and on a rename failure keep the temp and print its path, since the alternative is re-fetching the whole window.

### R-AB-033 — P3 — CONFIRMED — app/price_text.hpp:60-68, :82-96
**`kPriceTextMax`'s proof is stated for `[1e-29, 1e15)` but `format_price` accepts `[0, 1e15)`, so for a value below 1e-29 the refusal comes out as `BufferTooSmall` — the caller's fault, for a value that is the problem.**
- A subnormal or very small positive value in `chars_format::fixed` needs hundreds of characters (5e-324 is "0." plus 323 zeros plus digits), `to_chars` returns `value_too_large`, and :94-95 maps that to `BufferTooSmall`. The refusal happens — the header says so at :66-67 — but it is reported as the wrong cause, and `append_prices` turns it into "unwritable price", which `kite_fetch_main.cpp:524-525` prints as "UNWRITABLE PRICE (NaN, infinite or negative)": three causes, none of them the actual one.
- It also makes writability depend on the caller's buffer: the same value with a 4 KB buffer would succeed and write `0.000…0005` into the CSV.
- **Expected:** a `kPriceTextFloor = 1e-29` refused as `OutOfRange`, symmetric with the ceiling, so the proof's domain and the function's domain are the same set.
- **Suggested regression test:** `price_text_refuses_below_the_floor` — `format_price(1e-30, buf, kPriceTextMax)` is `OutOfRange`, not `BufferTooSmall`.

### R-AB-034 — P3 — CONFIRMED — gate 2 / gate 4; app/price_text.hpp:46-54 vs prompts/cx02/README.md:43
**The frozen `PriceTextError` gained an enumerator in the MIDDLE, and three of the four named A1a/A2 tests do not carry their contract names.**
- Frozen: `{ NotFinite, Negative, BufferTooSmall }`. Actual: `{ NotFinite, Negative, OutOfRange, BufferTooSmall }` — `BufferTooSmall`'s ordinal changes from 2 to 3. CX-02's own convention for the same situation is to append (README.md:97, "IntentError gains `DuplicateField, UnknownField, TooLong, OutOfRange` (appended)"). CHECKPOINT.md:29-30 records the `kPriceTextMax` and `chars_format::fixed` decisions but not the new enumerator.
- Named tests: `price_text_refuses_nan_inf_negative` is `price_text_refuses_what_is_not_a_price`; `replace_checked_leaves_target_intact_when_temp_cannot_open` and `load_counts_short_rows` are unnamed `check()` lines inside test 1 and test 5; `symbols_list_is_exact_not_substring` is `symbols_are_exact_and_days_are_numbers`. Same class as R-AB-024's second half — `ctest -R` for the contract's own test names finds nothing.

### R-AB-035 — P3 — CONFIRMED — app/dataset_merge.hpp:229-230, :257-260, :316-318, :332-341
**Two smaller merge defects.**
- (1) `load_file` overwrites `s.header` with **every** file's header (:259), so a new month file created by `merge_bar` (:316-318) inherits whichever file was read last in name order. With `2026-08.csv` at `time,open,high,low,close,volume` and `2026-09.csv` at `...,volume,oi` — the exact pair test_dataset_merge.cpp:113-124 builds — a candle for 2026-10 creates a file with the 7-column header over 6-column rows. `Series::header`'s doc ("the one the series already uses", :229-230) is true only when they agree, which is the case the per-file-header fix exists because they do not.
- (2) The 1 bp material-change test reads the **close** only (:332-341), so a stored bar with a wrong open, high or low is never corrected and reports `Same`. Test 4 varies only the close, so a regression narrowing the comparison further would not show.

### R-AB-036 — P3 — CONFIRMED — app/kite_fetch_main.cpp:612-614, :630-644
**A fetch that drops tens of thousands of candles into skipped months still exits 0.**
- The skip counts and prints what it dropped, which is the rule-11 fix and is right. But `return write_failed == 0 ? 0 : 1;` (:644) ignores `skipped`, so a script or a desktop `QProcess` that checks the exit status sees success for a run whose own output says "that data is now lost". `kite_update_main.cpp:420` does the opposite and returns 4 whenever anything failed.
- **Suggested:** a distinct non-zero exit when `skipped > 0`, or an explicit `--allow-skips`.

### R-AB-037 — P3 — CONFIRMED — app/main.cpp:89-96; gate 3
**The only executable caller of `ConfigStore::publish` overwrites the loader's version with a counter derived from the store, so the C02-004 guard can never fire in production — and the file is in no card manifest.**
- :89 sets `cfg.set_version(1)` (the loader's identity); :95 immediately replaces it with `store.version() + 1`. Every publish therefore rises by construction, whatever the content, so "the same version with different content is refused" — the property test_config.cpp:249-262 was written to pin — is unreachable from the only caller. `ConfigSnapshot::version()`'s contract ("Monotonic generation, assigned by whoever built this snapshot", config.hpp:251-252) no longer holds for it either.
- The re-run problem the comment names is real; the fix is to make the loader own the counter (or to refuse a republish whose `content_hash()` is unchanged), not to derive the identity from the store being published into.
- **Manifest:** `app/main.cpp`, `app/kite_quote_main.cpp` and `app/kite_ticker_main.cpp` are modified and appear in no card manifest. CHECKPOINT.md:77-78 mentions the quote and ticker edits as "C14-016 remainder" without declaring one, and the `app/main.cpp` change — which exists only because of CX02-C1, whose manifest is `core/` only — is recorded nowhere. A card manifest may not span two component directories (CLAUDE.md), so this needed its own card.

---

## desktop/ — the ticket, the halt panel and the watchlist pick (CX02-B3/B4/B4b/B5)

Checked and correct: `intent_line`'s field order and its `%1`…`%12` substitution (the two-digit markers
resolve as 10, 11, 12, and the multi-argument `arg` calls do not rescan substituted text, so a symbol
containing `%4` cannot inject); `intent_timestamp`'s UTC conversion; `rupees_text`; the HTML escaping in
`confirmation_text` (:201-202) and in `halt_state_text` (:174-179); `AppendResult::Unset = 0` and
`HaltFileState::Unreadable = 0`, both of which make a zeroed result read as the unsafe answer; the
`QSaveFile` write; `snap_too_aggressive`; and the `price_ > maximum()` refusal replacing the old silent
narrowing. `append_intent`'s `f.error()` check after `close()` is correct — `QFileDevice::close()` keeps
an error from a failed flush rather than clearing it.

### R-AB-038 — P2 — CONFIRMED — desktop/kill_switch.hpp:352-363, :196-247, :26-29, :294-304
**The CLEAR path passes `reason_out == nullptr`, so clearing a halt asks for NO reason at all — the exact inverse of the asymmetry this file states twice and builds its whole design around.**
- `do_clear` calls `confirm_phrase(..., QStringLiteral("RESUME TRADING"), nullptr)` (:361). Inside `confirm_phrase`, `if (reason_out != nullptr) { form->addRow("Reason", reason); }` (:212-214) means the Reason field is **not even added to the dialog**, and `reason_ok = reason_out == nullptr || ...` (:231-232) is unconditionally true. Meanwhile `do_request` — the *safe* direction — passes `&reason` (:332) and does demand one.
- The file asserts the opposite in two places: the header at :26-29 ("CLEAR admin only, a DIFFERENT and longer phrase, and it refuses while the reason field is empty, because 'why is it safe to resume' is the question a clear exists to answer") and the panel's own on-screen text at :300-303 ("So clearing is admin-only, takes a *different* phrase, and refuses without a reason"). The UI tells the operator a safety property the code does not implement.
- **Trigger:** any admin clear. Type `RESUME TRADING`, press OK — no reason is asked for, and `QFile::remove` (:365) then deletes the request file, so *who halted, when and why* is gone with it. Nothing records who cleared it either.
- **Expected:** `do_clear` passes a `QString*` like `do_request` does, and the clear is recorded (an audit line, or a `cleared_at`/`cleared_by` file) rather than being a bare `remove`.
- **Suggested regression test:** `clear_refuses_without_a_reason` — extract the enablement predicate from `confirm_phrase` into a pure `bool confirm_enabled(bool phrase_matches, bool reason_required, QString reason)` and assert it is false for the clear direction with an empty reason.

### R-AB-039 — P2 — CONFIRMED — desktop/kill_switch.hpp:310-317, :340-348, :365-372, :377-382
**The 2-second poll this card added erases the "could not write the request" notice within two seconds. The one message that says a halt was NOT recorded is transient.**
- `do_request`'s failure path writes its red notice straight into `state_` (:342-347), and `do_clear`'s does the same (:367-371). The new `QTimer` at :312-316 fires every 2,000 ms into `refresh()`, which unconditionally does `state_->setText(halt_state_text(k))` (:379) and overwrites it.
- So the sequence is: the operator types `HALT`, the write fails, a red "Could not write the request" appears, and within two seconds the panel is back to green "No halt requested." — which is *true about the file* and exactly the wrong thing for the operator to be left looking at after asking for a halt. Rule 9: this is the failure being made quiet, by the same card that made the state fail-safe.
- Both defects were introduced together and neither existed before: C17-014's complaint was that the panel *never* refreshed; the fix made it refresh over the error.
- **Expected:** a sticky notice — a separate label, or a latched `last_action_error_` that `refresh()` renders alongside the state until the operator acts again.
- **Suggested regression test:** not reachable without the widget as written; the fix makes it reachable — assert `refresh()` preserves a latched action error.

### R-AB-040 — P3 — HYPOTHESIS — desktop/panels.hpp:1026-1038; desktop/order_ticket.hpp:448-461
**The spec revocation is conditioned on `SpecState::Blocked` alone. The argument the comment gives applies equally to a master lookup that stops resolving, and that case emits nothing.**
- At :1026-1031 a `Blocked` row emits `contractPicked(token, name, 0, 0, QString())`, which `set_contract` turns into `specs_.erase(token)` (:451-455). At :1032-1033 a row whose `p.found && lot > 0 && tick > 0 && !exchange.isEmpty()` test **fails** emits no `contractPicked` at all, so whatever spec the ticket holds for that token survives the pick.
- The B4b comment at :1017-1024 argues "a row picked while resolvable and blocked afterwards must not leave its old spec in the ticket". The same sentence is true with "blocked" replaced by "no longer resolvable".
- **Why HYPOTHESIS:** I could not construct the trigger. `master_` is a `MasterIndex` built once, and `Watchlist` has no unblock, so within a session a token that once resolved cannot stop resolving. It becomes reachable the moment the master is reloaded (a new `data/instruments.csv` after `--dump-instruments`) or an unblock is added.
- **Suggested regression test:** make the revocation unconditional — emit the empty spec on every path that does not emit a real one — and assert `!has_spec(tok)` after a pick whose master lookup fails.

### R-AB-041 — P2 — CONFIRMED — desktop/panels.hpp:1032-1037; desktop/watchlist.hpp:24-30, :50-51, :156-176
**C17-021 was closed by promoting the row to `Resolved` from the instrument DUMP, which is the half of the fix `watchlist.hpp` explicitly forbids. An `Unresolved` row is now silently promoted by the act of clicking it.**
- `emit_pick` now calls `list_.resolve(token, p.lot_size, p.tick_paise)` (:1034) whenever `state != Resolved`, and paints the Spec column green (:1036-1037). `Watchlist::resolve` sets `r.spec = SpecState::Resolved` (watchlist.hpp:171).
- `SpecState::Resolved` is documented as "Resolved against the point-in-time spec store" (watchlist.hpp:50-51), and the file's header says an unresolved row is "NEVER SILENTLY PROMOTED" (:24-30). `p` comes from `master_.find(token)` — the `data/instruments.csv` dump parsed by the UI at startup — not from `instruments/`, which `desktop/` cannot link by construction.
- C17-021's finding was that *the label and the orderability disagree*. There were two ways to close it: make the label true, or make the behaviour true. This card made the label true by lowering it to whatever the UI's own CSV says, which is the reading rule 1 argues against ("All from the point-in-time spec store"). The row now reads `resolved` in green on the strength of a file the UI parsed itself.
- **Expected:** either a pick does not promote (the row stays `watch only` and the ticket refuses, which is what watchlist.hpp says today), or `SpecState::Resolved`'s contract is amended to say "resolved against the instrument master" and the difference from the spec store is written down.
- **Suggested regression test:** `a_pick_does_not_change_spec_state`, or — if promotion is the decision — a test asserting the promoted row's lot and tick equal the spec store's, which needs a spec-store seam the UI does not have.

### R-AB-042 — P2 — CONFIRMED — desktop/order_ticket.hpp:614-617, :620-627, :629-630
**`at` and `id` are stamped BEFORE the modal confirmation and the record is written after it, so every intent carries the instant the dialog opened. With B2's TTL that turns a confirmed request into a quarantined `Expired` one, logged as PENDING.**
- :614 `d.at = intent_timestamp(QDateTime::currentDateTimeUtc());` and :615-617 `d.id = ms-token`, then :620 `QInputDialog::getText(...)` blocks until the operator types the phrase, then :629 appends.
- **Trigger:** the operator opens the confirmation and reads it, or is interrupted, for longer than `DrainPolicy::ttl_ns`. `IntentDrainer` expires on the age of `at` (README.md:124), so the drainer quarantines it as `Expired` while the ticket has already logged `PENDING  <at>` (:651-654). The request is never executed and the operator has been told it is pending — the failure the whole "REQUEST/PENDING" vocabulary exists to prevent (:21-27).
- It also makes the audit trail wrong in the safe-looking direction: an intent is recorded as older than the decision that produced it, and `id`'s millisecond component no longer matches the write.
- **Expected:** stamp `at` and `id` after the phrase is accepted and immediately before `append_intent`. The confirmation does not display `at`, so nothing is lost by moving it.
- **Suggested regression test:** `at_is_stamped_at_the_write_not_at_the_dialog` — inject a clock into `submit()` (or extract `build_draft(now)`), advance it across the confirmation, and assert the emitted `at` is the later instant.

### R-AB-043 — P2 — CONFIRMED — desktop/order_ticket.hpp:129-139, :247, :563, :613
**The writer emits control characters it knows the reader refuses. A newline anywhere in `symbol` or `by` splits one record into two lines, both of which oms quarantines, while the ticket logs PENDING.**
- `intent_escape` escapes only `"` and `\` (:132-137). A `\n`, `\r` or `\t` is copied through verbatim into the line, and `append_intent` writes it as one byte string (:247). The queue then holds two partial lines; B1's grammar refuses control characters (README.md:83), so **both** halves are quarantined and the request never happens.
- `d.symbol` comes from `symbol_->currentText()` (:563), which originates in the instrument master — external data — and `d.by` from `user_` (:613), which `UserStore::add` accepts as any `QString`. G17:319 raised exactly this ("a newline in a user name would split a line") and CX02-B3 owned this file without addressing it.
- The header's own rule at :126-128 is the right one and is simply not enforced: "Anything else is refused there rather than half-decoded, so anything else must not be written here." Nothing here refuses it.
- This is the writer-side half of R-AB-006, with a sharper consequence: R-AB-006's out-of-class `by` produces one quarantined line, a control character produces two and can make the *following* record unparseable too.
- **Expected:** `submit()` refuses a draft whose fields are outside the classes B1 accepts, before the confirmation, and says which field.
- **Suggested regression test:** `a_field_the_reader_would_refuse_is_refused_here` — assert `intent_escape`/`submit` refuse `"NIF\nTY"` rather than emitting it; add the same bytes to `intents.jsonl` so `test_order_intent` proves the reader refuses them.

### R-AB-044 — P2 — CONFIRMED — gate 4; desktop/tests/test_order_ticket.cpp:285-324 vs prompts/cx02/README.md:151-155
**`submit()` — where B4's entire fix lives — has no test. Two of B4's four named tests do not exist, and B3's `append_reports_write_failure` does not test a write failure.**
- Named by the card: `confirmation_names_price_type_product_validity` (present, :258-281), `limit_cleared_on_contract_change` (present, :285-324), **`unset_limit_refused` (absent)**, `set_price_does_not_narrow` (present only as two unnamed checks at :315-319).
- Every refusal B4 added is in `submit()` (:569-607): no spec, no limit on a LIMIT order, off-tick limit, aggressive snap. None is executed by any test, because `submit()` opens a modal `QInputDialog`. The tests reach only the pure `confirmation_text` and the pure `snap_too_aggressive` — so a regression deleting any of the four `return`s at :576, :588, :596 or :606 passes the suite.
- `[6] append_reports_failure_instead_of_pending` (:225-238) covers `OpenFailed` and `LockTimeout` only. `AppendResult::WriteFailed` — the C17-011 defect itself, a failed write or flush reported as PENDING — is never produced by any test.
- **Expected:** extract the refusals into a pure function, e.g. `std::optional<QString> refuse_request(const Spec&, bool market, int limit_paise, bool buy, TickSnap)`, and call it from `submit()`; then all four are testable without a window. For `WriteFailed`, a read-only or full target, or a seam on the `QFile`.

### R-AB-045 — P2 — HYPOTHESIS — desktop/kill_switch.hpp:113-116
**`read_kill_request` has no path that turns a FAILED existence probe into `Unreadable`. A `data/` directory that cannot be stat'ed reads as `Absent`, and the panel prints "No halt requested" in green.**
- `if (!QFileInfo::exists(path)) { k.state = HaltFileState::Absent; return k; }` is the only producer of `Absent`. Qt's `QFileInfo::exists` returns `false` both for "no such file" and for "I could not find out" — a permission change on `data/`, or a network path holding it being unavailable. There is no error channel examined anywhere.
- This is R-AB-009's shape in the UI: C17-014's fix separated *malformed* from *absent* and left *undeterminable* on the absent side, which is the fail-safe direction reading as safe — the finding's own title.
- **Why HYPOTHESIS:** the conflation in the code is certain; that `QFileInfo::exists` returns false rather than throwing or reporting on a stat failure is a Qt-internals claim I did not verify by running.
- **Suggested regression test:** `a_path_that_cannot_be_stated_is_unreadable` — point the reader at a path under a *file* (`something.txt/kill_request.json`) and assert the state is not `Absent`.

### R-AB-046 — P3 — CONFIRMED — desktop/order_ticket.hpp:225-257
**B3 replaced an OS-level atomic append with a remembered offset, a seek, and an advisory lock that breaks itself after ten seconds.**
- The file is opened `ReadWrite` (:233), `size` is captured at :236, the tail byte is read at :240, and the write goes to `f.seek(size)` at :249. Any writer that appends between :236 and :249 has its bytes **overwritten**, where the old `QIODevice::Append` could not. The `QLockFile` serialises only writers that take the same lock: `oms/intent_queue.hpp` reads this file and never takes it, and a second tool or an editor takes nothing.
- `lock.setStaleLockTime(10'000)` (:228) means a lock whose holder has been suspended for ten seconds is treated as stale and broken by the next writer — a debugger breakpoint, a machine sleep, or a slow modal in the same process — after which two writers both hold the same `size`.
- **Expected:** read the tail under the lock with a read-only handle, then reopen with `QIODevice::Append` for the write, so the kernel guarantees the append even if the advisory lock is lost. Or re-read `f.size()` immediately before the seek and restart if it moved.

### R-AB-047 — P3 — CONFIRMED — desktop/order_ticket.hpp:183-203
**The confirmation shows every field the card named and still does not show the quantity. The one number that decides SIZE is absent from the safety step, which is the shape of C17-010 itself.**
- It shows side, lots, symbol, exchange, type, limit in rupees and paise, product and validity — the card's list (README.md:151) exactly. It does not show the lot size, the resulting quantity (`lots x lot`), or the notional, all of which the ticket holds in `specs_` (:666-671) at the moment the dialog is built.
- "1 lot of NIFTY26SEPFUT" is 75 units at one expiry and 65 at another after a lot-size revision — the number rule 1 exists for — and the operator confirming the request cannot see it.
- Root cause is the card, not the implementation: the frozen list omitted it. Worth a correction card rather than a code-only fix.

### R-AB-048 — P3 — CONFIRMED — desktop/tests/test_terminal.cpp:139-171; desktop/tests/test_kill_switch.cpp:71-95
**Two test gaps.**
- `[4b]`'s four substantive checks sit behind `if (resolvable >= 0 && table->rowCount() >= 2)` (:150). On a box whose `data/instruments.csv` resolves no seeded row, the block is skipped and only the `(a seeded row resolves against the master)` check fails — the C17-021 assertions themselves never run. The suite already carries this pattern (G17:322); this card added another instance rather than pinning the fixture.
- The counter at :146-148 only counts `lot > 0`, so `check(specs == before, "a BLOCKED row sends no usable spec")` cannot distinguish "no signal" from "the revocation signal". The following `!has_spec(tok)` check is the one carrying the weight.
- test_kill_switch covers malformed JSON, a non-object, and an empty file — all of which **open successfully**. README B5's named `unreadable_request_reads_unknown`, whose trigger C17-014 states as "an existing but locked file", has no test: the `!f.open(...)` branch (kill_switch.hpp:119-121) and the `f.error()` branch (:123-125) are never exercised.

### R-AB-049 — P3 — CONFIRMED — desktop/order_ticket.hpp:435-438, :563
**An invented `"token N"` label is written into the intent as the tradingsymbol.**
- When a caller passes an empty name, `set_instrument` labels the combo entry `token 17512194` (:436). `submit()` then copies `symbol_->currentText()` into `d.symbol` (:563), and B1's grammar accepts printable ASCII 1-64, so the line parses and reaches oms with a tradingsymbol that names no contract.
- The header's claim at :412-415 ("This never invents a name for a token it does not recognise: an unnamed instrument shows its token, which is unambiguous, rather than a guess that reads like a contract") is about the *display*; nothing stops the display string becoming the requested symbol. oms re-derives from the token, so nothing trades wrongly — the damage is an audit trail naming a contract that does not exist.
- **Suggested:** carry the label and the symbol separately, and refuse a submit whose symbol was invented.

---

## oms/tests/test_order_intent.cpp (CX02-B1) — the one in-scope file neither section had reached

This is the strongest test in the change set and I could not fault its substance. `refused_as` pins the
exact `IntentError` rather than "it failed" (:104-117); every variant is one edit away from a real vector
(:75-82), so a refusal is attributable to that edit; test 5 carries a working copy of the reader it
replaces and shows it reading `lots 3 at 5,711,490 paise` out of the torn line (:234-246), which is
planted-violation evidence of the kind PROTOCOL.md asks for; and the `at_ns` constant at :154 is right
(day 20704 x 86,400 + 46,351 seconds). The narrowing cases are chosen, not sampled — 4,295,223,561 is
exactly 2^32 + 256265, so the old code would have produced a *different real instrument* (:261-264).

### R-AB-050 — P3 — CONFIRMED — oms/tests/test_order_intent.cpp:18-19, :196-202, :310-347
**Three gaps, one of which leaves R-AB-011's boundary defect untested from both sides.**
- **The 1,024-byte boundary is only tested from above.** :345-347 pads with 1,100 spaces and expects `TooLong`. There is no line of *exactly* `kMaxIntentLineBytes` asserted to be accepted — which is the case R-AB-011 shows behaves differently with LF and with CRLF. Neither this test nor `test_intent_queue` covers it, so that defect is invisible to the suite from either end.
- **`by` and `symbol` are never taken to the edges of their classes.** `id` is tested at 65 characters (:341-344) and with a space (:337-340), but `by`'s `[A-Za-z0-9._@-]{1,64}` and `symbol`'s printable-ASCII-1-64 have no boundary or out-of-class case at all — the gap R-AB-006 names on the writer side and R-AB-043 sharpens. A 64-byte symbol accepted and a 65-byte one refused would cost two lines.
- **The header describes a test that no longer exists.** :18-19 says "Test 3 is the drain: offsets, a partial final line, and the fact that a malformed record is COUNTED rather than skipped". Test 3 is now a one-line pointer at `test_intent_queue.cpp` plus a single refusal check (:196-202). The drain moved in CX02-B1 (CHECKPOINT.md:54-55) and the header did not.

---

# Summary

## Counts by severity

| Severity | Count | of which HYPOTHESIS |
|---|---|---|
| **P1** | **0** | 0 |
| **P2** | **19** | 3 (R-AB-006, R-AB-015, R-AB-045) |
| **P3** | **31** | 2 (R-AB-017, R-AB-040) |
| **Total** | **50** | 5 |

P2 findings: R-AB-001, 007, 008, 009, 015, 016, 019, 022, 026, 027, 028, 029, 038, 039, 041, 042, 043, 044, 045.

**No P1.** The audit's P1s that CX-02 set out to close — C14-001 (six-significant-digit prices),
C13-001/C13-002 (lost fills), C13-010 (the exit ladder's fictional gap P&L), C17-010 (the confirmation
that hid the price) — are genuinely closed, and the planted-violation evidence in CHECKPOINT.md:42-50
holds up against the code. The sharpest remaining findings are R-AB-038 (the halt panel's CLEAR asks for
no reason, inverting what the file says twice), R-AB-039 (the new poll erases the "halt was NOT recorded"
notice in two seconds), R-AB-044 (`submit()`, where B4's whole fix lives, has no test), R-AB-042 and
R-AB-043 (a confirmed request that oms will quarantine while the ticket logs PENDING), R-AB-027/028
(dataset rows and whole series lost silently), and R-AB-008 (accepted intent ids burned by a failed save).

## Cross-cutting patterns

1. **Half a conflation fixed.** C02-004, C13-008(a), C17-014 and C14-006 are each "X looked like Y".
   Each fix separated one pair and left the neighbouring pair joined: R-AB-009 (`NoState` vs unstattable),
   R-AB-019 (`Unchanged` vs never-published), R-AB-045 (`Absent` vs unstattable), R-AB-028
   (`NothingNew` vs an empty answer).
2. **The fix's own test pins the hole.** test_config.cpp:215-218 asserts R-AB-019's behaviour as correct;
   test_dataset_merge.cpp:234-236 asserts R-AB-028's; test_dataset_merge's text-mode `slurp` cannot see
   R-AB-030.
3. **Contract drift is unrecorded.** R-AB-007 (`IntentBatch`), R-AB-024 (C1's manifest and
   `test_config.cpp`), R-AB-034 (`PriceTextError` inserted mid-enum), R-AB-037 (three app files in no
   manifest). CHECKPOINT.md stops before Tier C entirely.
4. **Named acceptance tests are not named in the code.** R-AB-024, R-AB-034, R-AB-044 — `ctest -R` for a
   card's own test names finds nothing in four cards, and in B4's case the named test does not exist.
5. **Writers do not refuse what their readers refuse.** R-AB-006, R-AB-026, R-AB-043.

## Files fully reviewed in this continuation

`core/lockfree/snapshot_slots.hpp` · `core/lockfree/tests/test_snapshot_slots.cpp` ·
`core/lockfree/CMakeLists.txt` (diff) · `core/config/store.hpp` · `core/config/tests/test_config.cpp` ·
`app/price_text.hpp` · `app/dataset_merge.hpp` · `app/tests/test_price_text.cpp` ·
`app/tests/test_dataset_merge.cpp` · `app/kite_update_main.cpp` · `app/kite_fetch_main.cpp` ·
`app/main.cpp` (diff) · `app/kite_quote_main.cpp` (diff) · `app/kite_ticker_main.cpp` (diff) ·
`app/CMakeLists.txt` (diff) · `desktop/order_ticket.hpp` · `desktop/kill_switch.hpp` ·
`desktop/panels.hpp` (`emit_pick` :1011-1041, `block_instrument` :985-990) ·
`desktop/tests/test_order_ticket.cpp` · `desktop/tests/test_kill_switch.cpp` ·
`desktop/tests/test_terminal.cpp` (diff) · `desktop/CMakeLists.txt` (diff) ·
`oms/tests/test_order_intent.cpp`

Read for context, not claimed as coverage: `core/config/config.hpp`, `desktop/watchlist.hpp`,
`broker/kite_historical.hpp` (`parse_date`, `date_string`, `verify_coverage`, `read_number`,
`read_string`), `prompts/cx02/{README,CORRECTIONS,CHECKPOINT}.md`, and the frozen audit files
`G02_core_b`, `G14_broker_kite_tools`, `G17_desktop_shell_terminal`.

Reviewed by the first section and not re-examined here: `oms/order_state.hpp`, `oms/order_intent.hpp`,
`oms/intent_queue.hpp`, `oms/exit_ladder.hpp`, `oms/tests/test_order_state.cpp`,
`oms/tests/test_intent_queue.cpp`, `oms/tests/test_exit_ladder.cpp`.

**Nothing was built, run or tested** (the brief forbids it, and the build directories are in use by other
processes). No credential file, `.env`, or anything under `data/` was opened. The only file this review
wrote is this one.

