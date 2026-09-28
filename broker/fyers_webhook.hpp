#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace altair::fyers_webhook {

inline constexpr std::size_t kMaxBodyBytes = 64 * 1024;
inline constexpr std::size_t kMaxSecretBytes = 256;

enum class Status : std::uint8_t { Pending, Rejected, Cancelled, Traded, Other };
enum class WebhookError : std::uint8_t {
    EmptySecret, SecretMismatch, BodyTooLarge, Malformed, MissingStatus,
    UnselectedStatus
};

struct Policy {
    bool pending = true;
    bool rejected = true;
    bool cancelled = true;
    bool traded = true;
};

struct Event {
    Status status{Status::Other};
    std::string_view order_id{};
    std::string_view raw_body{};
};

[[nodiscard]] inline bool equal_secret(std::string_view a,
                                       std::string_view b) noexcept {
    if (a.empty() || a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff = static_cast<unsigned char>(diff | static_cast<unsigned char>(a[i] ^ b[i]));
    }
    return diff == 0;
}

[[nodiscard]] inline std::string_view json_value(std::string_view body,
                                                 std::string_view key) noexcept {
    const std::string needle = "\"" + std::string{key} + "\"";
    const auto at = body.find(needle);
    if (at == std::string_view::npos) return {};
    auto colon = body.find(':', at + needle.size());
    if (colon == std::string_view::npos) return {};
    ++colon;
    while (colon < body.size() && (body[colon] == ' ' || body[colon] == '\t')) ++colon;
    if (colon >= body.size() || body[colon] != '"') return {};
    const auto begin = ++colon;
    const auto end = body.find('"', begin);
    return end == std::string_view::npos ? std::string_view{} : body.substr(begin, end - begin);
}

[[nodiscard]] inline Status parse_status(std::string_view value) noexcept {
    if (value == "Pending" || value == "pending") return Status::Pending;
    if (value == "Rejected" || value == "rejected") return Status::Rejected;
    if (value == "Cancelled" || value == "cancelled") return Status::Cancelled;
    if (value == "Traded" || value == "traded" || value == "COMPLETE") return Status::Traded;
    return Status::Other;
}

[[nodiscard]] inline bool selected(Status status, const Policy& policy) noexcept {
    switch (status) {
    case Status::Pending: return policy.pending;
    case Status::Rejected: return policy.rejected;
    case Status::Cancelled: return policy.cancelled;
    case Status::Traded: return policy.traded;
    case Status::Other: return false;
    }
    return false;
}

[[nodiscard]] inline std::expected<Event, WebhookError>
validate(std::string_view secret, std::string_view supplied_secret,
         std::string_view body, const Policy& policy = {}) noexcept {
    if (secret.empty() || secret.size() > kMaxSecretBytes) {
        return std::unexpected(WebhookError::EmptySecret);
    }
    if (!equal_secret(secret, supplied_secret)) {
        return std::unexpected(WebhookError::SecretMismatch);
    }
    if (body.empty() || body.size() > kMaxBodyBytes) {
        return std::unexpected(WebhookError::BodyTooLarge);
    }
    const auto status_text = json_value(body, "status");
    if (status_text.empty()) return std::unexpected(WebhookError::MissingStatus);
    const Status status = parse_status(status_text);
    if (status == Status::Other) return std::unexpected(WebhookError::Malformed);
    if (!selected(status, policy)) return std::unexpected(WebhookError::UnselectedStatus);
    return Event{status, json_value(body, "order_id"), body};
}

} // namespace altair::fyers_webhook