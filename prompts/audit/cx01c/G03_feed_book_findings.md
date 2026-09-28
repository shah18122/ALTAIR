# CX-01C — G03_feed_book findings

Reader: claude-subagent:G03_feed_book · Date: 2026-09-14 · Baseline HEAD a34af5c5c9fa8c7498e8cc4df00907548a41e1a5
Scope: read-only audit. No build, no tests run, no executables run.

## 1. Scope & coverage

20 files / 5,432 lines assigned; **20 files fully read** with Read in chunks of 300 lines or fewer. sha256 before == inventory == after for all 20 (no drift). Nothing is unread or partial.

| Area | Files |
|---|---|
| feed/ sources | tick.hpp (224), replay.hpp (190), kite_decoder.hpp (283), normaliser.hpp (192), failover.hpp (203), tick_store.hpp (277), CMakeLists.txt (42) |
| feed/ tests | test_tick (298), test_replay (340), test_kite_decoder (568), test_normaliser (381), test_failover (308), test_tick_store (399) |
| book/ sources | l2_book.hpp (234), microstructure.hpp (185), flow.hpp (262), CMakeLists.txt (28) |
| book/ tests | test_l2_book (368), test_microstructure (323), test_flow (327) |

Other groups' files were read only in part, to check integration. They are **not** claimed as coverage:
core/time/exchange_ts.hpp 1-251; instruments/contract_spec.hpp 190-369; app/kite_ticker_main.cpp 180-407 (+grep); app/price_service_main.cpp 120-199, 270-469 (+grep); desktop/data/real_tape.hpp 1-204; desktop/quant_pages.hpp 2150-2238; features/book_flow.hpp 80-149; research/reference/gokiteconnect/ticker/ticker.go (grep only); grep hits in desktop/depth_ladder.hpp, desktop/main_window.hpp, desktop/tick_model.hpp, server/price_payload.hpp.

## 2. Findings table

| ID | Sev | Class | Title |
|---|---|---|---|
| C03-001 | P2 | CONFIRMED DEFECT (latent) | Stale-flagged depth gets a fresh seq from Normaliser, L2Book accepts it and `is_tradable` still says true |
| C03-002 | P2 | DESIGN GAP | Normaliser, gate, failover, L2Book and TickStoreReader are built but not connected. Live executables publish raw decoder output |
| C03-003 | P2 | CONFIRMED DEFECT (integration) | The only tick-store writer drops every DepthUpdate, ignores write errors and stamps the wrong session_date |
| C03-004 | P2 | DESIGN GAP | Tick-store format cannot resolve InstrumentId, truncates on reopen, reports a read error as clean EOF, and cannot see a torn record mid-file |
| C03-005 | P2 | CONFIRMED DEFECT (latent) | `bid_levels`/`ask_levels` are not validated against kDepthLevels. Consumers read past the level arrays |
| C03-006 | P3 | CONFIRMED DEFECT | `classify_trade` classifies against a crossed book. The test that claims otherwise passes by coincidence |
| C03-007 | P2 | CONFIRMED DEFECT (latent) | Watchdog samples health only at poll(). Outages between polls are invisible and the failback stability window is defeated |
| C03-008 | P3 | DESIGN GAP / doc contradiction | Unknown+Unknown reports NoneHealthy, contradicting D2 and the enum comments. The pre-session count pollutes `none_healthy` |
| C03-009 | P3 | CONFIRMED DEFECT | ShortFrame after a partial decode loses the ticks already decoded and leaves a seq gap. Trailing bytes are never checked |
| C03-010 | P3 | CONFIRMED DEFECT (rule 11) | Vpin config clamps silently, toward fewer buckets. A zero bucket_volume discards trades uncounted. The hot loop is unbounded |
| C03-011 | P3 | CONFIRMED DEFECT (rule 11, minor) | Normaliser skips its ordering guard for id >= kMaxInstruments without a counter. The standby-gating comment is false |
| C03-012 | P2 | DESIGN GAP (cross-group) | price_service pads the shorter book side with zero-price levels, which undoes DepthUpdate D6 |
| C03-013 | P3 | DESIGN GAP | L2Book seq and Normaliser seq have no session epoch. `reset_session` without `L2Book::clear` rejects every update |
| C03-014 | P3 | DESIGN GAP | Kyle lambda and VPIN give no error bar. Session accumulators have no window and no Kahan summation |
| C03-015 | P3 | TEST HYGIENE | Tick-store test writes a fixed-name file in CWD, depends on a missing file being absent, and never checks fopen |

No P0 or P1 findings. Nothing in G03 is on a path that places orders today: see §5 "implemented vs connected".

## 3. Finding details

### C03-001 — Stale depth can overwrite a newer book and still read as tradable (P2, CONFIRMED DEFECT, latent)
- **Evidence.** normaliser.hpp:154-155 sets `TickFlag::Stale` on a floor breach, and 163-165 does the same on a per-instrument regression. Both then fall through to `return Verdictum::Publish` (172), and `submit(DepthUpdate&)` assigns `d.seq = seq_++` (103). l2_book.hpp:152 rejects only `d.seq <= b.seq`, so the new seq passes. 164 copies `b.flags = d.flags` but never reads it again. `is_tradable` (74-76) checks only levels and `crossed`. microstructure.hpp:65/73/117/158 gate on `is_tradable` alone. Neither the book nor `is_tradable` has any notion of age.
- **Trigger.** Normaliser publishes DepthUpdate A for id 1 with exchange_ts 10:00:10, which gets seq 5 and is applied. DepthUpdate B for id 1 then arrives with exchange_ts 10:00:05. That can happen with a delayed packet, or after failover with the standby behind. `judge` flags B Stale (out_of_order++) and publishes it with seq 6. `L2Book::apply(B)` succeeds because 6 > 5. The book now holds the older snapshot, `is_tradable` is true, and `microprice`/`obi` compute on it. The same holds for a floor-breach update.
- **Impact.** The "flag, do not drop" policy (l2_book.hpp:12-18, normaliser.hpp:150-155) only works if consumers honour the flag, and none do. A strategy would see a superseded book as current. Latent: no executable constructs Normaliser or L2Book (§5).
- **Existing tests.** test_normaliser.cpp:286-292 asserts that an ancient depth update publishes with Stale set. test_l2_book never sets flags. No test combines the two.
- **Regression test.** Feed A then B above through `Normaliser::submit` and then `L2Book::apply`. Assert either that the apply is refused, or that `is_tradable(*book.at(1))` is false or reports stale. Separately, apply one update, poll with `now` at +10 s, and assert that some staleness predicate exists.

### C03-002 — The normalise/book/failover path exists only in tests (P2, DESIGN GAP)
- **Evidence.** The only production callers of `decode_kite_frame` are app/kite_ticker_main.cpp:293 and app/price_service_main.cpp:378. Neither constructs `Normaliser`, `PlausibilityGate`, `FailoverWatchdog` or `L2Book`. A grep across all first-party dirs finds no instance outside feed/ and book/ tests; desktop/model_status.hpp:323 only names the normaliser in a string. Decoder output goes straight to `TickStoreWriter` (kite_ticker_main.cpp:303) or to the bus (price_service_main.cpp:394-447).
- **Consequences.**
  - No rule-7 ceiling. A future exchange_ts is published unchanged.
  - No Stale flags are ever set.
  - `seq` is the decoder's counter, not the normaliser's.
  - `recv_ts` comes from `std::chrono::system_clock` (kite_ticker_main.cpp:288-291, price_service_main.cpp:374-377), a non-monotonic wall clock. tick.hpp:85-88 documents it as the TSC clock and "locally monotonic".
  - The NoExchangeTs fallback therefore puts wall-clock time into `exchange_ts`.
- **Impact.** The rule-6 claim that live and replay share one path has no code carrying it. Today these streams feed a recorder and a display bus, not orders, hence P2.
- **Regression test.** An integration test that runs recorded frames through decoder → Normaliser → L2Book, the same function the live binary calls. It should fail if the binary bypasses that function.

### C03-003 — The recorder writes ticks only, ignores errors and records the wrong session date (P2, CONFIRMED DEFECT, integration; file owned by the app group)
- **Evidence.**
  - kite_ticker_main.cpp:301-303 loops over `r->ticks` only. `r->depths`, decoded in full mode (the default, :180-189), is never written, although tick_store.hpp:8-12 and test_tick_store.cpp:139-171 treat the tick/depth interleaving as information.
  - `(void)writer.write(t)` at :303 and `(void)writer.close()` at :405 discard `WriteFailed`.
  - `session_date` is `system_clock::now()` (:274-277), while tick_store.hpp:58 documents `session_date_ns` as "IST midnight of the session".
- **Trigger.** Run `altair_kite_ticker --go --store X` in full mode: the tape has zero Depth records. Fill the disk: the recorder carries on and reports success.
- **Impact.** Recorded sessions cannot rebuild a book, and can be silently incomplete.
- **Tests.** None cover the executable.
- **Regression.** Record a synthetic full-mode frame through the same on_frame code, read it back, and assert one Tick and one Depth record. Assert that the header session date is 00:00 IST.

### C03-004 — Tick-store format and robustness gaps (P2, DESIGN GAP)
- **Identity.** The file stores `InstrumentId` (tick.hpp:73). Ids are SpecStore insertion indices (contract_spec.hpp:220-222), and the recorder registers placeholder specs `TOKEN%u` in command-line order (kite_ticker_main.cpp:243-254). The header (tick_store.hpp:51-60) has no `spec_version` (contract_spec.hpp:353) and no token map, so a tape replayed against any other store maps ticks to the wrong contracts. This breaks the rule-10 tuple.
- **Truncation on reopen.** `open` uses `"wb"` (tick_store.hpp:84). Restarting the recorder with the same path mid-session erases the earlier tape.
- **Read error reported as EOF.** `next` returns `false` (clean EOF) whenever `fread` of the tag returns 0 (236-238). A read error is indistinguishable, and `ReadFailed` is never returned from `next`.
- **Failed header write.** If the header fwrite fails (98-100), `f_` stays open and later `write()` calls append records to a headerless file.
- **Torn record mid-file.** There is no per-record length or CRC. A torn or partial record inside the file, for example after a failed fwrite followed by a successful one, desynchronises framing. That is caught only if the next tag byte happens not to be 1 or 2; otherwise plausible garbage comes back, which is the outcome D3 exists to refuse.
- **No content validation.** Nothing checks what the reader returns (for example `bid_levels <= 5`); see C03-005.
- **Cosmetic.** The magic constant 0x414C5441494B5354 is written little-endian, so the bytes on disk read "TSKIATLA", not the "ALTAIKST" in the comment at 32.
- **Tests.** test_tick_store covers a tail truncation, bad magic/version/size and an unknown trailing tag. It does not cover mid-file corruption, a read error, reopen truncation or identity.
- **Regression.** Write 3 ticks, overwrite byte 48+1+10 with a partial-record pattern whose next tag byte is 0x01, and assert that the reader does not return 3 valid records. Open an existing path and assert that it refuses or appends.

### C03-005 — Level counts are not bounded (P2, CONFIRMED DEFECT, latent; rule 11)
- **Evidence.** l2_book.hpp:165-166 stores `d.bid_levels`/`d.ask_levels` unchecked. The copy loop at 169-171 is bounded by kDepthLevels, but the stored count is not. microstructure.hpp:53-56 then sets `n = min(levels, populated)` and reads `lv[k]` for k < n. features/book_flow.hpp:115/118 calls `bid_depth(b, s.depth_levels)`. tick.hpp:134-135 carries a u8 count.
- **Trigger.** `DepthUpdate d{}; d.id=InstrumentId{1}; d.seq=1; d.bid_levels=9; d.ask_levels=9; set bid[0]<ask[0]; book.apply(d); bid_depth(**book.at(InstrumentId{1}), 9)` reads `bid[5..8]`, which aliases ask memory. `ask_depth(..., 9)` reads past `ask[]` into the tail of BookState and the next array element: UB. `weighted_obi(b, 9)` behaves the same way (microstructure.hpp:120-132).
- **Reachability.** The Kite decoder cannot produce a count above 5 (kite_decoder.hpp:253-276). A corrupted tick-store record (C03-004) or a future XTS decoder can. desktop/depth_ladder.hpp:112-115 clamps its own read correctly.
- **Tests.** None use a count above 5.
- **Regression.** Apply a DepthUpdate with `bid_levels = 6` and assert `BookError` (a new BadLevels), with the book unchanged.

### C03-006 — `classify_trade` ignores crossed books (P3, CONFIRMED DEFECT)
- **Evidence.** flow.hpp:44-55 uses `mid(prior)`. `mid(BookState)` (l2_book.hpp:89-97) checks only that both sides exist, not `crossed`. test_flow.cpp:71-73 claims "a crossed book classifies nothing -- it has no usable mid", but with bid 2,500,100 and ask 2,500,000 the computed mid is exactly 2,500,050, which is the traded price used. The assertion holds only by coincidence.
- **Trigger.** `classify_trade(Price{2'500'060}, quoted(2'500'100, 2'500'000))` returns `Buy`. A locked book returns Buy/Sell for any price away from the lock.
- **Impact.** The uncross print after the pre-open auction, and prints against crossed or locked books, feed a side into VPIN and Kyle lambda. No executable calls `classify_trade` today.
- **Regression.** Add the call above and assert `Unknown`. Fix by gating on `is_tradable(prior)`.

### C03-007 — Outage between polls is invisible, and hysteresis can be defeated (P2, CONFIRMED DEFECT, latent)
- **Evidence.** failover.hpp:76-87: `on_data` keeps only a high-water mark and never detects a gap. Health is derived only inside `poll()` (168-183) from `now - last_`. `live_since_` is reset only when a poll observes Stale (179-181). `stable_for` (185-193) trusts `live_since_`.
- **Trigger.** Config staleness 250 ms, failback 60 s, poll every 1 s. Primary Kite has failed over to XTS. Kite then delivers only in the last 100 ms of every second (silent 900 ms of each 1,000 ms). Every poll sees a gap of 250 ms or less, so Kite reads Live, `live_since_` is never reset, and after 60 s the watchdog fails back to a feed that is silent 90% of the time. Likewise, a 5 s outage that ends before the next poll is never counted or reported.
- **Other.** Switch-away has no hysteresis: one poll that sees a gap over 250 ms switches (128-138), which is a documented choice. Config is not validated; a negative `staleness_ms` marks every source Stale permanently.
- **Tests.** test_failover.cpp polls immediately after each on_data. There is no poll-cadence-longer-than-outage case.
- **Regression.** Reproduce the flapping pattern with polls at 1 s and assert no failback (or a gap counter above 0). Fix by detecting `recv_ts - last_ > staleness` inside `on_data` and resetting `live_since_` there.

### C03-008 — Unknown→NoneHealthy: the transition is defensible, the documentation contradicts it (P3)
- **Evidence.** failover.hpp:103-106 treats Unknown as not Live, so two Unknown sources give `NoneHealthy` and `++stats_.none_healthy`. The enum comment at :37 says NoneHealthy means "BOTH sources are stale". The D2 comment at :165-167 says treating unheard as stale "would trip `NoneHealthy` before the session has begun", yet the code trips it anyway. test_failover.cpp:67-68 asserts NoneHealthy at session start "which is the truth".
- **Verdict.** Refusing to call an unheard feed usable is correct. The defect is the contradictory comments, plus `none_healthy` counting every pre-open poll, so an alarm on that counter fires at every session start. `FailoverDecision` still carries `kite`/`xts` Unknown, which lets a caller tell the cases apart.
- **Fix.** Add an `Unknown` action, or a `not_started` count separate from `none_healthy`, and fix the comments. The regression test asserts `stats().none_healthy == 0` after polls with nothing heard.

### C03-009 — Partial-frame loss in the Kite decoder (P3, CONFIRMED DEFECT)
- **Evidence.** kite_decoder.hpp:147-157 returns `ShortFrame` mid-loop after earlier packets have already been written to `ticks[]`/`depths[]` and `seq++` has run (204, 245). The error carries no counts. Callers do `if (!r) { ++bad; return; }` (kite_ticker_main.cpp:296, price_service_main.cpp:381), losing the good packets and leaving a seq gap. The all-or-nothing rationale at 133-140 covers only capacity. After the loop, `pos < len` (trailing bytes) is never checked, so a `count` smaller than the real packet count silently drops the remainder.
- **Other.** The index layout is chosen by segment byte (200, 214), while the reference picks it by length (ticker.go:674). A non-index token in a 28/32-byte packet would be read with equity offsets. There is no `last > 0` check.
- **Trigger.** Frame count=3 with packets [LTP 8][LTP 8][len 184 but only 20 bytes present]: 2 ticks are written, seq advances by 2, and the result is `ShortFrame`.
- **Tests.** test_kite_decoder.cpp:213-220 covers only a single-packet overrun.
- **Regression.** Build the frame above and assert either a partial result carrying counts, or that neither seq nor the arrays were touched. Add a trailing-garbage frame and assert a counter.

### C03-010 — Vpin bounds (P3, CONFIRMED DEFECT, rule 11)
- **Evidence.** flow.hpp:155-160 silently clamps `window > 64` to 64 and lowers `min_buckets` to the window. That clamps toward less evidence, not the safe side: `Config{1000, 100, 100}` reports after 64 buckets. `Reading.buckets` (83) does expose n, so the clamp is partly visible. `bucket_volume <= 0` returns early for every trade with no counter (90-92). `on_trade` iterates `volume / bucket_volume` times inside ALTAIR_HOT (94-119): a 4e9-unit print against a 1-unit bucket loops about 4e9 times, although only 64 buckets are retained.
- **Callers.** desktop/quant_pages.hpp:2188-2211 uses window=50 (demo); features/book_flow.hpp is generic.
- **Regression.** Construct with window=100 and assert refusal, or assert `value()->buckets` matches config. Time `on_trade(Buy, Qty{1'000'000'000})` with bucket 1 and assert it is bounded.

### C03-011 — Normaliser silent-bound details (P3)
- **Bound.** normaliser.hpp:161-171: for `id >= kMaxInstruments` (for example `InstrumentId::Invalid`) the ordering check is skipped and the tick still publishes with a seq, with no counter. This cannot be reached from the Kite decoder, since SpecStore refuses more than kMaxInstruments (contract_spec.hpp:208). The bound nonetheless neither refuses nor counts.
- **Comment.** :70-72 says the standby feed "is gated and tracked so its staleness is observable", but the code returns DropInactive before `judge`, so the standby is never gated.
- **Doc mismatch.** :187 says "~128 KB"; the arrays are about 73 KB (8192×8 + 8192), and test_normaliser.cpp:60 says 72 KB.
- **Regression.** Submit a tick with id Invalid and assert it is refused or counted.

### C03-012 — Book payload pads the shorter side with zero-price levels (P2, DESIGN GAP, cross-group: app/server owners)
- **Evidence.** price_service_main.cpp:428-443 sets `levels = min(5, max(bid_levels, ask_levels))` and copies `d.bid[k]`/`d.ask[k]` for all k < levels. server/price_payload.hpp:101 states "Both sides carry the SAME count". A book with 5 bids and 0 asks is therefore published as 5 asks at price 0, the exact hazard tick.hpp:131-133 and best_ask() exist to prevent.
- **Impact.** HYPOTHESIS: depends on how desktop/price_client.hpp:226 onward renders zero prices. Not verified; outside G03 coverage.
- **Regression.** Publish from a DepthUpdate with bid_levels 5 and ask_levels 0, decode the frame, and assert the consumer sees 0 ask levels.

### C03-013 — Sequence has no session epoch (P3, DESIGN GAP)
- **Evidence.** `Normaliser::reset_session` sets `seq_ = 0` (normaliser.hpp:122-127). `L2Book` has only `clear()`, and `apply` rejects `d.seq <= b.seq` (l2_book.hpp:152). The u32 seq can also wrap.
- **Trigger.** Reset the normaliser at session rollover without clearing the book. Every instrument's updates are then StaleSequence until the new seq passes the old one, and the book keeps showing yesterday's `is_tradable` state.
- **Regression.** Apply seq 100, reset the normaliser, submit and apply a new depth (seq 0), and assert the book updates or the mismatch is reported.

### C03-014 — Estimators without an error bar (P3, DESIGN GAP)
- flow.hpp:206-210, 236-244: KyleLambda returns lambda, n and an uncentered R² but no standard error, so "size on the lower confidence bound" (CLAUDE.md physics discipline) cannot be applied. The accumulators at 226-228 are session-cumulative with no window and no Kahan summation. VPIN likewise has no dispersion. The desktop text claims lambda "is a MEASURED quantity with an error bar" (quant_pages.hpp:2227), which is not true of the returned Reading.

### C03-015 — Tick-store test hygiene (P3)
- test_tick_store.cpp:38 writes `altair_tick_store_test.bin` in the working directory; it is removed only at the end of test 8 (:380), so any earlier abort leaves it behind, and two concurrent runs in one directory collide. :303 assumes `altair_no_such_store.bin` does not exist. :221-231 and :272-296 use `fopen` without a null check before `fseek`/`fwrite`, so a failed open crashes the test. Use a unique temp path.

## 4. Lead revalidation

1. **"ReplayTick, canonical tick storage and CSV bar replay follow different paths" — CONFIRMED.**
   - **Path A: `ReplayTick` + `Replayer` (feed/replay.hpp).** A 40-byte struct `{ts, seqno u64, token u32 broker token, last, qty}` with no id, depth, flags or source. The cursor has no peek (replay.hpp:47-73, 89-188). Used by app/main.cpp (synthetic selftest), desktop/main.cpp demo, desktop/main_window.hpp, tick_model.hpp, chart/candles.hpp and desktop/data/real_tape.hpp. real_tape converts CSV 1-minute bars to ReplayTick with the Kite token, stamped at bar END (real_tape.hpp:184-199). Stale comments in desktop/main_window.hpp:15/487 and tick_model.hpp:200 say Replayer "has no rewind", but replay.hpp:174 has `rewind()`.
   - **Path B: canonical `Tick`/`DepthUpdate` (feed/tick.hpp).** Produced by `decode_kite_frame` in app/kite_ticker_main.cpp (ticks only, into TickStoreWriter, C03-003) and app/price_service_main.cpp (onto the PricePayload bus). No normaliser (C03-002).
   - **Path C: TickStoreReader.** Tests only. No cursor over `Tick`/`DepthUpdate` exists, and there is no Tick→ReplayTick adapter.
   - **Path D: CSV bar replay in price_service.** app/price_service_main.cpp:279-330 turns `Bar{ts_ns, double close}` into a PricePayload (`last_paise = close*100+0.5`, `exchange_ts_ns = b.ts_ns`, flag `kPriceReplay`) on the bus. HYPOTHESIS: if dataset stamps are bar START, as real_tape's end_ns handling suggests, this path publishes each close one interval early, unlike real_tape. Owning group to confirm.
   - **Verdict.** No single decision path is shared by live and recorded replay. Recorded ticks cannot today be replayed into a normaliser/book path: there is no reader caller, the store holds no depth, identity is unresolvable (C03-004), the recorded seq is the decoder's, and Replayer accepts only ReplayTick.
2. **"Stale depth and failover recovery semantics" — CONFIRMED / PARTIAL.**
   - Stale depth with a newer seq is applied as tradable: CONFIRMED (C03-001, latent).
   - An outage between polls can be missed and can defeat failback hysteresis: CONFIRMED (C03-007).
   - Recovery: switch-away is immediate; failback needs `failback_stable_s` of Live observed at polls.
   - Unknown→NoneHealthy: the transition is not wrong; the comments contradict it and the stat conflates pre-session with outage (C03-008). PARTIAL.
3. **feed/kite_decoder.hpp — PARTIAL (mostly sound).**
   - Sound: the heartbeat (<2 bytes) is accepted; `count` is checked up front against both caps; each packet length header and payload is bounds-checked; lengths other than 8/28/32/44/184 are skipped and counted.
   - Offsets match gokiteconnect ticker.go: LTP 4; qty 8; vol 16; OI 48; ts 60; index ts 28; depth at 64/124 in 12-byte entries.
   - Depth is bounded to 5 per side and stops at the first zero price. Big-endian words are assembled byte-wise. Seconds are widened to i64 before ×1e9. CD scales are refused.
   - Defects: partial ShortFrame loss, unchecked trailing bytes, and segment-vs-length index detection (C03-009).
   - Contract mismatch: tick.hpp:90-92 says the decoder "applies ContractSpec::price_scale", but the decoder derives the scale from the token's segment byte (kite_decoder.hpp:173-183) and never reads the spec's scale.
4. **feed/normaliser.hpp spec binding — PARTIAL.**
   - The normaliser binds no specs. It takes `InstrumentId` from the decoder, which maps through `SpecStore::id_of` (kite_decoder.hpp:186). Unknown tokens are skipped and counted (`unknown_token`); blocked ids are skipped and counted.
   - Placeholder specs are CONFIRMED in both executables: `TOKEN%u`, lot 1, tick 1, scale 100 (kite_ticker_main.cpp:243-254, price_service_main.cpp:350-361). They are documented as not used for sizing. Ids are recovered by insertion order (:316-318, :391-395).
   - The normaliser adds nothing for unknown ids apart from the silent bound (C03-011).
5. **feed/tick_store.hpp — CONFIRMED with qualifications.**
   - Header validation: magic, file version, wire version and both struct sizes are all checked (207-224).
   - A torn tail gives `Truncated`, and test_tick_store.cpp:209-265 proves 4 of 5 records are recovered.
   - Gaps: a torn middle record, a read error treated as EOF, truncation on reopen, unresolvable identity (C03-004).
   - The fixed-name test file in CWD is CONFIRMED (C03-015).
6. **book/ — PARTIAL.**
   - The crossed/locked guard is correct and the book is stored and marked (l2_book.hpp:176-180).
   - Level counts are unvalidated (C03-005).
   - Microprice uses the correct crossed weighting, exact integer arithmetic, and product and sum overflow guards (microstructure.hpp:156-183). One note: it truncates toward zero rather than rounding to tick.
   - OBI stays within [-1, 1] with empty-not-zero behaviour; weighted OBI uses 1/(1+k).
   - VPIN is bounded at 64 buckets, but the clamp is silent (C03-010).
   - Kyle lambda is a through-origin fit: uncentered R² clamped to [0, 1] with no SE (C03-014). By Cauchy-Schwarz R² = (Σvdp)²/(ΣvvΣdpdp) is already within [0, 1], so the clamp only absorbs rounding.
   - `classify_trade` ignores crossed books (C03-006).

## 5. Architecture facts

- **Ownership.**
  - feed/ owns the normalised struct layout (Tick 64 B, DepthUpdate 272 B, pinned by static_asserts), the Kite frame decoder, the normaliser (gate + per-instrument order + seq), the failover watchdog, the tick-store file format, and the Phase-0 ReplayTick cursor.
  - book/ owns the L2 snapshot store (BookState 272 B × 8192, about 2.13 MB) and stateless and stateful microstructure estimators.
  - Everything is header-only INTERFACE libraries (feed/CMakeLists.txt:6-16, book/CMakeLists.txt:4-14).
  - Nothing in G03 places orders or links oms/broker.
- **Threads and shared state.** No G03 component creates threads or uses atomics or locks. Replayer, PlausibilityGate, Normaliser, FailoverWatchdog, L2Book, Vpin and KyleLambda are single-owner mutable objects. BookState is trivially copyable "to ride a seqlock", but no seqlock exists here. TickStoreWriter is documented as owned by a recorder thread; in kite_ticker_main it runs inside the socket callback.
- **Queues, capacities and overflow.**
  - Queues: none inside G03. The tests push Tick and DepthUpdate through `SpscRing<.,64>` (test_tick.cpp:244-279).
  - Decoder: output capacity is the caller's (512/512 in both apps). Overflow gives `OutputFull`, all-or-nothing. Framing loss gives `ShortFrame`. Unknown token, blocked id, CD scale and bad length are skip+count.
  - Normaliser: `DropFuture` and `DropInactive` are counted. Floor breach and regression give Stale+publish. Ring-full is reported by the caller via `note_dropped_full`.
  - L2Book: `BadInstrument` and `StaleSequence` are reject+count, with a per-instrument `consecutive_rejects` that saturates at 0xFFFF. Crossed is store+mark.
  - Vpin: 64-bucket ring with a silent config clamp.
  - Tick store: `Truncated`, `BadRecord`, `BadMagic`, `BadVersion` and `BadWireVersion` are all refusals.
- **Hot vs cold.**
  - ALTAIR_HOT: `decode_kite_frame`, `be16`, `be32`, `Normaliser::submit`/`judge`, `FailoverWatchdog::on_data`, `L2Book::apply`/`at`, obi/weighted_obi/microprice/depth, `classify_trade`, `Vpin::on_trade`, `KyleLambda::on_observation`, `Replayer::next`/`now`.
  - Warm/cold: `FailoverWatchdog::poll`, `Replayer::validate`, the TickStore writer and reader (buffered stdio).
  - No heap allocation or exceptions anywhere in G03 headers (all noexcept, fixed arrays). The one unbounded hot loop is `Vpin::on_trade` (C03-010).
- **Conventions.**
  - Time: `Timestamp` is i64 ns since the Unix epoch, UTC. Kite exchange_ts has 1 s resolution. LTP, quote and index-quote modes have no timestamp, and `exchange_ts = recv_ts` with `NoExchangeTs` set. `recv_ts` is documented as TSC/monotonic but the executables use `system_clock` (C03-002). The store header's session date is documented as IST midnight; the recorder writes now() (C03-003).
  - Price: i64 paise, taken directly from the u32 wire at scale 100. CD segments are refused.
  - Qty: i64 units from u32 wire. OI is a bare i64.
  - Identity: canonical `InstrumentId` is the dense SpecStore insertion index. `ReplayTick` alone carries a broker token.
  - Sequence: u32, one counter shared by ticks and depth in the Normaliser. The decoder also advances the caller's u32 seq.
- **Implemented vs connected.**

  | Component | Production caller |
  |---|---|
  | decode_kite_frame | app/kite_ticker_main.cpp, app/price_service_main.cpp |
  | TickStoreWriter | app/kite_ticker_main.cpp (ticks only) |
  | TickStoreReader | tests only |
  | Normaliser / PlausibilityGate (via normaliser) | tests only |
  | FailoverWatchdog | tests only |
  | L2Book (class) | tests only |
  | BookState + microstructure | features/book_flow.hpp (itself not called by app/desktop per grep), desktop/depth_ladder.hpp (always `show_book(nullptr)` at main_window.hpp:677), desktop/quant_pages.hpp (constructed demo books) |
  | Vpin/KyleLambda | desktop/quant_pages.hpp demo text, features/book_flow.hpp generic |
  | classify_trade | none |
  | Replayer/ReplayTick | app/main.cpp, desktop (grid, chart, tick model, real_tape) |

- **Live vs recorded-tick vs bar replay.** Live Kite frames go decoder → bus (price_service) or decoder → tick store (kite_ticker). Recorded ticks: written, never read. Bar replay has two independent implementations: price_service `--replay` (bus, `b.ts_ns`) and desktop real_tape (ReplayTick at bar end). None of them share the normaliser/book decision path.

## 6. Test-suite observations

- **Style.** All nine tests are plain `main()` with `check()`. Latency gates are conditional on `latency_gate_active()` (test_kite_decoder.cpp:539-546, test_normaliser.cpp:351-360, test_l2_book.cpp:339-348). test_replay's throughput check only prints OK or OVER and never fails (:312-313).
- **Strong points.**
  - Byte-identical round trips: test_tick.cpp:263-279 and test_tick_store.cpp:84-136.
  - Odd-aligned decode (test_kite_decoder.cpp:418-454).
  - Up-front capacity with no write (:457-493).
  - The swapped-microprice trap (test_microstructure.cpp:109-146).
  - Snapshot replace zeroing old levels (test_l2_book.cpp:217-242).
  - Seq continuity across a switch (test_failover.cpp:242-290, test_normaliser.cpp:219-254).
- **Gaps and false assurances.**
  - test_flow.cpp:71-73 passes by coincidence (C03-006).
  - No Normaliser→L2Book composition test (C03-001).
  - No multi-packet partial ShortFrame or trailing-bytes test (C03-009).
  - No level count above 5 (C03-005).
  - No failover poll cadence longer than an outage (C03-007).
  - Tick store: no mid-file corruption, read-error or reopen-truncation test (C03-004).
  - No decoder test for a non-index token in 28/32-byte packets, or an index token in 184 bytes.
  - test_replay.cpp:136-143 uses `check(true, ...)`; the real guarantee is the static_asserts at :60-76.
  - test_failover and test_normaliser reset globals by explicit destructor plus placement new (test_failover.cpp:51-55, test_normaliser.cpp:64-68). That is legal but fragile.
  - Executables that decode frames have no test of their on_frame composition.

## 7. Open questions

1. Are dataset/ CSV stamps bar START? If so, does price_service `--replay` publish each close one interval early (lead 1, path D)?
2. How does desktop/price_client.hpp render zero-price padded levels from a `kPriceHasBook` payload (C03-012)?
3. What poll cadence is intended for `FailoverWatchdog::poll`, and is sub-poll outage detection a requirement (C03-007)?
4. Will an XTS decoder or other source ever emit more than 5 depth levels, or will recorded tapes be read without validation (C03-005)?
5. Do any tick-store tapes already exist from `altair_kite_ticker --store`? They lack depth and any token or spec mapping (C03-003/004).
6. Should the normaliser's Stale flag make a book non-tradable, or is freshness meant to be a separate consumer-side check? Either way, it needs a stated owner (C03-001).
