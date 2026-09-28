# CX-01 G14_broker_kite_tools — findings

Reader: claude-subagent:G14_broker_kite_tools · Date: 2026-09-14 · Baseline HEAD a34af5c5c9fa8c7498e8cc4df00907548a41e1a5

## 1 Scope & coverage

23 files assigned (broker/ incl. tests and tools, app/kite_*_main.cpp, app/session_file.hpp), 6,316 lines total per partition counts. **All 23 fully read** with Read in chunks of 300 lines or fewer; `unread_or_partial` is empty. Before and after hashes both match the partition inventory for every file (no changes during the review).

Nothing was built, run or fetched. data/, .env and .idea/ were not opened, and no secret values appear here. A few integration greps went into desktop/ and app/CMakeLists.txt only.

Not verifiable under the hard rules: Boost.Beast timeout semantics (vcpkg_installed/ is off-limits), and whether real dataset/ files show the precision loss (dataset/ is off-limits). Findings that depend on either are marked HYPOTHESIS or say so explicitly.

`app/session_file.hpp` is **not** a Kite credential file. It is the P0-10 synthetic replay tick session format, it touches no credential, and it has no findings.

## 2 Findings table

| ID | Sev | Class | Title |
|---|---|---|---|
| C14-001 | P1 | CONFIRMED DEFECT | Price columns written with 6 significant digits: kite_fetch (ostream default) and kite_update (`%g`) silently round NIFTY/BANKNIFTY bars in dataset/ |
| C14-002 | P1 | CONFIRMED DEFECT | Ticker `--seconds` deadline bypassed by heartbeat/text frames. A closed-market run never ends. |
| C14-003 | P2 | HYPOTHESIS | Ticker read has no idle timeout (`suggested(client)` plus `expires_never`), so a silent socket blocks forever and `TickerError::Idle` is unreachable |
| C14-004 | P2 | HYPOTHESIS | https_client `expires_after` does not bound synchronous Beast ops, and resolve is unbounded. A stalled server hangs every Kite tool. |
| C14-005 | P2 | CONFIRMED DEFECT | kite_update rewrite: unchecked I/O, remove-then-rename, whole-series rewrite, headerless first row dropped, single shared header |
| C14-006 | P2 | CONFIRMED DEFECT | kite_update never calls `verify_coverage`, so a truncated Kite answer is merged silently (the fetcher refuses the same case) |
| C14-007 | P2 | CONFIRMED DEFECT | kite_fetch `--force` truncates a whole month and writes only the requested sub-range. Month writes are non-atomic and unchecked. |
| C14-008 | P2 | CONFIRMED DEFECT | kite_quote_main drops oi/has_oi/average_price/last_quantity/net_change, and `timestamp` is always "" (never parsed) |
| C14-009 | P2 | CONFIRMED mechanism / HYPOTHESIS trigger | parse_quote scan window leaks into the NEXT instrument when an object has no `"depth"` |
| C14-010 | P2 | CONFIRMED DEFECT | data/kite_session.json written non-atomically, with no permission restriction |
| C14-011 | P2 | DESIGN GAP | Credential handling lives outside broker/: 6 app mains read env credentials and 5 copy-pasted session-file readers (3 variants) |
| C14-012 | P3 | CONFIRMED DEFECT | Snapshot writers use remove-then-`std::rename` (not atomic, old file lost on failure). Ticker store/status write errors are ignored. |
| C14-013 | P3 | CONFIRMED DEFECT | kite_callback.py: unescaped reflection into HTML on a tunnel-exposed endpoint. Any stray request_token hit ends the server. request_token on child argv. |
| C14-014 | P3 | CONFIRMED DEFECT | Form body and JSON scanner in kite_login.hpp: no URL-encoding, escape handling is wrong, first-match key search |
| C14-015 | P3 | CONFIRMED DEFECT / DESIGN GAP | kite_historical: impossible dates accepted, non-JSON numbers accepted, coverage checks calendar days only |
| C14-016 | P3 | CONFIRMED DEFECT | kite_update `--symbols banknifty` also selects nifty (substring match). printf format mismatches. Token narrowing. Unbounded body echo. |
| C14-017 | P3 | DESIGN GAP | `redact()` reveals 10 of 32 token chars on stdout that callback.py and desktop capture. Secret copies are not zeroised. |

## 3 Finding details

### C14-001 — P1 — CONFIRMED DEFECT — 6-significant-digit price serialisation into dataset/

**Evidence**
- `app/kite_fetch_main.cpp:541-542` streams doubles with the default ostream precision (6, %g-style):
  `f << stamp << ',' << c.open << ',' << c.high << ',' << c.low << ','`
- `app/kite_update_main.cpp:384-385` does the same with printf: `std::snprintf(buf, sizeof(buf), ",%g,%g,%g,%g,", c.open, c.high, ...)`.

**Trigger.** Any NIFTY/BANKNIFTY level at or above 10,000 has 5 integer digits, so the output keeps at most one decimal. 24520.25 becomes "24520.2" or "24520.3", and 51234.55 becomes "51234.5" or "51234.6". Anything at or above 1,000,000 would switch to exponent form. India VIX (about 10.68) is unaffected.

**Impact.** Silent quantisation of up to about 0.05 index points (about 0.2 bp) on every bar written by these tools. Per desktop/kite_panel.hpp that is "2.8M bars".
- Systematic noise in exactly the short-horizon returns the models use.
- It also defeats kite_update's own repair rule. A re-fetch differs from the rounded stored value by less than 1 bp, gets counted as "rounding", and is kept.
- The update header's belief that "Kite's index dailies carry one [decimal]" (`kite_update_main.cpp:19-22`) is plausibly this bug observed through the tool's own output (HYPOTHESIS; dataset/ was not read).
- Severity is P1 rather than P0 only because the magnitude is sub-bp.

**Tests.** None. No test exercises either writer.

**Regression test (offline).** Factor the row formatter out and assert that `RawCandle{open=24520.25, close=51234.55}` serialises to "24520.25" and "51234.55". Use `std::to_chars` shortest round-trip, or `%.2f` after checking the instrument's price scale.

### C14-002 — P1 — CONFIRMED DEFECT — ticker run not bounded by `--seconds`

**Evidence.** `broker/kite_ticker.hpp:346-350` (`++st.text_frames; continue;`) and `:357-360` (`++st.heartbeats; continue;`) both skip the only deadline check, at `:364` (`if (std::chrono::steady_clock::now() >= deadline) { break; }`). `app/kite_ticker_main.cpp:336` passes `never_stop`, so `should_stop` never ends the loop either.

**Trigger.** `altair_kite_ticker --go --seconds 30` outside market hours, or on tokens that never tick. Kite sends only 1-byte heartbeats, so the loop runs until the socket errors.

**Impact.**
- The process never exits by itself.
- The status file `data/kite_ticks.json` is never written.
- The "CONNECTED, AND NOTHING TICKED" branch (`kite_ticker_main.cpp:359-365`) is effectively unreachable in the one case it was written for.
- desktop/live_feed.hpp spawns this binary via QProcess ("runs altair_kite_ticker for N seconds, waits for it", per desktop/price_client.hpp:8). Whether the desktop kills it on a timeout was not verified (not in this group).
- The `--seconds 1..3600` cap (`kite_ticker_main.cpp:191`) is not actually enforced.

**Tests.** test_kite_ticker.cpp covers URL, messages, redaction and error text only. The loop is untested.

**Regression test.** Factor the frame-classification and deadline step into a pure function, e.g. `bool step(frame_kind, n, now, deadline, stats)`. Feed it heartbeats past the deadline and assert it stops.

### C14-003 — P2 — HYPOTHESIS — no idle timeout on the ticker read

**Evidence.**
- `kite_ticker.hpp:306` calls `expires_never()`.
- `:307-308` calls `ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::client))`. For the client role, Beast's suggested settings disable the idle timeout and keep-alive pings (server role enables them).
- `:330` is a synchronous `ws.read(buf, ec)`.
- The comment at `:333-334` ("A timeout with frames already delivered is a normal end") assumes a read timeout exists.

**Trigger.** A half-open TCP connection (Wi-Fi drop, NAT expiry) or a server that stops sending, heartbeats included.

**Impact.** Indefinite block regardless of C14-002. `TickerError::Idle` (`:118-119`) cannot occur.

**Why HYPOTHESIS.** Beast's client-role defaults, and the fact that its timeouts apply to async ops only, could not be confirmed from source (vcpkg_installed/ is off-limits).

**Reproducer.** A local loopback WebSocket server that completes the upgrade and then goes silent. Assert that `kite_ticker_run(read_timeout=2s)` returns within about 3s. Needs a test seam for the host/port and for TLS.

### C14-004 — P2 — HYPOTHESIS — https_client timeouts do not bound synchronous operations

**Evidence.**
- `broker/https_client.hpp:205-231` (POST) and `:317-354` (GET) call `beast::get_lowest_layer(stream).expires_after(timeout)` before synchronous `connect`, `handshake`, `http::write` and `http::read`.
- Beast documents basic_stream timeouts as applying to asynchronous operations; synchronous calls go straight to the socket.
- `resolver.resolve(host_s, "443", ec)` at `:202` and `:314` has no timeout at all.
- The header claim at `:159-160` ("`timeout` applies to each transport operation") is therefore likely false.

**Trigger.** The server accepts TCP and then stalls, or drip-feeds a body up to the 64 MB limit.

**Impact.**
- altair_kite_login/fetch/account/quote/update can hang indefinitely.
- For login, a hang wastes a single-use request_token. kite_callback.py's `timeout=60` is the only bound there.
- The desktop QProcess panels would wait on a hung child.

**Tests.** None for https_client.

**Reproducer.** A local TLS server (self-signed, with a test-only CA seam) that accepts and sleeps. Assert return within timeout plus margin. Alternatively, switch to async ops with `ioc.run_for(timeout)`.

### C14-005 — P2 — CONFIRMED DEFECT — kite_update merge write-back can corrupt dataset/

**Evidence, `app/kite_update_main.cpp`:**
- `:415-427`, the rewrite loop:
  - `std::ofstream f(tmp, std::ios::trunc); if (!f) { continue; }`
  - rows are written with no stream-state check, and `f.close()` is unchecked
  - then `std::remove(path.c_str()); std::rename(tmp.c_str(), path.c_str());` with the return value ignored
- `:416` iterates **every** file in the series (`for (const auto& [file, rows] : files)`) whenever any row was added or replaced, including untouched months and `vendor_pre2015.csv`.
- `:301-303` reads the first line and uses it as the header only if it starts with 't'. Otherwise that line is neither header nor row, so it is **dropped** on rewrite.
- `:280` and `:303` keep a single `header` for the whole series, taken from whichever file was read last. All files are rewritten with it.

**Triggers and impact.**
1. Disk full, or antivirus holding the file, mid-write: the truncated tmp is renamed over a good month (the stream is never checked), so rows are lost silently.
2. remove succeeds and rename fails (e.g. a reader holds the path): the month disappears and its data survives only in `*.csv.tmp`, which `is_month_file` (`:171-177`) and the loaders ignore.
3. A headerless CSV loses its first data row on every run.
4. Files with different header shapes (e.g. an `,oi` column) all get one header.
5. Output still reports "+N added" as success.

**Tests.** None.

**Regression test.** Extract the merge and write step behind a filesystem seam:
- (a) a headerless file keeps all rows;
- (b) a simulated write failure leaves the original byte-identical;
- (c) only changed files are rewritten.

### C14-006 — P2 — CONFIRMED DEFECT — kite_update skips the coverage check the fetcher enforces

**Evidence.** `app/kite_update_main.cpp:345-364` fetches chunks, and `:366-412` merges them. There is no `verify_coverage` call anywhere in the file. By contrast, `kite_fetch_main.cpp:428-437` refuses to write on incomplete coverage. `kite_historical.hpp:38-50` states that Kite truncates over-long requests instead of erroring.

**Trigger.** An empty series asks for `--days 400` (default) of `minute`, or any chunk that Kite truncates or returns short.

**Impact.** Holes merge into dataset/ silently, which is exactly the failure P2-12 was built to stop.

A related limitation in the same file: bar identity for intraday uses the raw stamp string (`bar_key`, `:163-168`). Stored intraday stamps in any form other than `format_ist`'s `+05:30` would duplicate rather than match (HYPOTHESIS; dataset/ was not read). Duplicates already present across files are never collapsed (`where[key]` keeps only the last file, `:310`).

**Test.** With a fake fetch that returns candles for only the first 10 of 60 days, assert that nothing is written and a failure is counted.

### C14-007 — P2 — CONFIRMED DEFECT — kite_fetch `--force` replaces a full month with a partial one

**Evidence.**
- `app/kite_fetch_main.cpp:499` skips a month only if `!force`.
- `:529` does `f.open(path, std::ios::trunc)`, and only candles in the requested range are written (`:541-551`).
- Writes and close are never checked (`:541-553`), and the target is written in place rather than temp-plus-rename.
- The program's own advice at `:556-562` is "re-run with --force, or narrow the range".
- The header claim "It never writes a partial file" (`:19-22`) covers coverage only, not I/O.

**Trigger.** `--from 2024-09-15 --to 2024-10-31 --force` with a full `2024-09.csv` on disk.

**Impact.** September 1-14 is deleted. Separately, a disk-full or crash mid-write leaves a torn CSV.

**Test.** Pure-function the month writer. Seed a full month, "force-fetch" a sub-range, and assert the rows outside the range survive (merge) or that the operation is refused.

### C14-008 — P2 — CONFIRMED DEFECT — lead revalidated: quote serialiser drops fields; timestamp never parsed

**Evidence.**
- `app/kite_quote_main.cpp:264-280` writes `token`, `last_paise`, `volume`, `buy_qty`, `sell_qty`, OHLC, circuits, `has_touch`, `timestamp` and depth. It does **not** write `oi`, `has_oi`, `average_price`, `last_quantity` or `net_change`, all of which are parsed at `broker/kite_quote.hpp:215-236`.
- `KiteQuote::timestamp` (`kite_quote.hpp:108`) is never assigned inside `parse_quote` (`:207-277`), so `:276` always emits `"timestamp": ""`.

**Impact.**
- The UI can show fetch age (`fetched_at_unix`) but not exchange time. After hours, or on a halted symbol, a stale last price and stale depth look fresh.
- `has_oi` (the "absent is not zero" distinction) is lost at the file boundary.

**Tests.** The test_kite_quote.cpp fixture includes `timestamp` but never asserts it. The serialiser has no test.

**Regression test.** Assert `parse_quote(kBody,"NFO:NIFTY26SEPFUT")->timestamp == "2026-09-07 15:29:58"`. Add a serialiser round-trip that checks `oi`, `has_oi` and `timestamp` are present.

### C14-009 — P2 — CONFIRMED mechanism, HYPOTHESIS trigger — parse_quote window leaks into the next instrument

**Evidence.** `broker/kite_quote.hpp:200-205`:
`const auto depth_at = body.find("\"depth\"", at);`
The end of the scan window is derived from that match. If this instrument's object has no `"depth"`, the next instrument's depth is found, and `obj` spans both objects. `find_number(obj, 0, ...)` then returns the first match, so fields absent from this instrument (`volume`, `oi` which sets `has_oi=true`, `buy_quantity`, circuits, and the `"buy":[` / `"sell":[` depth arrays) are read from the neighbour.

**Trigger.** A /quote response where an earlier key's object omits `depth`. Go serialises map keys sorted, so `NSE:...` precedes `NSE:...`/`NFO:...` in lexical order. The test fixture gives the index a zero-filled depth. Whether live Kite omits depth for indices is unverified.

**Impact.** An index row could show another instrument's bid/ask (`has_touch()==true`) and OI in the watchlist.

**Test.** A fixture with `"NSE:NIFTY 50"` having no `depth` key, followed by a future with a book. Assert `!has_touch()` and `!has_oi` for the index.

### C14-010 — P2 — CONFIRMED DEFECT — session credential file write

**Evidence.** `app/kite_login_main.cpp:194-207`:
- `std::ofstream out{out_path, std::ios::binary | std::ios::trunc}; ... out << r->raw_json;`
- written directly over `data/kite_session.json`, with no temp-plus-rename;
- no ACL or mode restriction: it inherits the data/ ACL on Windows, and on POSIX it is umask-default, typically 0644.

The snapshot tools use temp-plus-rename (`kite_account_main.cpp:190-224`), but the credential file does not.

**Trigger.** A crash, kill or disk-full during the write.

**Impact.** The previous file is truncated and the new token is live but unrecorded, which is the state the code itself calls "worse than not having it". It is also readable by other local users on POSIX.

**Test.** Hard to unit-test in main(). Extract `persist_session(path, json)` and assert that the original survives an injected write failure, and on POSIX that the mode is 0600.

### C14-011 — P2 — DESIGN GAP — credentials are touched outside broker/

**Evidence.**
- `broker/CMakeLists.txt:3` and `broker/kite_session.hpp:3-4` say broker/ is "The ONLY directory that touches a credential".
- In practice:
  - `ALTAIR_KITE_API_KEY` is read in `app/kite_login_main.cpp:133`, `kite_account_main.cpp:148`, `kite_fetch_main.cpp:154/371`, `kite_quote_main.cpp:191`, `kite_ticker_main.cpp:211` and `kite_update_main.cpp:255`.
  - `ALTAIR_KITE_API_SECRET` is read only in `kite_login_main.cpp:134`, which is correct.
  - broker/tools/kite_callback.py:237-241 checks the key value and the secret's presence.
- The access-token reader is copy-pasted 5 times in 3 variants:
  - `kite_fetch_main.cpp:55-74` (text mode, whitespace-aware);
  - `kite_quote_main.cpp:68-83` (next quote after key, ignoring the colon);
  - `kite_account_main.cpp:77-91`, `kite_ticker_main.cpp:94-108` and `kite_update_main.cpp:95-109`, which use `all.find(':', k) + 1`. If no ':' follows, that is `npos + 1 == 0` and the scan restarts from byte 0, returning a wrong substring. Harmless (403), but divergent.
- All 5 hardcode the relative path `"data/kite_session.json"`, so they depend on the CWD.

**Impact.** The audit surface for credential handling is 7 files and 2 languages, not one directory, and a fix to one reader will not reach the others.

**Recommendation.** A `broker/kite_credentials.hpp` with a single `load_kite_auth()` returning `expected<AuthHeader,...>`.

### C14-012 — P3 — CONFIRMED DEFECT — snapshot/store write discipline

**Evidence.**
- remove-then-rename in `kite_account_main.cpp:219-220`, `kite_quote_main.cpp:288-289` and `kite_ticker_main.cpp:398-399`. `std::rename` on Windows fails onto an existing file, hence the remove, which leaves a window with no file. If rename then fails, the old snapshot is gone.
- `kite_fetch_main.cpp:242-248` uses `std::filesystem::rename`, which replaces on MSVC, then falls back to remove-and-retry.
- `kite_ticker_main.cpp:377-397` never checks stream state after writing.
- `:303` and `:405` do `(void)writer.write(t)` / `(void)writer.close()`, so tick-store failures are silent and uncounted, which is against the spirit of rule 11.
- `kite_account_main.cpp:217` returns on a bad stream but leaves the `.tmp` behind.

**Impact.** A transiently missing or stale snapshot, and silently dropped stored ticks.

**Fix and test.** Use `std::filesystem::rename` (MoveFileEx with REPLACE_EXISTING) and count store write failures in the status JSON.

### C14-013 — P3 — CONFIRMED DEFECT — kite_callback.py exposure

**Evidence, `broker/tools/kite_callback.py`:**
- `:156`: `f"Kite reported status <code>{status}</code>."` reflects an attacker-controlled query parameter into HTML, unescaped.
- `:177`: `f"<pre>{msg}</pre>"` embeds the child's output, unescaped.
- `:168-179`: any GET carrying `request_token` triggers an exchange, and `_result["done"] = True` is set on failure too, so the server stops.
- `:104`: `subprocess.run([exe, req_tok], ...)` puts the single-use request_token on the child's command line.

The tunnel makes the endpoint internet-reachable (`:41-44`). Binding is 127.0.0.1 only (`:68`), which is correct. The request_token value is not logged, only its length (`:165`).

**Trigger.** While the tunnel is up, a scanner or third party requests `/zerodha/callback?request_token=x`, or `?status=<script>...`.

**Impact.** Reflected XSS on the tunnel domain. The login flow is aborted, so the user must restart it. The request token is briefly visible in the process list.

**Fix and test.** Use `html.escape`. Only treat as terminal a callback whose `status=success` and whose token has the expected shape. Pass the token via stdin. Test with `http.server` in-process (no network beyond loopback).

### C14-014 — P3 — CONFIRMED DEFECT — kite_login.hpp body and JSON scanner

**Evidence.**
- `broker/kite_login.hpp:173-175` concatenates `api_key` and `request_token` into the form body without URL-encoding.
- `app/kite_login_main.cpp:29-37` extracts the token from a pasted URL without percent-decoding.
- `json_string_field` (`kite_login.hpp:117-139`):
  - finds the first `"key"` anywhere in the document, including inside string values;
  - treats `\n` as `n` and `\uXXXX` as `uXXXX`.
- It is used for `access_token`, `public_token` and `user_id`, which Kite issues as alphanumeric, so the practical risk is low.

**Test.** Unit-test `json_string_field` with escaped values, and with a message containing `"access_token"` as text.

### C14-015 — P3 — kite_historical parser strictness

**Evidence.**
- `broker/kite_historical.hpp:212` validates day in 1..31 regardless of month, so `2026-02-31` becomes a March day number silently. `chunk_requests`, `historical_uri` and `verify_coverage` accept it.
- `read_number` (`:362-380`) accepts a leading `+` and `1e`-without-digits, which are not JSON.
- `verify_coverage` (`:512-548`) measures gaps in whole calendar days, so a missing half session or hours of minute bars passes (DESIGN GAP).
- The chunk caps (`:90-97`) match Kite's documented per-interval limits, and the seams are tested.

**Test.** `parse_date("2026-02-31")` must fail. Coverage must reject a day with a 3-hour intraday hole when an expected-bar count is supplied.

### C14-016 — P3 — CONFIRMED DEFECT — small CLI defects

**Evidence.**
- `app/kite_update_main.cpp:214`: `if (want.find(s.name) != std::string::npos)` means `--symbols banknifty` also updates `nifty`.
- `:204`: `--days` goes through `atoi` unvalidated.
- `kite_fetch_main.cpp:170` and `:406` print `%d` with `unsigned` status. `kite_quote_main.cpp:210` prints `%ld` with `unsigned`, which is UB on LP64 and works on MSVC.
- The same two fetch lines echo the entire non-200 body (up to 64 MB) to stdout. It carries no credential.
- `kite_ticker_main.cpp:165-167`: `static_cast<std::uint32_t>(atoll(...))` silently wraps tokens above 2^32.
- `kite_fetch_main.cpp:289`: `atoll` accepts a trailing garbage token.

### C14-017 — P3 — DESIGN GAP — redaction policy and secret lifetime

**Evidence.**
- `broker/kite_login.hpp:239-246`: `redact()` keeps the first 6 and last 4 characters.
- `app/kite_login_main.cpp:214-215` prints that. The stdout is captured by kite_callback.py (`:109`, printed at `:173`) and by desktop/kite_link.hpp via QProcess.
- By contrast, `redact_ws_url` (`kite_ticker.hpp:164-184`) keeps the length only.
- The secret is copied into un-zeroised `std::string`s: `kite_login.hpp:161-163` and `kite_login_main.cpp:134`.

22 unrevealed alphanumeric characters is not a practical brute-force leak, but the two redaction policies disagree and the partial token lands in any captured log. Recommend length-only redaction.

## 4 Lead revalidation

**Credential hygiene: only from env vars.**
- The API key and secret are read only from env (`_dupenv_s`/`getenv`) and are never printed.
- The API key does appear in the login URL (`kite_login_main.cpp:145-146`, callback.py:249-250). That is by Kite design; the key is not secret.
- The access token comes only from `data/kite_session.json` and is never printed in full, but is partially redacted (C14-017).
- broker/ is **not** the only directory that touches a credential (C14-011).
- `app/session_file.hpp` touches no credential (it is the replay tick format).
- Session write is non-atomic with default permissions (C14-010).
- No secret-looking literals were found in any assigned file. Test values are dummies.

**https_client.hpp.**
- Verification is real: `verify_peer` (`:181`, `:298`), the Windows ROOT store loaded with zero roots treated as fatal (`:124-151`, `:186`), SNI (`:195`, `:308`) and `host_name_verification` (`:199`, `:311`). No downgrade, no retry.
- Timeouts are likely ineffective for synchronous ops, and resolve is unbounded (C14-004).
- Max body: GET has `body_limit` of 64 MB (`:287`, `:350-351`); POST uses Beast's default 8 MB.
- Redirects are not followed; a 3xx is returned as a status, and every caller treats non-200 as failure.
- `X-Kite-Version` is hardcoded in the "generic" client (`:219`, `:330`).
- No minimum TLS version is set, so it relies on OpenSSL defaults.
- `kite::kRequestTimeoutMs = 7000` (`kite_api.hpp:47`) is unused; the tools use 20s or 60s.

**kite_login.hpp / sha256.hpp / kite_session.hpp.**
- The checksum is SHA256(api_key + request_token + api_secret), lowercase hex (`kite_session.hpp:89-112`).
- SHA-256 was reviewed line by line against FIPS 180-4: K constants, IV, σ0/σ1 schedule, Σ0/Σ1 rotations, Ch/Maj, and the 64-bit big-endian bit-length padding are all correct.
- NIST vectors are tested: empty, "abc", 56 bytes, 112 bytes, 1M 'a', streaming and the reset path (`test_kite_auth.cpp:57-143`). Ordering and lowercase checks are present.
- Error mapping separates retryable from spent-token failures.
- Overflow is refused on copy (`kite_login.hpp:221-231`).

**kite_api.hpp / kite_historical.hpp.**
- The paise→rupees formatter is exact and tested against the parser, including INT64_MIN.
- The chunk caps match Kite's limits, and seams are contiguous (tested).
- Coverage is checked in kite_fetch but **not** in kite_update (C14-006).
- Rate limiting is a 400 ms sleep between chunks (fetch `:417-419`; update `:356-358` plus 400 ms between series `:438`). That is 2.5 req/s or less, under about 3 req/s.
- There is no 429 handling, backoff or retry. A failure aborts the series or run, so there is **no retry storm**.
- kite_account makes 5 sequential GETs with no sleep, which is acceptable against the general API limit.
- Unfinished bars are dropped in update (`bar_is_complete`, `:140-151`, daily settles at 15:30 IST). kite_fetch does **not** drop the in-progress bar when `--to` is today (DESIGN note, folded into C14-007's writer).

**kite_quote.hpp / kite_quote_main.cpp.** Lead **CONFIRMED**: oi, has_oi, average_price, last_quantity and net_change are dropped, and `q.timestamp` is never assigned (C14-008). The new window-leak mechanism is C14-009.

**kite_ticker.hpp / kite_ticker_main.cpp.**
- Single thread, blocking; no threads spawned; no reconnect (returns on error).
- Frame size is bounded only by Beast's default websocket `read_message_max` (not set explicitly).
- The sampling duration is **not bounded** (C14-002), and there is no idle timeout (C14-003).
- The credential is in the URL, which is never printed; the dry run prints only the host.

**kite_update_main.cpp merge discipline.**
- Normalised key: daily only (date prefix); intraday uses the raw stamp.
- The >1 bp replace rule is on close only.
- Unfinished bars are dropped and counted.
- Writes go back to the originating file (`where`), or `all.csv` or month files.
- **It can corrupt dataset/ on partial writes (C14-005)**, and it also writes 6-significant-digit prices (C14-001).

**kite_callback.py / set_kite_secret.ps1.**
- The callback binds 127.0.0.1 and never logs the token value, but see C14-013.
- The PS1 reads the value masked (`Read-Host -AsSecureString`), zero-frees the BSTR, checks its shape, and stores it in **User-scope environment variables** (`HKCU\Environment`, plaintext, readable by any process running as the user), then reads it back.
- It prints only the length and a shape verdict, never the value.
- No history or file exposure.

**--go dry-run guards.** Verified effective in account (`:142-146`), quote (`:185-189`), ticker (`:204-209`), update (`:246-253`) and fetch (`:347-362`). No env or session read and no network before the guard.

Exceptions:
- `kite_fetch --dump-instruments` (`:147-258`) has **no --go guard**: it downloads and replaces the file immediately. The usage text (`:125-128`) lists it beside `--go` without saying so.
- `kite_login` has no guard, by design.

## 5 Architecture facts

**Processes.** Six standalone CLI binaries, built only with `ALTAIR_HAVE_NET` (app/CMakeLists.txt:30-79): altair_kite_login, altair_kite_fetch, altair_kite_account, altair_kite_update, altair_kite_ticker and altair_kite_quote. Each is single-threaded and blocking.

**Who spawns them.** desktop/ spawns all six via QProcess:
- kite_link.hpp → login, update
- kite_panel.hpp → account
- panels.hpp → quote
- live_feed.hpp → ticker
- live_forecast.hpp → fetch

`altair_price_service` also links `altair_broker_net` (app/CMakeLists.txt:92-95). Not in this group, so its use of kite_ticker_run was not reviewed.

**Credential flow (no values).**
1. The user runs set_kite_secret.ps1, which stores the key and secret in User env.
2. kite_callback.py (or a manual paste) obtains a request_token and passes it on altair_kite_login's argv.
3. altair_kite_login reads the key and secret from env and computes SHA-256 locally.
4. It POSTs to `api.kite.trade/session/token` and writes the raw JSON response, including the access_token, to `data/kite_session.json` (relative to CWD).
5. The other tools read the key from env and the token from that file, and build `Authorization: token key:token`, or put both in the WSS query string (ticker).

**Endpoints.**
- `https://api.kite.trade`: /session/token (POST); /instruments, /instruments/historical/{tok}/{iv}, /quote, /user/profile, /user/margins, /portfolio/positions, /portfolio/holdings and /orders (all GET).
- `wss://ws.kite.trade/`
- Browser login at `kite.zerodha.com/connect/login`.
- The callback tunnel `altair.thesmitshah.com` forwards to `127.0.0.1:53123`.

**No order-placing calls exist anywhere in the group.** kite_api.hpp holds only constants.

**What writes where.**

| Tool | Writes |
|---|---|
| login | `data/kite_session.json` (in place) |
| account | `data/kite_account.json` (tmp + remove + rename) |
| quote | `data/kite_quotes.json` (same) |
| ticker | `data/kite_ticks.json` (same) plus an optional `--store` tick store |
| fetch | `<out>/YYYY-MM.csv` (in place, trunc), `--dump-instruments PATH` (tmp + fs::rename) |
| update | `dataset/spot/{nifty,banknifty,indiavix}/{1m,5m,15m,60m,1d}/*.csv` (tmp + remove + rename, unchecked) |

## 6 FYERS reuse notes

**Reusable as-is:**
- `sha256.hpp` (FYERS appIdHash is also SHA-256);
- the TLS setup pattern (`load_platform_roots`, verify_peer, SNI, hostname check);
- `kite_historical.hpp` date utilities (`days_from_civil`, `parse_stamp` accepting both offset forms, `format_ist`);
- `chunk_requests` / `verify_coverage` logic, once the interval-cap table is parameterised;
- `kite_api.hpp`'s paise→rupee formatter.

**Needs generalising:**
- https_client hardcodes `X-Kite-Version` and has only form-encoded POST. FYERS token validation is a JSON POST, and FYERS auth header formats differ.
- The WebSocket scaffold in kite_ticker.hpp (connect/TLS/upgrade/read loop) is reusable only after C14-002/003 are fixed and the URL, heartbeat and subscribe vocabulary are injected. FYERS data sockets use a different binary format and auth.

**Kite-specific:**
- `KiteSession`, `kite_login.hpp`, `kite_quote.hpp` and the candle parser shape;
- the session-file path and the 5 duplicated readers.

There is **no broker-neutral session abstraction**. One is needed (see C14-011).

## 7 Test-suite observations

- 5 test executables are registered: kite_auth, kite_api, kite_historical and kite_quote in all presets, and kite_ticker only with net. None open a socket, read env or read data/ (confirmed by reading every body).
- `test_kite_auth` with argv prints a login URL for a supplied key. It is not run by ctest.
- Untested:
  - https_client entirely;
  - `kite_exchange_token`, `json_string_field` and `redact`;
  - the ticker read loop;
  - every app/*_main.cpp (writers, merge, session persist, dry-run guards);
  - kite_callback.py and set_kite_secret.ps1;
  - `verify_coverage` with interior gaps;
  - `parse_quote` timestamp and missing-depth leakage.
- The quote fixture deliberately gives the index a zero-filled depth, which is exactly what masks C14-009.
- Harness style: `check()` counts failures and main returns non-zero, so it is ctest-compatible.

## 8 Open questions

1. Does Beast 1.8x `basic_stream` / `websocket::stream` apply `expires_after` or the timeout options to synchronous ops? This decides C14-003 and C14-004; check the vcpkg_installed Beast headers.
2. Does live Kite `/quote` omit `depth` for indices (C14-009)?
3. Do the existing dataset/ CSVs written by kite_fetch/kite_update show 1-decimal NIFTY/BANKNIFTY prices (C14-001)? Are any stored intraday stamps in a non-`+05:30` form (C14-006)? Is `vendor_pre2015.csv` headerless (C14-005)?
4. Does desktop/live_feed.hpp enforce a kill timeout on altair_kite_ticker (C14-002 impact)? Does altair_price_service call `kite_ticker_run` with a real `should_stop`?
5. Is `--dump-instruments` bypassing `--go` intentional?
