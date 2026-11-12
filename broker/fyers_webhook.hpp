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

/// The request target split: "/fyers/callback?key=abc" -> path
/// "/fyers/callback", key "abc". The key in the URL is how the secret reaches
/// us whatever FYERS does with its own Secret field: the URL registered in the
/// FYERS dashboard carries it.
struct Target {
    std::string_view path{};
    std::string_view key{};
};
[[nodiscard]] inline Target split_target(std::string_view target) noexcept {
    Target t;
    const auto q = target.find('?');
    t.path = target.substr(0, q);
    if (q == std::string_view::npos) return t;
    std::string_view query = target.substr(q + 1);
    while (!query.empty()) {
        const auto amp = query.find('&');
        const std::string_view kv = query.substr(0, amp);
        if (kv.substr(0, 4) == "key=") t.key = kv.substr(4);
        if (amp == std::string_view::npos) break;
        query = query.substr(amp + 1);
    }
    return t;
}

/// Whether a post is ours: the secret in the URL's key, in the
/// X-Fyers-Webhook-Secret header, or in the body's "secret" field.
[[nodiscard]] inline bool authorised(std::string_view secret, std::string_view url_key, std::string_view header,
                                     std::string_view body) noexcept {
    if (secret.empty() || secret.size() > kMaxSecretBytes) return false;
    return equal_secret(secret, url_key) || equal_secret(secret, header) || equal_secret(secret, json_value(body, "secret"));
}

/// The order id in a FYERS post ("id", else "order_id"); only [A-Za-z0-9-_]
/// is kept, so it can be written into a log line as it is.
[[nodiscard]] inline std::string order_id_of(std::string_view body) {
    std::string_view v = json_value(body, "id");
    if (v.empty()) v = json_value(body, "order_id");
    std::string out;
    for (const char c : v.substr(0, 64))
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_') out.push_back(c);
    return out;
}

} // namespace altair::fyers_webhook