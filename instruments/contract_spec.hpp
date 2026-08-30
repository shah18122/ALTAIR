#pragma once

// P1-01 — ContractSpec and the point-in-time spec store.
//
// CLAUDE.md rule 1, first on the list: no lot size, tick size, strike step or
// expiry appears as a literal anywhere. ROADMAP §6 says why — NSE has revised
// F&O lot sizes repeatedly, and a stale literal scales every position and every
// P&L number by a wrong constant while nothing complains.
//
// So every contract detail is learned daily and kept POINT-IN-TIME: a backtest
// of March must see March's lot size, not today's. A store that only knows the
// present makes every historical result wrong in a way no test catches.
//
// ROADMAP §6.2 sketches ContractSpec with `std::string underlying`. OVERRIDDEN
// DELIBERATELY: this store is read on the tick path and rule 4 forbids
// allocation there. Symbols are fixed char arrays and the spec stays trivially
// copyable, so it can ride a seqlock exactly as ConfigSnapshot does.
//
// Nothing here touches a network. Fetching and parsing is P1-02..P1-05;
// reconciling the sources is P1-06.

#include <time/timestamp.hpp>
#include <types/units.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
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
// literal. Trivially copyable and allocation-free.
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

static_assert(std::is_trivially_copyable_v<ContractSpec>,
              "ContractSpec must ride a seqlock unchanged");

/// Wire units per rupee for a segment, as Kite encodes them.
/// UNIT: units/rupee. Confirmed against gokiteconnect's convertPrice.
[[nodiscard]] constexpr std::int64_t default_price_scale(Segment s) noexcept {
    return s == Segment::Currency ? 10'000'000LL : 100LL;
}

/// True iff `scale` is one this build can represent exactly in Price (paise).
/// UNIT: none. Only 100 qualifies: anything finer loses digits in integer
/// paise, and anything coarser has not been observed. Deliberately strict.
[[nodiscard]] constexpr bool price_scale_is_representable(std::int64_t scale) noexcept {
    return scale == 100LL;
}

namespace detail {

/// splitmix64 finaliser, so a commutative sum still moves on any single-bit
/// change. UNIT: none. PRECONDITION: none.
[[nodiscard]] constexpr std::uint64_t spec_mix64(std::uint64_t x) noexcept {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

/// Length of a NUL-terminated string, capped so a runaway pointer cannot spin.
/// UNIT: bytes. PRECONDITION: s non-null.
[[nodiscard]] constexpr std::size_t spec_str_len(const char* s, std::size_t cap) noexcept {
    std::size_t n = 0;
    while (n <= cap && s[n] != '\0') {
        ++n;
    }
    return n;
}

} // namespace detail

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

    /// Insert a spec and register its tokens. UNIT: none.
    /// Assigns `id` densely from 0 and returns it, ignoring any id the caller
    /// set. Returns Full, BadSymbol, BadPriceScale, or DuplicateToken.
    /// A rejected add changes nothing.
    [[nodiscard]] std::expected<InstrumentId, SpecError>
    add(const ContractSpec& s) noexcept {
        // Validate in a fixed order, and mutate nothing until every check passes.
        if (s.symbol[0] == '\0') {
            return std::unexpected(SpecError::BadSymbol);
        }
        if (detail::spec_str_len(s.symbol, kMaxSymbolLen) > kMaxSymbolLen) {
            return std::unexpected(SpecError::BadSymbol);
        }
        // The currency-derivative guard. A scale finer than a paisa is REFUSED,
        // not truncated. Rule 9: failing loud beats trading wrong.
        if (!price_scale_is_representable(s.price_scale)) {
            return std::unexpected(SpecError::BadPriceScale);
        }
        if (count_ >= kMaxInstruments) {
            return std::unexpected(SpecError::Full);
        }
        for (std::size_t f = 0; f < kFeedSourceCount; ++f) {
            // Token 0 means "this feed does not carry the contract" and is
            // never registered, so many specs may share it.
            if (s.token[f] != 0 &&
                find_by_token(static_cast<FeedSource>(f), s.token[f]) != kNoIndex) {
                return std::unexpected(SpecError::DuplicateToken);
            }
        }

        const std::uint32_t idx = count_;
        specs_[idx] = s;
        specs_[idx].id = static_cast<InstrumentId>(idx);   // identity is OURS
        blocked_[idx] = false;
        for (std::size_t f = 0; f < kFeedSourceCount; ++f) {
            token_insert(static_cast<FeedSource>(f), s.token[f], idx);
        }
        ++count_;
        return static_cast<InstrumentId>(idx);
    }

    /// Block a symbol for the session. UNIT: none. Rule 9: three-way
    /// disagreement blocks the symbol, it does not guess. Idempotent.
    /// PRECONDITION: id was returned by add().
    [[nodiscard]] std::expected<void, SpecError> block(InstrumentId id) noexcept {
        const std::uint32_t i = index_of(id);
        if (i == kNoIndex) {
            return std::unexpected(SpecError::NotFound);
        }
        blocked_[i] = true;   // only ever sets; there is deliberately no unblock
        return {};
    }

    /// Resolve a broker token to canonical identity. UNIT: none.
    /// This is the ONLY legitimate way a decoder turns a wire token into an
    /// instrument. Returns NotFound.
    [[nodiscard]] ALTAIR_HOT std::expected<InstrumentId, SpecError>
    id_of(FeedSource src, std::uint32_t token) const noexcept {
        const std::uint32_t i = find_by_token(src, token);
        if (i == kNoIndex) {
            return std::unexpected(SpecError::NotFound);
        }
        return static_cast<InstrumentId>(i);
    }

    /// The broker token for a feed, for subscription. UNIT: none.
    /// Returns NotFound when that feed does not carry the instrument.
    [[nodiscard]] std::expected<std::uint32_t, SpecError>
    token_of(InstrumentId id, FeedSource src) const noexcept {
        const std::uint32_t i = index_of(id);
        if (i == kNoIndex) {
            return std::unexpected(SpecError::NotFound);
        }
        const std::uint32_t t = specs_[i].token[static_cast<std::size_t>(src)];
        if (t == 0) {
            return std::unexpected(SpecError::NotFound);
        }
        return t;
    }

    /// The spec as of `when`. UNIT: none.
    /// Returns NotValidAt when the instrument exists but `when` falls outside
    /// [valid_from, valid_to), and Blocked when the symbol is blocked.
    /// A backtest MUST call this, never current().
    [[nodiscard]] ALTAIR_HOT std::expected<const ContractSpec*, SpecError>
    at(InstrumentId id, Timestamp when) const noexcept {
        const std::uint32_t i = index_of(id);
        if (i == kNoIndex) {
            return std::unexpected(SpecError::NotFound);
        }
        // Blocked wins over everything, at any date.
        if (blocked_[i]) {
            return std::unexpected(SpecError::Blocked);
        }
        // HALF-OPEN, so two consecutive specs never both match an instant.
        if (when < specs_[i].valid_from || !(when < specs_[i].valid_to)) {
            return std::unexpected(SpecError::NotValidAt);
        }
        return &specs_[i];
    }

    /// The spec ignoring point-in-time validity. UNIT: none.
    /// For pre-open setup and reporting only. Still honours blocking.
    [[nodiscard]] std::expected<const ContractSpec*, SpecError>
    current(InstrumentId id) const noexcept {
        const std::uint32_t i = index_of(id);
        if (i == kNoIndex) {
            return std::unexpected(SpecError::NotFound);
        }
        if (blocked_[i]) {
            return std::unexpected(SpecError::Blocked);
        }
        return &specs_[i];
    }

    /// Resolve by exchange trading symbol. UNIT: none. Linear scan — pre-open
    /// only. PRECONDITION: symbol non-null.
    [[nodiscard]] std::expected<InstrumentId, SpecError>
    id_of_symbol(const char* symbol) const noexcept {
        if (symbol == nullptr || symbol[0] == '\0') {
            return std::unexpected(SpecError::NotFound);
        }
        for (std::uint32_t i = 0; i < count_; ++i) {
            if (std::strcmp(specs_[i].symbol, symbol) == 0) {
                return static_cast<InstrumentId>(i);
            }
        }
        return std::unexpected(SpecError::NotFound);
    }

    [[nodiscard]] bool is_blocked(InstrumentId id) const noexcept {
        const std::uint32_t i = index_of(id);
        return i != kNoIndex && blocked_[i];
    }

    /// Instruments held. UNIT: count.
    [[nodiscard]] std::size_t size() const noexcept {
        return static_cast<std::size_t>(count_);
    }

    /// Blocked instruments. UNIT: count. Non-zero is a session health signal.
    [[nodiscard]] std::size_t blocked_count() const noexcept {
        std::size_t n = 0;
        for (std::uint32_t i = 0; i < count_; ++i) {
            if (blocked_[i]) { ++n; }
        }
        return n;
    }

    /// Instruments whose spec came from an older snapshot. UNIT: count.
    /// Non-zero means a source download failed and a fallback was used.
    [[nodiscard]] std::size_t stale_count() const noexcept {
        std::size_t n = 0;
        for (std::uint32_t i = 0; i < count_; ++i) {
            if (specs_[i].stale) { ++n; }
        }
        return n;
    }

    /// Hash over every spec's source_hash, ORDER-INDEPENDENT. UNIT: none.
    /// This is the `spec_version` in rule 10's reproducibility tuple. Order
    /// dependence here would make the same masters, loaded twice, produce two
    /// versions — and the reproducibility claim would be false.
    [[nodiscard]] std::uint64_t spec_version() const noexcept {
        std::uint64_t acc = 0;
        for (std::uint32_t i = 0; i < count_; ++i) {
            acc += detail::spec_mix64(specs_[i].source_hash);   // commutative
        }
        return acc;
    }

private:
    static constexpr std::uint32_t kNoIndex = 0xFFFF'FFFFu;

    /// Open-addressed token index, one per feed. Power of two, load factor
    /// 0.5 at full occupancy, linear probing, and no deletion — so no
    /// tombstones. An empty slot is token 0, which is already reserved to mean
    /// "this feed does not carry the contract" and is never inserted.
    ///
    /// This replaced a linear scan. The scan measured **2624 ns** at only 2000
    /// instruments against a 20 ns budget — 131x over, and it runs per tick in
    /// the decoder, so on its own it blew ROADMAP §11's whole 1 us
    /// "wire decode -> normalised tick" allowance by 2.6x. At the real universe
    /// size it would have been ~10 us. Gate 6 exists to catch exactly this.
    static constexpr std::size_t kTokenIndexCap = 16384;   // 2 * kMaxInstruments
    static_assert((kTokenIndexCap & (kTokenIndexCap - 1)) == 0,
                  "token index capacity must be a power of two");
    static_assert(kTokenIndexCap >= 2 * kMaxInstruments,
                  "keep the load factor at or below 0.5");

    struct TokenSlot {
        std::uint32_t token;   // 0 == empty
        std::uint32_t idx;
    };

    /// Fibonacci-style mix, then mask. UNIT: none.
    [[nodiscard]] static constexpr std::size_t token_hash(std::uint32_t t) noexcept {
        std::uint64_t x = static_cast<std::uint64_t>(t) * 0x9E3779B97F4A7C15ull;
        x ^= x >> 29;
        return static_cast<std::size_t>(x) & (kTokenIndexCap - 1);
    }

    void token_insert(FeedSource src, std::uint32_t token, std::uint32_t idx) noexcept {
        if (token == 0) {
            return;
        }
        auto* tbl = token_index_[static_cast<std::size_t>(src)];
        std::size_t h = token_hash(token);
        while (tbl[h].token != 0) {
            h = (h + 1) & (kTokenIndexCap - 1);
        }
        tbl[h].token = token;
        tbl[h].idx = idx;
    }

    [[nodiscard]] std::uint32_t index_of(InstrumentId id) const noexcept {
        const auto v = static_cast<std::uint32_t>(id);
        return v < count_ ? v : kNoIndex;
    }

    [[nodiscard]] ALTAIR_HOT std::uint32_t
    find_by_token(FeedSource src, std::uint32_t token) const noexcept {
        if (token == 0) {
            return kNoIndex;
        }
        const auto* tbl = token_index_[static_cast<std::size_t>(src)];
        std::size_t h = token_hash(token);
        while (tbl[h].token != 0) {
            if (tbl[h].token == token) {
                return tbl[h].idx;
            }
            h = (h + 1) & (kTokenIndexCap - 1);
        }
        return kNoIndex;
    }

    ContractSpec specs_[kMaxInstruments]{};
    bool blocked_[kMaxInstruments]{};
    TokenSlot token_index_[kFeedSourceCount][kTokenIndexCap]{};
    std::uint32_t count_ = 0;
};

} // namespace altair
