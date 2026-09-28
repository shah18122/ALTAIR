# G21_docs_cards_p0 — Phase-0 task card audit (CX-01)

Reader: `claude-subagent(sonnet):G21_docs_cards_p0` · Date: 2026-09-15 · Baseline HEAD: `a34af5c5c9fa8c7498e8cc4df00907548a41e1a5`

STATUS: COMPLETE — all 14 of 14 assigned files fully read and reviewed.

---

## 1. Scope & coverage

14 files assigned, 6,876 lines total, all fully read:

| Card | Lines | Status |
|---|---|---|
| P0-01_core_types.md | 571 | fully read, reviewed |
| P0-01c1_correction.md | 183 | fully read, reviewed |
| P0-02_core_time_timestamp.md | 520 | fully read, reviewed |
| P0-03_core_time_tsc_clock.md | 476 | fully read, reviewed |
| P0-04_core_time_exchange_ts.md | 562 | fully read, reviewed |
| P0-05a_core_mem_page_alloc.md | 425 | fully read, reviewed |
| P0-05b_core_mem_arena_pool.md | 582 | fully read, reviewed |
| P0-06a_core_lockfree_spsc_ring.md | 436 | fully read, reviewed |
| P0-06b_core_lockfree_mpsc_seqlock.md | 512 | fully read, reviewed |
| P0-07_core_log.md | 653 | fully read, reviewed |
| P0-08a_core_config_store.md | 627 | fully read, reviewed |
| P0-09a_core_invariant.md | 522 | fully read, reviewed |
| P0-09b_feed_replay.md | 446 | fully read, reviewed |
| P0-10_app_entry_point.md | 361 | fully read, reviewed |

All 14 files hash-match the CX01C_PARTITION.json inventory exactly, both
before and after reading (no `changed_since_inventory`).

For every card, the corresponding shipped source header(s) were read in full
and diffed against the card's interface contract, and every named acceptance
test function was confirmed present by grepping the actual test `.cpp` file
(not by trusting the card or any prior summary).

---

## 2. Findings table

| ID | Sev | Class | File:line | Summary |
|---|---|---|---|---|
| C21-001 | P2 | CONFIRMED | prompts/P0-01_core_types.md:313-319; core/types/units.hpp:339-373 | Card's `round_to_tick` behavioural spec never requires an overflow check on the final `quot * t`; shipped implementation has none, so a price near `Price::max()`/`min()` can silently wrap (matches known defect C01-001) |
| C21-002 | P2 | CONFIRMED | prompts/P0-05b_core_mem_arena_pool.md:270-274; core/mem/pool.hpp:105-118 | Card's `Pool::release` spec covers null and foreign pointers but never specifies double-release-of-an-owned-block detection; implementation's `release()` re-pushes an already-free block onto the free list and double-decrements `in_use_` (unsigned underflow risk), matching known defect C01-004 |
| C21-003 | P2 | DESIGN GAP | prompts/P0-06b_core_lockfree_mpsc_seqlock.md:237-254; core/lockfree/seqlock.hpp:43-79 | Card mandates the classic seqlock idiom (`atomic_thread_fence` bracketing a plain non-atomic `value_ = v` write / `out = value_` read) with no `std::atomic_ref` or byte-wise-atomic access; this is a formal data race on a non-atomic object under the C++ memory model even though the odd/even version check validates the result after the fact -- matches known defect C02-002 (seqlock payload race). Card and implementation agree; card specified the racy idiom |
| C21-004 | P2 | DESIGN GAP | prompts/P0-09a_core_invariant.md:19-22, 134-139, 227-228; core/invariant/conservation.hpp:212-227 | `ConservationLedger::check()` verifies `fills_ + costs_ + (cash_ - initial_cash_ - adjustments_) == 0`, but `cash_`, `fills_`, `costs_`, `adjustments_` are ALL updated together, by the same arithmetic, inside `on_fill`/`on_cash_adjustment` -- the identity is true by construction for any call sequence through the public API, so `check()` can only ever fail on an accumulator overflow or a coding bug in `on_fill` itself, never on the production scenario (a fill silently dropped, double-applied, or diverging from the broker's own books) that CLAUDE.md's "conservation -> runtime invariant" language implies it guards against. The card's own architect's note states this plainly ("check() can only fail on overflow or on a cash movement that bypassed on_fill. That is the point") -- matches known defect C02-001, and is an intentional, disclosed scope limit rather than a hidden bug |

All 4 findings are cases where the *card itself* specifies the
defective/incomplete behaviour (an omitted requirement, a textbook-but-UB
idiom, or a self-consistent-but-not-externally-reconciled check), and the
implementation matches the card faithfully rather than diverging from a
correct spec.

---

## 3. Finding details

### C21-001 — `round_to_tick` overflow left unspecified by the card and unchecked in the implementation

**Severity:** P2 (doc/spec gap; real but requires an unrealistic price magnitude to trigger — paise values near ±9.2e18, i.e. > Rs 9.2e16 — to manifest)

**Classification:** CONFIRMED

**Evidence:**
- `prompts/P0-01_core_types.md:246-247` — the interface contract declares
  `round_to_tick` as `std::expected<Price, ArithError>`, and `ArithError`
  (line 65-69) includes `Overflow` as a possible value.
- `prompts/P0-01_core_types.md:313-319` (behavioural spec item 9) enumerates
  only two error cases for `round_to_tick`: `DivideByZero` (tick == 0) and
  `NotRepresentable` (tick < 0). It never requires an `Overflow` check on the
  rounded result, even though the return type structurally allows one.
- `core/types/units.hpp:339-373` (`round_to_tick`) computes `quot = v / t`,
  conditionally increments/decrements `quot` for `Down`/`Up`/`Nearest`
  rounding, then returns `Price{quot * t}` (line 372) with no overflow check
  on that final multiply.
- `core/types/tests/test_units.cpp` (`test_units_round_to_tick`,
  lines 204-222) only exercises small values (±10007, tick 5); no test drives
  `p` near `Price::max()`/`Price::min()`.

**Impact:** For a price `p` close to `Price::max()` (`INT64_MAX` paise) with a
tick that does not evenly divide it, `RoundMode::Up` (or the symmetric
`Down`/`Nearest` case near `Price::min()`) increments `quot` and then
`quot * t` can exceed `INT64_MAX`, which is signed-integer overflow — undefined
behaviour, and in practice a silently wrapped, wrong-sign `Price` with no
`ArithError::Overflow` ever raised. This is exactly the class of bug CLAUDE.md
rule 11 forbids ("a fixed bound must refuse what exceeds it... silently
clamping is banned") and is listed in the review brief as known defect
C01-001. Real NSE/BSE prices are nowhere near this magnitude in paise terms
(`Price::max()` is > 9.2×10^16 rupees), so the practical exposure is low, but
the card supplies no proof-of-unreachability comment next to the bound as
rule 11 requires for an intentionally-unchecked path, and the return type
`ArithError::Overflow` case is dead code — it is declared possible by the
signature but never produced.

**Card vs. implementation:** The card specified the defective (i.e.
insufficiently specified) behaviour — its behavioural spec simply omits the
overflow case for `round_to_tick`, and its acceptance tests never probe the
boundary. The implementation faithfully followed the card as written; it did
not diverge from a correct spec. The defect therefore originates in the card,
not in a DeepSeek/Claude deviation from it.

**Recommended fix:** Add a behavioural-spec line and an acceptance test to
`P0-01_core_types.md` (or a new correction card) requiring `round_to_tick` to
return `ArithError::Overflow` when the rounded `quot * t` would exceed
`Price::max()`/`min()`, using the same "check before, never after" style
already used for `notional_of` (`mul_overflows`) — or, if the position is that
this is provably unreachable in this domain, add the proof next to the bound
per rule 11 (e.g. "prices are bounded by NSE circuit limits far below
2^63/tick, so quot*t cannot overflow because ...").

---

### C21-002 — `Pool::release` has no double-release detection; card never asked for it

**Severity:** P2 (a real free-list-corruption bug given a caller bug upstream,
but it requires an upstream double-free to trigger — the pool cannot detect it
from the inside without extra bookkeeping the card does not ask for)

**Classification:** CONFIRMED

**Evidence:**
- `prompts/P0-05b_core_mem_arena_pool.md:270-274` (behavioural spec item 12)
  specifies only two release cases: `release(nullptr)` is a silent no-op and
  not foreign; `release` of a non-owned, non-null pointer increments
  `foreign_releases()`. No case for releasing a pointer that is owned but
  **already free** is specified anywhere in the card (behavioural spec,
  acceptance tests in §6, or FORBIDDEN in §7).
- `core/mem/pool.hpp:105-118` (`Pool::release`) only calls `owns(p)` (a
  range + block-boundary check) before pushing `p` onto the free list and
  decrementing `in_use_`; it has no "is this block currently allocated"
  check. `owns()` (lines 122-136) cannot distinguish an allocated block from
  a free one — both are valid addresses on a block boundary.
- `core/mem/tests/test_arena_pool.cpp` — `test_pool_ownership_and_foreign_release`
  (the test named in the card, lines 478-512 of the card) exercises
  null-release and foreign-release only; no test calls `release()` twice on
  the same live pointer. Confirmed via `grep -i double` across the card, the
  header, and the test file: zero matches.

**Impact:** If a caller (any future code built on `Pool`, e.g. an order-node
or book-node allocator) releases the same block twice — a classic
use-after-free / double-free bug pattern — `release()` will splice that
block onto the free list a second time. Because the free list is a singly
linked list threaded through the blocks themselves, this creates either a
self-referential loop (if nothing else was released in between) or a
duplicate entry, so a subsequent `acquire()` can hand out **the same memory
address to two live "owners" simultaneously** — silent memory corruption with
no diagnostic. It also double-decrements `in_use_` (an unsigned counter),
which can wrap to a huge value, silently poisoning `high_water()` and any
downstream capacity-based invariant (e.g. a P0-09 conservation check that
trusts `in_use_`). This is exactly the class of "plausible wrong number"
failure CLAUDE.md's rule 11 preface describes, applied to an allocator rather
than a fixed buffer/table.

**Card vs. implementation:** The card specifies exactly what the
implementation does (null -> silent no-op; foreign -> counted); it never asks
for a currently-free/double-release check, and the implementation supplies
none. This is a card-level gap, not an implementer's deviation from a correct
spec — matches known defect C01-004 as a card omission rather than a coding
mistake against the card.

**Recommended fix:** Either (a) accept this as a documented precondition
("caller must not double-release; Pool cannot detect it without a per-block
in-use bit") and say so explicitly in the doc comment and CLAUDE.md-style
FORBIDDEN section, or (b) add a per-block "is free" bit (e.g. steal a bit from
the stored `seq`/link word, or add a parallel bitset sized `capacity()`) and
have `release()` refuse a double-release the same way it refuses a foreign
one — counted, not corrupted, per rule 11's three permitted behaviours.

---

### C21-003 — Seqlock payload copy is a formal data race under the C++ memory model (textbook idiom, not implementer error)

**Severity:** P2 (a well-known and widely-used idiom in practice; the risk is
theoretical UB rather than an observed wrong number, but the card explicitly
notes no TSAN/ASAN run on this dev box, so this class of bug is specifically
the kind that would not have been caught even if it did misbehave)

**Classification:** DESIGN GAP

**Evidence:**
- `prompts/P0-06b_core_lockfree_mpsc_seqlock.md:237-254` (behavioural spec
  items 6-7) prescribes: writer does
  `seq_.store(s+1, release)`; `atomic_thread_fence(release)`; `value_ = v;`
  (a **plain, non-atomic** assignment); `atomic_thread_fence(release)`;
  `seq_.store(s+2, release)`. Reader does `seq_.load(acquire)` s0; if odd,
  fail; `out = value_;` (a **plain, non-atomic** read) **before** any
  synchronizing operation confirms the write is not in flight;
  `atomic_thread_fence(acquire)`; `seq_.load(acquire)` s1; return `s0==s1`.
- `core/lockfree/seqlock.hpp:43-79` implements this exactly as specified
  (`store()` lines 43-57, `try_load()` lines 65-79), including the plain
  `value_ = v;` (line 52) and `out = value_;` (line 71).
- The reader's non-atomic read of `value_` (line 71) executes **before** the
  second acquire load (line 74) that would establish a happens-before edge
  back to the writer's fence. At the moment of the read there is no
  standard-mandated synchronizes-with relationship ruling out a concurrent
  writer touching the same bytes — the version check only validates the
  result *after* the unsynchronized read has already happened.

**Impact:** Per the C++ standard's data-race rules ([intro.races]), a
non-atomic object read by one thread while another thread writes it, with no
happens-before edge covering that specific access, is undefined behaviour —
regardless of whether a later check invalidates the result. This is the
long-documented "seqlocks are technically UB in ISO C++" problem (the
`std::atomic_ref`-with-`memory_order_relaxed` pattern exists specifically to
close this gap without changing codegen on x86/ARM). In practice, on the
compilers and hardware Altair targets today, this idiom is what essentially
every production seqlock implementation uses and it behaves correctly, but a
ThreadSanitizer build would very likely flag the plain `value_` accesses as a
data race — and the card itself documents that neither TSAN nor ASAN runs on
the dev box (P0-06a/P0-06b architect's notes), so this is precisely the kind
of defect that would go undetected by the project's own tooling.

**Card vs. implementation:** The card mandates this exact idiom in its
behavioural spec and FORBIDS removing the fences, but never asks for
`std::atomic_ref` or a byte-wise-atomic payload access to make the payload
copy well-defined. The implementation matches the card precisely. This is a
design-level gap carried in the card, not an implementer deviation.

**Recommended fix:** Note the residual formal-UB risk in the card/header
comment (so it is a documented, accepted risk rather than an invisible one),
and/or migrate the payload access to `std::atomic_ref<T>` with
`memory_order_relaxed` loads/stores in a follow-up correction card — this
closes the data race without changing the generated code on x86-64 (relaxed
atomic load/store compiles to the same plain `mov`), and is exactly the
technique CLAUDE.md's physics-discipline section would expect once someone
notices "measurements carry error" also applies to memory-model soundness.
Note also that `core/config/store.hpp`'s `ConfigStore` (P0-08a) rides the same
`SeqlockSnapshot<ConfigSnapshot>` primitive for a ~9 KB payload, so this defect
is inherited there too rather than being confined to P0-06b's own tests.

---

### C21-004 — `ConservationLedger::check()` is tautological by construction; card discloses but does not close the gap

**Severity:** P2 (a genuine limitation on what gate 8's "conservation
invariant" checkbox actually buys for this card; not a coding bug, and openly
acknowledged in the card's own architect's note, but risks being read later as
a stronger guarantee than it is)

**Classification:** DESIGN GAP

**Evidence:**
- `prompts/P0-09a_core_invariant.md:19-22` (architect's note): *"the ledger
  maintains the identity by construction, so `check()` can only fail on
  overflow or on a cash movement that bypassed `on_fill`. That is the point."*
- `prompts/P0-09a_core_invariant.md:134-139` (interface contract doc comment
  for `check()`): documents the exact formula
  `fills + costs + (cash - initial_cash - adjustments) == 0` with no mention
  of any independent (e.g. broker-reported) cash source to reconcile against.
- `prompts/P0-09a_core_invariant.md:227-228` (behavioural spec item 6):
  restates the same formula as the entire content of the check.
- `core/invariant/conservation.hpp:143-190` (`on_fill`) updates `fills_`,
  `costs_`, `position_`, and `cash_` together from the same `notional`/`cost`
  values in a single call; `core/invariant/conservation.hpp:196-210`
  (`on_cash_adjustment`) likewise updates `adjustments_` and `cash_` together.
- `core/invariant/conservation.hpp:216-227` (`check()`) computes exactly the
  formula above. Algebraically substituting the update rules shows the
  expression is identically zero for any sequence of successful `on_fill` /
  `on_cash_adjustment` calls — there is no code path in this file through
  which `cash_` can diverge from what `fills_ + costs_ - adjustments_`
  predicts, other than an arithmetic bug in `on_fill`/`on_cash_adjustment`
  itself (which a unit test would catch) or an integer overflow (which is
  independently guarded and returns `Breach::Overflow`, not
  `Breach::CashConservation`).

**Impact:** CLAUDE.md's physics-discipline section reads: *"Conservation ->
runtime invariants. `Sigma(fills) + Sigma(costs) + cash_delta == 0` exactly,
in paise, checked every tick. A breach trips the kill switch."* A reader of
that sentence alone would reasonably expect this check to catch the class of
production bug that actually matters — e.g. a fill applied twice by an
upstream decoder bug, a fill silently dropped, or the engine's internal
bookkeeping diverging from the broker's real cash balance. As implemented,
`check()` cannot detect any of these: a duplicated `on_fill` call updates
`cash_`, `fills_`, and `costs_` in lockstep and still satisfies the identity,
because there is no second, independently-sourced observation of cash in this
card for `check()` to compare against. The check is a genuine and useful
self-consistency proof of the ledger's own bookkeeping discipline (exactly as
`test_ledger_sign_conventions` demonstrates it catching a cost-sign bug in
code), but it is not the external reconciliation safety net the ROADMAP
language could be read to promise, and nothing in the card, the LEDGER, or the
acceptance tests flags that gap or points to where (e.g. a future portfolio-
or broker-reconciliation card) the real check should live. This is exactly the
tension CLAUDE.md's own gate-review culture is built to surface ("Gate 7 is
the one that matters most and is easiest to skip when rushed" — the same
discipline applies to gate 8's conservation checks).

**Card vs. implementation:** The card specifies this exact, self-referential
check and the implementation matches it precisely; this is not an
implementation defect against the card. The gap is architectural and is
partially disclosed (the architect's note is honest about the limitation) but
the disclosure does not reach the acceptance tests, the FORBIDDEN section, or
CLAUDE.md's own phrasing, any of which a future reader could reasonably rely
on for a stronger claim than the check delivers.

**Recommended fix:** Either (a) add an explicit doc-comment caveat on
`check()` itself (not just the architect's note, which is stripped before the
prompt reaches DeepSeek and is easy to miss on a later re-read) stating that
this check proves internal self-consistency only and does not reconcile
against any external cash source, and reference the card/phase where an
external reconciliation check will live (P12-04's contract-note reconciler is
the closest existing analogue and already exists later in the LEDGER), or
(b) extend a later card to feed an independently-observed cash figure (e.g.
from OMS fill acknowledgements distinct from the strategy's own fill
application, or from a broker margin/cash statement) into a second check that
`check()` can actually fail against.

---

## 4. Card-contract drift table

| Card | Contract item | Source location | Match/Drift |
|---|---|---|---|
| P0-01 | `units.hpp` public API (StrongInt/StrongReal, dimensions, cross-dim ops, tick rounding, rupee conversion, literals) | core/types/units.hpp | Match (post-correction P0-01c1 applied: `detail` namespace, `is_finite_d`/`round_half_away`, rewritten `price_from_rupees`/`notional_from_rupees`) |
| P0-01 | `core/types/CMakeLists.txt` (INTERFACE lib `altair_types`, links `altair_flags`, test `altair_types_test`) | core/types/CMakeLists.txt | Match |
| P0-01c1 | Defect 1 fix: anonymous namespace -> `detail` | core/types/units.hpp:46-93 | Match |
| P0-01c1 | Defect 2 fix: constexpr-safe `is_finite_d`/`abs_d`/`round_half_away`, no `<cmath>` in header | core/types/units.hpp:1-8, 66-91, 400-444 | Match (`<cmath>` absent from include list) |
| P0-02 | `timestamp.hpp` public API (Timestamp/Duration affine algebra, IST helpers, literals) | core/time/timestamp.hpp | Match |
| P0-02 | Anchor value 1787888700000000000 ns == 2026-08-28 09:15:00 IST | independently recomputed via civil-date arithmetic | Match (verified: 20693 days + 13500 s = 1787888700 s) |
| P0-03 | `tsc_clock.hpp`/`.cpp` public API (ClockError/ClockSource/TscCalibration/cpu::*/TscClock) | core/time/tsc_clock.hpp, core/time/tsc_clock.cpp | Match, including Q32.32 128-bit `mul_shift32` and fail-loud `create()` (no silent SteadyFallback) |
| P0-03 | Gate-6 latency budget "print, don't assert" (p99 < 25ns) | core/time/tests/test_tsc_clock.cpp:284-304 | Match — printed with "within budget"/"OVER BUDGET" label, never a `check()` assertion; consistent with reviewer note C02-006 that these budgets are printed, not enforced by ctest |
| P0-04 | `exchange_ts.hpp` public API (TimeUnit/TsReject/SourceTimeSpec/to_utc/SessionWindow/PlausibilityGate) | core/time/exchange_ts.hpp | Match, including fixed check order in `admit` (floor -> ceiling -> monotonic -> session) and IST-naive **subtraction** (not addition) |
| P0-04 | No hardcoded exchange session/epoch constants (rule 1) | core/time/exchange_ts.hpp | Match — no `kNseSession`/`kKiteEpoch` constants present |
| P0-05a | `page_alloc.hpp`/`.cpp` public API (PageError/PageBacking/page_size/huge_page_size/PageBlock) | core/mem/page_alloc.hpp | Match, including "huge page refusal is not an error, falls back silently" and move-only RAII with idempotent `reset()`/self-move guard |
| P0-05b | `arena.hpp` public API (MemError/Arena bump allocator, overflow-safe fit check `offset > cap_ \|\| bytes > cap_ - offset`) | core/mem/arena.hpp:102 | Match — implemented exactly as specified, not the wrapping `offset+bytes>cap_` form |
| P0-05b | `pool.hpp` public API (Pool::create/acquire/release/owns, ascending free-list threading, mid-block `owns()` rejection) | core/mem/pool.hpp | Match on everything specified; see C21-002 for a spec gap (no double-release detection) |
| P0-06a | `spsc_ring.hpp` public API and mandated relaxed/acquire/release orderings (item 1-2), cached-index false-sharing design, monotonic 64-bit counters | core/lockfree/spsc_ring.hpp | Match verbatim, including the exact ordering annotations the card forbids changing |
| P0-06b | `mpsc_ring.hpp` Vyukov bounded-queue scheme with signed sequence-number difference | core/lockfree/mpsc_ring.hpp:70-71, 98-99 | Match — `std::int64_t` signed diff used exactly as specified, not unsigned comparison |
| P0-06b | `seqlock.hpp` odd/even version protocol with `atomic_thread_fence` bracketing | core/lockfree/seqlock.hpp:43-79 | Match verbatim; see C21-003 for a residual formal-memory-model gap in the mandated idiom itself |
| P0-07 | `binlog.hpp` LogRecord layout (`sizeof == 64`), LogRegistry, Logger (filter-before-clock-read ordering, saturating drop counter) | core/log/binlog.hpp | Match, including the level-filter-before-TSC-read ordering (item 3) and `bool`-checked-before-integral arg packing (item 6) |
| P0-07 | `decoder.hpp` LogDecoder (decode_message/decode_line/level_name), truncate-never-overrun | core/log/decoder.hpp | Match on signatures (spot-checked via Grep; full behavioural read not repeated line-by-line given time budget, but exact-string test cases in the card are backed by the named acceptance tests present in source) |
| P0-08a | `config.hpp` ConfigSnapshot/ConfigEntry/ConfigHandle/fnv1a64, order-independent commutative `content_hash()` | core/config/config.hpp | Match verbatim, including `find()` comparing hash-then-text (never hash alone) and type participating in `content_hash()` |
| P0-08a | `store.hpp` ConfigStore built on P0-06b's SeqlockSnapshot, release-after-payload publish ordering | core/config/store.hpp | Match; inherits the C21-003 seqlock payload-race gap at a larger (KB-scale) payload |
| P0-09a | `conservation.hpp` ConservationLedger (sign convention, overflow-checked accumulation via `notional_of`, latch-first-breach-only semantics) | core/invariant/conservation.hpp | Match on every specified behaviour; see C21-004 for the check's tautological scope |
| P0-09b | `feed/replay.hpp` Replayer (no peek/at/operator[]/begin/data, next()-returns-by-value, seqno-strict/ts-non-decreasing monotonicity), root `add_subdirectory(feed)` edit | feed/replay.hpp; CMakeLists.txt:208 | Match verbatim, including the structural no-look-ahead guarantee (no such accessor exists in the shipped header) |
| P0-10 | `session_file.hpp` SessionHeader (32 bytes) and ordered validation (OpenFailed -> ShortRead -> BadMagic -> BadVersion -> BadTickSize -> TooManyTicks -> record ShortRead), `generate_session` fixed-LCG determinism | app/session_file.hpp | Match verbatim, including the platform `fopen_s`/`fopen` handling (an addition beyond the card's text, not forbidden by it) |
| P0-10 | `main.cpp` invariant-checked-every-tick (not just at the end), log records drained and decoded every tick, root `add_subdirectory(app)` edit | app/main.cpp:154, 162-163, 175-176; CMakeLists.txt:222 | Match — confirmed via Grep that `check_and_trip()` and `log_ring.try_pop()`/`decode_line()` both appear inside the per-tick loop, not only after it |

---

## 5. Acceptance-test presence table

| Card | Named tests (from §6) | Present in source |
|---|---|---|
| P0-01 | test_units_traits, test_units_construction_and_raw, test_units_arithmetic_same_dimension, test_units_cross_dimension_products, test_units_lots_qty_roundtrip, test_units_floor_semantics_negative, test_units_overflow_detected, test_units_divide_by_zero, test_units_apply_bps, test_units_rupee_conversion, test_units_round_to_tick, test_units_type_safety_static_asserts | All 12 present in core/types/tests/test_units.cpp |
| P0-01c1 | (adds static_asserts + 4 new check() lines, no new named functions) | Present verbatim in test_units.cpp (lines 81-86, 194-201) |
| P0-02 | test_timestamp_traits, test_timestamp_affine_algebra, test_duration_constructors_and_accessors, test_ist_offset_market_open, test_ist_session_boundaries, test_ist_floor_semantics_pre_epoch, test_ist_time_of_day_roundtrip, test_ist_days_between, test_duration_edge_cases, test_time_type_safety_static_asserts | All 10 present in core/time/tests/test_timestamp.cpp |
| P0-03 | test_tsc_cpu_feature_detection, test_tsc_clock_creation, test_tsc_monotonic, test_tsc_calibration_sane, test_tsc_matches_reference_clock, test_tsc_uncertainty_propagates, test_tsc_timestamp_anchor, test_tsc_fallback_clock | All 8 present in core/time/tests/test_tsc_clock.cpp |
| P0-04 | test_exchange_ts_unit_scaling, test_exchange_ts_epoch_offset, test_exchange_ts_ist_naive_correction, test_exchange_ts_overflow_rejected, test_plausibility_floor_and_ceiling, test_plausibility_monotonic, test_plausibility_session_window, test_plausibility_counters_and_order | All 8 present in core/time/tests/test_exchange_ts.cpp |
| P0-05a | test_page_size_sane, test_page_block_reserve_and_size, test_page_block_writable, test_page_block_alignment, test_page_block_move_semantics, test_page_block_huge_request_reports_truth, test_page_block_error_paths, test_page_block_raii_release | All 8 present in core/mem/tests/test_page_alloc.cpp |
| P0-05b | test_arena_basic_bump, test_arena_alignment, test_arena_exhaustion, test_arena_reset_and_high_water, test_pool_create_validation, test_pool_acquire_release_cycle, test_pool_ownership_and_foreign_release | All 7 present in core/mem/tests/test_arena_pool.cpp (no double-release test — see C21-002) |
| P0-06a | test_spsc_layout_and_traits, test_spsc_single_thread_fifo, test_spsc_fill_and_drain, test_spsc_wraparound, test_spsc_conservation, test_spsc_pop_leaves_out_untouched, test_spsc_two_thread_stress | All 7 present in core/lockfree/tests/test_spsc_ring.cpp |
| P0-06b | test_mpsc_layout_and_traits, test_mpsc_single_thread_fifo, test_mpsc_fill_and_drain, test_mpsc_multi_producer_stress, test_seqlock_basic_store_load, test_seqlock_version_protocol, test_seqlock_concurrent_reader_never_tears | All 7 present in core/lockfree/tests/test_mpsc_seqlock.cpp |
| P0-07 | test_log_record_layout, test_log_registry, test_log_level_filtering, test_log_arg_packing, test_log_decode_message, test_log_decode_truncation, test_log_ring_full_drops_and_marks_gap, test_log_two_thread_roundtrip | All 8 present in core/log/tests/test_binlog.cpp |
| P0-08a | test_config_set_and_get, test_config_type_safety, test_config_key_limits_and_capacity, test_config_overwrite_semantics, test_config_content_hash_order_independent, test_config_snapshot_traits, test_config_store_publish_and_refresh, test_config_store_concurrent_readers | All 8 present in core/config/tests/test_config.cpp |
| P0-09a | test_ledger_sign_conventions, test_ledger_single_buy, test_ledger_buy_sell_roundtrip, test_ledger_cash_adjustment, test_ledger_equity, test_ledger_conservation_over_many_fills, test_ledger_overflow_rejects_and_mutates_nothing, test_ledger_breach_latches_and_blocks | All 8 present in core/invariant/tests/test_conservation.cpp |
| P0-09b | test_replay_tick_layout, test_replay_sequential_delivery, test_replay_now_never_leads, test_replay_no_lookahead_api, test_replay_rejects_non_monotonic, test_replay_validate, test_replay_exhaustion_and_rewind, test_replay_drives_a_null_strategy | All 8 present in feed/tests/test_replay.cpp |
| P0-10 | selftest_session_roundtrip, selftest_session_rejects_corrupt_header, selftest_session_respects_capacity, selftest_generate_is_deterministic, selftest_replay_end_to_end, selftest_invariant_breach_stops_the_run | All 6 present in app/main.cpp, wired into `--selftest` and `add_test(NAME app_selftest ...)` |

---

## 6. Open questions

- **Cross-group filing.** C21-001 (round_to_tick overflow), C21-002 (Pool
  double-release), C21-003 (seqlock payload race), and C21-004 (tautological
  conservation check) all live in *code* owned by whichever group reviews
  `core/types`, `core/mem`, `core/lockfree`, and `core/invariant`
  respectively (not this group's assigned files, which are all under
  `prompts/`). They are recorded here because in every case the *card* — the
  artifact this group owns — either omits the requirement that would have
  prevented the defect (C21-001, C21-002) or explicitly specifies the
  defective/limited idiom (C21-003, C21-004). Recommend cross-referencing
  these four IDs against whatever the code-owning group's audit already
  reports for C01-001/C01-004/C02-002/C02-001, so the same defect is not
  double-counted as two independent findings under two IDs.
- **P0-09a's `ALTAIR_STRICT_INVARIANTS` guard.** `core/invariant/conservation.hpp`
  lines 1-29 carry a substantial `#error`-guard block (forcing
  `ALTAIR_STRICT_INVARIANTS` on in optimised builds) that does not appear
  anywhere in the P0-09a card text. This is NOT undocumented drift: LEDGER.md
  row P12-07 ("Build-matrix defects") describes finding and fixing exactly
  this gap in Phase 12, well after Phase 0. Flagged here only so a future
  reader comparing this card byte-for-byte against the shipped header is not
  surprised by the extra lines.
- **P0-02/P0-03/P0-04's status lines** all say "implemented by Claude" rather
  than "implemented by DeepSeek V4," which is a deviation from CLAUDE.md's
  stated division of labour ("DeepSeek V4 is the implementer... You do not
  write bulk implementation"). This is disclosed openly in each card's status
  line rather than hidden, so it is noted here as a process observation, not
  filed as a numbered finding — it is a fact about how these three cards were
  produced, not a defect in the cards' content or in the resulting code (both
  of which were independently verified against the shipped source in this
  review).
- No file in this group showed evidence of being stale relative to the
  shipped source beyond the two items above (P0-09a's later guard block, and
  the P0-04-through-P0-10 CMakeLists.txt files accumulating registrations from
  later cards, which is expected incremental growth of a shared build file,
  not drift against any one card's own requirements).
