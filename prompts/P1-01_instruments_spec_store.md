# P1-01 — `instruments`: `ContractSpec` and the point-in-time spec store

> Phase 1 · Card 1 of 7 · Status: TODO
> Depends on: P0-01 · P0-02 — DONE
> Paste everything below the line into DeepSeek V4 as a single prompt.
>
> **Architect's note (not part of the prompt).** This card settles three things
> the Phase 0 ledger left open, and each one is a decision, not a detail:
>
> 1. **`price_scale` is a field.** Confirmed against `gokiteconnect`: the wire
>    price divisor is segment-dependent — NSE currency derivatives are 10⁻⁷
>    rupees, five decimals finer than a paisa. CLAUDE.md rule 3 ("all money is
>    integer paise") is true for equity and F&O and **false for CDS**. The spec
>    carries the scale so the decoder can normalise, and the store **refuses**
>    a spec whose scale it cannot represent rather than truncating.
> 2. **Broker tokens live on the spec**, in a small array indexed by feed
>    source, with a bidirectional map in the store. Kite's `instrument_token`
>    and XTS's `ExchangeInstrumentID` are different number spaces; nothing
>    downstream of the decoder may ever see either.
> 3. **`SpecSource` records where a spec came from and whether it is stale.**
>    That is the hook the P1-02 download-failure policy hangs on.
>
> ROADMAP §6.2 sketches `ContractSpec` with `std::string underlying`. **Overridden
> deliberately**: the store is read on the tick path and rule 4 forbids
> allocation there. Symbols are fixed `char` arrays, and the spec stays trivially
> copyable so it can ride a seqlock exactly as `ConfigSnapshot` does.

---

## 1. CONTEXT

You are implementing the instrument master of Altair, a C++23 low-latency
trading engine for Indian equity markets.

CLAUDE.md rule 1, first on the list: **no lot size, tick size, strike step or
expiry appears as a literal anywhere.** ROADMAP §6 says why — NSE has revised
F&O lot sizes repeatedly, and a stale literal scales every position and every
P&L number by a wrong constant while nothing complains. It is the bug that
silently corrupted the predecessor's every number.

So every contract detail is learned daily, reconciled across sources, and kept
**point-in-time**: a backtest of March must see March's lot size, not today's.
A spec store that only knows the present makes every historical result wrong in
a way no test will catch.

This card is the type and the store. Fetching and parsing the sources is
P1-02..P1-05; reconciling them is P1-06. Nothing here touches a network.

---

## 2. FILE MANIFEST

Create exactly these three files, and modify exactly one.

```
CREATE   instruments/contract_spec.hpp
CREATE   instruments/CMakeLists.txt
CREATE   instruments/tests/test_spec_store.cpp
MODIFY   CMakeLists.txt          (root — uncomment add_subdirectory(instruments))
```

In the root `CMakeLists.txt`, activate `add_subdirectory(instruments)`.
**Change nothing else in that file.**

`instruments/CMakeLists.txt` declares `altair_instruments` as an **INTERFACE**
library, aliases it `altair::instruments`, exports
`${CMAKE_CURRENT_SOURCE_DIR}/..` as the include root, links `altair_types`,
`altair_time` and `altair_flags`, and registers `altair_spec_store_test` with
`add_test(NAME spec_store COMMAND altair_spec_store_test)`.

**Do not touch** `vcpkg.json`, `core/`, `feed/`, `app/`, or `config/`.

---

## 3. INTERFACE CONTRACT

```cpp
#pragma once

#include <time/timestamp.hpp>
#include <types/units.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

/// Canonical, internal instrument identity. UNIT: none.
/// NOT a broker token and NOT an exchange token — those are per-source and
/// live in ContractSpec::token. Assigned by the store, dense from 0.
enum class InstrumentId : std::uint32_t { Invalid = 0xFFFF'FFFFu };

/// Which feed a broker token belongs to. UNIT: none.
enum class FeedSource : std::uint8_t { Kite = 0, Xts = 1 };
inline constexpr std::size_t kFeedSourceCount = 2;

/// Which authority a spec's contract detail came from. UNIT: none.
enum class SpecSource : std::uint8_t {
    NseMaster = 0,   // primary for NSE lot size, tick size, expiry
    BseMaster = 1,   // primary for BSE
    KiteDump  = 2,   // cross-check
    XtsMaster = 3,   // cross-check
    Manual    = 4    // a human override; always flagged
};

enum class Exchange  : std::uint8_t { NSE = 0, BSE = 1 };
enum class Segment   : std::uint8_t { Cash = 0, Fut = 1, Opt = 2, Currency = 3, Commodity = 4 };
enum class OptionType: std::uint8_t { None = 0, CE = 1, PE = 2 };

enum class SpecError : std::uint8_t {
    NotFound,        // no such id, token, or symbol
    Full,            // the store is at kMaxInstruments
    DuplicateToken,  // that (source, token) already maps to another instrument
    BadSymbol,       // null, empty, or longer than kMaxSymbolLen
    BadPriceScale,   // a scale this build cannot represent — see item 4
    NotValidAt,      // the instrument exists but not at that instant
    Blocked          // the symbol is blocked for the session
};

inline constexpr std::size_t kMaxInstruments = 8192;
inline constexpr std::size_t kMaxSymbolLen = 31;      // excluding the NUL
inline constexpr std::size_t kMaxUnderlyingLen = 23;  // excluding the NUL

// ─────────────────────────────────────────────────────────────────────────
// ContractSpec — everything about one tradable contract that must never be a
// literal. Trivially copyable and allocation-free: the store is read on the
// tick path, and rule 4 forbids allocation there.
//
// ROADMAP §6.2 sketches this with std::string. Overridden deliberately.
// ─────────────────────────────────────────────────────────────────────────
struct ContractSpec {
    /// Canonical identity. UNIT: none.
    InstrumentId id;

    /// Per-feed broker token, 0 when that feed does not carry this contract.
    /// UNIT: none. Kite's instrument_token and XTS's ExchangeInstrumentID are
    /// DIFFERENT NUMBER SPACES — index by FeedSource, never compare across.
    std::uint32_t token[kFeedSourceCount];

    /// Units per contract. UNIT: units. AUTO-LEARNED — never a literal.
    LotSize lot_size;

    /// Minimum price increment. UNIT: paise. AUTO-LEARNED.
    Price tick_size;

    /// Strike, options only, else zero. UNIT: paise.
    Price strike;

    /// Exchange max single-order quantity. UNIT: units.
    Qty freeze_qty;

    /// Daily price band. UNIT: paise. Zero when the exchange publishes none.
    Price band_lower;
    Price band_upper;

    /// Wire integer units per ONE RUPEE. UNIT: units/rupee.
    /// 100 for equity and F&O, so the wire value IS paise and rule 3 holds.
    /// 10'000'000 for NSE currency derivatives, 10'000 for BSE CD — those are
    /// FINER than a paisa and rule 3 does NOT hold for them. The decoder
    /// normalises; the store refuses what it cannot represent.
    std::int64_t price_scale;

    /// Expiry instant, or Timestamp::epoch() for cash. UNIT: ns since the
    /// Unix epoch, UTC. AUTO-LEARNED.
    Timestamp expiry;

    /// POINT-IN-TIME validity, half-open [valid_from, valid_to).
    /// UNIT: ns since the Unix epoch. valid_to is Timestamp::max() while the
    /// spec is current. A backtest of March MUST see March's lot size.
    Timestamp valid_from;
    Timestamp valid_to;

    /// Hash of the source records this spec was reconciled from. UNIT: none.
    /// Part of rule 10's reproducibility tuple.
    std::uint64_t source_hash;

    /// The snapshot date this came from. UNIT: ns since the Unix epoch.
    /// Compared against the session date to detect a stale fallback.
    Timestamp snapshot_at;

    Exchange exchange;
    Segment segment;
    OptionType opt_type;
    /// Which authority supplied the contract detail.
    SpecSource source;
    /// True when this came from an older snapshot than the session date —
    /// the hook P1-02's download-failure policy hangs on.
    bool stale;
    std::uint8_t reserved[3];

    /// Exchange trading symbol, NUL-terminated. UNIT: none.
    char symbol[kMaxSymbolLen + 1];
    /// Underlying, NUL-terminated. UNIT: none.
    char underlying[kMaxUnderlyingLen + 1];
};

/// Wire units per rupee for a segment, as Kite encodes them.
/// UNIT: units/rupee. Confirmed against gokiteconnect's convertPrice.
[[nodiscard]] constexpr std::int64_t default_price_scale(Segment s) noexcept;

/// True iff `scale` is one this build can represent exactly in Price (paise).
/// UNIT: none. Only 100 qualifies: anything finer loses digits in integer
/// paise, and anything coarser is not a scale we have seen.
[[nodiscard]] constexpr bool price_scale_is_representable(std::int64_t scale) noexcept;

// ─────────────────────────────────────────────────────────────────────────
// SpecStore — the point-in-time instrument master.
//
// Built once pre-open, then READ-ONLY for the session. Not thread-safe to
// mutate; safe to read concurrently once building is finished.
// ─────────────────────────────────────────────────────────────────────────
class SpecStore {
public:
    SpecStore() noexcept = default;

    SpecStore(const SpecStore&) = delete;
    SpecStore& operator=(const SpecStore&) = delete;

    // ── Build side. Pre-open only. ──

    /// Insert a spec and register its tokens. UNIT: none.
    /// Assigns `id` densely from 0 and returns it, ignoring any id the caller
    /// set. Returns Full, BadSymbol, BadPriceScale, or DuplicateToken.
    /// **A rejected add changes nothing.**
    [[nodiscard]] std::expected<InstrumentId, SpecError>
    add(const ContractSpec& s) noexcept;

    /// Block a symbol for the session. UNIT: none. Rule 9: three-way
    /// disagreement blocks the symbol, it does not guess. Idempotent.
    /// PRECONDITION: id was returned by add().
    [[nodiscard]] std::expected<void, SpecError> block(InstrumentId id) noexcept;

    // ── Read side. Hot. ──

    /// Resolve a broker token to canonical identity. UNIT: none.
    /// This is the ONLY legitimate way a decoder turns a wire token into an
    /// instrument. Returns NotFound.
    [[nodiscard]] ALTAIR_HOT std::expected<InstrumentId, SpecError>
    id_of(FeedSource src, std::uint32_t token) const noexcept;

    /// The broker token for a feed, for subscription. UNIT: none.
    /// Returns NotFound when that feed does not carry the instrument.
    [[nodiscard]] std::expected<std::uint32_t, SpecError>
    token_of(InstrumentId id, FeedSource src) const noexcept;

    /// The spec as of `at`. UNIT: none.
    /// Returns NotValidAt when the instrument exists but `at` falls outside
    /// [valid_from, valid_to), and Blocked when the symbol is blocked.
    /// **A backtest MUST call this, never current().**
    [[nodiscard]] ALTAIR_HOT std::expected<const ContractSpec*, SpecError>
    at(InstrumentId id, Timestamp when) const noexcept;

    /// The spec ignoring point-in-time validity. UNIT: none.
    /// For pre-open setup and reporting only. Still honours blocking.
    [[nodiscard]] std::expected<const ContractSpec*, SpecError>
    current(InstrumentId id) const noexcept;

    /// Resolve by exchange trading symbol. UNIT: none. Linear scan — pre-open
    /// only. PRECONDITION: symbol non-null.
    [[nodiscard]] std::expected<InstrumentId, SpecError>
    id_of_symbol(const char* symbol) const noexcept;

    [[nodiscard]] bool is_blocked(InstrumentId id) const noexcept;

    /// Instruments held. UNIT: count.
    [[nodiscard]] std::size_t size() const noexcept;

    /// Blocked instruments. UNIT: count. Non-zero is a session health signal.
    [[nodiscard]] std::size_t blocked_count() const noexcept;

    /// Instruments whose spec came from an older snapshot. UNIT: count.
    /// Non-zero means a source download failed and a fallback was used.
    [[nodiscard]] std::size_t stale_count() const noexcept;

    /// Hash over every spec's source_hash, ORDER-INDEPENDENT. UNIT: none.
    /// This is the `spec_version` in rule 10's reproducibility tuple.
    [[nodiscard]] std::uint64_t spec_version() const noexcept;

private:
    ContractSpec specs_[kMaxInstruments]{};
    bool blocked_[kMaxInstruments]{};
    std::uint32_t count_ = 0;
};

} // namespace altair
```

---

## 4. BEHAVIOURAL SPEC

1. `default_price_scale` returns `10'000'000` for `Segment::Currency` on NSE
   semantics, and `100` for everything else. It is a **default**, not a
   promise — a real spec carries whatever its source reported.
2. `price_scale_is_representable(scale)` is `scale == 100` and nothing else.
   Anything finer loses digits when stored in integer paise; anything coarser
   has not been observed. It is deliberately strict.
3. `add` validates in this order: `BadSymbol` (null, empty, or over
   `kMaxSymbolLen`), `BadPriceScale`, `Full`, then `DuplicateToken` for every
   non-zero token. **On any rejection nothing is mutated** — not `count_`, not
   the token maps.
4. `BadPriceScale` is the currency-derivative guard. A spec with
   `price_scale == 10'000'000` is **refused**, loudly, rather than admitted and
   silently truncated to paise. CLAUDE.md rule 9: failing loud beats trading
   wrong. When CDS support is wanted, that is a deliberate change here, not an
   accident at the decoder.
5. `add` assigns `id` densely from 0 and **overwrites** whatever the caller put
   in `s.id`. Identity is the store's to give.
6. A token of `0` means "this feed does not carry this contract" and is never
   registered. Two specs may both have token 0 for a feed; neither is a
   duplicate.
7. `id_of` and `token_of` are exact inverses for every registered non-zero
   token, in both directions.
8. `at(id, when)` returns the spec iff
   `valid_from <= when && when < valid_to` — **half-open**, so two consecutive
   specs never both match an instant. Otherwise `NotValidAt`.
9. `at` and `current` return `Blocked` for a blocked instrument, **before** any
   validity check. A blocked symbol is unavailable regardless of the date.
10. `block` is idempotent and only ever sets. There is deliberately no
    `unblock`: a session that blocked a symbol for a disagreement does not get
    to change its mind halfway through.
11. `spec_version()` is an **order-independent** combination of every spec's
    `source_hash` — same reasoning as `ConfigSnapshot::content_hash` in P0-08a.
    Two stores built from the same specs in a different order must agree, or
    rule 10's reproducibility claim is false.
12. `ContractSpec` is trivially copyable and the whole store allocates nothing.

---

## 5. CONSTRAINTS

- C++23. Standard library plus P0-01 and P0-02 headers. **No new dependency.**
- Header-only, one file, and it must stay under ~400 lines.
- **No `std::string`, `std::vector`, `std::map`, or any allocation.** ROADMAP
  §6.2's `std::string underlying` is overridden; say so in a comment.
- No file I/O, no network, no `<chrono>`. Parsing is P1-02..P1-05.
- No exceptions, no `throw`, no `iostream`.
- Internal helpers in `namespace altair::detail`, never an anonymous namespace.
  Name them distinctly from existing members (`mul_overflows`,
  `add_overflows_i64`, `is_pow2_size`, `round_up_pow2`, `pack_log_arg`,
  `config_mix64`, `config_key_len`, `ledger_add_overflows`, `session_fopen`).
- No `using namespace` at file scope.
- Compiles clean at `/W4` and `-Wall -Wextra -Wpedantic -Wconversion
  -Wsign-conversion -Wold-style-cast`.

---

## 6. ACCEPTANCE TESTS

`instruments/tests/test_spec_store.cpp`, plain `main()`,
`check(bool, const char*)`, returns 0 only if all pass. Exactly these names:

```cpp
void test_spec_traits_and_scale();
void test_store_add_and_identity();
void test_store_token_map_is_bidirectional();
void test_store_rejects_bad_input();
void test_store_point_in_time();
void test_store_blocking();
void test_store_spec_version_order_independent();
void test_store_capacity();
```

Anchor on the P0-02 instant, `Timestamp{1787888700000000000LL}` (2026-08-28
09:15 IST). Build specs with a helper so each test reads clearly.

**test_spec_traits_and_scale**
```
std::is_trivially_copyable_v<ContractSpec>
default_price_scale(Segment::Fut) == 100
default_price_scale(Segment::Cash) == 100
default_price_scale(Segment::Currency) == 10'000'000
price_scale_is_representable(100) == true
price_scale_is_representable(10'000'000) == false     // finer than a paisa
price_scale_is_representable(10'000) == false
kFeedSourceCount == 2
// Print sizeof(ContractSpec) and sizeof(SpecStore); assert neither is zero.
```

**test_store_add_and_identity**
```
SpecStore st;
st.size() == 0
auto a = st.add(nifty_fut_spec());     // token[Kite]=256265, token[Xts]=35001
a.value() == InstrumentId{0}            // dense from 0, caller's id ignored
auto b = st.add(banknifty_fut_spec());
b.value() == InstrumentId{1}
st.size() == 2

st.current(a.value()).value()->lot_size == LotSize{75}
st.current(a.value()).value()->id == a.value()       // the STORE set it
st.current(InstrumentId{99}).error() == SpecError::NotFound
```

**test_store_token_map_is_bidirectional**
The whole reason this card exists.
```
st.id_of(FeedSource::Kite, 256265).value() == a.value()
st.id_of(FeedSource::Xts,  35001).value()  == a.value()
st.token_of(a.value(), FeedSource::Kite).value() == 256265
st.token_of(a.value(), FeedSource::Xts).value()  == 35001

// The SAME NUMBER in two feeds is two different instruments.
// Kite token 35001 must NOT resolve to the XTS instrument.
st.id_of(FeedSource::Kite, 35001).error() == SpecError::NotFound

st.id_of(FeedSource::Kite, 999999).error() == SpecError::NotFound

// A feed that does not carry the contract: token 0, and token_of says so.
auto c = st.add(kite_only_spec());      // token[Xts] = 0
st.token_of(c.value(), FeedSource::Xts).error() == SpecError::NotFound
// and 0 was never registered as a token
st.id_of(FeedSource::Xts, 0).error() == SpecError::NotFound
```

**test_store_rejects_bad_input**
```
SpecStore st;
st.add(spec_with_symbol(nullptr)).error() == SpecError::BadSymbol
st.add(spec_with_symbol("")).error()      == SpecError::BadSymbol
st.add(spec_with_symbol(<32 chars>)).error() == SpecError::BadSymbol   // over the limit
st.add(spec_with_symbol(<31 chars>)).has_value()                        // exactly at it

// THE CURRENCY-DERIVATIVE GUARD.
auto cds = usdinr_spec();  cds.price_scale = 10'000'000;
st.add(cds).error() == SpecError::BadPriceScale
// Refused, not truncated. Rule 9.

// A duplicate token is refused and changes nothing.
st.add(nifty_fut_spec()).has_value();
const auto before = st.size();
st.add(nifty_fut_spec()).error() == SpecError::DuplicateToken   // same tokens
st.size() == before
// and the first instrument still resolves
st.id_of(FeedSource::Kite, 256265).has_value()
```

**test_store_point_in_time**
The one that protects every backtest.
```
// Two specs for the same symbol, different lot sizes, adjacent windows.
// old: valid [kOpen - 60d, kOpen - 30d), lot 50
// new: valid [kOpen - 30d, Timestamp::max()), lot 75
// (distinct tokens so both can be added)

st.at(old_id, kOpen - duration::days(45)).value()->lot_size == LotSize{50}
st.at(new_id, kOpen).value()->lot_size == LotSize{75}

// Half-open: the boundary belongs to the NEW window only.
st.at(old_id, kOpen - duration::days(30)).error() == SpecError::NotValidAt
st.at(new_id, kOpen - duration::days(30)).has_value()

// Before either window exists.
st.at(old_id, kOpen - duration::days(90)).error() == SpecError::NotValidAt

// current() ignores validity; at() does not. A backtest MUST use at().
st.current(old_id).has_value()
```

**test_store_blocking**
```
st.block(a.value()).has_value()
st.is_blocked(a.value())
st.blocked_count() == 1

// Blocked wins over everything, at any date.
st.at(a.value(), kOpen).error() == SpecError::Blocked
st.current(a.value()).error() == SpecError::Blocked

// Idempotent, and the count does not double.
st.block(a.value()).has_value()
st.blocked_count() == 1

// Other instruments are unaffected — rule 9 blocks the symbol, not the engine.
st.current(b.value()).has_value()

st.block(InstrumentId{999}).error() == SpecError::NotFound
```

**test_store_spec_version_order_independent**
```
Two stores, the same three specs added in DIFFERENT order (adjust tokens so
both stores accept all three).
  st1.spec_version() == st2.spec_version()

// Any change to a source_hash moves it.
st3 with one spec's source_hash altered
  st3.spec_version() != st1.spec_version()

// An empty store has a stable version, different from a populated one.
SpecStore e;  e.spec_version() == SpecStore{}.spec_version()
e.spec_version() != st1.spec_version()
```

**test_store_capacity**
```
Fill to kMaxInstruments with distinct symbols and tokens.
  every add succeeded; st.size() == kMaxInstruments
  the next add is SpecError::Full
  st.size() is unchanged
  the first instrument still resolves by token
```

### Latency reporting — not a pass/fail assertion

Batch-timed: `id_of` (the decoder's per-tick lookup) and `at`.
**Budget: < 20 ns each** — both sit inside ROADMAP §11's 1 µs
"wire decode → normalised tick". Print; do not assert.

**Measure `id_of` at its WORST case — the last token inserted** — and with at
least 2'000 instruments in the store. A lookup benchmarked on the first token
tells you nothing.

**`find_by_token` must be O(1), not a scan.** A linear scan was tried and
measured **2624 ns at only 2'000 instruments** — 131x over budget, and it runs
per tick, so on its own it blew the whole 1 µs decode allowance by 2.6x. At the
real universe size (20 strikes each side, 5 indices, CE and PE, several
expiries) it would have been ~10 µs per tick.

The fix is a fixed open-addressed index per feed source: power-of-two capacity
at `2 * kMaxInstruments` so the load factor stays at 0.5, linear probing, and no
deletion so no tombstones are needed. An empty slot is token `0`, which is
already reserved as "this feed does not carry the contract" and is never
inserted. That took it to **1.26 ns**. The store grows to ~1.6 MB, which is the
right trade.

---

## 7. FORBIDDEN

- Adding a file not in the manifest. Editing the root `CMakeLists.txt` beyond
  the single `add_subdirectory(instruments)` line. Touching `core/`, `feed/`,
  `app/`, `config/`, or `vcpkg.json`.
- Changing any signature in the interface contract.
- `std::string`, `std::vector`, `std::map`, or any allocation.
- **Accepting a `price_scale` other than 100.** It is refused, not truncated;
  item 4.
- Letting a caller's `s.id` survive `add`. Identity is the store's.
- Registering token `0` in the token map.
- Comparing a Kite token against an XTS token, or sharing one map between feeds.
- A closed validity interval. It is half-open, or two specs match one instant.
- Checking validity before blocking. Blocked wins.
- An `unblock`. A session does not get to change its mind.
- An order-dependent `spec_version()`.
- Any file I/O or network — that is P1-02..P1-05.
- Writing a test that asserts whatever your implementation happens to produce.

---

## 8. RULES

```
RULES — violating any of these fails review:
1. Produce complete files. No "...", no "rest unchanged", no placeholder bodies.
2. Do not create, rename, or delete any file not in the File Manifest.
3. Do not change any signature in the Interface Contract. If you believe a
   signature is wrong, implement it as specified AND add a comment block at the
   top of the file titled "CONTRACT OBJECTION" explaining why. Do not act on it.
4. Do not add any third-party dependency. Only what vcpkg.json already lists.
5. No exceptions on the hot path. Return std::expected<T, Error>.
6. No dynamic allocation inside any function marked ALTAIR_HOT.
7. No `using namespace` at file scope in a header.
8. Every public function gets a doc comment stating units and preconditions.
9. Write the acceptance tests exactly as named. Do not rename or merge them.
10. If a requirement is ambiguous, implement the most conservative reading and
    list the ambiguity under "ASSUMPTIONS" at the end of your response.
```

Return the four files in full, then your ASSUMPTIONS section, stating
`sizeof(ContractSpec)`, `sizeof(SpecStore)`, and the measured ns for `id_of`.
