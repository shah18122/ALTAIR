# CX-01 G15_app_server -- findings

Reader: claude-subagent:G15_app_server · Date 2026-09-14 · Baseline a34af5c5c9fa8c7498e8cc4df00907548a41e1a5

## 1 Scope & coverage

All 20 assigned files (5,561 lines) were fully read in chunks of 300 lines or fewer:
app/{CMakeLists.txt, main.cpp, instruments_demo.cpp/.hpp, price_service_main.cpp, monitor.hpp, note_reconcile.hpp, warm_restart.hpp, tests/test_monitor.cpp, tests/test_note_reconcile.cpp, tests/test_warm_restart.cpp}
and server/{CMakeLists.txt, protocol.hpp, price_payload.hpp, price_bus.hpp, tests/test_protocol.cpp, tests/test_price_payload.cpp, tests/test_price_bus.cpp, tests/emit_vectors.cpp, tests/vectors/frames.txt}.

- sha256 before and after both match the inventory. `git status` shows no changes under app/ or server/.
- Nothing was built, run, or tested.
- Outside-group code was read only to check integration, and is not counted as coverage: desktop/price_client.hpp:86-235, broker/kite_ticker.hpp:225-376, instruments/contract_spec.hpp:205-229, plus greps of feed/kite_decoder.hpp and feed/tick.hpp.
- A caller grep across first-party dirs found that app/monitor.hpp, app/note_reconcile.hpp, app/warm_restart.hpp and protocol.hpp's SeqTracker are referenced only by their own tests.

## 2 Findings table

| ID | Sev | Class | Where | Summary |
|---|---|---|---|---|
| C15-001 | P2 | CONFIRMED DEFECT | server/price_bus.hpp:202-206 | Coalescing can drop a head frame that is already partly on the wire and resets `head=0`. The subscriber stream loses frame alignment and the desktop client aborts. |
| C15-002 | P2 | CONFIRMED DEFECT | app/price_service_main.cpp:456-458 + broker/kite_ticker.hpp:256,326,364,332-338 | `--go` stops after about 60 s (default `read_timeout` used as a run deadline) despite the "run until interrupted" usage text. A mid-session socket error with frames already delivered returns success (exit 0). There is no reconnect. |
| C15-003 | P2 | CONFIRMED DEFECT | server/CMakeLists.txt:69-73, server/tests/emit_vectors.cpp:138-139,222-238 | ctest `protocol_vectors` rewrites the tracked `frames.txt` in the source tree, then checks only its own output. It cannot detect drift from the checked-in contract. |
| C15-004 | P2 | DESIGN GAP | app/CMakeLists.txt; app/main.cpp | No executable assembles feed → book → features → model → strategy → risk → OMS. `altair` is the Phase-0 null-strategy ledger demo; `altair_price_service` is a display republisher. |
| C15-005 | P3 | CONFIRMED (contained) | app/price_service_main.cpp:350-361 | Placeholder specs for every `--tokens` token: lot 1, tick 1 paise, scale 100, symbol `TOKEN%u`. |
| C15-006 | P2 | CONFIRMED | app/price_service_main.cpp:279-330, 122-207 | `--replay` publishes CSV bar closes on the trades topic, not canonical ticks. Volume means per-bar volume here but cumulative day volume live. Unknown symbols publish as token 0. |
| C15-007 | P2 | DESIGN GAP | server/price_bus.hpp:133-210; app/price_service_main.cpp:449; kite_ticker.hpp:357-362 | No Heartbeat, Snapshot or Bye frames are ever sent, and client messages are never read. `poll()` runs only on data frames, so a silent market stalls accept and flush. A protocol-conforming SeqTracker could never apply a frame. |
| C15-008 | P3 | CONFIRMED DEFECT | server/price_bus.hpp:185,257 | `++seq_[topic]` writes out of bounds for any topic ≥ 3. Current callers use only 1 and 2. |
| C15-009 | P3 | DESIGN GAP | server/price_bus.hpp:133-161,197-199,261 | Client count is unbounded, a stalled reader is never disconnected, `refused_` has no accessor, and each publish allocates one frame copy per client. |
| C15-010 | P3 | CONFIRMED | app/price_service_main.cpp:231,303-329,137-168 | Replay exits without draining queued frames. `--port` is truncated through atoi. Timestamp parsing accepts garbage, ignores the stated offset, and stamps daily bars at 00:00 IST, contrary to its own comment. |
| C15-011 | P3 | CONFIRMED | app/price_service_main.cpp:403-410 | A measured zero volume or OI is published as "absent". This contradicts the payload contract asserted in test_price_payload.cpp:132-135. |
| C15-012 | P3 | CONFIRMED | server/protocol.hpp:112-123,315-339 | `decode_header` never returns `PayloadTooLarge`, has no length bound, and does not check enum ranges or reserved bytes. The desktop client buffers whatever `payload_len` claims. |
| C15-013 | P3 | CONFIRMED / DESIGN GAP | app/warm_restart.hpp:166-178,196,221-235,92-107 | No flush-error check or fsync. The rename fallback removes the target first, leaving a window with no snapshot. Duplicate keys can satisfy `seen==8` while `breached` is missing. Trailing bytes after the checksum are ignored. The snapshot holds a single position. Tests-only. |
| C15-014 | P2 | CONFIRMED DEFECT (latent, tests-only) | app/monitor.hpp:77-84,292-305,280-286 | `worst()` ranks Unobserved below Ok, so one never-wired gauge among healthy ones reports Ok. A NaN value reports Ok. Bucket counters are uint32 with no stated bound proof. |
| C15-015 | P2 | DESIGN GAP + HYPOTHESIS (tests-only) | app/note_reconcile.hpp:106-117,240-252,275-293,202 vs 54-61 | Rows are joined on order_id and duplicates are refused, so any order filled in more than one execution can never reconcile. Exact per-trade paisa comparison ignores note-level rounding (hypothesis). `ok()` contradicts its header on an empty/empty day. |
| C15-016 | P3 | CONFIRMED | app/instruments_demo.cpp:435,455-456 | Demo only: an empty `if (master_path != nullptr) {}` block, and a hard-coded session date 2026-08-31 used as the snapshot time for real files. |

No P0 or P1 findings. Nothing in this group places or amends an order.

## 3 Finding details

### C15-001 (P2, CONFIRMED DEFECT): coalescing drops a partially written frame
- **Evidence.** In price_bus.hpp:202-206:
  `while (c->bytes > kOutboxBytesCap && c->out.size() > 1) {` … `c->out.pop_front();` / `c->head = 0;`
  - In `flush()` (229-243), `write_some` may accept part of the front frame (`c.head += n`, frame stays at front).
  - If the outbox then exceeds 1 MiB, that partly written front frame is popped and `head` reset.
  - The wire now carries `front[0..head)` followed by the next frame from byte 0.
- **Trigger.**
  1. A subscriber stops reading.
  2. The 64 KiB SO_SNDBUF fills, and the last successful `write_some` is partial.
  3. More than 1 MiB of further publishes arrive (about 11k trade frames of 96 bytes).
  - This is exactly the scenario the cap exists for. Partial sends at a full buffer are routine on Linux (ops/ targets Linux). Windows behaviour is not verified, which may be why the test passes.
- **Impact.**
  - The resuming reader hits BadMagic, or a misread `payload_len`.
  - desktop/price_client.hpp:165-175 aborts the socket and counts `bad_`; drain() has no auto-reconnect.
  - A reader that did not validate magic would decode garbage prices.
  - Display only.
- **Tests.**
  - test_price_bus.cpp:147-265 floods a stopped reader and checks alignment, but cannot force a partial head write.
  - Its comment at 175-181 asserts that drops never touch bytes already handed to the kernel, which is the exact property the code does not guarantee.
- **Regression.**
  - Unit-test the outbox logic with a fake socket whose `write_some` returns `size/2` once and then `would_block`.
  - Publish past the cap, drain with a normal socket, and assert every header decodes.
  - Fix direction: never pop a front frame with `head > 0`; drop from index 1 instead.

### C15-002 (P2, CONFIRMED DEFECT): the live stream ends after about a minute and hides disconnects
- **Evidence.**
  - `kite_ticker_run(api_key, access, sub_tokens, TickerMode::Full, on_frame, expired)` (price_service_main.cpp:456-458) does not pass `read_timeout`.
  - kite_ticker.hpp:256 defaults it to `std::chrono::seconds{60}`.
  - :326 sets `deadline = now + read_timeout`, and :364 breaks the loop once past it.
  - :332-338: any read error after at least one frame or heartbeat → `break` → `return st` (success).
  - price_service_main.cpp:463-471 then prints delivery stats and returns 0.
  - Usage text at :68-69 says the service runs until interrupted by default.
- **Trigger.** Run `altair_price_service --go` without `--seconds`, or lose the network mid-session.
- **Impact.**
  - The desktop Terminal stream silently ends about 60 s after start.
  - Real disconnects are indistinguishable from a normal end: exit 0, no error text.
  - Nothing reconnects.
  - Hypothesis: `timeout::suggested(client)` (kite_ticker.hpp:307-308) has no idle timeout, so a half-open TCP connection could block `ws.read` indefinitely. The Beast default was not verified here.
- **Tests.** None. The executable has no test, and kite_ticker_run needs a live credential.
- **Regression.**
  - Refactor the loop so the deadline and the error classification are injectable.
  - Assert that the default run is unbounded, and that an error after frames returns `TransportFailed`, not stats.

### C15-003 (P2, CONFIRMED DEFECT): the conformance test overwrites its own contract
- **Evidence.**
  - server/CMakeLists.txt:71-73 sets `ALTAIR_VECTOR_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors"` and `add_test(NAME protocol_vectors COMMAND altair_emit_vectors)`.
  - emit_vectors.cpp:138-139 opens `ALTAIR_VECTOR_DIR "/frames.txt"` with mode `"wb"`.
  - The re-read at :222-238 only compares `decode_header(...).has_value()` against the `accept=` flag the program itself just wrote. Field values are not compared.
  - An encode failure in `emit()` (:88-91) silently omits that vector and still returns 0.
- **Trigger.** Every ctest run, including the default preset.
- **Impact.**
  - Any change to `encode_header` rewrites the tracked vectors with the new bytes and passes.
  - Drift shows only as a git diff, never as a test failure.
  - A read-only source checkout fails the test.
  - The file holds header vectors only; price payload vectors live only inside test_price_payload.cpp.
  - Its original consumer (client/) is retired, and desktop shares the C++ decoder, so nothing independent reads it.
  - The file is unchanged at baseline (hash matches), because the output is deterministic.
- **Regression.**
  - Split the program into `--emit` (manual) and a check mode that decodes the checked-in frames.txt and compares every field.
  - Register only the check mode with ctest.

### C15-004 (P2, DESIGN GAP): no executable assembles the execution loop
- **Evidence.**
  - app/CMakeLists.txt:5-13 links `altair` only against `altair_core altair_feed altair_instruments altair_flags`.
  - main.cpp:79-187 runs a `Replayer` over `ReplayTick`s into a null strategy (`seqno % every`) feeding `ConservationLedger::on_fill` directly. There is no book, features, model, strategy, risk, cost, or OMS code.
  - `altair_price_service` (:92-95) links server, broker_net, feed, instruments and flags, and only republishes prices.
  - The remaining app executables are kite_* tools (another group).
  - main.cpp:199 still prints "Live feeds arrive in Phase 2; execution in Phase 4."
- **Impact.** Any readiness claim that depends on an assembled live or replay trading loop is unsupported by app/.
- **Scope limit.** Only app/ and server/ CMake files were read; other directories may define executables.

### C15-005 (P3, CONFIRMED, contained): placeholder instrument specs in the price service
- **Evidence.** price_service_main.cpp:354-356:
  `cs.lot_size = altair::LotSize{1};` / `cs.tick_size = altair::Price{1};` / `cs.price_scale = 100;`
  The symbol is `"TOKEN%u"`, and no segment or exchange is set.
- **Why it is contained.**
  - The decoder takes its price scale from the token's segment byte (kite_decoder.hpp:173-174), so scale 100 is likely inert.
  - Lot and tick never cross the wire; only the token does.
  - Duplicate tokens are refused (contract_spec.hpp:214-216), so the dense id → token mapping at :391-392 holds.
- **Risk.** Low now. It becomes wrong the moment anything in this process reads lot or tick. Rule 1 is technically violated by design, and the choice is documented at :344-349.

### C15-006 (P2, CONFIRMED): replay is bar closes, not canonical tick replay
- **Evidence.**
  - :306-322 builds one `PricePayload` per CSV bar with `last_paise = close*100+0.5`, `kPriceReplay`, optional volume, `last_qty=0`, no book.
  - The frames go to `kTopicTrades`, paced by `sleep_until`.
  - Live volume is `be32(p+16)`, Kite's cumulative day volume (kite_decoder.hpp:225). Replay volume is the bar's own column 5. Both use the same field and presence bit with different meanings.
  - :298-301 falls back to token `0u` for any symbol other than nifty, banknifty or indiavix.
  - :137-168 parses stamps with `atoi` and no validation. It always subtracts IST regardless of the string's offset, and a date-only stamp becomes 00:00 IST although :132-133 says it must not.
- **Impact.**
  - Rule 6 holds only at the encoder and bus layer.
  - A consumer computing traded volume from replay versus live gets different quantities under one label.
  - An unknown symbol replays silently as token 0.
- **Regression.** Replay a two-bar CSV through `load_bars` plus the publish loop into a PriceBus reader; assert the token is refused for unknown symbols and that volume semantics are documented in a flag.

### C15-007 (P2, DESIGN GAP): no liveness, snapshot, or subscribe semantics
- **Evidence.**
  - `publish` always sets `FrameKind::Delta` (price_bus.hpp:182). No path emits Snapshot, Heartbeat, Gap or Bye, and no socket read exists.
  - `poll()` is called only from `on_frame` (price_service_main.cpp:449). Heartbeats never reach `on_frame` (kite_ticker.hpp:357-360).
  - `SeqTracker::accept` returns Gap for any Delta before a Snapshot, and `needs_snapshot` starts true (protocol.hpp:362,373-378), so a conforming client could never apply a frame.
  - desktop/price_client.hpp:205-210 uses its own ad-hoc gap counter instead, and `start()` (:92-99) does not reset `seq_`.
- **Trigger.** Pre-open, post-close, or index-only subscriptions: a connecting subscriber is not accepted and queued bytes are not flushed until the next data frame.
- **Impact.** A subscriber cannot tell a dead feed from a quiet market. Whether desktop greys out stale prices was not checked (outside group).
- **Regression.** A bus test that publishes nothing for N ms and asserts heartbeat frames arrive.

### C15-008 (P3, CONFIRMED DEFECT): unchecked topic index
- **Evidence.** price_bus.hpp:185 `h.seq = ++seq_[topic];` against :257 `std::uint64_t seq_[3]{};`. `topic` is an unchecked `std::uint32_t` parameter of a public method.
- **Trigger.** `publish(3, …)`. Current callers use only 1 and 2.
- **Impact.** Undefined behaviour (heap corruption inside PriceBus).
- **Regression.** Assert `publish(99, …)` is refused and counted.

### C15-009 (P3, DESIGN GAP): outbox and client bounds
- Each accepted client can pin 1 MiB of outbox plus 64 KiB of kernel buffer. `poll()` accepts without any count limit (:135-160).
- A stalled reader is kept forever (the test asserts this at test_price_bus.cpp:168-169). `Overflow::DisconnectOnBacklog` is unused.
- `refused_` (:179,192) has no accessor, so refused frames are counted where nobody can see them.
- `c->out.push_back(frame)` copies the frame vector once per client per publish.
- Impact is bounded because the bus is bound to loopback only (:87).

### C15-010 (P3, CONFIRMED): replay and argument handling
- The replay loop returns right after the last `bus.poll()` (:321-329), discarding frames still queued for slow clients.
- The first bar is published before any `poll()` has accepted a client.
- `--port 70000` → `static_cast<unsigned short>(atoi)` gives 4464 (:231).
- `--seconds` and `--tokens` accept garbage through atoi/atoll (e.g. token "abc" becomes 0).

### C15-011 (P3, CONFIRMED): measured zero published as absent
- **Evidence.** :403 `if (t.volume.raw() > 0) {` sets `kPriceHasVolume`, and OI is handled the same way (:407).
- **Contract.** test_price_payload.cpp:132-135 asserts that a real zero volume must travel PRESENT.
- **Trigger.** An option or equity with no trades yet today (decoder volume 0) is shown as "no such field".
- **Regression.** Feed a synthetic Kite frame with volume 0 for a non-index token and assert `kPriceHasVolume` is set.

### C15-012 (P3, CONFIRMED): header decoding is permissive
- `ProtocolError::PayloadTooLarge` (:121-122) is never produced.
- `decode_header` accepts any `payload_len` up to 0xFFFFFFFF, any `kind` or `channel` byte other than 0 (e.g. 200), and non-zero reserved bytes (:311 writes zero; decode does not check).
- desktop/price_client.hpp:177-178 waits for `kFrameHeaderBytes + payload_len` bytes. A corrupt length stalls the client silently rather than refusing.
- SeqTracker reuse after a server restart classifies every lower seq as Duplicate (:379). This is latent: no production user.
- Sequence wrap needs 2^64 frames, so it is unreachable in practice.
- **Regression.** A decode test with `payload_len = 1<<31`, `kind=200`, and reserved = 1, each expecting refusal.

### C15-013 (P3, CONFIRMED / DESIGN GAP): warm restart durability and parsing
- **Durability.**
  - `save_snapshot` checks `if (!f)` before the ofstream destructor flushes and closes (:166-171), so a close failure is unobserved. There is no fsync.
  - After a power loss the renamed file can be empty. This fails safe as Malformed.
  - The rename fallback `remove(path)` then `rename` (:174-177) leaves a crash window with no snapshot, which reads as NoSnapshot, i.e. "start fresh".
- **Parsing.**
  - `seen` counts lines, not distinct keys (:221-235), so a body with `cash` twice and no `breached` line passes with `breached=false`.
  - Text after the checksum line is ignored (:196,206-208).
  - Both parsing gaps need a matching FNV checksum, so they are not produced by torn writes.
- **Shape.** One `position_units` and no instrument, orders or margin (:92-107). It cannot restore a multi-instrument book.
- **Other.** int64 overflow in `conservation_holds` (:151-152) is possible on crafted input. The fixed `.tmp` name collides between concurrent writers.
- **Tests.** The four documented cases are covered; none of the above are.
- **Integration.** Tests only.

### C15-014 (P2, CONFIRMED DEFECT, latent/tests-only): an unwired gauge can report Ok
- **Evidence.**
  - `Unobserved = 0, Ok = 1 …` (:79-80).
  - `worst()` starts at `Health::Unobserved` and raises only when `h > w.health` (:293-302).
  - With gauges {Ok, Ok, Unobserved}, the result is Ok.
  - The header (:25-29) says "A SIGNAL THAT HAS NEVER REPORTED IS NOT `Ok`" and names this exact green-at-boot failure.
  - test_monitor.cpp:91-100 tests only the all-unobserved case.
- **Additional.**
  - NaN values compare false at :281-285, giving Ok.
  - `observe(i >= N)` is silently dropped (:260).
  - `bucket_` is uint32 (:223) with no rule-11 proof. At 10k samples/s into one bucket it wraps after about 5 days without `reset()`.
- **Impact.** Once wired, a monitor with one forgotten gauge shows green. There is no caller today.
- **Regression.** Observe 5 of 6 gauges as healthy and assert `worst().health == Unobserved` and names the unwired gauge. Also observe NaN and assert not Ok.

### C15-015 (P2, DESIGN GAP + HYPOTHESIS, tests-only): contract-note reconciliation
- **Multi-execution orders (confirmed behaviour).**
  - The join is by `order_id` (:107-110,283).
  - Every repeated id on either side is DuplicateId and never matched (:240-252,275-279,283).
  - A contract note lists one row per trade execution, so an order filled in two executions yields DuplicateId rows plus an EngineOnly row every day. `ok()` is never true on such days.
  - test_note_reconcile.cpp:292-311 enshrines this refusal.
- **Rounding (hypothesis).**
  - Charges are compared exactly per trade and per head (:213-234). Brokers commonly round STT, stamp and GST at contract-note or order level, not per trade.
  - The test builds both sides from the same `compute_cost` (:110-130), so it cannot see this.
- **Symbol format.** `e.symbol != b.symbol` (:303) will flag FillMismatch if note symbols differ from tradingsymbols.
- **Doc/code mismatch.** The header :60-61 says "`Report::ok()` requires that something was actually compared", but :202 returns `discrepancies.empty()`. The test at :286-289 asserts that an empty/empty day is ok.
- **Arithmetic.** Integer paise throughout; FillMismatch `qty*price` in int64 is safe at realistic sizes.
- **Integration.** No note parser and no caller exist.

### C15-016 (P3, CONFIRMED): instruments demo leftovers
- instruments_demo.cpp:455-456 contains an empty `if (master_path != nullptr) {` / `}` block, apparently dead leftover code.
- :435 hard-codes `today = 2026-08-31`, which becomes the parse snapshot time and universe "today" even for real files given on the command line. Point-in-time stamps and expiry filtering in the demo therefore reflect a fixed date.
- Demo only. No engine path uses it.

## 4 Lead revalidation

| Lead | Verdict | Evidence |
|---|---|---|
| price_service_main.cpp ~:344 placeholder lot/tick/scale specs | **CONFIRMED** | :350-361 (lot 1, tick 1 paise, scale 100, `TOKEN%u`). Contained: see C15-005. |
| ~:279 CSV bar replay emits bar closes, not tick replay | **CONFIRMED** | :279-330, :170-207 (CSV column 4 close, column 5 volume, one frame per bar). |
| ~:378 onward: Kite decode → PriceBus display frames, not an execution loop | **CONFIRMED** | :370-450 publish only. Target links (app/CMakeLists.txt:93-95) contain no book/features/strategy/risk/oms. |
| Process: threads, queues, slow/disconnecting client, Kite reconnect, flags | **CONFIRMED (documented §5)** | Single thread. No Kite reconnect. Live run ends at about 60 s (C15-002). Slow client is coalesced and never dropped. Dead client reaped on the next write error. |
| Any executable assembling feed → book → features → model → strategy → risk → OMS | **NOT FOUND in app/ or server/** | C15-004. `altair` = Phase-0 null-strategy ledger replay + selftest + instruments demo. Other directories' CMake not read. |
| price_bus 1 MiB per-client cap | **CONFIRMED** | :71 `kOutboxBytesCap = 1u << 20`. |
| Whole-frame coalescing correct | **PARTIAL** | Whole frames are dropped except when the front frame is partly written: C15-001. |
| SO_SNDBUF | **CONFIRMED** | 64 KiB, :79 and :157-158. The error code is ignored, so an OS refusal is silent. |
| Disconnect policy / blocking write | **CONFIRMED** | Removal only on a write error (:236,245-248). Slow readers are never disconnected. All writes are non-blocking. The blocking call is Kite's `ws.read` in the same thread. |
| SeqTracker gap detection | **CONFIRMED (works; tests-only)** | :365-388 and test_protocol.cpp:250-312. Unusable with the bus, which sends no Snapshot (C15-007). |
| Sequence wrap/exhaustion | **CONFIRMED unreachable** | 2^64. Reuse across a restart gives Duplicate (C15-012, latent). |
| Overflow policies | **PARTIAL** | Declared as pure functions (:400-423) and enforced nowhere. The bus hard-codes coalescing; no Event channel exists. |
| Frame length validation | **NOT PRESENT** | C15-012. |
| price_payload presence bits and depth bounds | **CONFIRMED correct** | Refuses depth > 5 on encode and decode, refuses flag/count contradiction (:122-130,179-191), size checks (:132,194). The live publisher breaks presence semantics for zero values (C15-011). The depth clamp at price_service_main.cpp:428-430 is unreachable, because DepthUpdate holds 5 levels (tick.hpp:163-164 offsets) and the decoder zero-fills (kite_decoder.hpp:243). |
| emit_vectors rewrites tracked frames.txt | **CONFIRMED** | Overwrites (`"wb"`) and self-checks only; never compares with the checked-in content (C15-003). |
| monitor.hpp thresholds / missable polls | **PARTIAL** | Staleness-before-threshold is correct. There is no poller at all; the caller supplies `now`. Aggregate defect in C15-014. Tests-only. |
| warm_restart atomic rename; torn/garbage refusal | **CONFIRMED with gaps** | Temp+rename, checksum, magic, `seen==8`, conservation, latched breach, stale session all present. Gaps in C15-013. Tests-only. |
| note_reconcile numerics in paise | **CONFIRMED integer paise** | Design gaps in C15-015. Tests-only. |
| instruments_demo | **CONFIRMED demo-only** | C15-016. |

## 5 Architecture facts

**Executables in app/CMakeLists.txt**
- `altair` (always built): main.cpp + instruments_demo.cpp.
  - `--selftest`: session round trip, corrupt header, capacity, determinism, 10k-tick replay, ledger latch.
  - `--instruments [kite.csv] [udiff.csv]`: Phase-1 walkthrough.
  - `--gen` / `--replay <.tick>`: synthetic session through the null strategy and ConservationLedger.
  - Opens no socket, holds no credential, places no orders.
- Net-gated (`ALTAIR_HAVE_NET`), not in this group: altair_kite_login, _fetch, _account, _update, _ticker, _quote.
- `altair_price_service` (net + Boost): this group.
- Test executables: altair_warm_restart_test, altair_monitor_test, altair_note_reconcile_test (links altair_risk).
- server/: altair_protocol_test, altair_price_payload_test, altair_price_bus_test (Boost only), altair_emit_vectors. `altair_server` is an INTERFACE library. Its CMake comments still describe uWebSockets and a TypeScript client, neither of which exists.

**altair_price_service process**
- **Threads.** One thread. `boost::asio::io_context io` is constructed but never `run()`; it only provides an executor for synchronous non-blocking acceptor and socket calls.
- **Live mode (`--go`).**
  - Reads `ALTAIR_KITE_API_KEY` and `data/kite_session.json` relative to the CWD.
  - Registers placeholder specs, then calls `kite_ticker_run`: TLS WebSocket, blocking `ws.read` loop.
  - For each non-heartbeat binary frame, `on_frame` stamps the receive time with `system_clock`, decodes into fixed 512-tick and 512-depth vectors, publishes each tick (topic 1) and each book (topic 2), then calls `bus.poll()`.
  - `--seconds` is honoured via `should_stop` between reads.
  - No reconnect. The run ends after about 60 s by default, or on error (exit 0 if any frames were seen).
- **Replay mode (`--replay SYM IV [--tail N] [--rate N]`).**
  - Loads every `*.csv` under the compile-time `ALTAIR_DATASET_DIR/spot/<sym>/<iv>` (source-tree dataset), sorted by filename.
  - Keeps rows with close > 0 and a parsed stamp > 0. `--tail` keeps the last N.
  - Publishes one trades frame per bar at `rate` per second using `sleep_until`, then `poll()`.
  - Exits at the end without draining.
- **Queues and capacities.**
  - Per client: `std::deque<std::vector<uint8_t>>` outbox, 1 MiB cap, oldest whole frames dropped and counted in `coalesced()` (partial-head defect C15-001).
  - Kernel SO_SNDBUF 64 KiB. Number of clients unbounded. Decoder batch caps 512/512.
- **Slow client.** Coalesced forever, never disconnected, does not stall publishing (writes are non-blocking).
- **Disconnected client.** Reaped when a write returns a non-would-block error, which only happens on a later publish or poll with data queued.
- **Frame format.** 48-byte header (Delta, State, per-topic seq starting at 1, `engine_time` = exchange time or receive time, `server_time` = now) + 48-byte payload + 2·levels·24 bytes.

**Live vs recorded-tick vs bar replay**
- Live Kite ticks exist only in altair_price_service and altair_kite_ticker, and flow only to display.
- Recorded-tick replay (`.tick` session files) exists only in `altair --replay`, driving the null strategy and ledger.
- Bar replay exists only in `altair_price_service --replay`, flagged `kPriceReplay`.
- None of the three feeds a strategy, risk or OMS path.

## 6 Test-suite observations

- **protocol_vectors** writes the tracked source file server/tests/vectors/frames.txt on every run and verifies only self-consistency (C15-003).
- **app_selftest** creates and removes `altair_selftest_session.tick` in the ctest working directory, and probes `no_such_file_here.tick`.
- **warm_restart** writes fixed names (`altair_snap_*.txt`) under the system temp dir and removes them. Concurrent runs from two build trees would collide.
- **price_bus** binds an ephemeral 127.0.0.1 port.
  - Timing-dependent: 50 ms sleep, a drain loop of up to 200k iterations with 1 ms sleeps, and a client SO_RCVBUF of 8 KiB.
  - Behaviour depends on the OS's loopback auto-tuning, which the test comments call flaky in earlier versions.
  - It does not exercise a partial head write (C15-001).
  - It asserts a stalled client is never disconnected (policy, not a proof of safety).
- **price_payload** uses a hand-derived hex vector, which is strong evidence; hostile depth and flag contradictions are covered. That vector is not in frames.txt.
- **protocol** prints its measured coalescing numbers and asserts only inequalities. It covers SeqTracker transitions, and has no length-bound, enum-range or reserved-byte cases.
- **monitor**: no partial-wiring or NaN case (C15-014).
- **note_reconcile**: both sides come from one cost model; no multi-execution order, note-level rounding, or symbol-format case (C15-015).
- **app_instruments**: built-in 6-row sample only; asserts exit code only.
- **Untested:** altair_price_service as a whole (argument parsing, replay loader, stamp parser, live on_frame mapping, publish flags). desktop/tests/test_price_client.cpp also drives PriceBus (other group, not read).

## 7 Open questions

1. Does desktop mark cached `LivePrice` stale or disconnected when the stream ends or goes silent, and does anything reconnect after `drain()` aborts on BadMagic? (desktop/, outside group)
2. Is there an assembled execution-loop executable outside app/ and server/ (desktop/, backtest/, tools)? Only these two CMake files were read.
3. On the current Windows box, does a non-blocking `send` with a 64 KiB SO_SNDBUF ever return a partial count? This decides how reachable C15-001 is here; on Linux it is expected.
4. Does Beast's `timeout::suggested(role_type::client)` leave the idle timeout at none, so a half-open Kite socket blocks `ws.read` forever (C15-002 hypothesis)?
5. Does Zerodha's contract note list one row per execution, and at what level are STT, stamp and GST rounded? This settles the C15-015 hypothesis.
6. Is scale 100 in the placeholder spec read anywhere in the decode path? A grep suggested the scale comes from the segment byte, but the full decoder was not read.
