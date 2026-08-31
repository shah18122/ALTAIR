// feed/tick.hpp — the normalised Tick and DepthUpdate.
//
// P2-01. This is the type every later phase reads: the book, the analytics,
// the features, the strategies and the backtester all consume these two
// structs and nothing else. Rule 6 depends on it — the replay feed and the
// live feed emit the SAME struct into the SAME pipeline, and if they ever
// diverge the backtest is a lie.
//
// It also closes the P0-09b carried debt. `ReplayTick` carries a bare
// `uint32 token`, which means different things depending on which feed
// produced it; a normalised Tick carries a canonical InstrumentId and no
// broker token at all. See D1 below.

#pragma once

#include <core/time/timestamp.hpp>
#include <core/types/units.hpp>
#include <instruments/contract_spec.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>

namespace altair {

/// Bumped whenever the layout of Tick or DepthUpdate changes.
///
/// The tick store (P2-06) and the replayer both persist these bytes. A silent
/// layout change makes every previously recorded session unreadable in a way
/// that looks like file corruption rather than like a version skew.
inline constexpr std::uint16_t kTickWireVersion = 1;

/// Kite's "full" mode publishes five levels per side. Fixed, so the struct is
/// trivially copyable and allocates nothing.
inline constexpr std::size_t kDepthLevels = 5;

enum class TickFlag : std::uint16_t {
    None        = 0,
    Snapshot    = 1u << 0,   // a full image, not a delta
    Stale       = 1u << 1,   // exchange_ts failed P0-04's plausibility gate
    Synthetic   = 1u << 2,   // produced by replay; a live feed never sets this
    ClosingAuct = 1u << 3    // inside the closing auction window
};

[[nodiscard]] constexpr bool has_flag(std::uint16_t flags, TickFlag f) noexcept {
    return (flags & static_cast<std::uint16_t>(f)) != 0;
}

[[nodiscard]] constexpr std::uint16_t set_flag(std::uint16_t flags,
                                               TickFlag f) noexcept {
    return static_cast<std::uint16_t>(flags | static_cast<std::uint16_t>(f));
}

// ─────────────────────────────────────────────────────────────────────────
// Tick — one trade / price update.
// ─────────────────────────────────────────────────────────────────────────
struct Tick {
    /// D1: CANONICAL IDENTITY. Never a broker token.
    ///
    /// Kite's instrument_token and XTS's ExchangeInstrumentID are different
    /// number spaces for the same contract. P2-05 switches the primary feed
    /// mid-session: with a broker token here, every instrument would appear to
    /// vanish and a stranger appear in its place, and the book, the ledger and
    /// every strategy would follow it. Decoders map (source, broker_token) to
    /// this through SpecStore::id_of, and nothing downstream sees a token.
    InstrumentId  id;

    /// D5: OUR sequence, assigned at normalisation. Not the exchange's — an
    /// exchange sequence is per-feed and does not survive a failover.
    /// Part of rule 10's reproducibility tuple.
    std::uint32_t seq;

    /// D2: when the exchange says it happened. UNIT: ns since the Unix epoch.
    /// THIS is what a strategy reads (rule 7). Not monotonic: exchange clocks
    /// step, which is why P0-04's PlausibilityGate exists.
    Timestamp     exchange_ts;

    /// D2: when we received it, off the TSC clock. UNIT: ns since the epoch.
    /// Locally monotonic. For latency measurement and stall detection (P2-05),
    /// never for trading decisions.
    Timestamp     recv_ts;

    /// UNIT: paise. D4 — always paise. The decoder applies
    /// ContractSpec::price_scale and REFUSES what it cannot represent rather
    /// than truncating. NSE currency derivatives are finer than a paisa.
    Price         last;
    Qty           last_qty;
    /// Cumulative for the session, not per-trade. UNIT: units.
    Qty           volume;
    /// Open interest. UNIT: units. Zero for cash.
    std::int64_t  oi;

    /// Which feed produced this. A DIAGNOSTIC, not identity — see D1.
    FeedSource    source;
    std::uint8_t  reserved;
    std::uint16_t flags;         // TickFlag bitset
    std::uint32_t reserved2;
};

struct DepthLevel {
    Price         px;
    Qty           qty;
    std::uint32_t orders;        // order count resting at this level
    std::uint32_t reserved;
};

// ─────────────────────────────────────────────────────────────────────────
// DepthUpdate — one book image.
//
// D3: separate from Tick, because Kite delivers price and depth in one packet
// while XTS delivers touchline (1501) and depth (1502) separately. One struct
// would mean SYNTHESISING depth on a touchline-only update, and a synthesised
// book level is a fabricated measurement.
// ─────────────────────────────────────────────────────────────────────────
struct DepthUpdate {
    InstrumentId  id;
    std::uint32_t seq;
    Timestamp     exchange_ts;
    Timestamp     recv_ts;
    DepthLevel    bid[kDepthLevels];
    DepthLevel    ask[kDepthLevels];
    FeedSource    source;

    /// D6: how many levels are REAL. Five slots do not mean five levels of
    /// liquidity, and an unpopulated level is zeroed — a strategy reading a
    /// zeroed bid as real would see a price of 0 and infinite edge.
    std::uint8_t  bid_levels;
    std::uint8_t  ask_levels;
    std::uint8_t  reserved;
    std::uint16_t flags;
    std::uint16_t reserved2;
};

// ── D7: the layout is pinned ─────────────────────────────────────────────
// These structs ride an SPSC ring and a seqlock, and are memcpy'd into the
// tick store. A field inserted in the middle must break the BUILD, not a file
// format that replay silently depends on.
static_assert(std::is_trivially_copyable_v<Tick>);
static_assert(std::is_trivially_copyable_v<DepthLevel>);
static_assert(std::is_trivially_copyable_v<DepthUpdate>);

static_assert(sizeof(DepthLevel) == 24);
static_assert(sizeof(Tick) == 64, "Tick must stay one cache line");
static_assert(sizeof(DepthUpdate) == 272);

static_assert(offsetof(Tick, id) == 0);
static_assert(offsetof(Tick, seq) == 4);
static_assert(offsetof(Tick, exchange_ts) == 8);
static_assert(offsetof(Tick, recv_ts) == 16);
static_assert(offsetof(Tick, last) == 24);
static_assert(offsetof(Tick, last_qty) == 32);
static_assert(offsetof(Tick, volume) == 40);
static_assert(offsetof(Tick, oi) == 48);
static_assert(offsetof(Tick, source) == 56);

static_assert(offsetof(DepthUpdate, bid) == 24);
static_assert(offsetof(DepthUpdate, ask) == 144);
static_assert(offsetof(DepthUpdate, source) == 264);

// ── accessors ────────────────────────────────────────────────────────────

/// Best bid, or nullptr when the bid side is empty.
///
/// Reading `bid[0]` directly is the bug this exists to prevent: an unpopulated
/// level is zeroed, and a bid at price 0 looks like free money.
[[nodiscard]] constexpr const DepthLevel* best_bid(const DepthUpdate& d) noexcept {
    return d.bid_levels > 0 ? &d.bid[0] : nullptr;
}

[[nodiscard]] constexpr const DepthLevel* best_ask(const DepthUpdate& d) noexcept {
    return d.ask_levels > 0 ? &d.ask[0] : nullptr;
}

/// UNIT: paise. Empty unless BOTH sides have a level.
///
/// A one-sided book has no mid. Returning the side that exists is how a spread
/// of zero gets into a signal and an arbitrage appears out of nothing.
[[nodiscard]] constexpr std::optional<Price> mid(const DepthUpdate& d) noexcept {
    const DepthLevel* b = best_bid(d);
    const DepthLevel* a = best_ask(d);
    if (b == nullptr || a == nullptr) {
        return std::nullopt;
    }
    // b + (a - b) / 2, NOT (a + b) / 2. Two int64 paise near the top of the
    // range sum to an overflow, which is UB and not a large number.
    const std::int64_t lo = b->px.raw();
    const std::int64_t hi = a->px.raw();
    return Price{lo + (hi - lo) / 2};
}

/// UNIT: paise. Empty unless both sides have a level. Negative on a crossed
/// book, which is a real state and must not be clamped to zero.
[[nodiscard]] constexpr std::optional<Price> spread(const DepthUpdate& d) noexcept {
    const DepthLevel* b = best_bid(d);
    const DepthLevel* a = best_ask(d);
    if (b == nullptr || a == nullptr) {
        return std::nullopt;
    }
    return Price{a->px.raw() - b->px.raw()};
}

/// True when the book is crossed or LOCKED (bid >= ask).
///
/// Locked counts: a book where bid equals ask is not tradable either, and
/// treating it as a zero-spread opportunity is exactly the false arbitrage
/// Phase 5 exists to disprove. Empty on either side is not crossed — it is
/// empty, which is a different thing.
[[nodiscard]] constexpr bool is_crossed(const DepthUpdate& d) noexcept {
    const DepthLevel* b = best_bid(d);
    const DepthLevel* a = best_ask(d);
    if (b == nullptr || a == nullptr) {
        return false;
    }
    return b->px.raw() >= a->px.raw();
}

} // namespace altair
