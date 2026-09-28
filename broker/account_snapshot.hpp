// broker/account_snapshot.hpp -- bounded, provider-neutral read-side account
// evidence.  It deliberately contains no transport and no order capability.
#pragma once

#include <types/broker_positions.hpp>
#include <types/broker_state.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace altair::broker_view {

inline constexpr std::uint32_t kAccountSnapshotSchemaVersion = 1;
inline constexpr std::size_t kBrokerAccountIdMax = 64;

enum class SnapshotSectionStatus : std::uint8_t {
    Absent,          ///< provider did not return this section
    Present,         ///< section parsed and typed
    Invalid,         ///< response was present but failed validation
    TransportError,  ///< no HTTP response was obtained
    HttpError        ///< provider returned a non-success status
};

struct SnapshotSection {
    SnapshotSectionStatus status{SnapshotSectionStatus::Absent};
    std::uint16_t http_status{};
    std::uint16_t item_count{};
    std::uint16_t error_code{};
};

/// A single provider account snapshot. Sections are independent: a failed
/// holdings request never turns a successful funds response into zero, and a
/// missing field remains absent rather than being inferred.
struct AccountSnapshot {
    std::uint32_t schema_version{kAccountSnapshotSchemaVersion};
    SessionKey account_session{};
    EvidenceWindow observed{};
    std::array<char, kBrokerAccountIdMax + 1> account_id{};
    SnapshotSection profile{};
    SnapshotSection funds{};
    SnapshotSection positions{};
    SnapshotSection holdings{};
    SnapshotSection orders{};
    AccountFunds typed_funds{};
    PositionSnapshot typed_positions{};
    bool typed_positions_present{};
};

[[nodiscard]] inline bool account_id_present(const AccountSnapshot& s) noexcept {
    return s.account_id[0] != '\0';
}

/// Structural and freshness validation. `now` is supplied by the service;
/// this function never reads a clock and never treats an absent section as an
/// empty successful section.
[[nodiscard]] inline bool usable(const AccountSnapshot& s,
                                 Timestamp now) noexcept {
    if (s.schema_version != kAccountSnapshotSchemaVersion
        || !valid(s.account_session) || !account_id_present(s)
        || !fresh(s.observed, now)
        || s.profile.status != SnapshotSectionStatus::Present
        || (s.typed_positions_present
            && !broker_view::usable(s.typed_positions, now))) {
        return false;
    }
    return true;
}

[[nodiscard]] inline bool section_present(const SnapshotSection& s) noexcept {
    return s.status == SnapshotSectionStatus::Present;
}

/// All five read-only account surfaces answered successfully and passed the
/// section parser. This is deliberately stricter than `usable()`: an identity
/// may be usable while one optional account section is unavailable, but the UI
/// must label that snapshot PARTIAL rather than silently presenting it as a
/// complete account.
[[nodiscard]] inline bool complete(const AccountSnapshot& s) noexcept {
    return section_present(s.profile) && section_present(s.funds)
        && section_present(s.positions) && section_present(s.holdings)
        && section_present(s.orders);
}

} // namespace altair::broker_view
