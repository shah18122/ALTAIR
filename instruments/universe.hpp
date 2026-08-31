// instruments/universe.hpp — which contracts are allowed to exist at all.
//
// P1-09. The filter that the capacity debt made unavoidable: the live Kite
// dump is 106'150 rows and the NSE bhavcopy 30'488, against a SpecStore of
// 8'192. Without a filter the first file loaded fills the store and the second
// is refused entirely -- which is exactly what happened the first time both
// real files were run through the pipeline.
//
// IT RUNS AT THE GATE, NOT AT EACH DOOR.
//
// `Reconciler::add` is the single point where a contract enters the system, so
// the filter lives there. A filter applied by each loader is a filter one
// loader can forget, and the one that forgets is the one that fills the store
// with 40'000 far-out-of-the-money options nobody will trade.
//
// Decisions D1..D6 are fixed in prompts/P1-09_universe.md.

#pragma once

#include <instruments/contract_spec.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace altair {

/// Why a contract was excluded. Counted per reason, because "the universe came
/// out empty" is undiagnosable without knowing which rule emptied it.
enum class UniverseReject : std::uint8_t {
    Admitted = 0,
    Underlying,   // not in the configured list
    Segment,      // segment not enabled
    ExchangeOff,  // exchange not enabled
    Expired,      // expiry already past
    TooFarOut,    // expiry beyond the horizon
    StrikeBand    // strike too far from the reference
};

/// One tradable underlying, and the reference price its strike band is
/// measured from.
struct UniverseEntry {
    const char* underlying = nullptr;
    /// UNIT: paise. Zero means NO strike filter for this underlying — used for
    /// futures-only names and for cash, where there is no strike at all.
    Price reference{};
};

struct UniverseConfig {
    const UniverseEntry* entries = nullptr;
    std::size_t entry_count = 0;

    /// Bit i set == Segment(i) admitted.
    std::uint8_t segments = 0;
    /// Bit i set == Exchange(i) admitted.
    std::uint8_t exchanges = 0;

    /// Contracts expiring beyond `today + this` are excluded. UNIT: days.
    /// Zero means no horizon.
    std::int32_t max_expiry_days = 0;

    /// |strike - reference| beyond this is excluded. UNIT: paise.
    /// Zero means no strike filter at all.
    std::int64_t strike_band = 0;
};

[[nodiscard]] constexpr std::uint8_t seg_bit(Segment s) noexcept {
    return static_cast<std::uint8_t>(1u << static_cast<unsigned>(s));
}

[[nodiscard]] constexpr std::uint8_t ex_bit(Exchange e) noexcept {
    return static_cast<std::uint8_t>(1u << static_cast<unsigned>(e));
}

class UniverseFilter {
public:
    struct Stats {
        std::uint64_t admitted = 0;
        std::uint64_t underlying = 0;
        std::uint64_t segment = 0;
        std::uint64_t exchange = 0;
        std::uint64_t expired = 0;
        std::uint64_t too_far_out = 0;
        std::uint64_t strike_band = 0;

        [[nodiscard]] std::uint64_t rejected() const noexcept {
            return underlying + segment + exchange + expired + too_far_out
                 + strike_band;
        }
    };

    UniverseFilter() noexcept = default;
    explicit UniverseFilter(UniverseConfig cfg) noexcept : cfg_(cfg) {}

    /// Decide one contract. `today` is a PARAMETER — nothing here reads a
    /// clock, so a replay of the same universe on the same date admits exactly
    /// the same set (rule 6, rule 10).
    [[nodiscard]] ALTAIR_HOT UniverseReject
    judge(const ContractSpec& s, Timestamp today) const noexcept {
        if (cfg_.exchanges != 0
            && (cfg_.exchanges & ex_bit(s.exchange)) == 0) {
            return UniverseReject::ExchangeOff;
        }
        if (cfg_.segments != 0 && (cfg_.segments & seg_bit(s.segment)) == 0) {
            return UniverseReject::Segment;
        }

        const UniverseEntry* e = find(s.underlying);
        if (e == nullptr) {
            return UniverseReject::Underlying;
        }

        // Cash has no expiry, and `Timestamp::epoch()` is how P1-04 and
        // P1-02c both spell that. It is EXEMPT from both expiry rules --
        // filtering it on a date it does not have would silently drop the
        // entire cash universe.
        if (!s.expiry.is_epoch()) {
            const std::int64_t days =
                ist_days_between(today, s.expiry);
            if (days < 0) {
                // Already expired. A bhavcopy is a snapshot of a trading day
                // and can carry the contracts that expired on it.
                return UniverseReject::Expired;
            }
            if (cfg_.max_expiry_days > 0 && days > cfg_.max_expiry_days) {
                return UniverseReject::TooFarOut;
            }
        }

        // The strike band is what makes an options universe fit at all: the
        // real Kite dump carries 40'913 calls and 40'891 puts, and all but a
        // few hundred are strikes nobody will trade today.
        if (cfg_.strike_band > 0 && s.segment == Segment::Opt
            && e->reference.raw() > 0) {
            const std::int64_t d = s.strike.raw() - e->reference.raw();
            const std::int64_t ad = d < 0 ? -d : d;
            if (ad > cfg_.strike_band) {
                return UniverseReject::StrikeBand;
            }
        }
        return UniverseReject::Admitted;
    }

    /// Judge and count. Returns true when the contract is admitted.
    [[nodiscard]] ALTAIR_HOT bool admit(const ContractSpec& s,
                                        Timestamp today) noexcept {
        switch (judge(s, today)) {
            case UniverseReject::Admitted:    ++stats_.admitted;    return true;
            case UniverseReject::Underlying:  ++stats_.underlying;  return false;
            case UniverseReject::Segment:     ++stats_.segment;     return false;
            case UniverseReject::ExchangeOff: ++stats_.exchange;    return false;
            case UniverseReject::Expired:     ++stats_.expired;     return false;
            case UniverseReject::TooFarOut:   ++stats_.too_far_out; return false;
            case UniverseReject::StrikeBand:  ++stats_.strike_band; return false;
        }
        return false;
    }

    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    [[nodiscard]] const UniverseConfig& config() const noexcept { return cfg_; }
    void reset_stats() noexcept { stats_ = Stats{}; }

private:
    [[nodiscard]] const UniverseEntry* find(const char* u) const noexcept {
        for (std::size_t i = 0; i < cfg_.entry_count; ++i) {
            const UniverseEntry& e = cfg_.entries[i];
            if (e.underlying != nullptr && std::strcmp(e.underlying, u) == 0) {
                return &e;
            }
        }
        return nullptr;
    }

    UniverseConfig cfg_{};
    Stats stats_{};
};

} // namespace altair
