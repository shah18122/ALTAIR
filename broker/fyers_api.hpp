// broker/fyers_api.hpp -- FYERS API v3 authentication vocabulary.
//
// This is the credential-boundary vocabulary shared by the FYERS login helper
// and read-only tests. It builds the browser URL, app-id hash and Authorization
// header without opening a socket, reading an environment variable or placing
// an order. The networked OAuth exchange lives in app/fyers_login_main.cpp,
// outside the desktop process.

#pragma once

#include <broker/sha256.hpp>

#include <cstddef>
#include <array>
#include <cstdio>
#include <cstring>
#include <expected>
#include <string>
#include <string_view>

namespace altair::fyers {

inline constexpr const char* kAuthBaseUri =
    "https://api-t1.fyers.in/api/v3";
inline constexpr const char* kGenerateAuthCodePath =
    "/generate-authcode";
inline constexpr const char* kValidateAuthCodePath =
    "/validate-authcode";
inline constexpr const char* kValidateRefreshTokenPath =
    "/validate-refresh-token";
inline constexpr const char* kDataBaseUri = "https://api-t1.fyers.in";

struct ReadOnlyAccountEndpoint {
    const char* name;
    const char* path;
};

/// The complete read-only account surface used by Altair. Keeping this list
/// here prevents the fetch helper from drifting into order-capable endpoints.
inline constexpr std::array<ReadOnlyAccountEndpoint, 5> kAccountEndpoints{{
    {"profile", "/api/v3/profile"},
    {"funds", "/api/v3/funds"},
    {"positions", "/api/v3/positions"},
    {"holdings", "/api/v3/holdings"},
    {"orders", "/api/v3/orders"},
}};

/// The trade book, fetched beside the five above for the GETS trade-history
/// and expense screens. Kept out of kAccountEndpoints so the typed snapshot's
/// five sections keep their order; still a read-only GET.
inline constexpr ReadOnlyAccountEndpoint kTradebookEndpoint{"tradebook", "/api/v3/tradebook"};

/// Market quotes (LTP, change, OHLC, previous close) for up to 50 symbols per
/// request, on the data host. Read-only.
inline constexpr const char* kQuotesPath = "/data/quotes";
inline constexpr std::size_t kQuotesMaxSymbols = 50;
inline constexpr std::size_t kQuotesTargetMax = 8192;

enum class QuotesError : unsigned char { Empty, TooMany, BadSymbol, TooLong };

/// "/data/quotes?symbols=NSE%3ASBIN-EQ%2CNSE%3AM%26M-EQ". Every byte outside
/// the URL-unreserved set is percent-encoded, so '&' in "M&M" cannot end the
/// query early. Refuses an empty list, more than kQuotesMaxSymbols symbols,
/// an empty or comma-bearing symbol, or a target longer than kQuotesTargetMax.
template <typename Range>
[[nodiscard]] inline std::expected<std::string, QuotesError>
quotes_target(const Range& symbols) {
    std::string out{kQuotesPath};
    out += "?symbols=";
    std::size_t count = 0;
    constexpr char kHex[] = "0123456789ABCDEF";
    for (const auto& symbol : symbols) {
        const std::string_view sv{symbol};
        if (sv.empty() || sv.find(',') != std::string_view::npos)
            return std::unexpected(QuotesError::BadSymbol);
        if (++count > kQuotesMaxSymbols) return std::unexpected(QuotesError::TooMany);
        if (count > 1) out += "%2C";
        for (const char ch : sv) {
            const auto c = static_cast<unsigned char>(ch);
            const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
            if (unreserved) {
                out += ch;
            } else {
                out += '%';
                out += kHex[c >> 4];
                out += kHex[c & 0x0F];
            }
        }
        if (out.size() > kQuotesTargetMax) return std::unexpected(QuotesError::TooLong);
    }
    if (count == 0) return std::unexpected(QuotesError::Empty);
    return out;
}

inline constexpr std::size_t kClientIdMax = 128;
inline constexpr std::size_t kAuthCodeMax = 256;
inline constexpr std::size_t kSecretMax = 256;
// FYERS access tokens are opaque OAuth artifacts and are no longer safely
// modelled as short JWT-sized values. Keep a defensive upper bound, but do not
// reject current multi-kilobyte tokens merely because an old stack buffer was
// sized for 512 bytes.
inline constexpr std::size_t kAccessTokenMax = 16 * 1024;
inline constexpr std::size_t kLoginUrlMax = 1024;
inline constexpr std::size_t kHeaderMax = kClientIdMax + 1 + kAccessTokenMax + 1;

enum class AuthError : unsigned char {
    MissingClientId,
    MissingRedirectUri,
    MissingState,
    MissingAuthCode,
    MissingSecret,
    MissingAccessToken,
    BufferTooSmall,
};

[[nodiscard]] inline bool bounded_nonempty(const char* value,
                                           std::size_t max) noexcept {
    return value != nullptr && value[0] != '\0' && std::strlen(value) <= max;
}

/// Build the FYERS browser login URL.
///
/// `redirect_uri` must already be percent-encoded.  Encoding belongs at the
/// UI/browser boundary; silently encoding twice would produce a different
/// callback URI from the one registered in the FYERS app.  `state` is a
/// caller-generated CSRF value and must be URL-safe.
[[nodiscard]] inline std::expected<std::size_t, AuthError>
login_url(const char* client_id, const char* redirect_uri,
          const char* state, char* out, std::size_t cap) noexcept {
    if (!bounded_nonempty(client_id, kClientIdMax)) {
        return std::unexpected(AuthError::MissingClientId);
    }
    if (!bounded_nonempty(redirect_uri, kLoginUrlMax)) {
        return std::unexpected(AuthError::MissingRedirectUri);
    }
    if (!bounded_nonempty(state, kAuthCodeMax)) {
        return std::unexpected(AuthError::MissingState);
    }
    if (out == nullptr || cap == 0) {
        return std::unexpected(AuthError::BufferTooSmall);
    }
    const int n = std::snprintf(
        out, cap,
        "%s%s?client_id=%s&redirect_uri=%s&response_type=code&state=%s",
        kAuthBaseUri, kGenerateAuthCodePath, client_id, redirect_uri,
        state);
    if (n < 0 || static_cast<std::size_t>(n) >= cap) {
        return std::unexpected(AuthError::BufferTooSmall);
    }
    return static_cast<std::size_t>(n);
}

/// Build the SHA-256 appIdHash expected by validate-authcode.
///
/// The input is the byte sequence `<client_id>:<secret>`.  The secret is
/// consumed only for this call and is not copied into the returned value.
[[nodiscard]] inline std::expected<void, AuthError>
app_id_hash(const char* client_id, const char* secret, char* out,
            std::size_t cap) noexcept {
    if (!bounded_nonempty(client_id, kClientIdMax)) {
        return std::unexpected(AuthError::MissingClientId);
    }
    if (!bounded_nonempty(secret, kSecretMax)) {
        return std::unexpected(AuthError::MissingSecret);
    }
    if (out == nullptr || cap < kSha256HexChars + 1) {
        return std::unexpected(AuthError::BufferTooSmall);
    }
    Sha256 hash;
    hash.update(client_id);
    hash.update(":");
    hash.update(secret);
    sha256_hex(hash.finish(), out);
    return {};
}

/// Build the request Authorization value: `<client_id>:<access_token>`.
///
/// FYERS access tokens are daily artifacts.  This helper intentionally does
/// not add a `Bearer` prefix and never stores the token.
[[nodiscard]] inline std::expected<std::string, AuthError>
authorization_header(const char* client_id, const char* access_token) {
    if (!bounded_nonempty(client_id, kClientIdMax)) {
        return std::unexpected(AuthError::MissingClientId);
    }
    if (!bounded_nonempty(access_token, kAccessTokenMax)) {
        return std::unexpected(AuthError::MissingAccessToken);
    }
    std::string value{client_id};
    value.reserve(value.size() + 1 + std::strlen(access_token));
    value.push_back(':');
    value.append(access_token);
    return value;
}

/// Compatibility overload for callers that own an appropriately sized buffer.
[[nodiscard]] inline std::expected<std::size_t, AuthError>
authorization_header(const char* client_id, const char* access_token,
                      char* out, std::size_t cap) {
    const auto value = authorization_header(client_id, access_token);
    if (!value) return std::unexpected(value.error());
    if (out == nullptr || cap == 0) {
        return std::unexpected(AuthError::BufferTooSmall);
    }
    if (value->size() >= cap) {
        return std::unexpected(AuthError::BufferTooSmall);
    }
    std::memcpy(out, value->data(), value->size());
    out[value->size()] = '\0';
    return value->size();
}

}  // namespace altair::fyers
