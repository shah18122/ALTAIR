# G13_oms findings -- CX-01 Phase 1 audit

Reader: claude-subagent:G13_oms · Date: 2026-09-14 · Baseline HEAD a34af5c5c9fa8c7498e8cc4df00907548a41e1a5
Mode: read-only static review. Nothing was built, run or tested. Kite API behaviour was not checked against live docs (no network).

## 1 Scope & coverage

- **Assigned:** 19 files, 4,655 lines (oms/ headers, CMakeLists, 8 test files, `tests/vectors/intents.jsonl`).
- **Fully read, every line, chunks of 300 lines or less:** all 19. No partial or unread files.
- **Hashes:** checked before and after reading. All matched the partition inventory, and nothing changed during the review.
- **Outside the group, for comparison only:**
  - `desktop/order_ticket.hpp` lines 100-159 (`intent_escape`, `intent_line`, `append_intent`).
  - Grep hits across the first-party dirs, used for integration.
  - Grep hits in `broker/` for the order endpoints.

## 2 Findings table

| ID | Sev | Class | File | Title |
|---|---|---|---|---|
| C13-001 | P1 | CONFIRMED DEFECT | oms/order_state.hpp | A partial fill during PendingCancel/PendingReplace erases the pending state. The following CancelAck/ReplaceAck is refused and the order stays "live". |
| C13-002 | P1 | CONFIRMED DEFECT | oms/order_state.hpp | A rejected cancel/amend REQUEST makes the ORDER terminal Rejected. Later fills are refused, so it believes it is flat while holding a position. |
| C13-003 | P2 | DESIGN GAP | oms/order_state.hpp | TimeWentBackwards refuses crossed messages if `ts` is event time. What `ts` means is never defined. |
| C13-004 | P1 | DESIGN GAP | order_state / kite_adapter / reconcile | No uncertain/unknown order state, no idempotency key, no order-level reconciliation |
| C13-005 | P1 | CONFIRMED DEFECT | oms/order_intent.hpp | An interrupted append followed by the next append makes one line that mixes two records and parses successfully |
| C13-006 | P2 | CONFIRMED DEFECT | oms/order_intent.hpp | Unchecked int64 accumulation (UB), and uint32 token narrowing to a different instrument |
| C13-007 | P2 | CONFIRMED DEFECT | oms/order_intent.hpp | Not a strict schema: first-wins duplicates, nested-key match, decimal/exponent truncation, string-typed ints, escape mis-decoding, empty/unenumerated strings |
| C13-008 | P2 | DESIGN GAP | oms/order_intent.hpp | Drain offset has no file identity. A missing file looks the same as an empty queue. No id dedupe, and the batch is unbounded. |
| C13-009 | P1 | DESIGN GAP | integration | Nothing drains intents, and no strategy->risk->OMS->broker pipeline exists. The queue has no expiry, so a future drainer would act on stale intents. |
| C13-010 | P1 | CONFIRMED DEFECT (gate 7) | oms/exit_ladder.hpp | On a gap, a stop exit is attributed at the stop level, not a price you could actually get. The test measures a fictional Rs 1.5 lakh. |
| C13-011 | P2 | DESIGN GAP | oms/reconcile.hpp | QuantityMismatch is documented "Fatal" but never trips the kill switch. `present` is unused. Product is not part of the key. |
| C13-012 | P2 | CONFIRMED DEFECT | oms/shortfall.hpp | Large kappa*T: sinh overflows, inf/inf gives NaN, and the int cast is UB. The comment claims overflow is handled. |
| C13-013 | P3 | CONFIRMED DEFECT (rule 11) | execution.hpp, shortfall.hpp | Out-of-range accessors silently clamp to slot 0 or to the last slot |
| C13-014 | P2 | DESIGN GAP | oms/execution.hpp | TWAP/VWAP/POV split raw units with no lot multiple, so F&O children cannot be placed |
| C13-015 | P2 | CONFIRMED DEFECT | oms/kite_adapter.hpp | `parse_order_id` ignores status and accepts `null`. The rupee parser accepts negatives and overflows. Snapshots have no bridge to the transition table. |
| C13-016 | P3 | CONFIRMED DEFECT | oms/execution.hpp | VWAP curve accepts +inf, giving NaN and a UB int cast |
| C13-017 | P3 | HYPOTHESIS | oms/throttle.hpp | The daily window is 24h from the first order, not the broker day. Modify/cancel are not throttled. Limit values were not verified. |

**How to read severity:** no file in oms/ has a non-test caller (C13-009). Every P1 here is a defect in code that would place, track or exit real orders once connected. **Current live impact is nil**, because nothing sends an order.

## 3 Finding details

### C13-001 -- partial fill while a cancel/amend is pending loses the pending state (P1, CONFIRMED DEFECT)

**Evidence**
- `oms/order_state.hpp:306`: `o.state = o.done() ? OrderState::Filled : OrderState::PartiallyFilled;` This runs unconditionally, including from PendingCancel and PendingReplace.
- `:176-180`: PartiallyFilled admits Fill, ReplaceSent, CancelSent, Expire and Ack only. It does not admit CancelAck or ReplaceAck.

**Trigger:** `open_order(Qty{150}, t0)`, then `apply(Ack, 10ms)`, then `apply(CancelSent, 20ms)`, then `apply_fill({75, 2408000, 25ms})`. That returns Changed and the state is PartiallyFilled. Then `apply(CancelAck, 30ms)` returns `unexpected(IllegalTransition)`, and `refused` becomes 1.

The same happens with ReplaceSent, then a partial Fill, then ReplaceAck (refused).

**Result:** the state machine believes a cancelled order is still working 75 units. `is_live()` is true and `leaves()` is 75. A second CancelSent is legal, which re-cancels a dead order. If the broker rejects that request, C13-002 turns the order into Rejected.

**Related points**
- A late fill out of Cancelled (`:193`, then `:306`) also "reopens" the order to the live state PartiallyFilled, even though the exchange has pulled it. Test 3 asserts this reopening as intended.
- Also related: a marketable order whose Fill arrives before its Ack goes PendingNew, then Filled. The late Ack is then refused (`:194-196`) and counted in `refused`. The header says a healthy order has no refusals.

**Existing test coverage:** `test_order_state.cpp` test 5 case c covers only a COMPLETING fill during PendingReplace, which correctly goes to Filled. Partial fills during PendingCancel/PendingReplace are not tested.

**Regression test:** the sequence above. Expected: CancelAck is accepted, the state is Cancelled, `cum_qty` is 75, `is_live` is false, and `refused` is 0. Same idea for ReplaceAck: expect PartiallyFilled with `refused` 0.

### C13-002 -- a rejected cancel/amend request makes the order terminal (P1, CONFIRMED DEFECT)

**Evidence**
- `oms/order_state.hpp:182-188`: PendingReplace and PendingCancel both admit `Reject`.
- `:249`: `case OrderEvent::Reject: o.state = OrderState::Rejected;`
- `:197-200`: Rejected admits nothing, not even a Fill.

**Trigger:** open 150, `Ack`, then `CancelSent(20)`. The cancel is refused because the order already completed at the exchange: `Reject(30)`. Then `apply_fill({150, px, 40})` returns IllegalTransition. The state stays Rejected with `cum_qty` 0.

Likewise, a modify rejected (for example, price outside the circuit) leaves a resting order recorded as Rejected, which is terminal and not live.

**Impact:** this is exactly the "believes it is flat while holding a position" failure the header calls the worst (`:24-29`). A single `Reject` event mixes up "order refused" and "request refused".

**Existing test coverage:** test 3 (`:173-182`) exercises Reject only from PendingNew.

**Regression test:** the sequence above. Expected: the Reject returns the order to Open/PartiallyFilled (or introduces CancelReject/ReplaceReject events), and the fill is applied with state Filled.

### C13-003 -- event-time ordering refuses the crossed messages the design claims to accept (P2, DESIGN GAP)

**Evidence**
- `oms/order_state.hpp:227` and `:276`: `if (ts < o.last_event) { ++o.refused; return ... TimeWentBackwards; }`
- The header (`:8-9`) says messages arrive "out of order, and after the event they describe".
- `Order::last_event` (`:139`) says "From the tick, never a clock". It does not say whose time: broker receipt or exchange event.

**Trigger:** if `FillReport.ts` carries the exchange fill time, a fill that crossed the cancel (fill at 25ms, CancelAck stamped 30ms) is refused as TimeWentBackwards. The position is lost. A stale duplicate report that carries its original earlier timestamp is also counted as `refused`, not as a duplicate.

**Existing test coverage:** test 3 stamps the late fill at 40ms, after the ack at 30ms. It passes only because `ts` is treated as arrival order. The comment in test 6 (`:280-283`) records that this check already hid one test path.

**Regression test:** Ack@10, CancelSent@20, CancelAck@30, then a Fill carrying ts=25. Expected: accepted (or the struct documents that `ts` must be receipt time and the adapter enforces that).

### C13-004 -- no representation of an uncertain order; no idempotency (P1, DESIGN GAP)

**Evidence**
- `OrderState` (`order_state.hpp:51-73`) has no Unknown or SendFailed state. PendingNew (`:53-54`) "may or may not be live and only the broker knows", but there is no timeout or transport-failure event (`OrderEvent :87-104`).
- `reconcile.hpp` compares POSITIONS only (`PositionView :48-61`). There is no reconciliation of open orders.
- The Kite `tag` (`kite_adapter.hpp:95-98`) is described as Altair's order id, but nothing generates it, checks uniqueness, or validates length/charset (`:240`).
- `parse_order_id` does not check the response status (C13-015).
- Intents carry an `id`, but `drain_intents` does not dedupe on it.

**FYERS/backup implication**
- The model DOES distinguish Rejected from PendingNew.
- It does NOT distinguish "sent, awaiting ack" from "send failed / outcome unknown".
- There is no retry or failover code anywhere, and `Venue` (`throttle.hpp:212`) has no FYERS entry. `route()` refuses unbuilt venues instead of falling back (`:240-244`).
- So a blind retry at another broker cannot happen today. But nothing in the types would forbid one once a sender exists.

**Regression test (design):** a `SendTimeout` event takes PendingNew to Unknown. Unknown can be left only by reconciliation-sourced events. `route()` or a failover must refuse a different venue for an order key that is Unknown.

### C13-005 -- interrupted append + next append = a Frankenstein intent that parses (P1, CONFIRMED DEFECT)

**Evidence**
- `oms/order_intent.hpp:131`: `const auto p = line.find(needle);`, which takes the FIRST occurrence of each key anywhere in the line.
- `:289-295`: a tail with no newline is left unconsumed "so the next drain sees the record whole". That assumes the SAME writer finishes the line.
- The writer (`desktop/order_ticket.hpp:141-145`) opens in Append mode and writes `intent_line(d) << "\n"`. It never checks that the file ends in a newline.

**Trigger:** writer crashes after emitting this prefix (no newline):

```
{"v":1,"id":"A1","at":"2026-09-14T10:00:00+05:30","by":"smit","token":738561,"symbol":"RELIANCE","exchange":"NSE","side":"SELL","lots":50
```

The next session appends:

```
{"v":1,"id":"B2","at":"2026-09-14T10:05:00+05:30","by":"smit","token":256265,"symbol":"NIFTY 50","exchange":"NSE","side":"BUY","lots":1,"order_type":"LIMIT","limit_paise":100,"product":"MIS","validity":"DAY"}\n
```

**Parsed result of the joined line:** success, with `rejected == 0`:
- id `A1`, token 738561, symbol RELIANCE, side **SELL**, lots **50** (the digit scan stops at `{`)
- order_type LIMIT, limit_paise **100**, product MIS, validity DAY. These four come from B.

That is a SELL of 50 lots of RELIANCE at Rs 1.00, which nobody wrote. Intent B disappears.

**Existing test coverage:** `test_order_intent.cpp:175-186` checks only that the partial tail is not consumed. It never appends a following record.

**Regression test:** write the A prefix, then B plus a newline, then drain from the pre-A offset. Expected: 0 intents and `rejected >= 1` (for example, refuse any line containing a second `{"v":`, or refuse a line that is not exactly one closed object). Never an intent with id A1 and limit_paise 100.

**Current impact:** bounded, because nothing drains (C13-009).

### C13-006 -- integer overflow and token narrowing (P2, CONFIRMED DEFECT)

**Evidence**
- `oms/order_intent.hpp:170`: `v = v * 10 + (s[i] - '0');`, with no overflow check. Signed overflow is UB.
- `:205-208`: checks only `n <= 0`, then `in.token = static_cast<std::uint32_t>(n);`

**Triggers** (vector line 2 with one field replaced)

| Input | Result |
|---|---|
| `"token":4295223561` | token **256265** (= 4295223561 - 2^32). Parse succeeds and `complete()` is true: a different instrument. |
| `"token":4294967296` | token 0. Parse still succeeds, because `parse_intent` never calls `complete()`. |
| `"lots":18446744073709551617` (2^64+1) | UB. Under two's-complement wraparound this gives lots = 1, which is accepted. |
| `"limit_paise":18446744073709651616` | wraps to 100000 |

**Existing test coverage:** none.

**Regression test:** each input above must return BadValue (for example, `std::from_chars` with range checking, and token > UINT32_MAX refused).

### C13-007 -- the parser is not the strict schema its comments claim (P2, CONFIRMED DEFECT)

**Evidence**
- `oms/order_intent.hpp:120-125` claims it "refuses anything that does not look exactly like what the emitter writes".
- `:140-146`: the comment says other escapes are refused, but the code keeps the character after any backslash.
- `:181`: the only structural check is `line.front() == '{'`.

**Concrete lines and parsed results** (vector 1 with one change)

| Change | Parsed result |
|---|---|
| a. append `,"lots":40` before `}` (duplicate key) | lots = **1**, because the first key wins. A last-wins JSON reader would see 40. |
| b. prefix `{"meta":{"lots":40},` (valid JSON, nested key) | lots = **40** |
| c. `"lots":1.9` | 1 |
| c. `"lots":2e1` | 2 |
| c. `"limit_paise":5711490.99` | 5711490 |
| d. `"lots":"5"` and `"v":"1"` | accepted as integers |
| e. `"v":1abc` | accepted as 1 |
| e. a line with no closing `}` | accepted |
| f. `"symbol":"NIFTY\nBANK"` | symbol `NIFTYnBANK` |
| f. `"` | decoded as `u0022` |
| g. `"id":""`, `"symbol":""`, `"exchange":""`, `"product":"XYZ"`, `"validity":"GTT"`, `"at":"t"` | all parse. The test itself uses `"at":"t"`. `complete()` is not called by `parse_intent` or `drain_intents`. |
| h. key name inside a string value | not reproducible with VALID JSON. The emitter escapes `"` (`order_ticket.hpp:104-106`), so `\"lots\":` cannot match the needle `"lots":`. It is reproducible only on non-JSON lines (which the parser accepts) and through C13-005. |

**Existing test coverage:** test 2 covers 8 refusals, none of these.

**Regression test:** each line a-g must be refused (Malformed or BadValue).

### C13-008 -- drain offsets without file identity; no dedupe; unbounded batch (P2, DESIGN GAP)

**Evidence:** `oms/order_intent.hpp:283-286`: `if (!f) { return out; }`, then a seek to the persisted offset with no size or identity check. `:250` uses an unbounded `std::vector`, and `:289` an unbounded `getline`.

**Triggers**
- (a) The file is missing, locked or unreadable. The result is the same as "nothing new" (`lines=0`, `rejected=0`) with no error field.
- (b) The file is truncated, replaced or hand-edited. The header (`:37-39`) expects a person to "find and cancel" intents, but says nothing about how. With offset > size, drains return nothing until the file grows past the old offset, then resume mid-record. If the offset lands on a `{`, earlier records are silently skipped.
- (c) The process crashes after acting on a batch but before persisting `next_offset`. The re-drain returns the same intents. No `id` dedupe exists, so orders would be duplicated.
- (d) Rule 11: no cap on intents per drain or on line length, and nothing counts or refuses beyond a bound.

**Existing test coverage:** offsets are tested for append-only growth only.

**Regression tests**
- Drain at an offset beyond the file size must report an error.
- Drain with the file absent must report an error distinct from empty.
- The same id drained twice must be refused the second time by the consumer.

### C13-009 -- intents are written and never consumed; no assembled pipeline (P1, DESIGN GAP)

**Evidence (grep across core, feed, book, instruments, analytics, features, models, strategies, risk, oms, broker, flagging, backtest, server, desktop, app, research)**
- `drain_intents` and `parse_intent` appear only in `oms/order_intent.hpp` and `oms/tests/test_order_intent.cpp`.
- `build_place_order` and `OrderIntentKite` appear only in the oms header and its test.
- `broker/kite_api.hpp:117-119` (`kUriPlaceOrder`/`Modify`/`Cancel`) are referenced only by `broker/tests/test_kite_api.cpp`.
- No CMake target outside oms/ links `altair_oms`. `app/CMakeLists.txt` links `altair_broker_net` and others, not oms. `desktop/CMakeLists.txt:12` excludes it.
- `evaluate_exit`, oms `reconcile`, `BrokerThrottle`/`route`, `apply`/`apply_fill`, `twap`/`vwap`/`pov`, `almgren_chriss` and `capacity` have no non-test callers. The `reconcile(` hits in app/ and instruments/ are different functions.
- The desktop writes to `data/order_intents.jsonl` (`desktop/CMakeLists.txt:183, 567`).

**Plain statement:**
- No executable drains intents, applies risk checks, converts lots to quantity, or submits through `kite_adapter.hpp`.
- No code anywhere sends an order to a broker.

**Latent hazard**
- The queue is append-only with no expiry, and `at` is never parsed or compared.
- `DrainResult` starts from whatever offset the caller passes, and nothing persists one.
- A future drainer started at offset 0 would act on every intent ever written, possibly days old, at stale limit prices.
- The lots-to-quantity step the header promises "at drain time" (`:15-18`) does not exist.

**Regression test (for the future consumer)**
- Intents whose `at` is before the current session start are refused as expired.
- The first-run offset is set to end of file, with an operator acknowledgement.

### C13-010 -- a stop exit on a gap is priced at the stop, not at a reachable price (P1, CONFIRMED DEFECT, gate 7)

**Evidence**
- `oms/exit_ladder.hpp:205`: `d.level = p.tighter_stop;` and `:213`: `d.level = p.initial_stop;`
- The header (`:21-23`) says "the exit level is what the square-off order is priced against".

**Trigger** (the test's own case, `test_exit_ladder.cpp:40-53, 93-134`): long 750 @ 24,000, initial stop 23,800, tighter stop 24,000, tick at 23,700.
- The decision is TighterStop with level 2,400,000.
- `exit_pnl` = 0.
- No trade at 24,000 exists after the gap. The best exit you could actually get is at or below 23,700, so the realistic P&L is about -Rs 2,25,000.

**Why the test's number is fictional:** both check orderings would fill at the same gap price. The Rs 1,50,000 "value of the ordering" asserted at `:132-134` is the difference between two fills that could not happen.

**Live consequence:** a sell LIMIT at 24,000 with the market at 23,700 cannot fill immediately. The protective exit rests unfilled while the price falls. The kill-switch path already prices at market for exactly this reason (`:252-255`).

The rule-8 ORDERING itself is implemented correctly; see §4.

**Regression test:** the plan above with price 2,370,000. Expected: `level <= price` for a long (`min(stop, price)`; `max` for a short), and `exit_pnl <= -22,500,000` paise.

### C13-011 -- reconciliation does not enforce what it documents (P2, DESIGN GAP)

**Evidence**
- `oms/reconcile.hpp:69` documents QuantityMismatch as "Fatal -- see the header".
- `:217-224`: `enforce_reconciliation` trips only when `orphans > 0`.
- `PositionView::present` (`:60`) is never read.
- `key` is a bare uint64 (`:52`).

**Trigger:** broker `{10, +500}` against local `{10, +50}`. Result: `qty_mismatches=1`, `orphans=0`, `has_unmanaged()` false, kill switch not tripped. That leaves 450 units with no stop and no limit. A sign flip (broker -75, local +75) behaves the same way.

`test_reconcile.cpp:156-157` asserts that "it blocks", but nothing in this header blocks a symbol.

Kite reports MIS and NRML lines separately for one token. Keyed on the token, that raises DuplicateKey and refuses the WHOLE reconciliation, so orphan detection is lost for every other symbol too. This is fail-closed but blinding.

**Regression test:** broker 500 against local 50. Expect `has_unmanaged()` true or the kill switch tripped (or an explicit per-symbol block result). Also test a broker book with the same token under two products.

### C13-012 -- Almgren-Chriss overflow is not handled (P2, CONFIRMED DEFECT)

**Evidence**
- `oms/shortfall.hpp:149-152`: "sinh(kT) overflows for large kT ... Both are handled explicitly". Only `kT < 1e-8` is handled (`:153`).
- `:162`: `std::sinh(k * (T - tau)) / denom`
- `:166`: `static_cast<std::int64_t>(X * frac + 0.5)`

**Trigger:** `almgren_chriss(Qty{1000}, 1.0, {sigma=1, eta=1, lambda=1e6}, 256)` gives kappa=1000 and kT=1000.
- At i=1, k(T-tau) is about 996. Both sinh values are inf, so `frac` is NaN and the cast is UB.
- The monotone pass (`:173-175`) does not repair a negative or garbage value, and `trade(1)` can overflow.

**Existing test coverage:** tests 6 and 7 use kT close to 0 and slightly above 1 only.

**Regression test:** the input above. Expect BadParameter, or a stable exp-form trajectory with every `trade(i) >= 0` and `total_traded() == 1000`.

### C13-013 -- silent clamps in accessors (P3, CONFIRMED DEFECT, rule 11)

**Evidence**
- `oms/execution.hpp:92`: `return slice_[i < n_ ? i : 0];`. An out-of-range index returns the first child, which is a real order quantity.
- `oms/shortfall.hpp:95`: `rem_[i <= n_ ? i : n_]`

**Regression test:** a 2-slice schedule, `s[5]`. Expect a refusal or assert, not slice 0.

### C13-014 -- execution schedules ignore lot size (P2, DESIGN GAP)

**Evidence:** `oms/execution.hpp:122-130` (`even_split`), `:146-179` (twap), `:199-261` (vwap), `:286-311` (pov). None takes a lot size.

**Trigger:** `twap_schedule(Qty{750}, open, close, 7)` for 10 NIFTY lots of 75 gives 108,107,107,107,107,107,107. No child is a lot multiple. The exchange refuses every one, or a caller re-rounds and breaks the sum-equals-parent invariant the file exists to keep. Rule 1 requires the lot size to come from the spec store, but the API has nowhere to take it.

**Existing test coverage:** none. Test 1 uses 1000/7 with no lot concept.

**Regression test:** a lots-based schedule. Every child must be a multiple of the spec-store lot, and the total must equal the parent.

### C13-015 -- Kite adapter response parsing (P2, CONFIRMED DEFECT)

- **(a) `parse_order_id`** (`kite_adapter.hpp:326-349`) takes the first `"order_id"` substring, skips any quotes, and never checks `"status"`.
  - `{"status":"error","message":"x","data":{"order_id":null}}` returns the order id **"null"**.
  - `{"status":"error","data":{"order_id":"123"}}` returns "123", treated as success.
- **(b) `rupee_string_to_paise`** accepts a leading `-` (`:136`): `"-451.25"` becomes Price{-45125}. `map_update` (`:311-315`) stores it without a check, although `AdapterError` (`:71`) documents negatives as refused. `:143`/`:163` multiply without an overflow check (for example `"92233720368547758.08"`).
- **(c) `map_update`** returns a snapshot state (`:291-298`), not an OrderEvent. Nothing maps a snapshot onto `apply`/`apply_fill`. A consumer that assigns `m.state` directly bypasses the transition table.
- **(d) `build_place_order`**
  - Appends `price` to a MARKET order with `limit_price > 0` (`:230-234`); the intent parser refuses that combination.
  - Builds an SL order with no limit price (`:209-213`) — HYPOTHESIS: Kite requires a price for SL.
  - `tag` is not length- or charset-validated (Kite constraints not verified offline).
- **(e)** `paise_to_rupee_string` and `map_update` are `noexcept` but construct `std::string`, so `bad_alloc` terminates (P3).

**Existing test coverage:** test 6 has no error-status or null case. Test 4 has no negative-parse case.

**Regression tests:** the two bodies in (a) must be refused. `rupee_string_to_paise("-1.00")` must be refused.

### C13-016 -- VWAP accepts +inf (P3, CONFIRMED DEFECT)

**Evidence:** `oms/execution.hpp:215`: `if (!(curve[i] >= 0.0))` lets +inf through. At `:231-233`, `total * inf / inf` is NaN, and the int cast is UB.

**Regression test:** curve `{1.0, +inf, 1.0}`. Expect BadCurve.

### C13-017 -- throttle semantics (P3, HYPOTHESIS)

**Evidence:** `oms/throttle.hpp:124-126`, `:141`. The daily window is a fixed 24h measured from the first consume after a reset. It is not the broker's trading day. `never_refills` is reported although `rolled()` does refill after 24h of tick time.

**Other concerns**
- Only placements are throttled. Kite also rate-limits modify and cancel requests and caps modifications per order; not verified offline.
- `kKiteOrderLimits {10,200,3000}` (`:65`) could not be verified offline.

**Checked and fine:** refusal consumes no budget. Bounded, no queue, no wait loop.

## 4 Lead revalidation

### Codex leads on `oms/order_intent.hpp`

**~:127, substring field discovery — PARTIAL**
- Duplicate keys: CONFIRMED, first-wins (C13-007a).
- Nested-object key match: CONFIRMED on valid JSON (C13-007b).
- "Key name inside a string value": NOT CONFIRMED for valid JSON, because the emitter escapes quotes and the escaped form cannot match the needle. CONFIRMED for non-JSON lines, which the parser accepts, and in practice via tail concatenation (C13-005).

**~:170, unchecked signed accumulation — CONFIRMED** (C13-006)
- `"lots":18446744073709551617` gives lots 1 under wraparound. It is UB.

**~:208, narrowing to the broker token type — CONFIRMED** (C13-006)
- `"token":4295223561` gives token 256265.

**No strict schema (types, escapes, duplicates, completeness) — CONFIRMED** (C13-007)
- `"lots":"5"` gives 5.
- `\n` gives `n`.
- `"id":""` parses.
- `complete()` is never called.

**Partial file tails and restart offsets — CONFIRMED** (C13-005, C13-008)
- A tail followed by the next append parses as a mixed record.
- An offset past the end of the file returns silence.

### "Live price distribution is not an assembled strategy/risk/OMS pipeline" — CONFIRMED (C13-009)

No executable drains intents, runs risk checks on them, builds a Kite order body, or sends one. `oms/` compiles only into its own test executables.

### Other leads

**order_state.hpp transitions — defects found** (C13-001, C13-002, C13-003)
- Confirmed working: duplicate acks/cancels are absorbed, cumulative quantity never goes backwards, overfill is refused, and a completing fill during an amend goes to Filled.

**execution.hpp — C13-013, C13-014, C13-016**
- The sum-equals-parent invariant holds for TWAP and VWAP; there is no rounding leak.

**throttle.hpp: Venue::Xts refused rather than routed to Kite — CONFIRMED** (`:240-244`, test 5)
- Bounded rate limiting: CONFIRMED. Three fixed windows, refuse-not-queue, no waits or retries.
- Caveats: C13-017.

**kite_adapter.hpp builds the POST body only — CONFIRMED**
- No transport include, and there is no caller.
- Escaping: form percent-encoding over the unreserved set is correct, including `&` and `%` (`:169-182`).
- Tags and idempotency keys: a pass-through tag only, with no generation, uniqueness or validation (C13-004, C13-015).

**reconcile.hpp**
- Orphans trip the kill switch: CONFIRMED.
- Uncertain orders: NOT HANDLED, because it reconciles positions only (C13-004).
- Quantity mismatch is not enforced (C13-011).

**exit_ladder.hpp, rule 8 — ordering CONFIRMED correct**
- The tighter stop is checked before the initial stop (`:200-216`).
- `tighten_stop` requires the tighter stop to sit inside the initial stop (`:129-131`), so on a gap it always fires first.
- However, the level used for the exit is financially wrong on gaps (C13-010).

**shortfall.hpp — C13-012**
- `attribution_sums` is algebraically the same as summing the components: delay plus execution telescopes. It checks code consistency, not input correctness.

**capacity.hpp — no defect found.** Pure double analytics; no money crosses a boundary.

### FYERS backup implication — PARTIAL (C13-004)

- Rejected and PendingNew are distinct states.
- There is no uncertain/unknown state, so "an uncertain Kite order must never become a blind retry at another broker" is not enforceable by the type system.
- It is only vacuously true today, because there is no retry, no failover and no sender at all.

## 5 Architecture facts

**Order lifecycle**
- States: Unset(0), PendingNew, Open, PartiallyFilled, PendingReplace, PendingCancel, Filled, Cancelled, Rejected, Expired.
- Events: Ack, Fill (only through `apply_fill`, cumulative qty), ReplaceSent, ReplaceAck, CancelSent, CancelAck, Reject, Expire.
- Terminal: Filled, Cancelled, Rejected, Expired. Cancelled admits a late Fill; Filled admits a duplicate Fill.
- `Order` carries no id, side, price, symbol or venue. No order registry or container exists.

**Ownership**
- `altair_oms` is a header-only INTERFACE library linking `altair_risk`, `altair_types`, `altair_time` and `altair_flags`.
- `kite_adapter.hpp` includes `broker/kite_api.hpp`, but the INTERFACE target does not link `altair_broker`; only its test does.
- Nothing outside oms/ links it. The desktop is kept apart by conformance vectors, not a shared header.

**Threads:** none. No locks or atomics. Every function is pure or mutates a caller-owned struct, so the caller must serialize access.

**Hot vs cold**
- `apply`, `apply_fill` and `evaluate_exit` are `ALTAIR_HOT`, `noexcept` and allocation-free (gate 5 holds).
- Throttle and schedules are allocation-free, using fixed arrays on the stack: Schedule is about 8 KB, and vwap uses about 20 KB of locals plus the return.
- `order_intent` and kite body/response handling allocate; they are cold.

**Idempotency**
- Present: duplicate broker updates are absorbed by the state machine.
- Absent: idempotent placement (no client order id generation, tag not validated), intent id dedupe, and offset/action atomicity.

**Implemented vs connected:** everything in oms/ is implemented and unit-tested. None of it is connected to any executable.

## 6 Test-suite observations

- **test_order_intent**
  - Covers vector parsing, 8 refusals, offsets, and a partial tail.
  - Missing: overflow, narrowing, duplicates, decimals, escapes, tail-plus-append, missing file, and offset beyond EOF.
  - Temp file in the working directory, removed at the end.
- **vectors/intents.jsonl**
  - 3 lines. The field order matches `desktop/intent_line`.
  - Vectors 1 and 2 target INDEX tokens (NIFTY BANK 260105, NIFTY 50 256265, exchange NSE), which cannot be ordered. That is fine for a schema test, but a future drainer must refuse them.
  - The byte-for-byte desktop comparison lives in `desktop/tests/test_order_ticket.cpp`, which I did not read (seen by grep only).
- **test_order_state**
  - Crossed-message tests stamp later timestamps (C13-003).
  - Missing: a partial fill during PendingCancel/PendingReplace (C13-001), and a Reject from a pending request (C13-002).
  - Test 3 asserts that a Cancelled order reopens to the live state PartiallyFilled.
- **test_exit_ladder:** test 1's money assertion measures fictional fills (C13-010). Ordering, tightening, clock-before-target and kill switch are well covered.
- **test_kite_adapter**
  - No error-status, null order id or negative average price case.
  - `AMO REQ RECEIVED` is refused (fail-closed; AMO orders would block the symbol).
- **test_reconcile**
  - Asserts "it blocks" for a qty mismatch that nothing blocks (C13-011).
  - Truncation is declared and counted: rule 11 honoured.
- **test_throttle:** good coverage of the windows, refusal-not-consumed, the Xts refusal and determinism.
- **test_execution:** no large-kappa*T, inf curve or lot-multiple case. The attribution check is algebraically guaranteed to pass.
- **test_capacity:** uses `std::min`/`std::max` without `<algorithm>`, relying on a transitive include (gate-1 portability, P3).

## 7 Open questions

1. What does `Timestamp ts` on broker events mean: receipt time or exchange event time (C13-003)?
2. How is a person meant to cancel a pending intent in an append-only file without breaking offsets (`order_intent.hpp:37-39`)?
3. Where will the drain offset be persisted, what sets the first-run offset, and how is "acted on" made atomic with "offset advanced"?
4. Which component will turn Kite snapshots (`map_update`) into `apply`/`apply_fill` events, and how will it disambiguate request rejections from order rejections?
5. Needs verifying against current Kite docs (not done offline):
   - the status list (AMO, MODIFIED and so on)
   - the tag length/charset
   - a market-protection requirement on MARKET orders
   - SL price requirement
   - modify/cancel rate limits
6. Should `altair_oms` link `altair_broker`, since `kite_adapter.hpp` includes `broker/kite_api.hpp`?
7. Cross-group (desktop): `append_intent` (`desktop/order_ticket.hpp:138-147`) returns true without checking the QTextStream/QFile write status, and never repairs a newline-less tail (see C13-005).
