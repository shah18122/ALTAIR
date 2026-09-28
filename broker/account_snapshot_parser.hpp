// broker/account_snapshot_parser.hpp -- bounded parsers for read-only account
// responses.  This is intentionally small: it parses only fields needed by
// the shared snapshot contract and refuses malformed/ambiguous values.
#pragma once

#include "account_snapshot.hpp"

#include <charconv>
#include <cctype>
#include <cstdint>
#include <expected>
#include <limits>
#include <string_view>

namespace altair::broker_view {

struct ProviderResponse {
    int status{};
    std::string_view body{};
};

enum class SnapshotParseError : std::uint8_t {
    MissingProfile,
    InvalidProfile,
    MissingAccountId,
    InvalidAmount,
    AmountOverflow,
    InvalidSession
};

namespace snapshot_detail {

[[nodiscard]] inline std::string_view field_value(std::string_view json,
                                                   std::string_view key) noexcept {
    std::size_t k = 0;
    for (;;) {
        k = json.find('"', k);
        if (k == std::string_view::npos) return {};
        const auto start = k + 1;
        if (start + key.size() < json.size()
            && json.substr(start, key.size()) == key
            && json[start + key.size()] == '"') {
            break;
        }
        ++k;
    }
    auto p = json.find(':', k + key.size() + 2);
    if (p == std::string_view::npos) return {};
    ++p;
    while (p < json.size() && std::isspace(static_cast<unsigned char>(json[p]))) ++p;
    if (p >= json.size()) return {};
    if (json[p] == '"') {
        ++p;
        const auto begin = p;
        bool escaped = false;
        for (; p < json.size(); ++p) {
            if (escaped) { escaped = false; continue; }
            if (json[p] == '\\') { escaped = true; continue; }
            if (json[p] == '"') return json.substr(begin, p - begin);
        }
        return {};
    }
    const auto begin = p;
    while (p < json.size() && json[p] != ',' && json[p] != '}'
           && !std::isspace(static_cast<unsigned char>(json[p]))) ++p;
    return json.substr(begin, p - begin);
}

[[nodiscard]] inline bool success(std::string_view body) noexcept {
    return field_value(body, "status") == "success"
        || field_value(body, "s") == "ok";
}

[[nodiscard]] inline std::expected<Notional, SnapshotParseError>
amount_paise(std::string_view text) noexcept {
    if (text.empty() || text.size() > 48) {
        return std::unexpected(SnapshotParseError::InvalidAmount);
    }
    bool neg = false;
    if (text.front() == '-') { neg = true; text.remove_prefix(1); }
    if (text.empty()) return std::unexpected(SnapshotParseError::InvalidAmount);
    std::uint64_t whole = 0;
    std::uint64_t frac = 0;
    unsigned frac_digits = 0;
    bool dot = false;
    for (const char c : text) {
        if (c == '.' && !dot) { dot = true; continue; }
        if (c < '0' || c > '9' || (dot && frac_digits == 2)) {
            return std::unexpected(SnapshotParseError::InvalidAmount);
        }
        if (dot) { frac = frac * 10u + static_cast<unsigned>(c - '0'); ++frac_digits; }
        else {
            const auto d = static_cast<std::uint64_t>(c - '0');
            if (whole > (std::numeric_limits<std::uint64_t>::max() - d) / 10u)
                return std::unexpected(SnapshotParseError::AmountOverflow);
            whole = whole * 10u + d;
        }
    }
    if (frac_digits == 1) frac *= 10u;
    if (whole > (std::numeric_limits<std::uint64_t>::max() - frac) / 100u)
        return std::unexpected(SnapshotParseError::AmountOverflow);
    const std::uint64_t magnitude = whole * 100u + frac;
    if (!neg && magnitude > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return std::unexpected(SnapshotParseError::AmountOverflow);
    if (neg && magnitude > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1u)
        return std::unexpected(SnapshotParseError::AmountOverflow);
    const auto value = neg
        ? (magnitude == static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1u
            ? std::numeric_limits<std::int64_t>::min()
            : -static_cast<std::int64_t>(magnitude))
        : static_cast<std::int64_t>(magnitude);
    return Notional{value};
}

[[nodiscard]] inline SnapshotSection section(const ProviderResponse& r) noexcept {
    SnapshotSection out{};
    if (r.status <= 0) { out.status = SnapshotSectionStatus::TransportError; return out; }
    out.http_status = static_cast<std::uint16_t>(r.status);
    if (r.status != 200) { out.status = SnapshotSectionStatus::HttpError; return out; }
    out.status = success(r.body) ? SnapshotSectionStatus::Present
                                 : SnapshotSectionStatus::Invalid;
    return out;
}

} // namespace snapshot_detail

/// Parse the five read-only provider responses. `account_key` is `user_id` for
/// Kite and `fy_id` for FYERS. The caller supplies a session and observation
/// window, so this parser never reads credentials or a clock.
[[nodiscard]] inline std::expected<AccountSnapshot, SnapshotParseError>
parse_account_snapshot(BrokerId broker, SessionKey session,
                        EvidenceWindow observed,
                        ProviderResponse profile,
                        ProviderResponse funds,
                        ProviderResponse positions,
                        ProviderResponse holdings,
                        ProviderResponse orders,
                        std::string_view account_key) noexcept {
    if (!valid(session) || session.broker != broker)
        return std::unexpected(SnapshotParseError::InvalidSession);
    AccountSnapshot out{};
    out.account_session = session;
    out.observed = observed;
    out.profile = snapshot_detail::section(profile);
    out.funds = snapshot_detail::section(funds);
    out.positions = snapshot_detail::section(positions);
    out.holdings = snapshot_detail::section(holdings);
    out.orders = snapshot_detail::section(orders);
    if (out.profile.status != SnapshotSectionStatus::Present)
        return std::unexpected(out.profile.status == SnapshotSectionStatus::Invalid
            ? SnapshotParseError::InvalidProfile : SnapshotParseError::MissingProfile);
    const auto id = snapshot_detail::field_value(profile.body, account_key);
    if (id.empty() || id.size() > kBrokerAccountIdMax)
        return std::unexpected(SnapshotParseError::MissingAccountId);
    for (std::size_t i = 0; i < id.size(); ++i) out.account_id[i] = id[i];
    out.account_id[id.size()] = '\0';

    // The raw provider positions section may be present and complete even while
    // the provider-neutral typed row decoder is not implemented. Only an
    // explicitly empty array can currently be represented as typed positions;
    // non-empty raw data remains available to the read-only account UI without
    // being silently converted into typed zero exposure.
    if (out.positions.status == SnapshotSectionStatus::Present) {
        out.typed_positions.account_session = session;
        out.typed_positions.observed = observed;
        out.typed_positions_present =
            positions.body.find("\"data\":[]") != std::string_view::npos;
    }

    // Providers use different names, so the parser accepts the common
    // read-only cash aliases and leaves the field absent when none is present.
    if (out.funds.status == SnapshotSectionStatus::Present) {
        for (const auto key : {"cash", "available_cash", "available"}) {
            const auto raw = snapshot_detail::field_value(funds.body, key);
            if (raw.empty()) continue;
            const auto amount = snapshot_detail::amount_paise(raw);
            if (!amount) return std::unexpected(amount.error());
            out.typed_funds.available_trading_balance = *amount;
            break;
        }
    }
    return out;
}

} // namespace altair::broker_view
