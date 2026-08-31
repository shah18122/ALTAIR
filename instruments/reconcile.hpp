// instruments/reconcile.hpp — three-way agreement, disagreement flags, blocking.
//
// P1-06. Pure logic over ContractSpec values: this file reads no file, opens no
// socket, and does not know what a CSV is. That is deliberate — it lets the
// reconciler be built and fully tested before two of its three input parsers
// exist, and it stays correct unchanged when they land.
//
// Rule 9 is the whole point. Three authorities describe one contract; when they
// disagree about how big an order is or what it may be priced at, the symbol is
// blocked and flagged. It is never averaged, and a broker is never quietly
// preferred over the exchange.
//
// The eight design decisions (D1..D8) are fixed in prompts/P1-06_*.md and are
// referenced by number below.

#pragma once

#include <instruments/contract_spec.hpp>
#include <instruments/universe.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>

namespace altair {

inline constexpr std::size_t kSpecSourceCount = 5;   // SpecSource has 5 values

/// Why a contract was accepted or blocked. D3, D6, D7.
enum class Verdict : std::uint8_t {
    Agreed,          // every source present agreed on all three compared fields
    SingleSource,    // only one source described it — accepted, uncorroborated
    MissingPrimary,  // a primary WAS loaded but does not carry this — BLOCK
    NoBroker,        // no broker carries it, so it cannot be routed — BLOCK
    ValueConflict    // sources disagree on lot, tick or scale — BLOCK
};

/// Which of the three compared fields conflicted. D2.
enum class ConflictField : std::uint8_t { None = 0, LotSize, TickSize, PriceScale };

enum class ReconcileError : std::uint8_t {
    Full,          // more distinct contracts than kMaxInstruments
    BadSymbol,     // underlying null, empty, or over-long
    BadSource,     // SpecSource out of range
    OutOfRange,    // verdict_at called past size()
    NotReconciled, // verdicts requested before reconcile(), or stale after add()
    /// D11 -- outside the configured universe. NOT a failure: it is the filter
    /// working, and a caller counts it apart from anything broken.
    OutOfUniverse
};

/// One contract's outcome. Carries enough to debug a conflict at 08:20 without
/// re-reading the source files — which at 08:20 you will not have time to do.
struct ContractVerdict {
    ContractSpec  merged;        // the spec to add to the store
    Verdict       verdict;
    ConflictField field;         // None unless verdict == ValueConflict
    std::uint8_t  present_mask;  // bit i set == SpecSource(i) carried it
    std::uint8_t  conflict_mask; // bit i set == source i differed from the winner
    /// The conflicting values, indexed by SpecSource. Meaningful only where
    /// present_mask has the bit set. UNIT: as the named field.
    std::int64_t  values[kSpecSourceCount];
};

[[nodiscard]] constexpr bool verdict_blocks(Verdict v) noexcept {
    return v == Verdict::MissingPrimary || v == Verdict::NoBroker
        || v == Verdict::ValueConflict;
}

struct ReconcileReport {
    std::size_t contracts;         // distinct contracts seen
    std::size_t agreed;
    std::size_t single_source;
    std::size_t blocked;           // == missing_primary + no_broker + conflicts
    std::size_t missing_primary;
    std::size_t no_broker;
    std::size_t conflicts;
    bool primary_source_seen;      // D7 — false means nothing was cross-checked
};

namespace detail {

/// D1: the cross-source identity of a contract.
///
/// NOT the symbol and NOT a token. Broker tokens are different number spaces
/// for the same contract (P0-09b), so they cannot join anything. Symbols are
/// per-source formatting — Kite writes NIFTY25SEP25000CE where an exchange
/// master writes the components apart. This tuple is what the contract IS.
struct ContractKey {
    Timestamp  expiry;
    Price      strike;
    char       underlying[kMaxUnderlyingLen + 1];
    Exchange   exchange;
    Segment    segment;
    OptionType opt_type;
};

[[nodiscard]] inline bool key_eq(const ContractKey& a, const ContractKey& b) noexcept {
    return a.expiry == b.expiry
        && a.strike == b.strike
        && a.exchange == b.exchange
        && a.segment == b.segment
        && a.opt_type == b.opt_type
        && std::strcmp(a.underlying, b.underlying) == 0;
}

[[nodiscard]] inline std::uint64_t key_hash(const ContractKey& k) noexcept {
    std::uint64_t h = spec_mix64(static_cast<std::uint64_t>(k.expiry.ns_since_epoch()));
    h ^= spec_mix64(static_cast<std::uint64_t>(k.strike.raw()) + 0x100u);
    h ^= spec_mix64(static_cast<std::uint64_t>(k.exchange) * 3u
                    + static_cast<std::uint64_t>(k.segment) * 17u
                    + static_cast<std::uint64_t>(k.opt_type) * 251u + 0x200u);
    std::uint64_t s = 0xCBF29CE484222325ull;
    for (std::size_t i = 0; k.underlying[i] != '\0' && i <= kMaxUnderlyingLen; ++i) {
        s ^= static_cast<std::uint64_t>(static_cast<unsigned char>(k.underlying[i]));
        s *= 0x100000001B3ull;
    }
    return h ^ spec_mix64(s);
}

/// D4 precedence. Higher wins. Exchange-relative: a BseMaster row describing an
/// NSE contract is nonsense, so it ranks below the brokers rather than above.
[[nodiscard]] constexpr std::uint8_t source_prec(SpecSource s, Exchange ex) noexcept {
    switch (s) {
        case SpecSource::Manual:    return 4;
        case SpecSource::NseMaster: return ex == Exchange::NSE ? 3u : 0u;
        case SpecSource::BseMaster: return ex == Exchange::BSE ? 3u : 0u;
        case SpecSource::KiteDump:  return 2;
        case SpecSource::XtsMaster: return 1;
    }
    return 0;
}

[[nodiscard]] constexpr bool is_primary(SpecSource s, Exchange ex) noexcept {
    return (s == SpecSource::NseMaster && ex == Exchange::NSE)
        || (s == SpecSource::BseMaster && ex == Exchange::BSE);
}

/// Everything accumulated for one contract, before a verdict exists.
///
/// The per-source arrays are kept rather than whole specs: five ContractSpecs
/// per contract would be 7 MB of mostly-identical bytes. Only the three
/// compared fields (D2) and the provenance actually differ per source.
struct Entry {
    ContractKey  key;
    ContractSpec winner;                      // the D4 precedence winner so far
    std::int64_t lot[kSpecSourceCount];
    std::int64_t tick[kSpecSourceCount];
    std::int64_t scale[kSpecSourceCount];
    std::uint64_t src_hash[kSpecSourceCount];
    Timestamp    src_snapshot[kSpecSourceCount];
    std::uint32_t kite_token;
    std::uint32_t xts_token;
    std::uint8_t present_mask;
    std::uint8_t src_stale_mask;
    std::uint8_t winner_prec;
    SpecSource   winner_src;
};

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────
// Reconciler
//
// Multi-megabyte: owns fixed storage for kMaxInstruments contracts and
// allocates nothing. NOT a stack object — hold it as a member, like SpecStore.
// ─────────────────────────────────────────────────────────────────────────
class Reconciler {
public:
    Reconciler() noexcept { clear(); }

    Reconciler(const Reconciler&) = delete;
    Reconciler& operator=(const Reconciler&) = delete;

    /// Feed one source's view of one contract. UNIT: none.
    ///
    /// Order-independent (D8). The same (key, source) twice is last-writer-wins,
    /// so re-loading a source is idempotent rather than a phantom conflict.
    /// Returns BadSymbol, BadSource, or Full. A rejected add changes nothing.
    [[nodiscard]] std::expected<void, ReconcileError>
    add(const ContractSpec& spec) noexcept {
        const auto si = static_cast<std::size_t>(spec.source);
        if (si >= kSpecSourceCount) {                       // req 3: check first
            return std::unexpected(ReconcileError::BadSource);
        }
        if (spec.underlying[0] == '\0'
            || detail::spec_str_len(spec.underlying, kMaxUnderlyingLen)
                   > kMaxUnderlyingLen) {
            return std::unexpected(ReconcileError::BadSymbol);
        }

        // D11: the universe filter runs HERE, at the single point where a
        // contract enters the system, and BEFORE anything is stored.
        //
        // A filter applied by each loader is a filter one loader can forget,
        // and the one that forgets is the one that fills the store with 40'000
        // far-out-of-the-money options nobody will trade. Filtering after the
        // fact would be worse still: the store holds 8'192 and the two real
        // files carry 136'000 rows between them, so whichever loaded first
        // would win and WHICH 8'192 survived would depend on CSV row order --
        // an ordering nobody chose and nobody could reproduce.
        if (universe_ != nullptr && !universe_->admit(spec, universe_today_)) {
            return std::unexpected(ReconcileError::OutOfUniverse);
        }

        detail::ContractKey key{};
        key.expiry   = spec.expiry;
        key.strike   = spec.strike;
        key.exchange = spec.exchange;
        key.segment  = spec.segment;
        key.opt_type = spec.opt_type;
        std::memcpy(key.underlying, spec.underlying, sizeof(key.underlying));
        key.underlying[kMaxUnderlyingLen] = '\0';

        std::uint32_t idx = find(key);
        if (idx == kNoIndex) {
            if (count_ >= kMaxInstruments) {
                return std::unexpected(ReconcileError::Full);
            }
            idx = count_;
            detail::Entry& fresh = entries_[idx];
            fresh = detail::Entry{};
            fresh.key = key;
            fresh.winner = spec;
            fresh.winner_prec = detail::source_prec(spec.source, spec.exchange);
            fresh.winner_src = spec.source;
            insert_index(key, idx);
            ++count_;
        } else {
            detail::Entry& e = entries_[idx];
            const std::uint8_t p = detail::source_prec(spec.source, spec.exchange);
            // Same source replaces itself (last-writer-wins). Otherwise strictly
            // higher precedence wins, and an exact tie — only reachable when two
            // wrong-exchange masters both rank 0 — breaks on the lower ordinal,
            // so the outcome does not depend on arrival order (D8).
            const bool same_src = (spec.source == e.winner_src);
            const bool outranks =
                p > e.winner_prec
                || (p == e.winner_prec
                    && static_cast<std::uint8_t>(spec.source)
                           < static_cast<std::uint8_t>(e.winner_src));
            if (same_src || outranks) {
                e.winner = spec;
                e.winner_prec = p;
                e.winner_src = spec.source;
            }
        }

        detail::Entry& e = entries_[idx];
        e.lot[si]   = spec.lot_size.raw();
        e.tick[si]  = spec.tick_size.raw();
        e.scale[si] = spec.price_scale;
        e.src_hash[si] = spec.source_hash;
        e.src_snapshot[si] = spec.snapshot_at;
        e.present_mask = static_cast<std::uint8_t>(e.present_mask | (1u << si));
        const std::uint8_t bit = static_cast<std::uint8_t>(1u << si);
        e.src_stale_mask = static_cast<std::uint8_t>(
            spec.stale ? (e.src_stale_mask | bit)
                       : (e.src_stale_mask & static_cast<std::uint8_t>(~bit)));
        if (spec.source == SpecSource::KiteDump) {
            e.kite_token = spec.token[static_cast<std::size_t>(FeedSource::Kite)];
        }
        if (spec.source == SpecSource::XtsMaster) {
            e.xts_token = spec.token[static_cast<std::size_t>(FeedSource::Xts)];
        }
        dirty_ = true;
        return {};
    }

    /// Install the universe filter. UNIT: none.
    ///
    /// `today` is STORED rather than read from a clock, so the same universe on
    /// the same date admits exactly the same set (rule 6, rule 10). Pass
    /// nullptr to admit everything, which is what the reconciler's own tests do.
    void set_universe(UniverseFilter* u, Timestamp today) noexcept {
        universe_ = u;
        universe_today_ = today;
    }

    [[nodiscard]] const UniverseFilter* universe() const noexcept {
        return universe_;
    }

    /// Decide every accumulated contract. UNIT: none. Idempotent (req 10).
    [[nodiscard]] ReconcileReport reconcile() noexcept {
        ReconcileReport r{};
        r.contracts = count_;

        // D7: whether ANY primary was loaded is a property of the run. Compute
        // it over every contract before judging any single one, or the first
        // contract is judged against an answer that does not exist yet.
        for (std::uint32_t i = 0; i < count_; ++i) {
            const detail::Entry& e = entries_[i];
            for (std::size_t s = 0; s < kSpecSourceCount; ++s) {
                if ((e.present_mask & (1u << s)) != 0
                    && detail::is_primary(static_cast<SpecSource>(s),
                                          e.key.exchange)) {
                    r.primary_source_seen = true;
                }
            }
        }

        for (std::uint32_t i = 0; i < count_; ++i) {
            ContractVerdict& v = verdicts_[i];
            decide(entries_[i], r.primary_source_seen, v);
            switch (v.verdict) {
                case Verdict::Agreed:         ++r.agreed;          break;
                case Verdict::SingleSource:   ++r.single_source;   break;
                case Verdict::MissingPrimary: ++r.missing_primary; break;
                case Verdict::NoBroker:       ++r.no_broker;       break;
                case Verdict::ValueConflict:  ++r.conflicts;       break;
            }
        }
        r.blocked = r.missing_primary + r.no_broker + r.conflicts;
        dirty_ = false;
        return r;
    }

    [[nodiscard]] std::size_t size() const noexcept { return count_; }

    /// Verdict i, in insertion order. UNIT: none.
    ///
    /// Returns NotReconciled before the first reconcile(), and again after any
    /// add() that followed one. This is not pedantry: a default-constructed
    /// ContractVerdict has verdict == Agreed, because Agreed is ordinal 0 — so
    /// without this guard, FORGETTING to call reconcile() reads as "everything
    /// agreed, nothing blocked", which is the exact silent all-clear rule 9
    /// exists to prevent.
    [[nodiscard]] std::expected<const ContractVerdict*, ReconcileError>
    verdict_at(std::size_t i) const noexcept {
        if (dirty_) {
            return std::unexpected(ReconcileError::NotReconciled);
        }
        if (i >= count_) {
            return std::unexpected(ReconcileError::OutOfRange);
        }
        return &verdicts_[i];
    }

    /// False when a verdict would be stale or absent. UNIT: none.
    [[nodiscard]] bool reconciled() const noexcept { return !dirty_; }

    void clear() noexcept {
        count_ = 0;
        dirty_ = false;
        for (std::size_t i = 0; i < kKeyIndexCap; ++i) {
            key_index_[i] = kNoIndex;
        }
    }

private:
    static constexpr std::size_t kKeyIndexCap = 16384;   // 2 * kMaxInstruments
    static constexpr std::uint32_t kNoIndex = 0xFFFF'FFFFu;

    [[nodiscard]] std::uint32_t find(const detail::ContractKey& k) const noexcept {
        std::size_t h = static_cast<std::size_t>(detail::key_hash(k))
                        & (kKeyIndexCap - 1);
        for (std::size_t probe = 0; probe < kKeyIndexCap; ++probe) {
            const std::uint32_t slot = key_index_[h];
            if (slot == kNoIndex) {
                return kNoIndex;
            }
            if (detail::key_eq(entries_[slot].key, k)) {
                return slot;
            }
            h = (h + 1) & (kKeyIndexCap - 1);
        }
        return kNoIndex;
    }

    void insert_index(const detail::ContractKey& k, std::uint32_t idx) noexcept {
        std::size_t h = static_cast<std::size_t>(detail::key_hash(k))
                        & (kKeyIndexCap - 1);
        for (std::size_t probe = 0; probe < kKeyIndexCap; ++probe) {
            if (key_index_[h] == kNoIndex) {
                key_index_[h] = idx;
                return;
            }
            h = (h + 1) & (kKeyIndexCap - 1);
        }
        // Unreachable: capacity is 2x kMaxInstruments and count_ is capped.
    }

    /// Requirement 5's ladder, in exactly its order.
    static void decide(const detail::Entry& e, bool primary_seen,
                       ContractVerdict& v) noexcept {
        v = ContractVerdict{};
        v.present_mask = e.present_mask;
        v.merged = merge(e);

        // The comparison reference is no longer the precedence winner's value
        // but the highest-precedence NON-ZERO one (D10), so `winner_src` is
        // not consulted here any more.

        // ValueConflict first: lot, then tick, then scale (req 5 ordering).
        const std::int64_t* fields[3] = { e.lot, e.tick, e.scale };
        const ConflictField names[3] = { ConflictField::LotSize,
                                         ConflictField::TickSize,
                                         ConflictField::PriceScale };
        for (std::size_t f = 0; f < 3; ++f) {
            // D10: a ZERO means "this source does not carry the field", not
            // "the value is zero", so it is excluded from the comparison.
            //
            // D2 already said exactly this about freeze_qty and the bands and
            // then failed to apply it to the three compared fields. Real data
            // made the gap obvious: the NSE UDiFF bhavcopy has no tick-size
            // column at all, so it reports 0, and comparing that against
            // Kite's 5 made every single NSE contract a ValueConflict -- the
            // reconciler blocking the entire universe because its most
            // authoritative source declined to answer one question.
            //
            // A zero lot size stays dangerous, but the danger is that it might
            // WIN, not that it disagrees -- and merge() below refuses to let a
            // zero win. Absence and disagreement are different claims.
            const std::int64_t ref = pick_nonzero(fields[f], e.present_mask,
                                                  e.key.exchange);
            std::uint8_t mask = 0;
            for (std::size_t s = 0; s < kSpecSourceCount; ++s) {
                if ((e.present_mask & (1u << s)) != 0 && fields[f][s] != 0
                    && ref != 0 && fields[f][s] != ref) {
                    mask = static_cast<std::uint8_t>(mask | (1u << s));
                }
            }
            if (mask != 0) {
                v.verdict = Verdict::ValueConflict;
                v.field = names[f];
                v.conflict_mask = mask;
                for (std::size_t s = 0; s < kSpecSourceCount; ++s) {
                    v.values[s] = fields[f][s];
                }
                return;
            }
        }

        bool has_primary = false;
        for (std::size_t s = 0; s < kSpecSourceCount; ++s) {
            if ((e.present_mask & (1u << s)) != 0
                && detail::is_primary(static_cast<SpecSource>(s), e.key.exchange)) {
                has_primary = true;
            }
        }
        if (primary_seen && !has_primary) {
            v.verdict = Verdict::MissingPrimary;
            return;
        }

        const bool has_broker =
            (e.present_mask & (1u << static_cast<std::size_t>(SpecSource::KiteDump))) != 0
            || (e.present_mask & (1u << static_cast<std::size_t>(SpecSource::XtsMaster))) != 0;
        if (!has_broker) {
            v.verdict = Verdict::NoBroker;
            return;
        }

        v.verdict = (popcount8(e.present_mask) == 1) ? Verdict::SingleSource
                                                     : Verdict::Agreed;
    }

    /// Requirements 6, 7, 8.
    /// The value of `field` from the highest-precedence source that actually
    /// carries it. Zero means "not carried" (D10), so a source that declined
    /// to answer never overrides one that did.
    [[nodiscard]] static std::int64_t
    pick_nonzero(const std::int64_t* field, std::uint8_t present,
                 Exchange ex) noexcept {
        std::int64_t best = 0;
        int best_prec = -1;
        for (std::size_t s = 0; s < kSpecSourceCount; ++s) {
            if ((present & (1u << s)) == 0 || field[s] == 0) {
                continue;
            }
            const int p = detail::source_prec(static_cast<SpecSource>(s), ex);
            if (p > best_prec) {
                best_prec = p;
                best = field[s];
            }
        }
        return best;
    }

    static ContractSpec merge(const detail::Entry& e) noexcept {
        ContractSpec m = e.winner;                      // req 6, req 8
        m.id = InstrumentId::Invalid;                   // the store assigns it

        // D10: each compared field comes from the highest-precedence source
        // that CARRIES it. Taking them blindly from the precedence winner
        // would let the exchange master's absent tick size (0) override the
        // broker's real one, and every order would then round to a zero tick.
        const std::int64_t lot = pick_nonzero(e.lot, e.present_mask, e.key.exchange);
        const std::int64_t tick = pick_nonzero(e.tick, e.present_mask, e.key.exchange);
        const std::int64_t scale = pick_nonzero(e.scale, e.present_mask, e.key.exchange);
        m.lot_size = LotSize{lot};
        m.tick_size = Price{tick};
        m.price_scale = scale;
        m.token[static_cast<std::size_t>(FeedSource::Kite)] = e.kite_token;
        m.token[static_cast<std::size_t>(FeedSource::Xts)]  = e.xts_token;

        // D8: a wrapping sum of a finalising mix, so the merged hash does not
        // depend on which order the sources arrived in.
        std::uint64_t sum = 0;
        bool any_stale = false;
        Timestamp oldest = Timestamp::max();
        for (std::size_t s = 0; s < kSpecSourceCount; ++s) {
            if ((e.present_mask & (1u << s)) == 0) {
                continue;
            }
            sum += detail::spec_mix64(e.src_hash[s]);
            if ((e.src_stale_mask & (1u << s)) != 0) {
                any_stale = true;
            }
            if (e.src_snapshot[s] < oldest) {
                oldest = e.src_snapshot[s];
            }
        }
        m.source_hash = sum;
        m.stale = any_stale;                            // req 7
        // The oldest, not the freshest: reporting the freshest would be a lie
        // about the weakest link in the set.
        m.snapshot_at = oldest;
        return m;
    }

    [[nodiscard]] static constexpr std::size_t popcount8(std::uint8_t m) noexcept {
        std::size_t n = 0;
        for (std::size_t i = 0; i < 8; ++i) {
            n += (m >> i) & 1u;
        }
        return n;
    }

    detail::Entry   entries_[kMaxInstruments]{};
    ContractVerdict verdicts_[kMaxInstruments]{};
    std::uint32_t   key_index_[kKeyIndexCap]{};
    std::uint32_t   count_ = 0;
    bool            dirty_ = false;
    UniverseFilter* universe_ = nullptr;
    Timestamp       universe_today_{};
};

/// Add every verdict to the store, blocking those that must be blocked (D6).
/// UNIT: count of blocked contracts. Returns the first store error and adds
/// nothing further after a failure.
///
/// A blocked contract IS added, then blocked — never dropped. A tick for it
/// must then resolve to `Blocked`, which is loud, rather than to `NotFound`,
/// which looks like an unknown instrument and gets lost in the noise (rule 9).
/// Takes a mutable reference and reconciles first: reconcile() is idempotent
/// (req 10) and this is a pre-open path, so making the stale state unreachable
/// is better than reporting it through an error type that cannot name it.
[[nodiscard]] inline std::expected<std::size_t, SpecError>
apply_to_store(Reconciler& rec, SpecStore& store) noexcept {
    (void)rec.reconcile();
    std::size_t blocked = 0;
    for (std::size_t i = 0; i < rec.size(); ++i) {
        const auto v = rec.verdict_at(i);
        if (!v) {
            return std::unexpected(SpecError::NotFound);
        }
        const auto id = store.add((*v)->merged);
        if (!id) {
            return std::unexpected(id.error());
        }
        if (verdict_blocks((*v)->verdict)) {
            const auto b = store.block(*id);
            if (!b) {
                return std::unexpected(b.error());
            }
            ++blocked;
        }
    }
    return blocked;
}

} // namespace altair
