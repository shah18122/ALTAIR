// broker/oauth_attempt.hpp -- pure validation for browser OAuth redirects.
#pragma once

#include <cctype>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace altair::broker {

enum class OAuthRedirectError : std::uint8_t {
    InvalidAttempt,
    Expired,
    CallbackMismatch,
    Cancelled,
    MissingCode,
    MissingState,
    StateMismatch,
    MalformedEncoding,
};

struct OAuthAttempt final {
    std::string callback;
    std::string state;
    std::int64_t issued_unix_seconds = 0;
    std::int64_t lifetime_seconds = 600;
};

namespace oauth_detail {

inline int hex(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

inline std::expected<std::string, OAuthRedirectError>
decode(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%') {
            if (i + 2 >= value.size())
                return std::unexpected(OAuthRedirectError::MalformedEncoding);
            const int high = hex(value[i + 1]);
            const int low = hex(value[i + 2]);
            if (high < 0 || low < 0)
                return std::unexpected(OAuthRedirectError::MalformedEncoding);
            result.push_back(static_cast<char>((high << 4) | low));
            i += 2;
        } else {
            result.push_back(value[i] == '+' ? ' ' : value[i]);
        }
    }
    return result;
}

inline std::expected<std::optional<std::string>, OAuthRedirectError>
query(std::string_view input, std::string_view key) {
    const auto question = input.find('?');
    if (question == std::string_view::npos) return std::optional<std::string>{};
    auto query = input.substr(question + 1);
    const auto fragment = query.find('#');
    if (fragment != std::string_view::npos) query = query.substr(0, fragment);
    while (!query.empty()) {
        const auto separator = query.find('&');
        const auto pair = query.substr(0, separator);
        const auto equal = pair.find('=');
        if (pair.substr(0, equal) == key) {
            const auto value = equal == std::string_view::npos
                ? std::string_view{} : pair.substr(equal + 1);
            auto decoded = decode(value);
            if (!decoded) return std::unexpected(decoded.error());
            return std::optional<std::string>{std::move(*decoded)};
        }
        if (separator == std::string_view::npos) break;
        query.remove_prefix(separator + 1);
    }
    return std::optional<std::string>{};
}

} // namespace oauth_detail

/// Validate one redirect and return its decoded one-time code.
/// Preconditions: callback has no query/fragment; timestamps are Unix seconds.
[[nodiscard]] inline std::expected<std::string, OAuthRedirectError>
validate_oauth_redirect(std::string_view redirect, const OAuthAttempt& attempt,
                        std::int64_t now_unix_seconds,
                        std::string_view code_key,
                        std::string_view state_key = "state") {
    if (attempt.callback.empty() || attempt.callback.find_first_of("?#")
            != std::string::npos
        || attempt.state.empty() || attempt.issued_unix_seconds <= 0
        || attempt.lifetime_seconds <= 0 || code_key.empty()) {
        return std::unexpected(OAuthRedirectError::InvalidAttempt);
    }
    if (now_unix_seconds < attempt.issued_unix_seconds
        || now_unix_seconds - attempt.issued_unix_seconds
               > attempt.lifetime_seconds) {
        return std::unexpected(OAuthRedirectError::Expired);
    }
    const auto question = redirect.find('?');
    if (question == std::string_view::npos
        || redirect.substr(0, question) != attempt.callback) {
        return std::unexpected(OAuthRedirectError::CallbackMismatch);
    }
    const auto error = oauth_detail::query(redirect, "error");
    const auto status = oauth_detail::query(redirect, "status");
    if (!error || !status) return std::unexpected(OAuthRedirectError::MalformedEncoding);
    if ((error->has_value() && !error->value().empty())
        || (status->has_value() && !status->value().empty()
            && status->value() != "success" && status->value() != "ok")) {
        return std::unexpected(OAuthRedirectError::Cancelled);
    }
    const auto state = oauth_detail::query(redirect, state_key);
    if (!state) return std::unexpected(state.error());
    if (!state->has_value() || state->value().empty())
        return std::unexpected(OAuthRedirectError::MissingState);
    if (state->value() != attempt.state)
        return std::unexpected(OAuthRedirectError::StateMismatch);
    const auto code = oauth_detail::query(redirect, code_key);
    if (!code) return std::unexpected(code.error());
    if (!code->has_value() || code->value().empty())
        return std::unexpected(OAuthRedirectError::MissingCode);
    return std::move(code->value());
}

} // namespace altair::broker
