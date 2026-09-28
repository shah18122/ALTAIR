# G17 desktop shell + terminal — findings

Reader: claude-subagent:G17_desktop_shell_terminal · review 2026-09-14/15 · baseline a34af5c5c9fa8c7498e8cc4df00907548a41e1a5 · READ-ONLY (nothing was built or run)

## 1 Scope & coverage

13 files, 7,726 lines. **All 13 are fully read**, every line through the Read tool in chunks of 300 lines or fewer. All sha256 hashes before and after reading match the partition inventory, so nothing changed while I was reading.

| File | Lines | Ranges read |
|---|---|---|
| desktop/main_window.hpp | 1673 | 1-300, 300-599, 600-899, 900-1199, 1200-1499, 1500-1673 |
| desktop/panels.hpp | 1310 | 1-300, 300-599, 600-899, 900-1199, 1200-1310 |
| desktop/live_forecast.hpp | 764 | 1-300, 300-599, 600-764 |
| desktop/CMakeLists.txt | 660 | 1-300, 300-599, 600-660 |
| desktop/terminal.hpp | 541 | 1-300, 300-541 |
| desktop/option_chain.hpp | 519 | 1-300, 300-519 |
| desktop/order_ticket.hpp | 500 | 1-300, 300-500 |
| desktop/main.cpp | 402 | 1-300, 300-402 |
| desktop/chain_panel.hpp | 335 | 1-300, 300-335 |
| desktop/tests/test_terminal.cpp | 324 | 1-300, 300-324 |
| desktop/kill_switch.hpp | 300 | 1-300 |
| desktop/watchlist.hpp | 215 | 1-215 |
| desktop/tests/test_order_ticket.cpp | 183 | 1-183 |

Context read outside the group, for revalidation only (not counted as coverage):
- `desktop/market_clock.hpp` (grep of observe/phase)
- `desktop/kite_link.hpp` 290-324
- engine `CMakeLists.txt` link lines for risk, models, backtest, feed, server, analytics and core
- `oms/order_intent.hpp` grep for the line reader and the `at` field
- `models/calibration.hpp` grep for next_lo

## 2 Findings table

| ID | Sev | Class | Title |
|---|---|---|---|
| C17-010 | **P1** | CONFIRMED DEFECT | The order confirmation omits limit price, order type, product and validity. The default limit of 2,400,000 paise survives every instrument change |
| C17-001 | P2 | CONFIRMED DEFECT | Fits, report generation and subprocess waits run synchronously in the GUI thread. `processEvents` allows re-entry. The Halt tab is frozen for up to about 65 s |
| C17-007 | P2 | CONFIRMED DEFECT | The option chain destroys and re-creates every table item, and re-centres the scroll, on every spot frame |
| C17-008 | P2 | CONFIRMED DEFECT | REPLAY/LIVE on the stream strip is decided from the NIFTY token alone. Nothing marks stale data. The chain header keeps saying LIVE after a disconnect |
| C17-009 | P2 | DESIGN GAP | PendingIntents shows every line ever appended as "not drained". The count only grows. Each open re-reads the whole file with no size bound |
| C17-011 | P2 | CONFIRMED DEFECT | `append_intent` checks only `open()`. A write or flush failure is still reported as PENDING |
| C17-014 | P2 | CONFIRMED DEFECT | Halt panel: a malformed or unreadable request file shows as "No halt requested". The write result is unchecked. The state label is never refreshed from disk |
| C17-015 | P2 | DESIGN GAP | Dev credentials and the `--as admin` login bypass are compiled into every build type except exactly `Release` |
| C17-018 | P2 | DESIGN GAP | The gate-3 link audit checks only direct link libraries. The UI can also start subprocesses and write the intent queue |
| C17-020 | P2 | CONFIRMED DEFECT (downstream impact is a HYPOTHESIS) | Live forecast merge appends fetched bars without re-sorting. History load stops at 2026 |
| C17-021 | P2 | CONFIRMED DEFECT | Watchlist labels rows "watch only / cannot be ordered", yet any row found in the master becomes requestable |
| C17-002 | P3 | CONFIRMED DEFECT | Seeking backward does not reset MarketClock (Codex UI-002) |
| C17-003 | P3 | CONFIRMED DEFECT | The scrubber maximum is not updated after `reload_tape` (UI-003) |
| C17-004 | P3 | CONFIRMED DEFECT | Candles stay stale when `applied_==0`, and a real tape is titled "synthetic replay" (UI-004) |
| C17-005 | P3 | CONFIRMED DEFECT (latent) | `reload_tape` refreshes status before `restart()`, so a chart rebuild can read past the end of a shorter new tape |
| C17-006 | P3 | DESIGN GAP | Chart disk series are re-read synchronously on every visit to the Chart page and every combo change |
| C17-012 | P3 | CONFIRMED DEFECT | The expiry list is silently capped at 8 and filtered by wall clock, even during replay |
| C17-013 | P3 | CONFIRMED DEFECT | `set_price` narrows to int and snaps sells down. `set_instrument` never renames a token it already has |
| C17-016 | P3 | CONFIRMED DEFECT | `--prime` and `--source` are not validated |
| C17-017 | P3 | CONFIRMED DEFECT | The Ratio Spread page shows expiry dates in UTC, one day early |
| C17-019 | P3 | HYPOTHESIS | The intent `at` field is local time with no UTC offset; the vectors carry +05:30 |
| C17-022 | P3 | CONFIRMED DEFECT | Watchlist misc: highlighted row and ticket can disagree after Remove; quote exchange is guessed; quote refresh blocks for up to 35 s |

## 3 Finding details

### C17-010 — P1 — The order confirmation does not show the price it requests
- **Evidence:**
  - order_ticket.hpp:195-200: `price_->setRange(1, 100'000'000); price_->setValue(2'400'000);`
  - order_ticket.hpp:286-288: `set_instrument` deliberately leaves the price alone. `set_contract` (311-317) and the chain click (option_chain.hpp:483) do not set it either.
  - order_ticket.hpp:405-416 only check that the price is a multiple of the tick.
  - order_ticket.hpp:426-436: the typed-confirmation text is built from `.arg(BUY/SELL).arg(d.lots).arg(d.symbol, phrase)`. It contains no limit price, no MARKET/LIMIT, no product (NRML/MIS/CNC) and no validity (DAY/IOC).
- **Trigger:** An admin clicks a NIFTY CE in the chain (tick 5 paise), presses F1, sets lots and clicks REQUEST BUY. The dialog reads "BUY 1 lot(s) of NIFTY…CE" and the admin types REQUEST BUY. The line written has `"limit_paise":2400000`, a ₹24,000 limit on a contract worth perhaps ₹100. That order is marketable at any price. The same happens with a MARKET checkbox or IOC left over from an earlier instrument.
- **Impact:** The one field that sets the execution price is never shown at the safety step. Whether oms/ refuses an unreasonable price is outside G17 and not verified. This finding is about the UI's validation.
- **Tests:** test_order_ticket only covers `intent_line` against the vectors. test_terminal [5] asserts that a chain click *leaves* the limit untouched, which is exactly this behaviour. `submit()` has no test.
- **Regression test:**
  - Pull the dialog text into a pure `confirm_text(IntentDraft)` and assert it contains the limit (in rupees and paise), order type, product and validity.
  - Assert that `set_contract` for a token with a different spec clears the price or marks it unconfirmed, so a request is refused until the operator sets a price.

### C17-001 — P2 — Heavy work runs in the GUI thread (Codex UI-001 CONFIRMED, and wider than reported)
- **Evidence:**
  - main_window.hpp:1164-1175 (`run_fc`: `set_text`, `QApplication::processEvents()`, `forecast_report(...)`).
  - The same pattern appears at 1079-1084 (GARCH), 1104-1109, 1117-1122, 1130-1135, 1190-1197, 1207-1213 and 1311-1319 (neural, "about 6 s"). The same pattern is in panels.hpp:434-449 (`run_walk_forward`).
  - Subprocess waits on the GUI thread: live_forecast.hpp:184 `waitForStarted(5000) || waitForFinished(60000)` (the default Forecast button fetches live), and panels.hpp:1116 `waitForFinished(30000)`.
  - main_window.hpp:1139-1143 itself says a 5-minute fit takes "tens of seconds".
- **Trigger:** Pressing Forecast, Neural, Volatility and similar buttons. No button is disabled while it runs, so a second click already queued is handled inside `processEvents()` and starts a nested run before the outer one begins.
- **Impact:** While the work runs:
  - the replay pump (16 ms timer), status pill and clocks stop;
  - price frames from PriceClient pile up;
  - the Terminal's Halt tab and REQUEST HALT cannot be clicked.

  Today no oms/ runs in this process, so this is P2. Under the in-process decision it becomes a halt-availability blocker (P1) once the engine shares the process.
- **Tests:** none.
- **Regression test / reproducer:**
  - Add a watchdog QTimer test that measures the longest event-loop stall across `compute_current()` on the Forecast page, with a stub fetcher that sleeps. Assert that the Halt button accepts a click within 250 ms.
  - Assert that the compute button is disabled during a run.

### C17-007 — P2 — The chain is rebuilt from scratch on every price frame
- **Evidence:** The call path is terminal.hpp:341-342 → `on_price` → `apply_price` (394-408) → `chain_->set_spot` → option_chain.hpp:165 `reprice()`. Inside it:
  - line 253 `grid_->setRowCount(0)` destroys every item;
  - line 322 builds a new `std::vector<const ChainRow*>` over the whole expiry;
  - `paint` (403-455) calls `new QTableWidgetItem` for all 13 columns per row;
  - line 367 relays out a rich-text header;
  - line 458 calls `scrollToItem(..., PositionAtCenter)`.
- **Worst case:** the default ±10 strikes gives 21 rows × 13 = 273 items destroyed and re-created per NIFTY frame. The width maximum of 60 gives 121 × 13 = 1,573 items per frame. `refresh_stream()` (a rich-text `setText`) also runs on every frame for every token.
- **Impact:**
  - CPU time on the GUI thread scales with the stream rate. The rate is set by PriceClient and the price service, outside this group.
  - The forced re-centre throws away any scroll the operator made to a wing strike.
  - When the ATM strike moves, the same row number shows a different strike from one frame to the next. The confirmation dialog does name the symbol.
  - The comment at option_chain.hpp:505, "no heap on reprice", is inaccurate.
- **Tests:** test_terminal [5] and [7] check correctness only, not cost or scroll.
- **Regression test:** call `apply_price` 1,000 times on a fixed chain and assert:
  - the item pointers stay stable (values updated in place);
  - `scrollToItem` is called only when the ATM row changes;
  - the elapsed time is under a budget.

### C17-008 — P2 — LIVE/REPLAY labels can be wrong
- **Evidence:**
  - terminal.hpp:515-521: `const LivePrice* n = client_->price(256265u); const bool replay = n != nullptr && n->replay;`. REPLAY is decided only from the NIFTY frame, and green "LIVE" is shown in every other case.
  - Nothing checks how old the last frame is.
  - option_chain.hpp:163 sets `spot_live_ = true`, and only `load_underlying` (194) clears it. No disconnect path reaches the chain.
- **Trigger:**
  - Run `altair_price_service --replay banknifty 1m`, or any replay before the first NIFTY frame arrives. The strip shows ● LIVE.
  - Disconnect or lose the service. The strip correctly says STREAM OFF, but the chain header keeps "spot … (LIVE)" at the frozen price.
  - Stay connected with no frames for minutes. The strip still says LIVE.
- **Impact:** A replayed or stale price is shown as live, which the file itself calls "the one mistake this screen must not make".
- **Tests:** test_terminal [5] and [7] cover only NIFTY `set_spot` and `apply_price`, and not the strip state.
- **Regression test:**
  - Feed a replayed BANKNIFTY frame with no NIFTY and assert the strip does not contain "LIVE".
  - Call `client_->stop()` and assert the chain header no longer says LIVE.

### C17-009 — P2 — The pending-intent view counts lines that were already drained
- **Evidence:**
  - terminal.hpp:108-111 says "A line here is a REQUEST that has not been drained".
  - terminal.hpp:161-171 counts every non-blank line as "request(s) in the queue".
  - order_ticket.hpp:135-137 and oms/order_intent.hpp:251-253: the file is append-only and oms tracks a byte offset, so drained lines stay in the file for ever.
  - The read has no size limit and runs synchronously on every switch to the Book tab (terminal.hpp:299-301) and on F3.
- **Impact:**
  - After a week of use the panel reads "57 request(s) in the queue", which trains the operator to ignore it. A genuinely undrained new request then looks the same as history, defeating the panel's purpose (terminal.hpp:85-88).
  - Memory and time grow with the file's lifetime.
- **Tests:** none.
- **Fix direction:** Show only lines past oms/'s persisted offset, or label them "lines in file (drained and undrained)". Cap the read and show the count of lines skipped.

### C17-011 — P2 — A failed intent write can still be reported as written
- **Evidence:** order_ticket.hpp:138-147: `if (!f.open(...)) return false; QTextStream ts(&f); ts << intent_line(d) << "\n"; return true;`. The stream's status and the flush are never checked.
- **Trigger:** The disk is full, or an antivirus or backup lock bites after `open`. The line is partly written or not written at all, yet the UI logs `PENDING` (455-458). The comment at 447-450 says a request that may or may not have been written is the worst case, and this is that case.
- **Knock-on:** A partial line without a newline has the next request appended onto it, so one corrupt line eats a valid one. oms's reader keeps an unterminated tail back (order_intent.hpp:290-297), but a concatenated line would be rejected.
- **Line endings:** Text mode writes CRLF on Windows. The oms reader strips `\r` (order_intent.hpp:297), so that part is handled.
- **Concurrent writers:** two desktop instances appending at once is a HYPOTHESIS. Qt's Windows append behaviour is not verified here.
- **Tests:** none.
- **Regression test:** make `append_intent` return failure unless `ts.flush()` succeeds, `ts.status() == QTextStream::Ok` and `f.error() == NoError`. Test against a read-only or full target.

### C17-014 — P2 — Halt request file handling
- **Evidence:**
  - kill_switch.hpp:81-84 returns `k` with `present=false` when the file parses as something other than a JSON object. Line 83's own comment says "malformed is NOT 'no request'". `refresh()` (266-272) then shows green "No halt requested. There is no request file." The same happens when `open` fails (78-79) on an existing but locked file.
  - Line 236 opens with `WriteOnly|Truncate`, then line 242 calls `f.write(...)` without checking the result. A failed write leaves an empty file where a previous valid request stood, and the UI then reports no halt.
  - `refresh()` runs only in the constructor and after this panel's own actions. There is no timer and no tab-change refresh, so a clear or request made by another process is never seen.
- **Impact:** The fail-safe direction is displayed as safe. What oms/ will do with an empty file is not defined anywhere in G17.
- **Tests:** none in the group.
- **Regression test:**
  - Write `"{"` to the file and assert the state text contains "HALT" or "UNREADABLE", never "No halt requested".
  - Make the write fail and assert the old content survives; write to a temp file and rename it into place.

### C17-015 — P2 — Dev accounts and login bypass outside Release builds
- **Evidence:**
  - CMakeLists.txt:203-205: `if(NOT CMAKE_BUILD_TYPE STREQUAL "Release") target_compile_definitions(... ALTAIR_DEV_CREDENTIALS)`.
  - main.cpp:280-293: under that define, `--as admin` grants `Role::Admin` with no password.
  - CMakeLists.txt:571 also defines it for the terminal test.
- **Trigger:** A RelWithDebInfo or MinSizeRel build, or a multi-config generator where `CMAKE_BUILD_TYPE` is empty, compiles in admin/admin and `--as admin`. That gives RequestOrder and Clear-halt with no login.
- **Impact:** panels.hpp:129-133 claims "a shipped binary has NO accounts provisioned", which holds only for the exact `Release` string. The risk is accidental rather than adversarial, because local file access to the intent queue is equivalent power.
- **Fix direction:** gate on an explicit `ALTAIR_DEV_BUILD=ON` option rather than the build type, and add a configure-time check that `prod` leaves it OFF.

### C17-018 — P2 — The gate-3 audit does not cover everything the UI can do
- **What the audit checks:** CMakeLists.txt:285-296 reads `LINK_LIBRARIES` of `altair_desktop`, which is the direct list only.
- **What it misses:** the engine targets link further libraries through their interfaces:
  - backtest/CMakeLists.txt:15-21 → altair_strategies
  - models/CMakeLists.txt:18-25 → altair_features, altair_strategies
  - risk/CMakeLists.txt:11-16 and feed/CMakeLists.txt:10-15 → altair_instruments

  None of these is on `_altair_ui_permitted`, and the audit still passes. That proves it never sees the transitive closure. A future INTERFACE link from any of them to altair_oms or altair_broker would pass silently, which is exactly the case the comment at 217-222 says the allow-list exists to catch. I did not grep the CMake files of strategies, features, instruments, flags or invariant, so the *current* closure is unverified.
- **Other capabilities left to the UI:**
  - **Network:** Qt6::Network (283).
  - **Subprocesses:** the UI starts external binaries with `--go`: live_forecast.hpp:167-183 (altair_kite_fetch), panels.hpp:1107-1115 (altair_kite_quote), and kite_link.hpp (altair_kite_login). Binaries are found through relative paths (`applicationDirPath()/../app/`, `ALTAIR_SOURCE_DIR/build/net/app/`).
  - **File writes:** the UI appends to the queue that oms/ executes.

  "The UI cannot trade" therefore holds only for placing an order *inside this process*.
- **Fix direction:** walk `INTERFACE_LINK_LIBRARIES` recursively in the audit, and list the subprocess binaries the UI may start next to the allow-list.

### C17-020 — P2 — Live forecast merge can leave the history out of order
- **Evidence:**
  - live_forecast.hpp:324-328: a fetched bar not found in the history is `push_back`-ed onto the end with no sort and no ordering check.
  - 349-354 then drop unfinished bars from the back.
  - 257: `for (int y = 2015; y <= 2026; ++y)` hard-codes the partition years.
- **Trigger 1:** dataset/ is missing a bar in the last 20 days (a hole), and Kite returns it. It is appended after yesterday's bar. Today's partial bar is appended and then popped. The anchor is now the old hole bar, and the walk-forward runs over a series whose tail is out of order.
- **Trigger 2:** dataset/ and the fetcher stamp daily bars differently. Every fetched bar is then "new" and gets duplicated. The `%2 new` count in the report would show it.
- **Trigger 3:** from 2027-01-01, monthly intraday files are ignored without any message. The anchor timestamp would show it.
- **Impact:** the forecast may be anchored on the wrong bar, and the backtest tail may carry look-ahead. The downstream effect depends on whether `backtest_forecasts` and `forecast_next` refuse unsorted timestamps (models/, not verified). Merge-time partial-bar guard: I verified it (bar_is_complete 74-87, applied at 349-354).
- **Tests:** none in the group.
- **Fix direction:** after merging, `stable_sort` by stamp, de-duplicate, and refuse the run if the stamps are not strictly ascending. Take the year range from the directory listing.

### C17-021 — P2 — "Watch only" rows can still be requested
- **Evidence:**
  - panels.hpp:867-873 says an unresolved row "cannot be sized or ordered".
  - panels.hpp:1259-1264 shows `spec_state_label(r.spec)`. Nothing in the UI calls `resolve()` or `block()`, so every row is Unresolved and shows "watch only".
  - Yet `emit_pick` (1004-1015) emits `contractPicked` with the master's lot, tick and exchange for any row found in the master, whatever its `SpecState`. The ticket then accepts it (order_ticket.hpp:392-400).
  - A row marked Blocked (watchlist.hpp:52-55, 181-192) would also emit a spec.
  - watchlist.hpp:24-30 says an unresolved row is "NEVER SILENTLY PROMOTED".
- **Impact:** The label and the actual orderability disagree. The Blocked state (rule 9) is not enforced on the path to the ticket.
- **Tests:** test_terminal [2]/[3] exercise only the pick. Nothing checks SpecState.
- **Regression test:** call `block(token)` and assert no `contractPicked` is emitted; separately assert the Spec column agrees with `has_spec`.

### C17-002 — P3 — Backward seek keeps the old clock (Codex UI-002)
- **Evidence:** main_window.hpp:488-495 resets the replayer, model, `applied_` and `last_ts_ns_`, but not `clock_`. `restart()` does reset it (476-477).
- **Trigger:** Drag the scrubber to 0 while paused. `MarketClock::observe` only overwrites and `seen_` stays true (market_clock.hpp:108-113). The status bar then shows `engine 05:30:00.000 IST` (a zero timestamp) and the phase of the last tick seen, beside an emptied grid. Seeking back to N>0 re-observes, so only seek-to-0 is visibly wrong.
- **Tests:** none.
- **Fix:** reset `clock_` inside the `want < applied_` branch.

### C17-003 — P3 — Scrubber range goes stale after reload (Codex UI-003)
- **Evidence:** main_window.hpp:1428 sets the maximum once. `reload_tape` (426-432) changes `count_` and never calls `setMaximum`.
- **Impact:** With a longer new tape, `setValue(applied_)` (1550) pins at the old maximum and the extra ticks cannot be reached by seeking. With a shorter one, the slider runs past the end, and the seek loop just stops.

### C17-004 — P3 — Chart can show stale candles and the wrong label (Codex UI-004)
- **Evidence:**
  - main_window.hpp:811-813: `if (instrument_ == nullptr || applied_ == 0) return;` leaves the previous candles in place.
  - 821-823 hard-code the title "(synthetic replay)", and 912 hard-codes the combo entry "Synthetic replay", both regardless of `tape_real_`.
- **Trigger:** Pause, then seek to 0. The chart keeps candles from beyond the scrubber position, a visible look-ahead, until ticks resume. A real NIFTY tape is still titled synthetic. That mislabel errs in the conservative direction, but it contradicts the status bar's "REAL · 1-min closes".

### C17-005 — P3 — Latent out-of-bounds read in `reload_tape`
- **Evidence:** main_window.hpp:427-432 swaps `ticks_` and `count_`, then calls `set_tape_is_real` → `refresh_status()` (308). Once every 15 calls that runs `rebuild_chart()` → `build_candles(ticks_, applied_, …)` (817-819), while `applied_` still holds the old count. `restart()` only runs after that (432).
- **Trigger:** The Chart page is showing with the synthetic-replay source, the old tape is exhausted, the reloaded tape is shorter, and the 1-in-15 frame counter lines up. Reachable through `--reload-tape` (main.cpp:373) or an asynchronous path. The Link Kite callback fires synchronously while its own page is showing (kite_link.hpp:314-316), which makes the trigger unlikely there.
- **Impact:** reading past the end of a vector inside the process that, by design, holds positions.
- **Fix:** call `restart()` before `set_tape_is_real`, or clamp `applied_` first.

### C17-006 — P3 — Chart data is re-read on every visit
main_window.hpp:614-618 and 833-848: every visit to the Chart page and every change of source, bucket or instrument calls `load_bars_dir` synchronously on the GUI thread. That can be a full 1-minute partition. A cache keyed on the directory's modification time would avoid it.

### C17-012 — P3 — Expiry list capped and filtered by wall clock
option_chain.hpp:247 `if (expiry_->count() >= 8) break;` silently hides further expiries, with no count shown (rule 11 requires a visible truncation). Lines 237-242 filter expiries by the wall clock, so a replay of an earlier date cannot list the contracts that were live then, even though valuation uses the tick's time (288-297).

### C17-013 — P3 — Ticket price and naming details
- `set_price` (order_ticket.hpp:328-338) snaps DOWN for both sides. For a sell that is a worse, more marketable price, and the stated reason covers buys only.
- Line 337 `static_cast<int>(v)` narrows a qint64. QSpinBox then clamps silently at 100,000,000 (196). There is no production caller; only test [6] uses it.
- `set_instrument` (289-295) returns early for a known token and never renames it, so `d.symbol` (386) can carry a stale name if a token is reused.
- `price_->setSingleStep(5)` (198) is a literal tick step (rule 1).

### C17-016 — P3 — Unvalidated command-line flags
main.cpp:339-343: `--prime` uses `toLongLong()` cast to size_t, so a negative value wraps to a huge count and drains the whole tape, and a parse failure gives 0 with no message. `--source` uses `toInt()`, which gives 0 on failure. This contradicts the validate-don't-coerce policy the same file applies to `--page` (239-264) and `--sessions` (206-216).

### C17-017 — P3 — Ratio Spread expiries shown a day early
chain_panel.hpp:240-241 formats the expiry with `QTimeZone::utc()`. The master stores IST midnight (option_chain.hpp:300-303), and panels.hpp:1147-1164 documents that formatting it in UTC "prints the day before" and fixes that for the watchlist. The Ratio Spread page's expiry combo therefore shows every date one day early.

### C17-019 — P3 — Intent timestamp may lack an offset (HYPOTHESIS)
order_ticket.hpp:420 `QDateTime::currentDateTime().toString(Qt::ISODate)`. The time spec is local, and per Qt's documentation no offset is appended for a local-time QDateTime (not verified by running). The vectors carry `+05:30`, and oms/order_intent.hpp:95 documents "ISO-8601 with offset", but its parser only requires the field to be present (199). The conformance test builds `at` by hand, so it cannot catch this drift. The result is an ambiguous audit timestamp.

### C17-022 — P3 — Watchlist details
- After Remove, `refresh()` rewrites cells in place, so the highlighted row can name a different instrument from the one the ticket still holds (panels.hpp:1033-1040, 1133-1137).
- `on_refresh_quotes` guesses the exchange from the segment, NSE or NFO only (1093-1096), although `p.kite_exchange` is available (1011-1013). A BSE or BFO row is keyed to the wrong venue.
- The quote subprocess blocks the GUI for up to 35 s (1116).
- The instrument master (about 108k rows) is parsed three times at startup: ChainPanel, OptionChainPanel and WatchlistPanel's MasterIndex.

## 4 Lead revalidation

| Lead | Verdict | Evidence |
|---|---|---|
| Codex UI-001: synchronous heavy compute, one `processEvents` before the fit | **CONFIRMED, and wider than reported** | main_window.hpp:1164-1175, plus 7 more pages. panels.hpp:436. Subprocess waits at live_forecast.hpp:184 (up to 65 s) and panels.hpp:1116. Buttons are not disabled, so a double click re-enters. Halt is blocked during the run. → C17-001 |
| Codex UI-002: backward seek does not reset MarketClock | **CONFIRMED** | main_window.hpp:488-495 against 476-477. Visible only when seeking to 0. → C17-002 |
| Codex UI-003: scrubber maximum stale after reload | **CONFIRMED** | 1428 is the only `setMaximum`. → C17-003 |
| Codex UI-004: stale candles at `applied_==0`, synthetic label | **CONFIRMED** | 811-813, 821-823, and the combo label at 912. → C17-004 |
| Per-update table rebuild blocks the loop | **CONFIRMED for the chain; NOT CONFIRMED for the watchlist and pending intents per tick** | The chain rebuilds 273-1,573 items per spot frame (C17-007). WatchlistPanel and PendingIntents rebuild only on user action or tab switch, and PriceClient is not wired to them. The PendingIntents read is unbounded (C17-009). |
| Order ticket submit path and intent writing | **PARTIAL: validation present, gaps found** | Role gate, spec required, tick check and typed phrase are present. Missing: price and type in the confirmation (C17-010), write verification (C17-011), timestamp offset (C17-019). Lots are bounded 1-100 by the spin box. |
| kill_switch.hpp | **PARTIAL** | The asymmetric confirmation and admin-only clear are correct. Unsafe display on a malformed file, unchecked truncate-and-write, stale label → C17-014 |
| main.cpp flags, dev credentials, synchronous load, borrowed tape | **PARTIAL** | The borrowed tape is safe: `real` and `synth` are `main()` locals declared before `window` (main.cpp:219-225, 309). Tape loading is synchronous before the window, which is acceptable. Dev credentials apply to non-Release builds (C17-015). `--prime` and `--source` are unvalidated (C17-016). `--stream` and `--train` are fine. |
| live_forecast: partial bar, look-ahead, conformal label | **PARTIAL** | Partial-bar guard CONFIRMED correct (74-87, 349-354). The conformal label is conditional and honest (429-436). Ordering after the merge is unchecked (C17-020). Minor: the RMSE band width comes from the same backtest errors its coverage is then scored on (384-399), so "RMSE band coverage" is not out of sample. The conformal row is shown alongside it. |

## 5 Architecture facts

**Navigation model**
- `nav_page_names()` (main_window.hpp:96-167) has 33 entries. `build_watchlist` (page 0), `build_chart` (page 1) and `build_pages` (pages 2-32) add exactly 33 widgets in order, and I checked the order name by name.
- Page 2 is Terminal and page 8 "Ratio Spread" is ChainPanel.
- Two startup checks call `std::abort()`: nav count ≠ page count (1362-1372), and an atlas row pointing at a page out of range (1384-1397).
- `--page` is validated by name or index before login (main.cpp:239-264).

**GUI-thread work**
- Everything is single-threaded on the GUI thread. There are no worker threads in these files.
- A 16 ms QTimer drains 10-1,000 ticks per frame and then runs `refresh_status()`. That function re-sets the text and stylesheet of 8 labels every frame, and rebuilds the chart every 15th frame.
- The broker pill re-reads a file every 300 frames.
- 20+ compute pages and 3 subprocess launches block synchronously (C17-001).
- MarketClock takes engine time from ticks. Staleness uses the wall clock at arrival (pump 527-528).

**Order intent flow**
- The flow is: chain click or watchlist pick → `OrderTicket::set_contract` stores a spec map keyed by token → REQUEST BUY/SELL runs `submit()`:
  1. checks the role capability;
  2. refuses without a spec;
  3. checks the price is on-tick;
  4. asks for the typed phrase in a modal QInputDialog;
  5. appends a line with QFile (Append|Text) to `${CMAKE_SOURCE_DIR}/data/order_intents.jsonl`.
- F1 and F2 only focus the ticket.
- oms/ drains the file by byte offset. There is no shared header, only shared vectors.

**Kill-switch flow**
- REQUEST HALT (RequestKillSwitch role, phrase "HALT" plus a reason) truncates and writes JSON to `data/kill_request.json`.
- Clear (Admin only, phrase "RESUME TRADING") calls `QFile::remove`.
- The panel says outright that nothing consumes the file yet.

**Gate-3 link facts**
- `altair_desktop` links altair_types, altair_time, altair_server, altair_feed, altair_analytics, altair_models, altair_risk, altair_backtest, altair_charges_toml (conditional), and Qt6 Core, Gui, Network, Svg and Widgets.
- There is no direct altair_oms or altair_broker link.
- The allow-list audit is direct-only (C17-018).
- Through interface links the closure also includes altair_strategies, altair_features, altair_instruments, altair_flags and altair_invariant.
- Header-only engine code is included without linking, for example instruments/kite_dump.hpp in chain_panel.hpp.
- The UI can open TCP sockets (PriceClient to 127.0.0.1:7421, hard-coded at terminal.hpp:334 and main_window.hpp:370) and can start subprocesses.

**Data sources and their labels**

| Surface | Source | Label |
|---|---|---|
| Grid replay | `load_real_tape` (dataset 1-minute closes), or a synthetic LCG walk with "(syn)" names | Status bar says REAL or SYNTHETIC |
| Chart | Replay ticks, or on-disk bars read directly | Replay is always titled "synthetic" (C17-004) |
| Terminal strip | Daily closes on disk ("close"), then the stream | "replay"/LIVE by the frame's flag |
| Option chain | Black-76 at one flat vol: India VIX for NIFTY, 20-day realised for BANKNIFTY. Strikes, lots, ticks and tokens from `data/instruments.csv` via kite_dump | Badge "MODEL — NOT MARKET QUOTES". Valued at the tick's time |
| Watchlist | Instrument master, plus a quote snapshot file from the altair_kite_quote subprocess | Snapshot age shown |
| Forecast page | Disk history, optionally merged with an altair_kite_fetch scratch fetch | Reports the anchor bar and any dropped partial bar |

## 6 Test-suite observations
- **test_order_ticket.cpp:** byte-for-byte check of `intent_line` against the oms vectors, the role gate, and escaping. It does **not** test `submit()` (spec refusal, tick refusal, confirmation text, `at` format), `append_intent` failure, or control characters. `intent_escape` handles only `"` and `\`, so a newline in a user name would split a line.
- **test_terminal.cpp:**
  - [4] is a tautology (`check(true, …)`, line 129).
  - Lines 68-72 return with PASS status, skipping [2]-[8], if the watchlist table has fewer than 2 rows. It is seeded with 4 rows, so this is unlikely to trigger.
  - It depends on the real `data/instruments.csv` and `dataset/`.
  - The target defines ALTAIR_INTENT_FILE and ALTAIR_KILL_REQUEST_FILE as the **production** paths (CMakeLists.txt:564-571), so constructing TerminalPage reads the real queue and kill file. A future test that submits or halts would write to production.
  - Not covered: the REPLAY/LIVE decision on the strip (C17-008), disconnect, PendingIntents, KillSwitchPanel, chain reprice cost or scroll, `set_price` on the sell side.
- No test in the group covers main_window seek, restart or reload (C17-002 to C17-005), or live_forecast merge ordering (C17-020).

## 7 Open questions
1. Does oms/ refuse a limit price far from the market, or a product that does not suit the segment (CNC on NFO)? This decides whether C17-010 should be P1 or P0.
2. What does oms/ do with an empty or malformed `kill_request.json`, and does it refuse an `at` with no offset?
3. What units are `calibrate_band`'s `next_lo`/`next_up`? live_forecast.hpp:432-433 adds them to a price. If they are bps, the published conformal band is wrong by roughly a factor of the price divided by 10⁴.
4. Do `backtest_forecasts` and `forecast_next` refuse timestamps that are not ascending (C17-020)?
5. Do the CMake link lists of strategies, features, instruments, flags and invariant stay free of oms and broker? The transitive closure is unverified.
6. What stream rate does altair_price_service produce in live and replay modes? That sets C17-007's real cost.
7. The Terminal's Escape shortcut (terminal.hpp:369) and MainWindow's F11/Escape action (main_window.hpp:276) share a key while the Terminal has focus. Qt may treat the key as ambiguous (unverified).
