// app/fyers_env_session.hpp -- a FYERS session from the environment.
//
// On a desktop the read-only data CLIs use data/fyers_session.json, written by
// altair_fyers_login. A headless host (a cloud session, a scheduled job) has no
// browser to log in with, so it may instead be given the day's session in two
// environment variables:
//   ALTAIR_FYERS_CLIENT_ID      the app id, e.g. ABCD1234-100
//   ALTAIR_FYERS_ACCESS_TOKEN   the access token altair_fyers_login obtained
// The file wins when both exist. Neither value is ever printed or logged.
// A FYERS access token lasts one trading day; the variable must be refreshed
// with it. The data CLIs contain no order endpoint, whatever the token allows.

#pragma once

#include <broker/fyers_api.hpp>

#include <cstdlib>
#include <optional>
#include <string>
#include <utility>

namespace altair::fyers_env {

struct EnvSession { std::string client, access; };

namespace detail {
inline std::string env(const char* name) {
#if defined(_MSC_VER)
    char* buf = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&buf, &len, name) != 0 || buf == nullptr) return {};
    std::string out{buf};
    std::free(buf);
    return out;
#else
    const char* v = std::getenv(name);
    return v == nullptr ? std::string{} : std::string{v};
#endif
}
} // namespace detail

/// The session from ALTAIR_FYERS_CLIENT_ID and ALTAIR_FYERS_ACCESS_TOKEN, or
/// nullopt when either is missing or out of bounds.
[[nodiscard]] inline std::optional<EnvSession> from_environment() {
    auto client = detail::env("ALTAIR_FYERS_CLIENT_ID");
    auto access = detail::env("ALTAIR_FYERS_ACCESS_TOKEN");
    if (client.empty() || access.empty() || client.size() > altair::fyers::kClientIdMax
        || access.size() > altair::fyers::kAccessTokenMax) {
        return std::nullopt;
    }
    return EnvSession{std::move(client), std::move(access)};
}

} // namespace altair::fyers_env
