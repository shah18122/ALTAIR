# CX-01C Audit — G22_docs_cards_rest_config_ops — Findings

Reader: `claude-subagent(sonnet):G22_docs_cards_rest_config_ops`
Date: 2026-09-15 · Baseline HEAD: `a34af5c5c9fa8c7498e8cc4df00907548a41e1a5`
Scope: 46 files, 7,283 lines — task cards P1-\* through P7-\*, `PHASE13_PLAN.md`,
`ROADMAP_GAP.md`, `config/altair.toml`, `config/nse_calendar.csv`,
`config/tools_gen_nse_calendar.py`, `config/fetch_banknifty.ps1`, `ops/*.md`.

---

## 1. Scope & coverage

All 46 assigned files were read in full (every line, in ≤300-line chunks),
hashed before and after review, and matched the partition inventory exactly
both times — no drift occurred during this read-only pass, and no file's
`sha256_inventory` differed from its hash at review time (i.e. nothing had
changed since the partition was cut). See
`G22_docs_cards_rest_config_ops_coverage.json` for the per-file ledger.

No file was skipped, truncated, or assessed from a grep hit alone. `oms/`,
`instruments/`, `feed/`, `book/`, `risk/`, `desktop/`, `app/`, `core/config/`
and `models/` source files were **spot-checked** with `Grep`/`Read` against
each card's interface contract — this is Gate-2-style verification of the
*cards*, not a full audit of those directories (owned by other groups). Where
a card is terse (`P3-*`, `P4-*`, `P7-00`) and already carries an embedded
review record with no separate interface-contract block, only an
existence-check on the named deliverable file was performed; this is recorded
per-file in the coverage JSON's `integration` field.

No secrets, tokens, or credential values were found in any of the 46 files —
only prose *describing* an incident (three exposed Kite API secrets,
disaster-recovery.md §6 / go-live.md 1.1), never the secret itself. `data/`,
`.env`, `.idea/`, `RXT_trade*`, `Quants/`, `GETSClient_*`, `build/`,
`vcpkg_installed/` and `dataset/` were not opened. Nothing was run.

---

## 2. Findings table

| ID | Sev | Class | File:line | Summary |
|---|---|---|---|---|
| C22-001 | P2 | CONFIRMED | `prompts/P1-02a_nse_fo_master.md:3`, `P1-02b_nse_mktlots.md:3`, `P1-02c_nse_equity_master.md:3`, `P1-03a_bse_masters.md:3` | Four cards' status lines still say "BLOCKED — needs one sample file"; all four were implemented 2026-09-01, with interfaces that diverge from what the cards specify, and the cards were never updated. |
| C22-002 | P3 | CONFIRMED | `prompts/P1-06_instruments_reconcile.md:150` vs `instruments/reconcile.hpp:31` | Card's interface contract names `enum class Verdict`; shipped code names it `ReconcileVerdict`. No `CONTRACT OBJECTION` comment anywhere, though RULES #3 requires one for any signature change. |
| C22-003 | P2 | CONFIRMED | `config/altair.toml` (whole file) | No C++ code anywhere in the tree reads any key from this file. The config-loading infrastructure exists and is tested against synthetic fixtures; nothing points it at `config/altair.toml` itself. `desktop/market_clock.hpp:24-27,78-81` explicitly documents using hardcoded demo values pending a not-yet-written card. |
| C22-004 | P2 | CONFIRMED | `prompts/ROADMAP_GAP.md:56-74` | Document's central finding ("no momentum/mean-reversion strategy — THE GAP") was closed by commit `73ba133` about one hour after this document was committed the same day. `strategies/momentum.hpp` and `strategies/meanrev.hpp` exist; the document was never updated and still presents the gap as current. |
| C22-005 | P3 | DESIGN GAP | `ops/disaster-recovery.md:31-38` | The kill-switch procedure is `touch /var/lib/altair/KILL`, and the document elsewhere says the whole mechanism must work "when the process is unresponsive" — but nothing in the runbook says who creates `/var/lib/altair/` (and with what permissions) ahead of time, and this differs from `config/altair.toml`'s own `kill_switch_file = "run/KILL"` default (relative, Windows dev). Consistent with the doc's own "WRITTEN, NEVER REHEARSED" status, but worth closing before it is needed under pressure. |
| C22-006 | P3 | DESIGN GAP | `prompts/PHASE13_PLAN.md:186-199` | "38 cards on top of the current 156 → 194" is now stale — `git log` shows work through at least Phase 41 at baseline HEAD. Expected aging for a forward plan, not a factual error (the "Altair HAS" table's claims were spot-checked and hold up), but the totals no longer describe the tree. |

No P0 or P1 findings. Nothing in this group's files instructs, or would cause,
a wrong live order, a credential leak, or a bypass of `oms/`-only order
placement — see §3 detail on why C22-001/002/003 stop at P2/P3.

---

## 3. Finding details

### C22-001 — Four P1-02/P1-03 cards show a stale BLOCKED status; interfaces diverge from the shipped parsers

**Severity:** P2 · **Classification:** CONFIRMED

**Evidence.**
- `prompts/P1-02a_nse_fo_master.md:3` — `> Phase 1 · Card 2a of 7 · Status: **BLOCKED — needs one sample file (§2)**`, and §2 (lines 45–77) is a placeholder titled "SCHEMA BLOCK — TO BE FILLED FROM THE SAMPLE FILE."
- `instruments/udiff_master.hpp:1-13` — `// P1-02a (NSE) and P1-03a (BSE), which turned out to be ONE parser. // The P1-03a card was written on the premise that BSE differs from NSE in four structural ways. For the DERIVATIVES master that premise is wrong...`
- `prompts/LEDGER.md:476` — `| P1-02a | NSE F&O UDiFF bhavcopy → ContractSpec — **DONE** · udiff_master.hpp · 43 checks | **DONE** |`; `:479` similarly marks P1-03a DONE via the same file.
- `prompts/P1-02b_nse_mktlots.md:3` (`BLOCKED`) vs `instruments/nse_mktlots.hpp:1-4`: `// P1-02b. Implemented 2026-09-01 from the real file; the skeleton this replaces returned SchemaNotConfigured.`
- `prompts/P1-02c_nse_equity_master.md:3` (`BLOCKED`) vs `instruments/nse_equity_master.hpp:1-4`: `// P1-02c. Implemented 2026-09-01 from the real file...`

**Interface drift, beyond the status line:**
- P1-02a/P1-03a: the card's `NseFoColumns`/`NseParseError`/`parse_nse_fo_row`/`load_nse_fo_master` and `BseColumns`/`BseParseError`/`parse_bse_row`/`load_bse_master` do not exist anywhere. The shipped `instruments/udiff_master.hpp` uses an entirely different, unified type set (`UdiffColumns`, `UdiffError`, `parse_udiff_row`, `load_udiff_master`) parameterised by `Exchange` rather than being two parsers.
- P1-02c: the card's `EquityColumns` includes `tick_size` (line 99) and `EquityMasterError` includes `DisallowedSeries` (line 89-90); the shipped `instruments/nse_equity_master.hpp:53-60` has no `tick_size` field and its `EquityMasterError` (line 48-51) has no `DisallowedSeries` value — series rejection is tracked only via the `rejected_series` counter. The registered ctest name is `nse_equity` (`instruments/CMakeLists.txt:45`) not the card's `nse_equity_master` (card line 69).
- P1-02b/P1-02c: none of the card's literal acceptance-test names (e.g. `header_months_parse`, `rollover_does_not_shift_lookup`, `quoted_fields`, `cash_has_epoch_expiry`) appear in `instruments/tests/test_nse_mktlots.cpp` or `instruments/tests/test_nse_equity_master.cpp`; the actual functions (`the_real_padded_file_parses`, `rollover_does_not_shift_the_lookup`, `quoted_fields_do_not_shift_columns`, `cash_expiry_is_epoch`, …) cover the same eight concepts under different names.
- P1-03a specifically: the card's four-structural-differences premise (§1) turned out correct for BSE **cash** (still genuinely unbuilt — no `bse_equity_master`/`bse_master.hpp` of any kind exists) but wrong for BSE **derivatives** (same UDiFF layout as NSE). So the BLOCKED status is half right, and the card does not say which half.

**Impact.** A reviewer running Gate 2 (contract honoured) or Gate 4 (named
tests exist) directly against these four card files, as the audit protocol
instructs, would report a false failure — the named signatures and test
names genuinely do not exist — while missing that the actual, working
implementation is elsewhere under different names. `prompts/LEDGER.md` has
the correct status; the cards themselves do not, and PROTOCOL.md places the
interface contract in the *card* as the thing "not anyone's memory" that
keeps later cards compiling against earlier ones. A card frozen at BLOCKED
with a placeholder schema section cannot serve that function once the
schema was in fact determined and shipped.

**Recommended fix.** Update the status line and §2 (or add a superseding
note, as `udiff_master.hpp`'s own header does) on all four cards. For P1-02a
and P1-03a specifically, either retire them in favour of a new
`P1-02a_udiff_master.md`-style card matching what shipped, or add a visible
"SUPERSEDED BY udiff_master.hpp — see instructions there" banner at the top.
For P1-02b/P1-02c, update the status line and reconcile the interface
contract (drop `tick_size` from the P1-02c contract, rename the registered
test) or add the RULES-#3-mandated `CONTRACT OBJECTION` note explaining the
divergence.

---

### C22-002 — `Verdict` renamed to `ReconcileVerdict` with no `CONTRACT OBJECTION`

**Severity:** P3 · **Classification:** CONFIRMED

**Evidence.**
- `prompts/P1-06_instruments_reconcile.md:150-156` (interface contract):
  ```
  enum class Verdict : std::uint8_t {
      Agreed, SingleSource, MissingPrimary, NoBroker, ValueConflict
  };
  ```
- `instruments/reconcile.hpp:31-37`:
  ```
  enum class ReconcileVerdict : std::uint8_t {
      Agreed, SingleSource, MissingPrimary, NoBroker, ValueConflict
  };
  ```
- `instruments/tests/test_reconcile.cpp:104` etc. use `ReconcileVerdict::Agreed` — confirming the source, not the card, is what actually ships.
- Grepped `instruments/reconcile.hpp` for `CONTRACT OBJECTION`: no match. The
  card's own "REVIEW RECORD — P1-06" section (lines 331-374) documents the D9
  amendment (the `dirty_`/stale-verdict fix) in detail but says nothing about
  a rename.

**Impact.** Low on its own (a name, not a behaviour), but it is exactly the
kind of undocumented drift CLAUDE.md's protocol is built to prevent — "the
interface contract in each card — not anyone's memory — is what keeps card
90 compiling against card 3." `Verdict` is a generic enough name that it
plausibly collided with something else once more headers were included
together (e.g. `snapshot.hpp`'s `SnapshotVerdict` sits right next to it
conceptually), which would be a legitimate reason for the rename — but that
reasoning is not recorded anywhere the process requires it to be.

**Recommended fix.** Add a one-line `CONTRACT OBJECTION` (or equivalent
review-record note) to `prompts/P1-06_instruments_reconcile.md` stating the
rename and why, or rename the card's contract instead.

---

### C22-003 — `config/altair.toml` has no consumer

**Severity:** P2 · **Classification:** CONFIRMED

**Evidence.**
- Whole-tree grep for representative keys (`risk.capital`, `risk.risk_per_trade`,
  `session.market_open`, `instruments.snapshot_dir`, `feed.staleness_ms`,
  `aggregator.enforce_horizon_match`) across every `*.hpp`/`*.cpp` in the repo
  returns exactly one file: `core/config/tests/test_toml_source.cpp`, which
  uses `feed.staleness_ms`/`risk.max_lots` as **synthetic test fixtures**, not
  as evidence the real file is loaded anywhere.
- Whole-tree grep for the literal string `altair.toml` (16 hits) turns up
  only: audit/prompt documentation, `CLAUDE.md`, `ROADMAP.md`, two P1-08
  cards (referencing `snapshot_dir`/`keep_snapshots_days` as *design intent*,
  not as consumed-today), `core/config/CMakeLists.txt` and
  `core/config/toml_source.hpp` (generic parser infrastructure, no call site
  naming this specific file), and `desktop/market_clock.hpp`.
- `desktop/market_clock.hpp:24-27`:
  ```
  // So `SessionWindow` is passed IN. The values used by the demo below come from
  // one place, are labelled as demo values, and are the thing P11Q-06 replaces
  // with `config/altair.toml` when a real feed is wired. They are not defaults
  // and nothing falls back to them.
  ```
  and `:78-81` repeats this for `DemoSessionTimes`, naming the not-yet-written
  card (`P11Q-06`) that would actually wire the file in.
- `app/main.cpp:85-105` — the one place `ConfigSnapshot`/`ConfigStore` are
  exercised outside tests — sets `strategy.fill_every_n_ticks`,
  `strategy.cost_paise`, `risk.initial_cash_paise` **directly in code**
  (`cfg.set_int(...)`), never loading any `.toml` file from disk.

**Impact.** Every key in `config/altair.toml` — including safety-relevant
ones like `risk.risk_per_trade_hard_cap`, `risk.daily_loss_halt`,
`session.no_new_entries_after`, `engine.kill_switch_file`,
`instruments.on_disagreement` — is currently decorative. This is not a
live-trading risk today (nothing trades), but it means an operator editing
`config/altair.toml` believing it changes engine behaviour would be wrong,
and the file's own header claim ("Hot-reloadable. Every load is hashed...")
does not yet hold for anything. This is squarely a design-gap finding, not a
P0/P1, precisely because nothing downstream currently depends on these
values being correct.

**Recommended fix.** Either add a visible "NOT YET WIRED — see P11Q-06" note
at the top of `config/altair.toml` (mirroring what `market_clock.hpp` already
says), or prioritise the wiring card. Track it as a known-open item the way
`apply_bps`/`long double` is tracked in the LEDGER's carried-debt section.

---

### C22-004 — `ROADMAP_GAP.md`'s central finding was closed the same day it was written

**Severity:** P2 · **Classification:** CONFIRMED

**Evidence.**
- `prompts/ROADMAP_GAP.md:9-21` (§0): *"Altair has Stage 4, 5 and 6
  infrastructure and is missing a Stage 3 deliverable... there is no
  standalone momentum or mean-reversion strategy in `strategies/` that
  produces a tradeable signal. That single sentence is the most useful output
  of this document."*
- `prompts/ROADMAP_GAP.md:56-74` (Stage 3 table): *"**Mean reversion
  strategy** | **NOT BUILT** — no z-score fade strategy in `strategies/`"*
  and *"**Momentum / trend following** | **NOT BUILT**..."*
- `prompts/ROADMAP_GAP.md:197-199` (§4, "What to build next, in order"):
  *"1. A momentum strategy and a mean-reversion strategy, in `strategies/`."*
- `git log --diff-filter=A -- prompts/ROADMAP_GAP.md`: committed
  `f1fd4a7` **2026-09-08 12:30:52 +0530**.
- `git log --diff-filter=A -- strategies/momentum.hpp strategies/meanrev.hpp`:
  both added in commit `73ba133` **2026-09-08 13:43:50 +0530** — about one
  hour later, same day — message: *"P21-01/02/03: momentum, mean reversion,
  and the control that killed them."*
- `strategies/momentum.hpp:1-13` opens by quoting this exact document:
  *"`ROADMAP_GAP.md` names it: Altair has the risk layer... and had nothing in
  `strategies/` that emits a directional signal... This is the first thing to
  put in them."*
- `prompts/LEDGER.md:848` records P21-01/02/03 as **DONE**, including the
  finding that neither strategy beats buy-and-hold once turnover and a
  buy-and-hold control are charged. `prompts/LEDGER.md:854` records a further
  Phase 22 (`strategies/overnight.hpp`) built directly on P21's decomposition.

All of `ROADMAP_GAP.md`'s *other* "NOT BUILT" claims (autoencoders, random
forest, ARIMA/SARIMA/VAR, PCA/factor models, alternative data) were
independently spot-checked (`grep`/`ls` across `models/`, `risk/`,
`analytics/`, `features/`) and remain accurate at baseline HEAD — this is a
narrow, single-topic staleness, not a wholesale one.

**Impact.** The document is one week stale on the single claim it flags as
its own headline finding. A reader — including a future DeepSeek card
session or Smit — using this document to decide "what to build next" would
either duplicate `momentum.hpp`/`meanrev.hpp`/`overnight.hpp`, or fail to
notice that the recommended next step already happened and already returned
a negative result ("nothing beats holding the index"), which is itself
useful context this document should carry.

**Recommended fix.** Add a dated addendum (in the style `PHASE13_PLAN.md`
itself uses for its P17-04 status section) noting P21/P22 closed the Stage 3
gap and summarising the (negative) result, or supersede the document with an
updated gap analysis.

---

### C22-005 — Kill-switch directory existence is unstated; path differs from the config default

**Severity:** P3 · **Classification:** DESIGN GAP

**Evidence.**
- `ops/disaster-recovery.md:31-38`:
  ```
  touch /var/lib/altair/KILL
  ```
  presented as "the whole mechanism... deliberately that simple: no RPC, no
  socket, no authentication, nothing that can itself be down. It works when
  the UI is frozen, when the process is unresponsive to signals..."
- Nothing in `disaster-recovery.md`, `go-live.md`, or `linux-deployment.md`
  states who creates `/var/lib/altair/` ahead of time, with what permissions,
  or on what schedule that is verified to still exist/be writable.
- `config/altair.toml:16`: `kill_switch_file = "run/KILL"` — a relative path,
  presumably relative to the Windows dev working directory, and different
  from the Linux runbook's absolute path. Given C22-003 (the file is not
  wired in anywhere yet), this discrepancy is currently cosmetic, but it is
  the kind of detail that is easy to miss once the Linux deployment and the
  config wiring both land close together.

**Impact.** `ops/disaster-recovery.md` already and correctly self-labels as
"WRITTEN, NEVER REHEARSED" (line 3) and this finding does not contradict
that self-assessment — it is exactly the kind of gap that label exists to
flag. `touch` on a path whose parent directory does not exist fails loudly
(good — consistent with rule 9), but under the pressure this document is
written for, discovering that at the moment of use is the wrong time.

**Recommended fix.** Add a one-line pre-requisite ("ensure
`/var/lib/altair/` exists and is writable by the operator account — verify
this in the rehearsal, not during an incident") and reconcile the path
against whatever `config/altair.toml`'s `kill_switch_file` ends up pointing
at once wired.

---

### C22-006 — `PHASE13_PLAN.md` totals are superseded by later phases

**Severity:** P3 · **Classification:** DESIGN GAP

**Evidence.**
- `prompts/PHASE13_PLAN.md:186-199`: *"38 cards on top of the current 156 →
  **194**."*
- `git log --oneline --since=2026-09-08 -- strategies/ models/ risk/` shows
  commits through Phase 41 (`00b057e P41: richer features...`) at baseline
  HEAD, i.e. work has continued well past the 19 phases / 194 cards this
  document projects.
- The document's factual "Altair HAS" table (§1, lines 43-58) was spot-checked
  against the tree (`risk/covariance.hpp`, `risk/var.hpp`, `risk/stress.hpp`,
  `oms/execution.hpp`, `oms/shortfall.hpp`, `models/markov.hpp`,
  `analytics/svi_fit.hpp`, `backtest/montecarlo.hpp`, `strategies/cointegration.hpp`
  all confirmed present) and holds up.

**Impact.** Low — this is an artefact of any dated forward plan continuing to
be read after the work described has progressed past it, not an error in the
document at the time it was written. Listed for completeness since the audit
brief asks for stale/false status claims; the "Altair HAS" claims (the part
someone might actually rely on for a fact-check) are accurate.

**Recommended fix.** None required; optionally add a "superseded by Phase
N" pointer if this document is kept as a long-lived reference rather than a
point-in-time plan.

---

## 4. Card-contract drift table

| Card | Contract item | Source location | Match / drift |
|---|---|---|---|
| P1-01 | `ContractSpec`, `SpecStore`, `kMaxInstruments=8192` | `instruments/contract_spec.hpp` | Match |
| P1-02a | `NseFoColumns`, `NseParseError`, `parse_nse_fo_row`, `load_nse_fo_master` | *(file does not exist)* | **Drift — superseded by `udiff_master.hpp`'s `UdiffColumns`/`UdiffError`/`parse_udiff_row`/`load_udiff_master`; see C22-001** |
| P1-02b | `MktLotsError`, `ExpiryMonth`, `MktLotsTable::lot_for` | `instruments/nse_mktlots.hpp` | Match (interface); test names drift — see C22-001 |
| P1-02c | `EquityColumns` (incl. `tick_size`), `EquityMasterError` (incl. `DisallowedSeries`) | `instruments/nse_equity_master.hpp` | **Drift — both dropped; see C22-001** |
| P1-03a | `BseColumns`, `BseParseError`, `parse_bse_row`, `load_bse_master` | *(file does not exist for derivatives; genuinely absent for cash)* | **Drift — see C22-001** |
| P1-04 | `KiteColumns`, `KiteParseError`, `parse_rupees_to_paise`, `parse_kite_expiry` | `instruments/kite_dump.hpp` | Match (card's own review record) |
| P1-06 | `enum class Verdict`, `ContractVerdict`, `Reconciler` | `instruments/reconcile.hpp` | **Drift — `Verdict` → `ReconcileVerdict`, undocumented; see C22-002.** Otherwise match, plus D10/D11 extension by P1-09 (in-manifest). |
| P1-08a | `SnapshotVerdict`, `judge_snapshot`, `expiry_crossed` | `instruments/snapshot.hpp` | Match (card's own review record) |
| P1-08b | `master_fetch.hpp`/`.cpp` | *(absent)* | Match — correctly still BLOCKED |
| P1-09 | `universe.hpp`, D11 addition to `reconcile.hpp` | `instruments/universe.hpp`, `instruments/reconcile.hpp:48-50,193-205` | Match |
| P2-01 | `Tick` (64B), `DepthUpdate` (272B), `kTickWireVersion` | `feed/tick.hpp:150-151` | Match |
| P2-02 | `KiteDecodeResult`, `decode_kite_frame` | `feed/kite_decoder.hpp` | Match (card's own review record) |
| P2-04 | `Verdictum`, `Normaliser`, forced `require_monotonic=false` | `feed/normaliser.hpp:27,33,134` | Match |
| P2-08 | `BookError`, `BookState`, `L2Book` | `book/l2_book.hpp:36,44,118` | Match |
| P2-09a | `bid_depth`, `weighted_obi`, `microprice` | `book/microstructure.hpp` | Match |

Cards not in this table (P1-08a's siblings already listed, P2-05/06/09b/10,
P3-\*, P4-\*, P7-00) were existence-checked only, per §1; no drift found in
that shallower check, and none had a byte-level interface-contract block to
diff against (terse card format — see the coverage JSON's `integration`
field per file for exactly what was and was not checked).

---

## 5. Config key usage table

`config/altair.toml`, by top-level table, against a whole-tree grep for each
key (excluding `RXT_trade*`, `Quants/`, `GETSClient_*`, `build/`,
`vcpkg_installed/`, `dataset/`):

| Key(s) | Read by |
|---|---|
| `[engine]`, `[engine.threads]`, `[session]` | Nobody — see C22-003 |
| `[feed]`, `[feed.kite]`, `[feed.xts]` | Nobody in application code; `core/config/tests/test_toml_source.cpp` uses `feed.staleness_ms`/`feed.kite.max_subscriptions`/`feed.xts.events.*` as **synthetic fixture values**, not as this file |
| `[instruments]`, `[instruments.margin]` | Nobody. `instruments/snapshot.hpp`'s `SnapshotPolicy::max_stale_days` is designed to receive this value as a parameter (P1-08a explicitly "takes one as a parameter and does not invent it") but nothing yet supplies it from this file |
| `[universe]` | Nobody |
| `[risk]`, `[risk.cooldown]`, `[risk.liquidity]`, `[risk.limits]`, `[risk.exit_ladder]` | Nobody. `risk/limits.hpp`, `risk/sizing.hpp` etc. take equivalent values as explicit constructor/struct parameters (per their cards), not read from this file |
| `[models]`, `[models.retrain]`, `[aggregator]` | Nobody |
| `[flagging]`, `[flagging.drift]` | Nobody |
| `[strategies.*]` | Nobody |
| `[storage]` | Nobody |
| `[ui]`, `[ui.rates]`, `[ui.grid]`, `[ui.charts]` | Nobody (desktop/ was not deep-audited in this pass beyond `market_clock.hpp`, which explicitly says it does *not* yet read this file) |

**Keys the code expects that the file lacks:** none identified — the
generic `core/config` layer (`ConfigSnapshot`/`ConfigStore`/`TomlSource`)
takes any key path as a runtime string, so there is no compiled-in
expectation of a specific key name to compare against. The absence is the
other direction: the file's keys have no reader (C22-003), not the reverse.

**Rule 1 literal check:** no lot size, tick size, strike step, or expiry
value appears anywhere in `config/altair.toml`. `universe.option_strikes_around_atm = 20` is a strike-window *count*
for universe construction, not a strike step or price, and does not fall
under rule 1's prohibition. Clean.

---

## 6. Ops runbook claims

### `ops/README.md`
| Claim | Status | Note |
|---|---|---|
| Cross-references `linux-deployment.md`/`disaster-recovery.md`/`go-live.md` to the right cards (P12-01/05/06) | Verified | Matches each file's own header |
| "Every claim states whether it has been verified, and on what" | Verified | Held throughout the other two ops files reviewed |

### `ops/disaster-recovery.md`
| Claim | Status | Note |
|---|---|---|
| Self-status "WRITTEN, NEVER REHEARSED" | Verified (by omission) | No evidence anywhere in `prompts/LEDGER.md` of a rehearsal having occurred |
| "Three Kite API secrets... exposed... and have not been rotated" | Verified — still current | `prompts/LEDGER.md:60` (top-of-file summary) still lists this as open at baseline |
| Kill switch is "one file, one flag, checked every loop" | Unverified here | Cross-directory claim about `oms/`/engine internals, outside this group's files |
| `touch /var/lib/altair/KILL` procedure | Design gap | See C22-005 |
| Restart refusal semantics (`BreachLatched`/`InvariantBreach`/`StaleSession`/`ChecksumMismatch`) reference `app/warm_restart.hpp` (P12-02) | Unverified here | File is outside this group; claim not independently checked |

### `ops/go-live.md`
| Claim | Status | Note |
|---|---|---|
| 1.1 API secrets unrotated | Verified — still current | Matches disaster-recovery.md and LEDGER header |
| 1.2 `charges.toml` `UNVERIFIED` | Verified — still current | `config/charges.toml:20` (outside this group, but grepped): `last_verified = "UNVERIFIED"` |
| 1.3 Phase-0 clang gate blocked | Unverified here | Build-environment claim, not independently reproducible in an audit-only pass |
| 1.4 "Live feed subscription... nothing subscribes" | Verified — still accurate, with a caveat | `git log` shows commit `a8f3d79` (P32-07) demonstrated a **bounded, manual** live tick sample (57 frames, 143 ticks) on 2026-09-09; both the commit message and `LEDGER.md:870` explicitly call this a bounded sample, not a standing/supervised subscription, so the checklist's claim is not stale |
| 2.1 "93/93" tests green | Unverified here | Not re-run (AUDIT ONLY; no test execution) |
| 4.5 no `.pt`/`.onnx`, `ALTAIR_ENABLE_TORCH` OFF | Unverified here | Build-config claim outside this group's files |
| §6 staged capital ramp definitions | Internally consistent | No arithmetic or rule contradicts CLAUDE.md §"Reality checks" |

**Go-live blocking items and their current truth** (cross-referencing
`prompts/LEDGER.md` and `prompts/audit/cx01c/CX01C_FINDINGS.md` as context,
per the brief): all six items in go-live.md §1 (1.1–1.6) are still
consistent with LEDGER's own P12-06 row (`prompts/LEDGER.md:776`), which
independently states the same six blockers as of its own writing and adds
the ¼-Kelly/−7.19σ sizing caveat. No contradiction found between the two
documents.

### `ops/linux-deployment.md`
| Claim | Status | Note |
|---|---|---|
| Self-status "PLAN, NOT PROCEDURE... never been compiled on Linux" | Verified (by omission) | Consistent with go-live.md 1.3 (Phase-0 clang gate blocked) |
| "Retail broker APIs cap you at 10-50ms... isolating a core... is a decoration" | Verified | Directly restates CLAUDE.md's own "Reality checks" section, no drift |
| `desktop/` runs in the same process, must stay on housekeeping cores | Verified | Matches CLAUDE.md's "The in-process decision" section verbatim in spirit |
| No dangerous/impossible steps found | — | Kernel parameters and `cyclictest` usage are standard; nothing instructs a destructive or irreversible action |

---

## 7. Open questions

1. **Should P1-02a and P1-03a be formally retired** in favour of a
   `P1-02a_udiff_master.md`-shaped replacement, or is a superseding banner
   sufficient? (C22-001)
2. **Is the `Verdict` → `ReconcileVerdict` rename intentional** (e.g. to avoid
   a future collision with `SnapshotVerdict`), or an oversight that should be
   reverted? (C22-002)
3. **What is the actual plan and timeline for wiring `config/altair.toml`**
   into the engine (P11Q-06, per `desktop/market_clock.hpp`)? Until then, is
   it worth a visible "NOT YET WIRED" banner on the file itself, given how
   easy it would be for a future reader (human or DeepSeek) to assume editing
   it has an effect? (C22-003)
4. **Should `ROADMAP_GAP.md` be amended or superseded** now that its
   headline gap is closed and returned a negative result worth carrying
   forward the same way QUANTLAB's findings were? (C22-004)
5. Several `ops/go-live.md` and `ops/disaster-recovery.md` claims reference
   files/behaviour in `app/`, `oms/` and build configuration that are outside
   this group's assignment (`G13_oms`, `G15_app_server`, `G01_core_a_build`) —
   worth confirming those groups independently verified 1.3/2.1/4.5/the
   `warm_restart.hpp` refusal semantics, since this group could not.
