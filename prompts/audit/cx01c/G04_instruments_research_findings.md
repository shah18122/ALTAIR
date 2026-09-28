# CX-01 Phase 1 -- G04 instruments + research -- findings

Reader: claude-subagent:G04_instruments_research. Baseline HEAD a34af5c5c9fa8c7498e8cc4df00907548a41e1a5. Review date 2026-09-14 (outputs finalised 2026-09-15). Finding prefix C04.

Read-only audit. Nothing was built, run or executed. data/, research/reference/, the reference trees and PDFs were not opened.

---

## 1 Scope & coverage

- **Assigned:** 26 files, 7,492 lines (CX01C_PARTITION.json, group G04_instruments_research).
- **Fully read:** 26 of 26 files, 7,492 of 7,492 lines. Every file was read with the Read tool in chunks of 300 lines or fewer, and no truncated lines were observed.
- **Hashes:** sha256 before reading equals the inventory value for all 26 files. sha256 after reading equals both. No file changed.
- **Partial or unread:** none.
- **Read outside the group, only to check integration** (not counted as coverage):
  - app/price_service_main.cpp:320-429
  - app/kite_ticker_main.cpp:230-259
  - app/instruments_demo.cpp:140-429
  - desktop/data/master_lookup.hpp:120-174
  - grep hits (with context) in feed/kite_decoder.hpp, feed/failover.hpp, feed/tick.hpp, feed/tick_store.hpp, desktop/option_chain.hpp, desktop/feed_status.hpp, desktop/data/real_tape.hpp, core/time/timestamp.hpp, core/types/units.hpp and prompts/CODEX_COMPLETION_PLAN.md
- **Revalidation:** earlier Codex notes were not used as evidence. prompts/audit/CX01_MANIFEST.json only lists these paths.

## 2 Findings table

| ID | Sev | Class | Title |
|---|---|---|---|
| C04-001 | P2 | DESIGN GAP | The live binaries register placeholder specs; no production path fills SpecStore from masters |
| C04-002 | P2 | DESIGN GAP | Point-in-time spec history cannot be represented (one window per token) |
| C04-003 | P2 | DESIGN GAP | Snapshot policy is inert without an expiry calendar, and loaders cannot stamp `stale` |
| C04-004 | P2 | CONFIRMED DEFECT | ingest_bars.py discards the exchange, so NSE and BSE exports merge into one dataset path |
| C04-005 | P2 | DESIGN GAP | The two-source identity (Kite, Xts) is hard-wired beyond `kFeedSourceCount`; a naive FYERS enum append compiles and misbehaves |
| C04-006 | P3 | CONFIRMED DEFECT | load_mktlots reads past `hf[64]` when a header has more than 64 fields |
| C04-007 | P3 | CONFIRMED DEFECT (partial fix) | Underlying truncation is counted but never surfaced, and the truncated key silently merges contracts in the Reconciler |
| C04-008 | P3 | CONFIRMED DEFECT | Kite lot, tick and token values are not validated (fractional lot truncates; zero or negative accepted); zero merges unblocked |
| C04-009 | P3 | DESIGN GAP | Kite parser ignores the `segment` column: INDICES rows become Cash specs, and CDS/BCD rows get price_scale 100 |
| C04-010 | P3 | DESIGN GAP | `ContractSpec::expiry` holds the IST midnight of the expiry date but is documented as the "expiry instant" |
| C04-011 | P3 | DESIGN GAP | `primary_source_seen` is global across exchange and segment; the EQUITY_L and fo_mktlots primaries are not connected |
| C04-012 | P3 | DESIGN GAP | Silent drops in parsers: mktlots over-long SYMBOL and bad cells, Kite in-quote commas, extra fields |
| C04-013 | P3 | CONFIRMED DEFECT | `ResearchRegistry::specify` has no status guard, so a Rejected paper resets to Specified and the trial count drops |
| C04-014 | P3 | CONFIRMED DEFECT | `emit_scaffold` silently truncates when 512 <= cap < output length, returning success without `#error` |
| C04-015 | P3 | CONFIRMED DEFECT (doc/test units) | Deflated Sharpe fed an annualised Sharpe against a per-observation T (gate 8) |
| C04-016 | P3 | DESIGN GAP | Replication and promotion evidence is not bound to the run; the gate uses a point estimate |
| C04-017 | P3 | DESIGN GAP / HYPOTHESIS | ingest_bars.py: unvalidated `time` keys, non-atomic overwrite, partial bars accepted |
| C04-018 | P3 | DESIGN GAP (doc) | papers README, registry.json and index.md describe a pipeline no code implements |

No P0 or P1 was found in this group. The instruments pipeline is not on any path that places orders today; C04-001 is why.

## 3 Finding details

### C04-001 -- P2 DESIGN GAP -- placeholder specs in the live binaries

**Evidence**
- app/price_service_main.cpp:350-356 registers a spec for each subscribed token:
  `cs.lot_size = altair::LotSize{1};` / `cs.tick_size = altair::Price{1};` / `cs.price_scale = 100;`
- app/kite_ticker_main.cpp:243-250 is identical.
- Both leave `exchange`/`segment` at NSE/Cash, `expiry` at epoch, and `valid_from == valid_to == Timestamp{}`. So `SpecStore::at(id, t)` returns NotValidAt for every t >= 0 (contract_spec.hpp:285).
- Ids are mapped back to tokens by insertion order, not through the store (price_service_main.cpp:386-392, comment "id i is sub_tokens[i]").
- The real pipeline (Kite dump -> UDiFF -> Reconciler -> UniverseFilter -> apply_to_store) is chained only in app/instruments_demo.cpp:240, 273, 396.
- `load_equity_master`, `load_mktlots` and `MktLotsTable` have no caller outside instruments/tests. A grep across all first-party directories returned no hits.

**Trigger:** any consumer that reads lot or tick from the live store. A 1-paisa tick is wrong for NSE F&O (5 paise in every fixture), and a lot of 1 is wrong for every derivative.

**Impact:** none today. The decoder uses only `id_of` plus a scale derived from the token (feed/kite_decoder.hpp:172-186), and neither binary sizes orders. It becomes a rule 1 violation the moment a strategy, risk or OMS consumer reads these specs.

**Tests:** none cover the live registration path.

**Regression test:** a startup check that refuses to run when any subscribed token resolves to a spec whose `source_hash == 0`, or which is not valid at session open (`at(id, open)` must succeed). Plus an integration test that loads fixture masters through `apply_to_store` and subscribes from `store.token_of`.

### C04-002 -- P2 DESIGN GAP -- no point-in-time history per token

**Evidence**
- contract_spec.hpp:211-217: `add` refuses any `(feed, token)` already present (`DuplicateToken`).
- `at()` (274-289) checks a single `[valid_from, valid_to)` window per id, and `id_of(token)` returns one id.
- Every parser stamps `valid_from = snapshot_at` and `valid_to = Timestamp::max()`: kite_dump.hpp:420-421, udiff_master.hpp:171-172, nse_equity_master.hpp:209-210. Nothing ever closes a window.
- The point-in-time test sidesteps the limit: test_spec_store.cpp:175-176, "Distinct tokens so both can live in the store at once".

**Trigger:** an NSE lot revision applied to a live contract whose Kite token does not change. Storing the old window and the new window is refused.

**Impact:** the header's claim "a backtest of March must see March's lot size" (contract_spec.hpp:10-12) cannot be met through the store. backtest/ does not reference SpecStore today, so the impact is latent. It is structural for rule 1 combined with rule 6 (the same code path for backtest and live).

**Regression test:** add two specs with the same Kite token and adjacent windows `[T0,T1)` and `[T1,max)`, lots 50 and 75. Expect both accepted, `lot_at(token, T0+1) == 50` and `lot_at(token, T1) == 75`.

### C04-003 -- P2 DESIGN GAP -- snapshot policy cannot protect a rollover in practice

**Evidence**
- snapshot.hpp:197-203: `expiry_crossed` loops over `cal.expiry_count` only, so an empty calendar never crosses.
- The header states "Altair has no holiday calendar yet" (67-71), and no expiry calendar source exists in the audited paths.
- test_snapshot.cpp:155-156 codifies the unsafe case: a one-day-old snapshot with no expiry in the calendar "IS StaleUsable".
- The only caller, app/instruments_demo.cpp:193-213, uses the empty calendar for most demo rows and does not gate any load on the verdict.
- Stale stamping cannot happen: loaders take no stale argument. kite_dump.hpp:430 and udiff_master.hpp:173 set `stale = false`, and nse_equity_master.hpp leaves it value-initialised. `stale_flag_for` (63-65) has no production caller, so `SpecStore::stale_count()` is always 0 through the loaders.
- Minor: `trading_days_between` silently saturates the span at `kMaxSpanDays = 4096` (160-163). This clamps toward TooOld only while `max_stale_days` stays below about 2,900.

**Trigger:** the NSE master fetch fails on the day after a monthly expiry and the caller supplies no expiry dates. The verdict is StaleUsable, so the pre-rollover lot table is reconciled. If the Kite dump is also a pre-rollover copy, both agree and the contract is admitted with old lots.

**Tests:** test_snapshot.cpp covers the calendar-present cases well. Nothing tests the calendar-absent case as unsafe, and nothing tests stale propagation through a loader.

**Regression test:**
- `judge_snapshot(state, now, MarketCalendar{}, p)` for an F&O master must return a withholding verdict, for example a new NoCalendar.
- A load under StaleUsable must yield `store.stale_count() > 0`.

### C04-004 -- P2 CONFIRMED DEFECT -- ingest_bars.py merges exchanges

**Evidence**
- research/tools/ingest_bars.py:70 captures `(?P<ex>[A-Z]+)`.
- Line 236 discards it: `_ex, sym, tf = info`.
- Line 262 builds the path from the segment, symbol and timeframe only: `out_dir = os.path.join(DATASET, args.segment, sym, tf)`.
- Line 212: a single `--segment` (default `spot`) is applied to every input in the run.

**Trigger:** `NSE_RELIANCE, 1_x.csv` covering 2026-08-27 and `BSE_RELIANCE, 1_y.csv` covering 2026-08-28. Timestamps do not overlap, so there is no conflict, and both land in `dataset/spot/reliance/1m/` as one series. Likewise, a futures export ingested without `--segment fut` lands under spot.

**Impact:** training-data corruption. It breaks the CLAUDE.md dataset invariant ("a model trained on NIFTY spot reads exactly one directory"). Overlapping timestamps are refused loudly (lines 273-281); non-overlapping ones merge silently.

**Tests:** none. The script has no tests.

**Regression test:** with the two files above, `--dry-run` must refuse, or must print two distinct output paths. Also refuse a filename exchange or suffix (e.g. `1!`) that is inconsistent with `--segment`.

### C04-005 -- P2 DESIGN GAP -- two-source identity and the FYERS implications

**Evidence**
- contract_spec.hpp:38-39: `enum class FeedSource : std::uint8_t { Kite = 0, Xts = 1 };` and `kFeedSourceCount = 2`.
- Hard-wired beyond the count:
  - reconcile.hpp:153-154: `Entry` has only `kite_token` and `xts_token`.
  - reconcile.hpp:261-266: tokens are captured only for `SpecSource::KiteDump` and `XtsMaster`.
  - reconcile.hpp:496-497: merge copies only those two.
  - reconcile.hpp:448-450: `has_broker` tests only the KiteDump and XtsMaster bits.
  - reconcile.hpp:124-133: the precedence switch covers the five SpecSource values; `kSpecSourceCount = 5` (28); the `uint8_t` presence masks have 3 spare bits.
  - feed/failover.hpp:92-99 is binary: `(active_ == FeedSource::Kite) ? d.kite : d.xts`.
  - test_spec_store.cpp:87 asserts `kFeedSourceCount == 2`, which is a useful tripwire.

**Trigger:** appending `Fyers = 2` and bumping the count to 3.
- It compiles.
- A FYERS-only contract from a new master becomes NoBroker (blocked), or loses its token in merge.
- Failover can never select FYERS, and would treat FYERS-active as the `d.xts` branch.

**Implications (not a recommendation to rename Xts)**
- **Layout:** `ContractSpec::token[]` grows from 8 to 12 bytes, which shifts later offsets and `sizeof`. No first-party code persists ContractSpec (no `fwrite` found), so there is no data migration for specs. SpecStore grows by 16,384 x 8 bytes, about 131 KB, for the token index.
- **Persisted data:** `FeedSource` is a byte inside `Tick` (feed/tick.hpp:101, 161 `offsetof(Tick, source) == 56`), and tick_store writes raw Tick bytes (feed/tick_store.hpp:95, 241). Existing recordings stay valid only if Kite=0 and Xts=1 are never renumbered; FYERS must take a new value.
- **UI enum:** desktop has a separate `ui::FeedSource` {Unspecified, Replay, Kite, Xts} (desktop/data/master_lookup.hpp:138-157). That comment records an earlier bug where the unqualified enum indexed `token[2]`. With a third slot the same slip becomes a silent wrong-token read instead of an out-of-bounds read, and the static_assert checks only Kite.
- **Token width:** `token` is `uint32_t`. Whether FYERS instrument identifiers fit is unverified; see Open questions.
- **Plan:** prompts/CODEX_COMPLETION_PLAN.md:151-152 already says to add a real FYERS identity without reinterpreting XTS values.

**Regression test:** a FYERS-sourced spec through `Reconciler::add`/`reconcile` must carry a non-zero FYERS token and not be NoBroker. A failover test must select FYERS when Kite and XTS are both stale.

### C04-006 -- P3 CONFIRMED DEFECT -- load_mktlots header out-of-bounds read

**Evidence**
- kite_dump.hpp:88-108: `kite_split` increments `n` even when `n >= cap`, so it returns the true field count.
- nse_mktlots.hpp:208-221 trusts it and indexes the fixed array:
  `detail::KiteField hf[64];` ... `for (std::size_t i = 2; i < hn; ++i) { const detail::KiteField t = detail::mktlots_trim(hf[i]);`
- The only early exits are BadMonth (a non-empty bad cell) and TooMany (more than 24 months), so empty trailing columns reach `i = 64`.
- UDiFF (udiff_master.hpp:110) and EQUITY_L (nse_equity_master.hpp:149) headers refuse `n > cap`; mktlots does not.

**Trigger:** `"UNDERLYING,SYMBOL,SEP-26" + 70 x "," + "\nNIFTY 50,NIFTY,65\n"`.

**Impact:** stack out-of-bounds read; `mktlots_trim` then dereferences a garbage `f.p` (undefined behaviour or a crash). Only malformed or hostile input triggers it, and there are no production callers.

**Tests:** test_nse_mktlots.cpp has no over-wide header case.

**Regression test:** the header above must return `MktLotsError::BadHeader` or `TooMany`. It should also run clean under the asan preset.

### C04-007 -- P3 CONFIRMED DEFECT (partial fix) -- underlying truncation still silent where it matters

**Evidence**
- The rule-11 fix exists. kite_dump.hpp:464-468 truncates and counts (`if (out_truncated != nullptr && truncated) { *out_truncated = true; }`), carried as `KiteLoadReport::underlying_truncated` (491) and tested at test_kite_dump.cpp:457-507.
- It is not surfaced. `underlying_truncated` has no reader outside instruments/: the grep over app/ and desktop/ is empty, and instruments_demo.cpp:246-248 prints added, unparseable and refused only. So rule 11's "put it on screen" is unmet.
- The spec itself carries no truncation mark.
- The Reconciler keys on the truncated string (reconcile.hpp:207-216), and a same-source re-add is last-writer-wins (237-247, 261-263).

**Trigger:** the two rows from test_kite_dump.cpp:467-470 (same expiry, names "PROCTER & GAMBLE HEALTH L" and "...LIMITE") loaded through `load_kite_dump` into a Reconciler. `rep.added == 2`, but `reconcile().contracts == 1`; after `apply_to_store`, token 111 is NotFound and token 222 carries the merged spec. No counter records the lost instrument.

**Impact:** a contract silently disappears in the no-primary path. With a primary loaded, the merged entry has no primary row, because UDiFF and EQUITY_L refuse more than 23 characters (udiff_master.hpp:188, nse_equity_master.hpp:195), so it blocks as MissingPrimary. Real-data reachability for derivatives is unverified: derivative `name` values are usually short symbols.

**Regression test:** the reconciler variant of the existing test must fail loudly, for example with `Reconciler::add` refusing a truncated underlying, or a collision counter. instruments_demo and any real loader must print `underlying_truncated`.

### C04-008 -- P3 CONFIRMED DEFECT -- Kite numeric fields not validated; zero merges unblocked

**Evidence**
- kite_dump.hpp:412: `s.lot_size = LotSize{lot_p->raw() / 100};`, so "75.5" becomes 75 and "0.5" becomes 0, silently.
- `parse_rupees_to_paise` accepts "-75" and "0" for lot, tick and strike (196-257), and the token accepts "0" (368-371).
- `SpecStore::add` validates symbol, scale, capacity and duplicates only, not lot or tick (contract_spec.hpp:197-218).
- reconcile.hpp:416-424 treats 0 as "not carried", and merge's `pick_nonzero` returns 0 when every source is 0 (490-495). Nothing then blocks.
- `has_broker` uses presence bits, not a non-zero token (448-450).

**Trigger:** a Kite-only load (`primary_source_seen == false`) of `...,0.05,0,FUT,NFO-FUT,NFO` gives SingleSource, and `apply_to_store` adds lot 0 unblocked. Tick "0" behaves the same whenever the primary also carries 0 (UDiFF and EQUITY_L always do). A row with `instrument_token` 0 is admitted but unroutable, and is not NoBroker.

**Contrast:** UDiFF and EQUITY_L refuse lot 0 (udiff_master.hpp:234-237, nse_equity_master.hpp:228-231), and test_nse_equity_master.cpp:264-277 tests that. Kite has no equivalent.

**Regression test:**
- `parse_kite_row` must refuse lot "75.5", "0" and "-75", tick <= 0, and token 0.
- `SpecStore::add` must refuse `lot_size <= 0` or `tick_size <= 0` for a non-blocked spec.
- `Reconciler::decide` must block when a merged lot or tick is 0.

### C04-009 -- P3 DESIGN GAP -- Kite `segment` column ignored

**Evidence**
- kite_dump.hpp:393-405 derives the segment from `exchange` plus `instrument_type` only, so NSE and BSE map to Cash.
- Kite dumps contain `segment INDICES, exchange NSE` rows: desktop/data/real_tape.hpp:95-97 ("tradingsymbol \"NIFTY BANK\", segment INDICES"), and desktop/panels.hpp:1235 notes the INDICES flag is lost.
- kite_dump.hpp:418 sets `s.price_scale = 100;` for every row, including CDS and BCD, where `default_price_scale` is 10^7 (contract_spec.hpp:143-145). The store's BadPriceScale guard (205-207) therefore can never fire for a Kite-loaded spec.
- Mitigations: the decoder independently refuses currency by token segment (feed/kite_decoder.hpp:173-177), and most CDS ticks (0.0025) are already BadDecimal.

**Impact:** non-tradable indices become tradable-looking Cash specs. The currency guard is dead for the only real loader.

**Caution for the fix:** setting the true scale makes `apply_to_store` abort the whole load on the first CDS spec (reconcile.hpp:560-563, "adds nothing further after a failure"). Exclude currency in the universe filter or block per contract instead.

**Tests:** none for INDICES, CDS or BCD rows.

**Regression test:** an INDICES row must be refused or flagged as non-tradable. A CDS row with a 2-decimal tick must reach the store as BadPriceScale or be blocked, never as an admitted scale-100 spec.

### C04-010 -- P3 DESIGN GAP -- expiry field semantics

**Evidence**
- contract_spec.hpp:104-106: "Expiry instant ... ns since the Unix epoch, UTC".
- `parse_kite_expiry` returns the IST midnight at the START of the expiry date (kite_dump.hpp:290-292; test_kite_dump.cpp:110-113). UDiFF reuses it (udiff_master.hpp:200).
- Settlement is 15:30 IST, which desktop/option_chain.hpp:300 and 487 add on their own.
- universe.hpp:117-124 compares IST day numbers, so it correctly admits contracts on expiry day.

**Trigger:** any consumer that writes `now >= spec.expiry`, or computes T = expiry - now, treats the contract as expired, or T as 15.5 h shorter, from 00:00 IST on expiry day. This is gate 7's T->0 boundary.

**Regression test:** a named accessor (e.g. `settlement_instant(spec)`) with a test at 2026-09-24 10:00 IST expecting "live", or rename or document the field as a date-at-IST-midnight.

### C04-011 -- P3 DESIGN GAP -- global primary flag; unconnected primaries

**Evidence**
- reconcile.hpp:293-302 sets `r.primary_source_seen = true` if any entry of either exchange carries its primary. 443-446 then marks MissingPrimary for every contract without one, whatever its exchange or segment.
- nse_mktlots.hpp is a standalone `MktLotsTable` with no Reconciler input and no callers.
- `load_equity_master` (nse_equity_master.hpp:251) has no callers.

**Trigger:** the demo configuration (Kite plus NSE UDiFF only, instruments_demo.cpp:240, 273) blocks every NSE cash contract and every BSE contract that passes the universe filter as MissingPrimary.

**Impact:** fail-safe (it blocks, never admits), but the cash and BSE universe is unusable until a cash primary and a BSE master are both wired in. The fo_mktlots cross-check, which carries lot sizes per month, never participates in reconciliation.

**Regression test:** Reconciler with a UDiFF NIFTY future plus Kite rows for NIFTY future and RELIANCE NSE cash. Current behaviour: RELIANCE MissingPrimary. Decide the intended behaviour (per exchange and segment "primary expected" configuration) and pin it.

### C04-012 -- P3 DESIGN GAP -- silent drops in parsers (rule 11)

**Evidence**
- nse_mktlots.hpp:249: rows with `sym.n > kMaxUnderlyingLen` are skipped with no counter. Lines 262-270: a non-parsing cell ("65.0", "abc", overflow) stays 0 and is reported as NotFound, the same answer as "does not trade that month".
- kite_dump.hpp:82-113: `kite_split` splits on commas inside quotes. The comment says Kite quotes only `name`, but a comma in a name shifts every later column. With the real column order the row usually fails as BadDate (counted as unparseable), which is loud but loses a real instrument.
- kite_dump.hpp:351-360: rows with MORE fields than the header are not refused.
- nse_equity_master.hpp:293-298: the series filter is skipped when a short row lacks the series column. Not reachable with the real header order (series=2 < market_lot=5).

**Regression tests:**
- mktlots with a 24-character SYMBOL must count or refuse it.
- A mktlots cell "65.0" must be BadNumber, not NotFound.
- A Kite row with a quoted name containing a comma, or with extra fields, must be a counted refusal with a distinct error.

### C04-013 -- P3 CONFIRMED DEFECT -- registry re-specify erases rejections

**Evidence:** research/feature_card.hpp:207-224. `specify()` checks id, provenance and completeness, then `merged.status = PaperStatus::Specified; *c = merged;`. There is no check on the current status. `advance()` (229-244) enumerates transitions, but `specify` bypasses them.

**Trigger:** ingest, specify, advance to Implemented, advance to Rejected, then specify again. The call succeeds, `count(Rejected)` drops by 1 and `trials()` drops by 1. It can be repeated until the paper replicates.

**Impact:** defeats the module's stated invariant ("the row survives being rejected", 16-19). The DSR trial count is understated, so significance is overstated. Research code is used only by its test.

**Tests:** test_research.cpp tests lifecycle transitions via `advance` only.

**Regression test:** the sequence above must return BadTransition and leave `count(Rejected) == 1`.

### C04-014 -- P3 CONFIRMED DEFECT -- scaffold truncation returns success

**Evidence**
- feature_card.hpp:176-182: `append` stops at `cap` silently.
- 318-320 refuses only `cap < 512`.
- 359-363 writes the `#error` lines near the end and returns `at` regardless.
- The fixed literals before `#error` add up to about 800 bytes by count, plus name x4, transform, universe and venue.

**Trigger:** `static char buf[600]; emit_scaffold(momentum_spec(), buf, 600)` returns `has_value()` with `scaffold_is_unfilled(buf) == false`.

**Impact:** contradicts test_research.cpp:180-184 ("refused rather than truncated -- a truncated scaffold could lose the #error"). The test only probes cap 64.

**Regression test:** the call above must return BufferTooSmall. Implement by tracking overflow in `append`.

### C04-015 -- P3 CONFIRMED DEFECT (units) -- deflated Sharpe periodicity

**Evidence**
- replication.hpp:168-189: z = (SR - SR0) * sqrt(n-1) / sqrt(1 - skew*SR + (kurt-1)/4*SR^2). This is the Bailey/Lopez de Prado form, where SR, SR0 and V must be per-observation.
- No unit is documented on `observed_sharpe` or `sharpe_variance`.
- test_research.cpp:193-196 uses `const double sr = 1.5; // annualised` with `obs = 1000` and `var = 1.0`. The header table (19-49) repeats those numbers.

**Trigger:** daily returns with an annual Sharpe of 0.5 (about 0.0315 per day), T=1000, N=1.
- Per-period input: z is about 1.0, DSR about 0.84.
- Annualised input: z is about 15.8, DSR about 1.0.
- Significance is overstated by about sqrt(252).

**Impact:** a replication verdict can pass on units alone. The module is unconnected.

**Regression test:** document the units in the function contract and add a test that converts an annualised Sharpe and variance to per-period values before the call, asserting the 0.84 case.

### C04-016 -- P3 DESIGN GAP -- replication evidence not bound

**Evidence**
- replication.hpp:247-274: `evaluate()` can be called repeatedly after a single `preregister`. `evaluated_` is set but never checked, so a caller can re-evaluate on new samples until one passes.
- `trials_at_registration` is caller-supplied and not tied to `ResearchRegistry::trials()`. That count excludes the paper under test (feature_card.hpp:266-276), so a first trial gives 0, which is refused as BadParameter.
- 316-340: `PromotionEvidence` fields are plain writable values. test_research.cpp:302 sets `e.replication.replicated = true;` on a result that may not have replicated.
- The gate accepts `net_edge_paise > 0` as a point estimate, with no lower confidence bound (against CLAUDE.md's "size on the lower confidence bound").

**Regression test:**
- A second `evaluate` on one run must be refused.
- The gate must take an opaque result produced by `ReplicationRun` together with edge error bounds, and veto when the lower bound is <= 0.

### C04-017 -- P3 DESIGN GAP / HYPOTHESIS -- ingest_bars.py data integrity

**Evidence**
- research/tools/ingest_bars.py:120-122 takes `time` as raw text, with no format validation or normalisation.
- 104-105 partitions by string slices (`time_text[:10]` / `[:7]`, documented as "already the local trading date").
- 252-257 and 265-270 key merges on the raw string, and 184 reads HH:MM from `[11:16]`.
- 123-131: empty or non-numeric price text is accepted.
- 292-296: the partition is rewritten in place with `open(out_path, "w")`, not temp-file plus rename. Manifest writes at 322-323 are also in place.
- 289 and 325 count every staged row as "+n from this run", including rows already on disk.

**Triggers**
- HYPOTHESIS (TradingView export formats not verified here): the same instant exported once with `+05:30` and once as `Z` or epoch seconds gives two distinct keys, so duplicate bars and a wrong partition (epoch text sliced as a date).
- CONFIRMED by reading: a crash or disk-full during `writerows` truncates an existing partition, so history is lost.
- The last, still-forming bar of an intraday export is ingested as final. A later full export then conflicts and blocks further ingests (loud, but the partial bar is already on disk).

**Positive:** the forward-projection, repainting and indicator columns are dropped (51-68), which removes the look-ahead columns; conflicting values are refused before any write (273-281).

**Regression test:** fixtures with mixed time formats must be refused or normalised. Simulate an exception mid-write and assert the old partition is intact. Empty price fields must be refused.

### C04-018 -- P3 DESIGN GAP (doc) -- paper intake docs vs code

**Evidence**
- research/papers/README.md:16-46 specifies slug ids, `status: unverified|replicated|rejected`, and `processed/`, `rejected/`, `notebooks/` and `replication/` directories. It also says `registry.json` is "consumed by the feature builder".
- registry.json is empty (`"papers": []`) and no first-party code reads it (grep for `registry.json` returned nothing).
- The C++ registry uses numeric ids and an 8-state PaperStatus (feature_card.hpp:58-75).
- None of those directories is in the audited inventory; only `inbox/.gitkeep` is.

**Impact:** a reader trusts a pipeline that does not exist, and the two schemas will diverge.

**Regression test:** a doc-consistency check, or bring the README in line with feature_card.hpp.

## 4 Lead revalidation

| Lead | Verdict | Evidence |
|---|---|---|
| Placeholder live instrument specs (price_service_main.cpp ~:344) | **CONFIRMED** | C04-001. Lines 344-361 register lot 1, tick 1, scale 100, and an empty validity window. kite_ticker_main.cpp:243-254 does the same. |
| Could instruments/ supply real point-in-time specs to that path today? | **NO (PARTIAL building blocks)** | Built: Kite, UDiFF (NSE and BSE F&O) and EQUITY_L parsers; the mktlots table; Reconciler; UniverseFilter; snapshot policy; `apply_to_store`. See the missing list below. |
| Lot / tick / strike-step / expiry literals in this group (rule 1) | **NOT CONFIRMED in production headers** | No such literal in instruments/*.hpp or research/*.hpp. Tests use fixture literals (75, 65, 30). `price_scale = 100` is a literal in kite_dump.hpp:418, nse_equity_master.hpp:205 and udiff_master.hpp:165 (wrong for CDS in Kite, C04-009). ingest_bars.py:147-148 hard-codes session hours. |
| contract_spec.hpp:38 source identity Kite=0 / Xts=1, fixed count | **CONFIRMED, with implications** | C04-005: layout growth, persisted Tick byte, the Reconciler's two-token merge, binary failover, the UI enum hazard, token width unverified. |
| kite_dump: CSV quoting | **PARTIAL** | One layer of surrounding quotes is stripped (101-104). Commas inside quotes are not honoured, and extra fields are accepted (C04-012). |
| kite_dump: 23-char underlying bound (137-prefix collision) | **PARTIAL (fixed as "count", not surfaced, still merges)** | The count is carried and tested (464-468, 491, 554). It is not displayed anywhere, and the Reconciler still silently merges truncated keys (C04-007). |
| kite_dump: token range / narrowing | **CONFIRMED safe, with a gap** | `kite_parse_u64(..., 0xFFFF'FFFFull, tok)` refuses values over uint32 before the cast (368-371, 409-410). Token 0 is accepted (C04-008). |
| kite_dump: expiry IST handling | **CONFIRMED correct as a date; semantic gap** | IST midnight is computed correctly (290-292, tested at test_kite_dump.cpp:105-127). It is not the 15:30 settlement instant (C04-010). |
| reconcile: disagreement blocks only the disagreeing symbol | **CONFIRMED** | Verdicts are per D1 key (expiry, strike, underlying, exchange, segment, opt_type) (90-106, 304-317). `apply_to_store` blocks per id (564-570). test_reconcile.cpp:327-361 shows the agreed contract is unaffected. |
| reconcile: missing-source behaviour | **CONFIRMED as designed, global scope** | MissingPrimary only when some primary was seen in the run, and that flag is global (C04-011). NoBroker when no Kite or XTS bit is present. A withheld source blocks through those verdicts (test_snapshot.cpp:239-274). |
| snapshot.hpp stale-master policy | **PARTIAL** | The verdict ladder is correct, IST-based and tested. It is inert without an expiry calendar, `stale` is never stamped, and it gates no real load (C04-003). |
| nse_mktlots / nse_equity_master / udiff_master bounds | **PARTIAL** | Capacity bounds refuse (TooMany at 24 months and 512 underlyings; BadHeader above 32 or 64 fields for equity and UDiFF). mktlots header out-of-bounds read (C04-006); silent row and cell drops (C04-012). |
| universe.hpp | **CONFIRMED sound** | Filters at `Reconciler::add` (reconcile.hpp:203-205), counts per reason, exempts cash by epoch, compares IST day numbers via `ist_days_between` (core/time/timestamp.hpp:296-301). |
| research: replication pass/fail and look-ahead | **PARTIAL** | Replicated iff DSR >= the pre-registered threshold; the gate is a 4-term conjunction. There are defects in units, binding and re-specify (C04-013/015/016). The harness takes summary statistics only, so look-ahead is not checkable there, and `out_of_sample` is a caller-attested bool. |
| research: feature_card | **PARTIAL** | Sound attempt registry; re-specify defect (C04-013); scaffold truncation (C04-014). |
| research: ingest_bars.py | **PARTIAL** | Look-ahead columns dropped and conflicts refused. Exchange merge (C04-004); time keys and non-atomic overwrite (C04-017). |
| research: papers registry / index | **CONFIRMED doc-only** | Empty registry, template index, no reader (C04-018). |

**Missing before the live binaries can use real specs (C04-001):**
1. A production load step in price_service and kite_ticker (fetch or snapshot files, judge_snapshot gating whether each source is withheld).
2. An expiry and holiday calendar source.
3. EQUITY_L wiring, a BSE cash master, and an XTS master (P1-05).
4. A universe configuration from config/.
5. Stale stamping on parsed specs.
6. Subscription tokens taken from `store.token_of` rather than insertion order.
7. INDICES and currency policy.
8. Point-in-time history (C04-002).
9. Lot and tick validation (C04-008).

## 5 Architecture facts

**Spec lifecycle as implemented:**
1. Each parser turns one row into a ContractSpec with source-specific fields zeroed. UDiFF and EQUITY_L carry no tick; Kite carries no XTS token.
2. `Reconciler::add` applies the UniverseFilter, then accumulates per D1 key (lot, tick and scale per source; last writer wins within a source).
3. `reconcile()` decides in order: ValueConflict (lot, then tick, then scale, zero excluded), then MissingPrimary (if a primary was seen in the run), then NoBroker, then SingleSource or Agreed.
4. `merge` takes each of lot, tick and scale from the highest-precedence non-zero source, the Kite and XTS tokens, a commutative hash, the oldest snapshot, and OR of stale.
5. `apply_to_store` adds every verdict and blocks the blocking ones. It aborts on the first store error.
6. SpecStore is built pre-open and read-only afterwards. `id_of` uses an open-addressed token index per feed (load factor 0.5, proven non-full) and `at` checks the half-open validity window. `spec_version` is an order-independent hash.

**Identity conventions:**
- `InstrumentId` is a dense store index assigned in insertion order.
- Broker tokens are `uint32` per FeedSource; 0 means "not carried".
- The cross-source key is the D1 tuple, not a symbol or token.
- Cash underlying = trading symbol; cash expiry = `Timestamp::epoch()`.
- Derivative underlying = Kite `name`, UDiFF `TckrSymb`, mktlots `SYMBOL`.
- Expiry = IST midnight of the date.
- Price in paise; `price_scale` = wire units per rupee, with only 100 accepted by the store.

**Precedence (D4):** Manual (4) > own-exchange master (3) > KiteDump (2) > XtsMaster (1) > wrong-exchange master (0).

**Hot vs cold:**
- `SpecStore::id_of`, `at` and `find_by_token` are ALTAIR_HOT and run on the decoder path (feed/kite_decoder.hpp:186). They do not allocate and do not throw.
- `UniverseFilter::judge`/`admit` are marked ALTAIR_HOT but run pre-open with a linear strcmp.
- Everything else in instruments/ is cold, pre-open. research/ is cold and offline.

**Memory:** SpecStore is about 1.63 MB (per comments at app/kite_ticker_main.cpp:133) and Reconciler about 5.25 MB (app/instruments_demo.cpp:29). Both are held as globals or statics everywhere in the code read; a stack instance would overflow a 1 MB stack.

**Threads:** no internal synchronisation. SpecStore is safe to read concurrently only after building finishes. Reconciler, UniverseFilter (whose `admit` mutates stats) and MktLotsTable are single-threaded.

**Implemented vs connected:**

| Component | Connected to |
|---|---|
| Kite parser | instruments_demo; desktop master_lookup and chain_panel (via `detail::load_kite_dump_into` with UI sinks and `Timestamp{0}`); broker test |
| UDiFF parser, Reconciler, UniverseFilter, snapshot policy, `apply_to_store` | instruments_demo only |
| EQUITY_L, mktlots | tests only |
| research/ headers | test_research.cpp only |
| ingest_bars.py | not referenced |
| registry.json | not read |

**FYERS implications:** summarised in C04-005.

## 6 Test-suite observations

The tests are plain `main()` programs; all bodies were read.

**Strong**
- Exact paise parsing loop (test_kite_dump.cpp:77-86).
- IST expiry and weekday traps (test_kite_dump.cpp:105-127; test_snapshot.cpp:114-139; test_nse_mktlots.cpp:150-169).
- Order independence by permutation with memcmp (test_reconcile.cpp:245-285).
- The dirty-flag guard against reading verdicts before `reconcile()` (test_reconcile.cpp:367-401).
- Blocked contracts still resolve as Blocked (test_reconcile.cpp:327-361).
- Real-file fixture rows for UDiFF, EQUITY_L and mktlots.

**Gaps**
- No Kite INDICES, CDS or BCD rows. No fractional, zero or negative lot; tick 0; token 0; in-quote commas; extra fields.
- No mktlots header wider than 64 fields, over-long SYMBOL, or malformed cell.
- No Reconciler case where every source carries 0; no `apply_to_store` mid-way failure (DuplicateToken, BadPriceScale).
- No truncation-collision test through the Reconciler. The existing test (test_kite_dump.cpp:500-504) asserts the collision exists, via the store path only.
- test_snapshot.cpp:155 asserts the calendar-absent StaleUsable outcome as correct.
- test_spec_store.cpp:175-176 sidesteps same-token history; the comment at 297 and 310 ("linear scan") is stale now that the token index exists.
- No test of the live registration path or of `underlying_truncated` being displayed.
- research: no re-specify test, scaffold cap tested only at 64, Sharpe units labelled annualised, gate test fabricates `replicated = true`.
- ingest_bars.py has no tests.

## 7 Open questions

1. **FYERS token width.** Do FYERS instrument identifiers fit in `uint32_t` (`ContractSpec::token`)? The public symbol master's token field is believed to be a long numeric string, but this is unverified in the repo and must be checked against a real fixture before choosing FYERS' key.
2. **Kite INDICES values.** What tick_size and lot_size do Kite INDICES rows carry? Values of 0 would make C04-008's unblocked-zero path reachable on real data. data/instruments.csv was deliberately not opened.
3. **UDiFF expiry column.** UDiFF has both `XpryDt` and `FininstrmActlXpryDt`. Which one matches Kite's `expiry` on holiday-shifted expiries? A mismatch splits the D1 key, and both halves block (fail-safe, but the contract becomes untradable).
4. **Bhavcopy timing.** A UDiFF bhavcopy for date D is published after D's close, so a D pre-open session must use D-1's file. How are contracts first listed on D handled (MissingPrimary)? Which `snapshot_at` is stamped, and could a backtest wrongly use D's file at D's open (look-ahead)?
5. **Kite lot format.** Does Kite ever write lot_size with a fractional part? The comment at kite_dump.hpp:377-378 says "75" or "75.0".
6. **Primary scope.** Should MissingPrimary be scoped per (exchange, segment)? Decide before wiring (C04-011).
