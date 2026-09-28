# CX-01 G18 desktop widgets & data -- findings

Reader: claude-subagent:G18_desktop_widgets_data · Date 2026-09-14 · Baseline a34af5c5c9fa8c7498e8cc4df00907548a41e1a5

## 1 Scope & coverage

28 of 28 assigned files fully read (7,811 lines), in chunks of 300 lines or fewer. The sha256 before and after reading matched the inventory for every file. Nothing was built, run or modified apart from the two output files.

Evidence from outside the group was read-only and is not counted as coverage. It is listed in the coverage JSON `notes`.

Group contents:
- **Engine-facing data:** bar_csv, real_tape, master_lookup, price_client, tick_model
- **Pills and status:** broker_status, feed_status, market_clock, audit_panel
- **Subprocess panels:** kite_link, kite_panel, live_feed
- **Widgets:** atlas, atlas_data, depth_ladder, filter, account_widgets, format, svg
- **Tests:** 8 files

No file in the group places an order or links `oms/` or `broker/`.

## 2 Findings table

| ID | Sev | Class | Where | One line |
|---|---|---|---|---|
| C18-013 | P2 | CONFIRMED DEFECT | price_client.hpp:157,177-178; server/protocol.hpp:335 | Reassembly buffer is unbounded because `payload_len` is never capped; one bad header can grow memory to about 4 GiB in the engine process |
| C18-015 | P2 | CONFIRMED DEFECT | live_feed.hpp:161-163; kite_link.hpp:260-265,399; kite_panel.hpp:355-356 | Synchronous subprocess waits on the GUI thread: up to 330 s, 600 s and 45 s |
| C18-006 | P2 | DESIGN GAP | desktop/CMakeLists.txt:203-204; CMakePresets.json:23; main.cpp:280-292 | admin/admin, staff/staff and a password-less `--as admin` are in every non-Release build, including the default RelWithDebInfo preset |
| C18-001 | P3 | CONFIRMED DEFECT | atlas.hpp:133,169-176 | Double-clicking a family node opens nav page 0 (Live Grid) |
| C18-002 | P3 | DESIGN GAP | atlas_data.hpp:80-83 | Page indices are range-checked but not checked for meaning; the header comment is stale |
| C18-003 | P3 | DESIGN GAP | account_widgets.hpp:144 | `account_table` returns a table with sorting on, so every fill must remember to turn it off |
| C18-004 | P3 | CONFIRMED DEFECT | audit_panel.hpp:168-171,190-198 | Every 500 ms, re-reads and FNV-hashes the whole instrument master on the GUI thread, forever |
| C18-005 | P3 | CONFIRMED DEFECT | audit_panel.hpp:245-249; main_window.hpp:1510,259 | The replay tick count is shown as "LIVE", in green, even at 0 |
| C18-007 | P3 | CONFIRMED DEFECT | broker_status.hpp:226,236-237,257 | login_time is parsed as local time but compared to an IST-shifted UTC instant, so the future-guard and "h ago" are off by the host UTC offset |
| C18-008 | P3 | DESIGN GAP | broker_status.hpp:192-196 | Whole session JSON, tokens included, is loaded into UI memory; "never reads the token" holds only at field level |
| C18-009 | P3 | DESIGN GAP | bar_csv.hpp:124-172,237-316 | No OHLC sanity check, duplicates survive the merge, no header-column check, exact "x.500" counted as rounded, `*100` overflow unchecked |
| C18-010 | P3 | DESIGN GAP | master_lookup.hpp:110-168 | Parser's per-row drops are never surfaced; 8.8 MB master loaded synchronously |
| C18-011 | P3 | CONFIRMED DEFECT | real_tape.hpp:139-142,78; main.cpp:328 | A missing instrument directory's error is dropped whenever any other instrument loaded |
| C18-012 | P3 | CONFIRMED DEFECT | main_window.hpp:1488; feed_status.hpp:277-298 | `api_key_present` is hard-coded `true`, so NO API KEY can never show |
| C18-014 | P3 | CONFIRMED DEFECT | price_client.hpp:205-210,92-99 | Duplicate or backward seq is silently re-applied; seq state is not reset on reconnect |
| C18-016 | P3 | CONFIRMED DEFECT | kite_link.hpp:69-81,305,307 | Redaction only matches `request_token=`; a bare pasted token is echoed to the log |
| C18-017 | P3 | CONFIRMED DEFECT | filter.hpp:81-105 | "-", "+", "." parse as 0 and "1.-5" as 95 paise, then apply as a filter bound |
| C18-018 | P3 | CONFIRMED DEFECT (by reading) | filter.hpp:264-268 | Blank cells sort FIRST in both directions; the comment says LAST |
| C18-019 | P3 | CONFIRMED DEFECT | main_window.hpp:542-544; filter.hpp:316-317,377 | Volume and Seq filters are parsed as rupees, so the bound is 100x the typed value |
| C18-020 | P3 | CONFIRMED DEFECT | tick_model.hpp:95-103,179-185 | "Change (INR)" is against the first tick since reset, not the session open; volume also accumulates across sessions |
| C18-021 | P3 | CONFIRMED DEFECT | kite_panel.hpp:416-427,492,515,527; live_feed.hpp:264-271 | On a parse failure, the previous snapshot's figures stay on screen without their age; absent JSON numbers render as ₹0.00 |
| C18-022 | P3 | DESIGN GAP | market_clock.hpp:82-98; main_window.hpp:254,476; bar_csv.hpp:199-200; real_tape.hpp:91-92 | Session times and a contract token/expiry are literals in the production window (rule 1) |

## 3 Finding details

### C18-013 · P2 · CONFIRMED DEFECT · unbounded client reassembly buffer
- **Evidence.**
  - `price_client.hpp:157` `buf_.append(sock_->readAll());`
  - `:177-178` `const std::size_t need = kFrameHeaderBytes + h->payload_len;` then `if (buf_.size() < need) { break; }`
  - `server/protocol.hpp:335` `h.payload_len = wire::get_u32(in + 20);`. `decode_header` (:315-338) validates magic, version, kind and channel but has no maximum for `payload_len`.
  - `set_read_buffer` caps Qt's buffer, not `buf_`.
- **Trigger.** A peer on the loopback port sends a valid 48-byte header with `payload_len = 0xFFFFFFF0`, then keeps streaming bytes. Possible peers: a price-service bug, a mismatched wire version that still passes the check, or any local process holding 7421 while the service is down. The client does not authenticate the server.
- **Impact.** `buf_` grows until allocation fails. Under the in-process decision, that kills the process holding engine state. This is rule 11: a buffer with no bound.
- **Tests.** test_price_client covers bad magic, gaps and reassembly only.
- **Regression.** A crafted header with valid magic and version, `payload_len = 16 MiB + 1`, followed by more bytes. Expect `undecodable() > 0` and `!connected()` with `buf_` bounded. This needs a `kMaxPayloadBytes` refusal in `decode_header` (server card) or in the client.

### C18-015 · P2 · CONFIRMED DEFECT · GUI thread blocked by subprocess waits
- **Evidence.**
  - `live_feed.hpp:161-163` `proc.waitForStarted(5000) && proc.waitForFinished((secs_->value() + 30) * 1000);` with the spin box range 5-300 (:84).
  - `kite_link.hpp:263` `waitForFinished(30000)` and `:399` `waitForFinished(600000)`. `update_dataset` runs automatically after every successful link (:313).
  - `kite_panel.hpp:355-356` `waitForFinished(45000)`.
  - Each has only a single `QApplication::processEvents()` before the wait.
- **Trigger.** Press Listen at 300 s, or complete Link Kite, which chains into the 10-minute updater.
- **Impact.** The engine and UI share one process, and much of it runs on the GUI event loop:
  - The replay pump and its timers (main_window).
  - The PriceClient socket drain (terminal.hpp:328).
  - The broker pill.
  - The admin kill-switch request control.

  All of these freeze for up to 10 minutes. The unread socket backs up and PriceClient then replays stale frames on resume.
- **Tests.** None; test_kite_link says so explicitly (:5-7).
- **Regression.** Point `find_kite_login` at a stub binary that sleeps 20 s. Assert that a `QTimer::singleShot(100 ms)` fires before the stub exits, i.e. the call is asynchronous via `QProcess::finished`.

### C18-006 · P2 · DESIGN GAP · dev accounts and `--as` bypass in the everyday build
- **Evidence.**
  - `desktop/CMakeLists.txt:203-204` `if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")` then `ALTAIR_DEV_CREDENTIALS`.
  - `CMakePresets.json:23`: the `default` preset is `RelWithDebInfo`.
  - `main.cpp:280-292`: under the same define, `--as admin` sets `Role::Admin` with no password.
  - `auth.hpp:97-99`: Admin may do everything, including `RequestOrder` and `RequestKillSwitch`.
- **Trigger.** `build\default\desktop\altair_desktop.exe --as admin`, or logging in as admin/admin. This works in every preset except `prod`.
- **Impact.** The gate is meant to stop "the person who sits down at an unattended desk" (auth.hpp:28-29). In the build actually run day to day, anyone can relaunch with `--as admin` and write order intents. The header is accurate about `prod`; it does not say that `default`, `debug`, `asan` and `tsan` all carry the bypass.
- **Password handling itself is sound:**
  - Constant-time compare (:143-155).
  - A dummy stretch for unknown users (:176-181).
  - A 16-byte system-RNG salt.
- **Minor.** 200k SHA-256 rounds run synchronously on the GUI thread, with no attempt lockout.
- **Tests.** None for auth.
- **Regression.** A configure-time check that `ALTAIR_DEV_CREDENTIALS` is absent from any preset intended for live use. Also a unit test on `may()` asserting Staff cannot RequestOrder or RequestKillSwitch.

### C18-001 · P3 · CONFIRMED DEFECT · family double-click opens page 0 (Codex UI-005)
- **Evidence.**
  - `atlas.hpp:169-175`: family items get text, font and expansion, but no `setData`. Only child rows call `it->setData(0, Qt::UserRole, r.page)` (:182).
  - `:133` `const int page = item->data(0, Qt::UserRole).toInt();`. An invalid QVariant gives 0.
  - `:137` `if (page >= 0 && go_) { go_(page); }` passes, and main_window.hpp:1347 calls `show_page(0)`, which is "Live Grid" (main_window.hpp:97).
- **Impact.** Navigates away from the Atlas to the wrong page. The code's own comment (:134-136) calls opening page 0 "a lie".
- **Tests.** test_atlas never emits `itemDoubleClicked`.
- **Regression.** In test_atlas: `auto* t = panel.findChild<QTreeWidget*>(); Q_EMIT t->itemDoubleClicked(t->topLevelItem(0), 0);` then check `jumped_to == -99`.

### C18-002 · P3 · DESIGN GAP · Atlas routing by raw index
- **Evidence.**
  - `atlas_data.hpp:80-83` says "post-P35-03b layout (32 pages, 0..31)" and that the audit "checks paths, not indices". nav_page_names() now has 33 entries, with "Model Atlas" at index 32 (main_window.hpp:163-167), so the comment is stale.
  - main_window.hpp:1384-1397 aborts at startup if `page < -1 || page >= nav count`. That is a range check, not a meaning check.
- **Impact.** Inserting a nav row before an index would silently re-route Atlas buttons. The name-based `--page` form already exists (main_window.hpp:105-108) but the Atlas does not use it.
- **Regression.** A startup check pairing each row's page with an expected nav name.

### C18-003 · P3 · DESIGN GAP · sorting-enabled table factory
- **Evidence.**
  - `account_widgets.hpp:144` `t->setSortingEnabled(true);`
  - Callers disable sorting around fills at live_feed.hpp:256/274 and kite_panel.hpp:565/592, 617/642, 667/709.
  - live_feed.hpp:241-254 records the real incident: "India VIX's 11.57 appeared on the NIFTY 50 row".
- **Status.** No defect at any current call site.
- **Impact.** The next caller that forgets reproduces a price or quantity landing on the wrong symbol.
- **Regression.** Fill a 3-row `account_table` with sorting left on via `account_cell` and assert rows stay aligned. That test fails today, and it documents the hazard.

### C18-004 · P3 · CONFIRMED DEFECT · 2 Hz full-file hashing on the GUI thread
- **Evidence.**
  - `audit_panel.hpp:168-171`: a 500 ms timer calls `refresh()`, started in the constructor and never stopped.
  - `:196-198`: `fnv1a_file(master)` runs over `data/instruments.csv` (8.8 MB per master_lookup.hpp:13), plus `readAll` and a per-byte loop (:72-76). All config/*.toml files are re-hashed as well.
- **Impact.** Continuous I/O and CPU on the GUI thread while the page is hidden. It also rebuilds 20 `QTableWidgetItem`s each tick.
- **Regression.** Count `refresh()` calls while the panel is hidden; expect 0. Alternatively, hash only on `showEvent` or when a file's mtime changes.

### C18-005 · P3 · CONFIRMED DEFECT · replay seqno labelled LIVE
- **Evidence.**
  - `audit_panel.hpp:246-249` shows `QString::number(seqno_)` in green with the text `"LIVE — moving while you watch"`.
  - `main_window.hpp:1510` `set_tick_seqno(applied_)`, while `:259` sets `FeedSource::Replay`.
  - `seqno_` starts at 0 (:270).
- **Impact.** The same "LIVE over a replay" mistake P39-05 fixed on the status pill (feed_status.hpp:115-135). A zero before any tick is also shown as LIVE.
- **Regression.** Construct the panel with no tick and assert the status cell does not contain "LIVE". With the source set to Replay, assert "REPLAY".

### C18-007 · P3 · CONFIRMED DEFECT · session timezone mix
- **Evidence.**
  - `broker_status.hpp:226` `QDateTime::fromString(login, "yyyy-MM-dd HH:mm:ss")`. With no zone in the format, Qt returns local time.
  - `:144-146` `now_ist()` is a UTC QDateTime whose wall clock reads IST. `:237` `issued_ist > now.addSecs(300)` and `:257` `secsTo` compare instants.
- **Trigger.** Host in IST (UTC+05:30). A `login_time` 3 hours in the future is not flagged. The "(N h ago)" figure for an expired token is 5.5 h too large.
- **Correct as-is.** The calendar-date comparison (:256) uses wall-clock dates on both sides.
- **Tests.** test case 5 uses `now.addDays(1)`, which is flagged under any offset.
- **Regression.** A session stamped `now_ist() + 2 h`. Expect "FUTURE" in `detail`.

### C18-008 · P3 · DESIGN GAP · token bytes loaded into the UI
- **Evidence.**
  - `broker_status.hpp:192` `const QByteArray raw = f.readAll();` and `:196` `QJsonDocument::fromJson(raw, &err)`.
  - The header (:44-56) says the UI "never reads" `access_token`. It is not extracted into a field, but its bytes are in `raw` and in the parsed document, on the GUI thread, every ~5 s (main_window.hpp:1512-1517), in the process "most likely to crash and dump core".
- **Tests.** test_broker_status (:104-115) only checks that rendered fields exclude the token.
- **Regression.** Not testable by output. Remedy options: a streaming or partial parse, or have the login binary write a separate metadata file.

### C18-009 · P3 · DESIGN GAP · CSV loader checks
- **Evidence and gaps.**
  - No check that `low <= open,close <= high`, or that prices are positive (bar_csv.hpp:250-268). A negative price parses fine (:134).
  - Columns are assumed to be `time,o,h,l,c,v` with no header validation (:231-237). Extra columns are silently accepted.
  - `load_bars_dir` concatenates files then sorts (:357-373). Overlapping files keep both copies of a timestamp.
  - `frac.size() > 2` marks "279.500" as rounded (:160-161).
  - `whole.toLongLong * 100` can overflow for 18-digit input (:157).
  - Comment :320 says the 1m series is "one file per DAY", contradicting :348 and test_bar_csv.cpp:66-73 (month files).
- **Counts that are good.** Rounded, zero, absent and skipped counts are all carried out to the caller.
- **Regression.** A 3-row CSV containing a high<low row, a duplicate timestamp and "1.500". Expect a skipped or flagged count and `rounded_fields == 0`.

### C18-010 · P3 · DESIGN GAP · master load drops are invisible
- **Evidence.**
  - `master_lookup.hpp:162-168`: only `if (!rep)` is checked. Any rows kite_dump rejected or skipped are not counted in `MasterIndex`.
  - `:113-119`: the 8.8 MB file is read into a `std::string` synchronously. The call is at panels.hpp:931.
- **Impact.** A watchlist token whose master row was dropped shows as `found=false` with no count on screen (rule 11: visible truncation). The exchange mapping itself is correct.
- **Regression.** Load a master with one malformed row; expect `size()` N-1 and a surfaced drop count of 1.

### C18-011 · P3 · CONFIRMED DEFECT · partial real-tape error swallowed
- **Evidence.**
  - `real_tape.hpp:139-142` `if (!r.ok()) { out.error = r.error; continue; }` and `:78` `ok()` is `!ticks.empty()`.
  - `main.cpp:327-328` `set_tape_is_real(real.ok(), real.sessions, real.ok() ? QString() : real.error, ...)`.
  - `main.cpp:311-325` still adds a grid row for every instrument.
- **Trigger.** `dataset/spot/banknifty/1m` absent.
- **Impact.** The NIFTY BANK row stays permanently blank with no reason given. Blank, not zero, so no false price, but the load error is hidden.
- **Regression.** Point `load_real_tape` at a root missing one instrument directory. Expect `ok()` true and a non-empty `error`, and add a check that the caller renders it.

### C18-012 · P3 · CONFIRMED DEFECT · NO API KEY state unreachable
- **Evidence.**
  - `main_window.hpp:1488` `probe_broker(path, true)`.
  - `feed_status.hpp:277-282` and `:293-298` also pass `true`.
  - `broker_status.hpp:175-180`: `NoCredentials` requires `!api_key_present`.
- **Impact.** In a net build with `ALTAIR_KITE_API_KEY` unset, the pill shows TOKEN UNVERIFIED or EXPIRED instead of the actionable NO API KEY.
- **Regression.** Unit test through a probe wrapper that reads the environment. The fix belongs to the caller.

### C18-014 · P3 · CONFIRMED DEFECT · seq regressions accepted
- **Evidence.**
  - `price_client.hpp:206-210` `if (seen != 0 && h.seq > seen + 1) {...} seen = h.seq;`. Equal or lower seq is applied without comment, so an older book frame overwrites a newer one.
  - `start()` (:92-99) clears `buf_` but not `seq_` or `last_`.
  - `protocol.hpp:345-353` already defines `SeqVerdict::Duplicate` / `Gap`, unused here.
- **Trigger.** The price service restarts with seq back at 1: no gap and no reset is reported. A duplicated frame is re-applied.
- **Regression.** Crafted frames with seq 1, 2, 2, 1. Expect a duplicate/regression count of 2 and the frame-2 price retained.

### C18-016 · P3 · CONFIRMED DEFECT · redaction only matches the `request_token=` form
- **Evidence.**
  - `kite_link.hpp:71-72` regex `request_token=([A-Za-z0-9]+)`.
  - `:305` `say("\n· exchanging %1".arg(redact_request_token(arg)))`, where `arg` is the raw paste.
  - test_kite_link.cpp:66-69 is headed "A bare token with no URL around it is the other thing people paste", yet tests `"request_token=abc123XYZ"`, which still carries the prefix.
- **Trigger.** Pasting only the 32-character token. It is echoed into the log pane the header calls "most likely to end up in a screenshot". If Kite refuses the checksum, the token is still unspent (:333-336).
- **Hypothesis, related.** Subprocess output is shown verbatim, relying on each binary's own promise: kite_link.hpp:307, kite_panel.hpp:366-369, live_feed.hpp:171-174. Not verified against the binaries.
- **Regression.** `redact_request_token("IIJDsuv8Gs1ey7vSX7Krux21a8WUtj9p")` must not contain the token when passed via the exchange path. Alternatively, redact the whole pasted argument.

### C18-017 · P3 · CONFIRMED DEFECT · malformed filter bounds accepted
- **Evidence.** `filter.hpp:82-86` strips one sign. `:89-90` splits on the first dot. `:97-99` `QString::toLongLong` accepts a sign inside `frac`. Empty `whole`/`frac` become 0.
- **Traced results.**
  - `"-"` gives 0; `"+"` and `"."` give 0.
  - `"1.-5"` gives 95.
  - `"--5"` gives 500.
- **Impact.** `FilterDialog::on_accept` (:377-388) applies these as real bounds. The comment at :379-384 says an unparseable bound must be refused.
- **Tests.** test_filter covers only "abc" and "1.234".
- **Regression.** Expect `!has_value()` for "-", "+", ".", "1.-5", "--5", "1.+5".

### C18-018 · P3 · CONFIRMED DEFECT (from Qt 6 sort semantics; not executed) · blanks sort first
- **Evidence.** `filter.hpp:267` `if (!va.isValid()) return sortOrder() == Qt::AscendingOrder;` and `:268` the mirror case.
- **Ascending.** `lessThan(blank, x)` is true, so the blank comes first.
- **Descending.** QSortFilterProxyModel's greater-than comparator calls `lessThan(i2, i1)`. `lessThan(x, blank)` is true, so the blank is treated as largest and again comes first.
- **Impact.** The comment (:264-265) says "Absent sorts LAST in both directions". Never-ticked instruments crowd the top of a sorted grid.
- **Tests.** test_filter [4] has no blank rows.
- **Regression.** Seed with a blank row, sort ascending then descending, and assert `token_at(rowCount()-1)` is the blank token both times.

### C18-019 · P3 · CONFIRMED DEFECT · non-money numeric filters parsed as rupees
- **Evidence.**
  - `main_window.hpp:542` `const bool numeric = column != TickModel::ColSymbol;`, which includes ColVolume and ColSeqno.
  - `filter.hpp:316-317` placeholder "value in rupees"; `:377` `parse_rupees_to_paise`.
  - `:130` the operand is meant to be "already in the column's STORAGE unit". Volume and seq are stored as plain counts.
- **Trigger.** Volume > 1000 filters for volume > 100,000. The dialog then re-displays the bound as "1,000.00".
- **Impact.** The same hundredfold mistake the file header warns about, in the other direction.
- **Regression.** Filter ColVolume with "10" on a seeded model with qty 1..20. Expect the rows with volume > 10.

### C18-020 · P3 · CONFIRMED DEFECT · change is not against the session open
- **Evidence.**
  - `tick_model.hpp:95-96` "Change against the session open".
  - `:179-182` `if (!r.has_open) { r.open_paise = t.last.raw(); ...}`: the first tick ever since reset.
  - `:185` `r.volume += t.qty.raw()`.
  - `reset_values` only runs on scrub or restart (main_window.hpp:473, 492).
  - real_tape spans the last N sessions (real_tape.hpp:154-172).
- **Impact.** On a multi-session tape, "Change (INR)" is change since the first bar of the tape (days ago), and Volume is a multi-day sum. Both are presented as the grid's current-session numbers.
- **Regression.** Apply ticks across two IST dates. Expect the change on day 2 to be measured from day 2's first tick.

### C18-021 · P3 · CONFIRMED DEFECT · stale account figures after a parse failure; absent shown as zero
- **Evidence.**
  - `kite_panel.hpp:416` `f.open(QIODevice::ReadOnly);` has its result unchecked.
  - `:422-427`: on a parse error, `head_` is replaced and the function returns without clearing `funds_cards_` or any of the three tables. The previous snapshot's positions and P&L stay visible under a header that no longer states their age, breaking rule 2 at :23-25.
  - `:492`, `:515`, `:527` `value(k).toDouble()`: a missing key renders "₹0.00".
  - `live_feed.hpp:264-271` `toInteger()` on a missing `last_price_paise` gives "0.00".
- **Trigger.** Press Re-read while `altair_kite_account` is mid-write, or after a schema change drops a field.
- **Tests.** test_kite_panel only checks `check(true)` after construction.
- **Regression.** Load a good snapshot, overwrite it with "{", call `reload()`, and assert the positions row count is 0 and the cards show "unreadable".

### C18-022 · P3 · DESIGN GAP · rule-1 literals in the production window
- **Evidence.**
  - `market_clock.hpp:84-90` hard-codes 09:00, 09:15 and 15:30, labelled "DEMO". Yet they are used by the production `MainWindow` (main_window.hpp:254, 476-477).
  - `bar_csv.hpp:199-200` `QTime(15, 30)` / `QTime(9, 15)`.
  - `real_tape.hpp:91-92` `{17512194, "NIFTY26SEPFUT", "fut/nifty/1m", true}`: a contract token and expiry as literals over a directory named for the near future.
- **Impact.**
  - After the September 2026 roll, the label and token may not match the contract in `fut/nifty/1m`. That depends on how the fetcher rolls, which is not verified.
  - A tick on a special-session day or holiday is phased purely by time of day.
- **Regression.** Load session windows from config; resolve the future's symbol and token from the master by expiry.

## 4 Lead revalidation

- **Codex UI-005: CONFIRMED.** See C18-001; line numbers match exactly (133, 169-176). The integer-index coupling to main_window's nav order is also confirmed (C18-002). It is partially mitigated by a startup range check at main_window.hpp:1384-1397.
- **atlas_data statuses: NOT CONFIRMED as mismatched.**
  - 26 rows were spot-checked (§6) and no status contradicts the code.
  - There are two wording notes and two page-mapping questions.
  - The header comment "32 pages, 0..31" is stale (33 pages now).
  - No change to the Atlas is proposed.
- **price_client: PARTIAL.**
  - Headers are validated for magic, version, kind and channel, and a bad header drops the connection. Tested.
  - Reassembly is correct. Tested.
  - Forward seq gaps are counted per topic. Tested.
  - Not handled: the payload length bound (C18-013), duplicate or backward seq, and seq reset on reconnect (C18-014).
  - There is no auto-reconnect; `start()` is caller-driven.
  - The replay flag is carried on every frame (:214), and terminal.hpp:512-521 renders REPLAY vs LIVE from it.
- **depth_ladder: no defect.**
  - The loop is bounded by `bid_levels`/`ask_levels`.
  - The clamp is documented as a safe-side clamp, with the refusal placed in the decoder (:101-111).
  - Crossed books are drawn and marked; never-updated is distinct from empty; imbalance is unset when one side is empty. All tested.
  - In main_window it is only ever given `nullptr` (:677).
- **tick_model per-update reconstruction: NOT CONFIRMED.** A tick emits a single-row `dataChanged` (:190); a model reset happens only in `reset_values`. A semantic defect was found instead (C18-020).
- **live_feed (QProcess bounded sample): CONFIRMED honest.**
  - It is labelled a bounded sample with its age.
  - Arguments are fixed integers, so there is no injection.
  - It blocks the GUI (C18-015) and shows absent as 0.00 (C18-021).
- **kite_link / kite_panel.**
  - No shell is used; `QProcess` receives an argv list. The pasted URL is one argument. A leading "-" could still reach the login binary as a flag (HYPOTHESIS, binary not read).
  - Token redaction is partial (C18-016).
  - kite_panel is read-only; the catalogue marks every mutating endpoint as `oms/` or not built. Tested.
  - The GUI thread blocks (C18-015).
- **auth: CONFIRMED.**
  - Dev credentials are compiled in wherever `CMAKE_BUILD_TYPE != Release`, which includes the default preset.
  - `--as admin` bypasses the password (C18-006).
  - Password comparison is constant-time, and unknown users cost the same.
- **broker_status / feed_status.**
  - Metadata-only extraction: CONFIRMED at the field level; the token bytes are still loaded (C18-008).
  - The REPLAYING vs LIVE pill is CONFIRMED fixed and tested (test_broker_status.cpp:188-209).
  - Liveness uses the receive clock (main_window.hpp:527).
  - Timezone defect: C18-007. NO API KEY unreachable: C18-012.
- **bar_csv:** visible counts are CONFIRMED; the remaining gaps are C18-009.
- **real_tape:** C18-011, C18-022.
- **master_lookup:** the exchange mapping is CONFIRMED correct.
  - Cash maps to NSE or BSE; Fut and Opt to NFO or BFO; Currency to CDS or BCD.
  - Commodity is always MCX, so a BSE commodity segment is not distinguished.
  - An unknown value gives an empty string, so the caller must refuse.
  - The FeedSource qualification and static_assert are correct.
  - Drop counts are not surfaced (C18-010).
- **filter:** there is no regex or expression evaluation, so no injection surface. Defects are C18-017, -018 and -019.
- **market_clock:** phases come only from the tick timestamp, and Unknown is kept distinct from Closed. Demo literals: C18-022.
- **account_widgets:** C18-003. **audit_panel:** C18-004, C18-005.
- **format:** Indian grouping is correct. `-paise` for INT64_MIN is theoretical UB, not reported.

## 5 Architecture facts

### Subprocesses
All are synchronous on the GUI thread, use `MergedChannels`, and have their working directory pinned to `ALTAIR_SOURCE_DIR` when it is defined.

| Binary | Arguments | Timeout | Caller |
|---|---|---|---|
| `altair_kite_login` | none, or the pasted redirect | 30 s | kite_link.hpp:235-268 |
| `altair_kite_update` | `--go` | 600 s | kite_link.hpp:354-413 |
| `altair_kite_account` | `--go` | 45 s | kite_panel.hpp:311-374 |
| `altair_kite_ticker` | `--seconds N --go` | N+30 s | live_feed.hpp:114-179 |

Binary lookup takes the first match among three locations. A stale binary from another preset can therefore be launched:
1. `applicationDirPath/../app/` (for kite_login: `applicationDirPath/`)
2. `applicationDirPath/../../net/app/`
3. `ALTAIR_SOURCE_DIR/build/net/app/`

### Sockets
- PriceClient: `QTcpSocket` to 127.0.0.1:7421 by default, owned by terminal.hpp:328, drained on the GUI thread. There is no server authentication.

### Files read by the UI
- `data/kite_session.json`: `probe_broker`, about every 5 s from main_window.hpp:1512-1517, plus every `wiring()` call.
- `data/instruments.csv`: MasterIndex once; AuditPanel every 500 ms.
- `ALTAIR_TICKS_FILE` (`data/kite_ticks.json`): live_feed.
- The account snapshot: kite_panel.
- `config/*.toml`: audit_panel.
- `dataset/**`: bar_csv and real_tape.

### Labels
| Surface | What it shows | Status |
|---|---|---|
| Status pill | REPLAYING on a replay; source set to Replay at main_window.hpp:259 | Correct |
| Terminal strip | REPLAY or LIVE from `kPriceReplay` | Correct |
| Kite account panel | Age of snapshot; STALE after 900 s | Correct |
| live_feed | Age of sample | Correct |
| Audit page, tick_seqno | "LIVE" | Wrong (C18-005) |
| Wiring table | `can_trade("Kite")` is always false, because the Margin fetch row is hard-coded BlockedOnInput (feed_status.hpp:358-360, 396-408; used at panels.hpp:281) | Safe side |

### Atlas routing
`AtlasPanel` callback → `MainWindow::show_page(int)`, indexing into `nav_page_names()`: 33 names, 0 Live Grid … 32 Model Atlas. A startup abort guards the count (:1362) and the Atlas range (:1384). There is no name-based routing.

### UI cannot trade
The files in this group include no `oms/` or `broker/` headers. The only engine headers included are:
- `core/types/units`, `core/time/*`
- `feed/replay`, `feed/tick`
- `book/l2_book`
- `instruments/kite_dump`
- `server/protocol`, `server/price_payload`

## 6 Atlas status spot-check

| # | Row (atlas_data.hpp) | Status / page | Evidence (grep, read-only) | Verdict |
|---|---|---|---|---|
| 1 | Factor models (PCA) | ABSENT | no `pca`, `principal component` or `fama` in the 9 engine dirs | consistent |
| 2 | Random forest | ABSENT | no `random forest` hit | consistent |
| 3 | EGARCH | ABSENT | no `egarch` hit | consistent |
| 4 | Trade arrival (Hawkes) | ABSENT | no `hawkes` hit | consistent |
| 5 | Mean-variance optimisation | ABSENT | no `markowitz` or `efficient frontier`; risk/optimise.hpp has min_variance and risk_parity only | consistent |
| 6 | ARMA/ARIMA | ABSENT | no `arima` hit | consistent |
| 7 | Binomial/trinomial | ABSENT | only analytics/american.hpp:24 "Measured against a 3000-step binomial" (benchmark) | consistent |
| 8 | Queue position | ABSENT | only comments deferring it (book/microstructure.hpp:5, backtest/engine.hpp:38) | consistent |
| 9 | Logistic regression | ABSENT | only models/tensor.hpp:148 `logistic` (the function) | consistent with "Only the logistic FUNCTION exists" |
| 10 | Kalman filter | BUILT, "Scalar only" | analytics/kalman.hpp:70-78 `class Kalman1D`, "Scalar state, scalar observation" | consistent |
| 11 | MLP | PARTIAL / 29 Neural | models/mlp.hpp:91-93 "hidden layer is fixed at its seeded initialisation and the linear readout is solved in closed form" | consistent |
| 12 | Pairs trading | PARTIAL / 26 | strategies/cointegration.hpp:380-429 `PairCandidate` (zscore, half-life, tradeable); "exit" appears only as the break alarm (:65, :441); no pair P&L found | consistent |
| 13 | Ornstein-Uhlenbeck | PARTIAL / 26 | cointegration.hpp:355-362 OU `half_life` only | consistent |
| 14 | Heston | PARTIAL / -1 | `heston` only in backtest/montecarlo.hpp (7 hits); none in analytics/ | consistent (simulator, no pricer) |
| 15 | Local vol (Dupire) | BUILT, sabr.hpp / 21 Options | analytics/sabr.hpp:1, 178, 188, 200 | consistent |
| 16 | Black-Litterman | BUILT, risk/stress.hpp / 15 Portfolio | risk/stress.hpp:1, 23, 138; also risk/optimise.hpp:8 | consistent |
| 17 | Risk parity / Min variance | BUILT, optimise.hpp / 15 | risk/optimise.hpp:121 min_variance, :150 risk_parity | consistent |
| 18 | TWAP/VWAP/POV | BUILT, oms/execution.hpp / 12 Execution | execution.hpp:1 "TWAP, VWAP, POV", :264 POV | consistent |
| 19 | Almgren-Chriss | BUILT, oms/shortfall.hpp / 12 | shortfall.hpp:1, 5, 119, 130 | consistent |
| 20 | Ledoit-Wolf covariance | BUILT, risk/covariance.hpp / 15 | covariance.hpp:22, 151, 158 | consistent |
| 21 | VAR | ABSENT, "Only a bivariate VECM step exists" | no `vecm` or `error correction` hit; only bivariate Johansen (cointegration.hpp:288-296) | status consistent; wording not directly supported |
| 22 | SVM / KNN / autoencoder | ABSENT, "None of the three" | no svm, autoencoder or knn model; a 1-NN predictor is described in backtest/validation.hpp:59-76 as a leak-measurement instrument | status consistent; wording note |
| 23 | Calendar / Parity | BUILT / 31 | nav[31] = "Parity & Calendar" | page consistent |
| 24 | Neural rows (MLP, LSTM, Transformer, CNN) | PARTIAL / 29 | nav[29] = "Neural" | page consistent |
| 25 | Hurst exponent | BUILT / 17 Regimes | nav comment main_window.hpp:147-149 says page 27 "Memory" carries "Hurst and EWMA" | OPEN: page may be 27; not verified which panel renders Hurst |
| 26 | Walk-forward & purged CV | BUILT / 16 ML — Trees | nav comment :144-146 says page 26 Cointegration carries "the walk-forward every backtest passes through" | OPEN: page may be 26; not verified |

The remaining 59 rows were not individually spot-checked. The nav pages named by the other checked rows match nav_page_names():
- 6 Cost, 9 Value — DCF, 10 Aggregator, 12 Execution, 13 Volatility, 14 Risk — VaR, 15 Portfolio
- 16 ML — Trees, 17 Regimes, 18 Forecast, 19 Strategies, 21 Options, 23 Flagging
- 24 Microstructure, 25 Sizing & Limits, 26 Cointegration, 30 Features

## 7 Test-suite observations

- All tests are hand-rolled `check()` executables, not QtTest. Each prints ok/FAIL and returns non-zero on failure.
- **test_bar_csv** depends on real `dataset/` files, so it passes or fails with the environment. On a fresh clone without dataset/ it fails; it does not skip.
- **test_kite_panel** section 3 is `check(true)` twice: a crash test with no assertion on the absent/empty distinction it claims to verify.
- **test_kite_link** has two gaps:
  - Its "bare token" case (:66-69) still includes the `request_token=` prefix.
  - The QProcess paths are untested by design (:5-7).
  - It also carries a literal 32-character request_token at :55. Presumably spent and single-use; worth confirming it was never a live token.
- **test_price_client** uses a real bus and real sockets. It covers reassembly, forward gaps and bad magic, but not oversized `payload_len`, duplicates, backward seq or reconnect.
- **test_filter** has no blank-row sort test, no sign-only or embedded-sign parse test, and no non-money numeric column test.
- **test_broker_status** tests the future case only at +1 day, so the timezone offset is not caught. There is no host-timezone test.
- **test_depth_ladder** is thorough for `ladder_view`.
- **test_atlas** does not exercise double-click routing. The page-index check lives in the MainWindow constructor, not in ctest.
- No tests exist for: auth.hpp, audit_panel.hpp, master_lookup.hpp, real_tape.hpp, live_feed.hpp, market_clock.hpp (only indirectly), tick_model.hpp (only via test_filter), account_widgets.hpp.

## 8 Open questions

1. Hurst (row page 17) and Walk-forward (row page 16): do those pages actually render the model, or do only pages 27 and 26 (per nav comments)? A panel-construction trace is needed.
2. Do `altair_kite_login`, `altair_kite_account` and `altair_kite_ticker` guarantee never printing access tokens or api secrets in their failure tails (displayed verbatim)? Binaries not in this group.
3. Does `altair_kite_login` treat an argv beginning with "-" as an option (argument injection via paste)?
4. How does the fetcher populate `dataset/fut/nifty/1m` across expiries? That decides whether the NIFTY26SEPFUT label and token in real_tape.hpp stay correct after the September 2026 roll.
5. Does `instruments/kite_dump.hpp`'s report expose a dropped-row count that MasterIndex could surface (C18-010)?
6. Is `default` (RelWithDebInfo) the build used on the trading desk? That decides whether C18-006 stays P2 or rises.
7. Does terminal.hpp pass `LivePrice` depth into a ladder anywhere? main_window only ever passes `nullptr` (:677), although price_client.hpp:10-16 says it is "the first thing ... that can hand it a real one".
8. `audit_panel.hpp:259-265` hard-codes "0 decisions recorded … oms/ builds an order body and does not send it". Does that still hold now that P25-04 order intents exist?
