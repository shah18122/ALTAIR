// broker/kite_session.hpp — the Kite Connect login handshake.
//
// P2-10b. `broker/` is a new component and exists for one reason: it is the
// ONLY directory that ever touches a credential. `feed/` needs an access token
// to open a socket and `oms/` needs one to place an order, so the session
// cannot live in either without the other including it — and a client that can
// include an OMS header is a client that can be made to trade. One component,
// one directory; this one is also one blast radius.
//
// WHAT THIS FILE DOES: builds the login URL, and computes the session checksum.
// WHAT IT DOES NOT DO: open a socket, read an environment variable, or hold a
// secret beyond the call that was handed one.
//
// The handshake, from Zerodha's own client (gokiteconnect/user.go
// GenerateSession, connect.go GetLoginURL):
//
//   1. Send the user to  https://kite.zerodha.com/connect/login?api_key=K&v=3
//   2. They log in IN A BROWSER. Zerodha redirects to the registered
//      redirect_url carrying ?request_token=...
//   3. POST /session/token with api_key, request_token, and
//      checksum = SHA256(api_key + request_token + api_secret), lowercase hex
//   4. The response carries access_token, valid until the next morning.
//
// Step 2 cannot be automated and must not be: it needs a Zerodha user ID,
// password and TOTP. Step 3 is the one that fails silently — Kite answers a
// wrong checksum with a generic error that does not say whether the fault is
// the concatenation order, the encoding, or the hash. So step 3's input is
// computed here and proven against NIST vectors, and only the HTTP call is
// left for when vcpkg unblocks.

#pragma once

#include <broker/sha256.hpp>
#include <core/time/timestamp.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>

namespace altair {

inline constexpr std::size_t kKiteApiKeyMax = 64;
inline constexpr std::size_t kKiteTokenMax = 128;
inline constexpr std::size_t kKiteSecretMax = 128;
inline constexpr std::size_t kKiteLoginUrlMax = 256;

enum class KiteAuthError : std::uint8_t {
    MissingApiKey,      // empty, or longer than kKiteApiKeyMax
    MissingRequestToken,
    MissingSecret,
    BufferTooSmall
};

/// Build the browser login URL. UNIT: none.
///
/// `v=3` is the Kite Connect version and is part of the contract, not a
/// nicety — the endpoint rejects a request without it.
///
/// The API key is NOT a secret: it travels in this URL, in plain sight, in the
/// user's address bar. The secret never appears here.
[[nodiscard]] inline std::expected<std::size_t, KiteAuthError>
kite_login_url(const char* api_key, char* out, std::size_t cap) noexcept {
    if (api_key == nullptr || api_key[0] == '\0') {
        return std::unexpected(KiteAuthError::MissingApiKey);
    }
    const std::size_t klen = std::strlen(api_key);
    if (klen > kKiteApiKeyMax) {
        return std::unexpected(KiteAuthError::MissingApiKey);
    }
    const int n = std::snprintf(
        out, cap, "https://kite.zerodha.com/connect/login?api_key=%s&v=3",
        api_key);
    if (n < 0 || static_cast<std::size_t>(n) >= cap) {
        return std::unexpected(KiteAuthError::BufferTooSmall);
    }
    return static_cast<std::size_t>(n);
}

/// The session checksum: SHA256(api_key + request_token + api_secret).
/// UNIT: none. `out` must hold kSha256HexChars + 1 bytes.
///
/// Concatenation ORDER is load-bearing and unguessable from the error Kite
/// returns. The three parts are joined with no separator and no encoding.
///
/// The secret is read from the caller's buffer and never copied, stored or
/// logged. Nothing in this file retains it past the call.
[[nodiscard]] inline std::expected<void, KiteAuthError>
kite_session_checksum(const char* api_key, const char* request_token,
                      const char* api_secret, char* out,
                      std::size_t cap) noexcept {
    if (api_key == nullptr || api_key[0] == '\0') {
        return std::unexpected(KiteAuthError::MissingApiKey);
    }
    if (request_token == nullptr || request_token[0] == '\0') {
        return std::unexpected(KiteAuthError::MissingRequestToken);
    }
    if (api_secret == nullptr || api_secret[0] == '\0') {
        return std::unexpected(KiteAuthError::MissingSecret);
    }
    if (cap < kSha256HexChars + 1) {
        return std::unexpected(KiteAuthError::BufferTooSmall);
    }
    Sha256 h;
    h.update(api_key);
    h.update(request_token);
    h.update(api_secret);
    const Sha256Digest d = h.finish();
    sha256_hex(d, out);
    return {};
}

/// What a successful exchange yields. Filled by the HTTP half (P2-10c, blocked
/// on vcpkg) and persisted to data/kite_session.json, which is gitignored.
///
/// The access token is a DAILY artifact: it expires the next morning and is
/// obtained by an interactive login, so it is never an environment variable
/// and never a config value.
struct KiteSession {
    char access_token[kKiteTokenMax + 1]{};
    char public_token[kKiteTokenMax + 1]{};
    char user_id[32]{};
    Timestamp issued_at{};
};

/// The endpoint the checksum is posted to. Config will carry this; the literal
/// is here so the card is self-contained.
inline constexpr const char* kKiteSessionEndpoint = "https://api.kite.trade/session/token";

} // namespace altair
