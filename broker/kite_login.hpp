// broker/kite_login.hpp -- the /session/token exchange, in C++.
//
// P2-10c. This is what `broker/tools/kite_login.py` was standing in for. The
// checksum half has been here and NIST-verified since P2-10a
// (`broker/kite_session.hpp`); only the one HTTPS POST was missing, and
// `broker/https_client.hpp` now supplies it.
//
// WHAT ALTAIR CAN AND CANNOT DO IN THIS HANDSHAKE.
//
// Step 1 is a browser login with a Zerodha user id, password and TOTP. Altair
// does not ask for those, cannot supply them, and must not: they are the
// credential that owns the account, not a token scoped to a session. The
// human logs in, and the redirect carries a `request_token` back.
//
// Step 2 is this file: request_token + api_key + SHA256(key+token+secret)
// exchanged for an access_token. That part is arithmetic and one POST, and
// there is no reason for it to have been in Python.
//
// THE REQUEST TOKEN IS SINGLE USE AND EXPIRES IN MINUTES. A failed exchange is
// not retryable with the same token -- the login has to be redone. So the
// errors here distinguish "the network did not work, try again" from "this
// token is spent, go log in", because they call for opposite actions.
//
// THE ACCESS TOKEN IS A LIVE TRADING CREDENTIAL until the next morning. It is
// written to data/kite_session.json (gitignored) and NEVER logged, never put
// in an environment variable, never in config. The secret is read from the
// environment, used once, and not stored anywhere by this file.

#pragma once

#include <broker/https_client.hpp>
#include <broker/kite_session.hpp>

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace altair {

enum class LoginError : std::uint8_t {
    /// ALTAIR_KITE_API_KEY or ALTAIR_KITE_API_SECRET is unset or empty.
    MissingCredentials,
    /// No request_token was supplied.
    MissingRequestToken,
    /// DNS did not resolve. Retryable, and almost always local.
    ResolveFailed,
    /// TCP connect failed. Retryable.
    ConnectFailed,
    /// The TLS handshake failed -- an untrusted chain, or a certificate not
    /// for this host. NOT a transient: something is wrong with the trust
    /// store or with the connection, and retrying repeats it. Kept separate
    /// from ConnectFailed because the two call for completely different
    /// investigations, which one debugging session made expensive to learn.
    TlsFailed,
    /// The request or response could not be moved. Retryable.
    TransportFailed,
    /// Kite refused the exchange -- almost always an expired or already-used
    /// request_token. NOT retryable: log in again.
    Refused,
    /// A 200 whose body did not contain what the contract says it should.
    MalformedResponse,
    /// The session file could not be written. The token is live and now
    /// unrecorded, which is worse than not having it, so this is loud.
    PersistFailed
};

/// A failure, WITH what Kite said about it.
///
/// A bare error code was not enough, and that was a defect in this file rather
/// than a limitation: the header claimed Kite "puts a machine-readable reason
/// in the body" and then threw the body away, so every refusal read as the
/// same generic sentence. A spent token, an expired token, a wrong checksum
/// and a disabled app are four different problems with four different fixes.
struct LoginFailure {
    LoginError code = LoginError::Refused;
    /// Kite's own `message`, when it gave one. Empty otherwise.
    std::string message;
    /// Kite's `error_type`, when it gave one.
    std::string error_type;
    /// HTTP status, 0 when the exchange never happened.
    unsigned status = 0;
};

/// What came back.
///
/// CONTAINS the `KiteSession` that P2-10a declared, rather than defining a
/// parallel one -- that struct's comment says it is "filled by the HTTP half
/// (P2-10c)", and this is the HTTP half. The extra fields here are display
/// only: Kite returns them, an operator wants to see them, and none of them
/// belongs in a fixed-size struct that other code copies around.
struct KiteLoginResult {
    KiteSession session{};       ///< the P2-10a struct, filled
    std::string user_name;
    std::string broker;
    std::string login_time;
    std::string raw_json;        ///< the full response, for the session file
};

namespace detail {
/// Copy into a fixed buffer, NUL-terminating and refusing to overflow.
[[nodiscard]] inline bool copy_into(char* dst, std::size_t cap,
                                    std::string_view src) noexcept {
    if (src.size() >= cap) { return false; }
    for (std::size_t i = 0; i < src.size(); ++i) { dst[i] = src[i]; }
    dst[src.size()] = '\0';
    return true;
}

/// Pull a top-level string field out of a flat JSON object.
///
/// Deliberately not a JSON parser. The response shape is fixed by Kite's
/// documented contract and the fields wanted are all strings at a known depth,
/// so a scanner is enough and adds no dependency to a header that already
/// pulls in OpenSSL and Beast. If this ever needs to understand nesting, use
/// simdjson (already vendored by the `net` feature) rather than growing this.
[[nodiscard]] inline std::string json_string_field(std::string_view body,
                                                   std::string_view key) {
    std::string needle;
    needle.reserve(key.size() + 3);
    needle += '"';
    needle.append(key);
    needle += '"';
    std::size_t p = body.find(needle);
    if (p == std::string_view::npos) { return {}; }
    p = body.find(':', p + needle.size());
    if (p == std::string_view::npos) { return {}; }
    ++p;
    while (p < body.size() && (body[p] == ' ' || body[p] == '\t')) { ++p; }
    if (p >= body.size() || body[p] != '"') { return {}; }   // null or number
    ++p;
    std::string out;
    while (p < body.size() && body[p] != '"') {
        if (body[p] == '\\' && p + 1 < body.size()) { ++p; }
        out += body[p];
        ++p;
    }
    return out;
}

} // namespace detail

/// Exchange a request_token for an access_token.
///
/// `api_key` and `api_secret` come from the caller, which reads them from the
/// environment. The secret is used to build one checksum and is not retained.
[[nodiscard]] inline std::expected<KiteLoginResult, LoginFailure>
kite_exchange_token(std::string_view api_key, std::string_view api_secret,
                    std::string_view request_token, Timestamp now) {
    if (api_key.empty() || api_secret.empty()) {
        return std::unexpected(LoginFailure{LoginError::MissingCredentials, {}, {}, 0});
    }
    if (request_token.empty()) {
        return std::unexpected(LoginFailure{LoginError::MissingRequestToken, {}, {}, 0});
    }

    // The checksum, from the P2-10a header that is verified against NIST
    // vectors. Not recomputed here: one implementation, already proven, and
    // its buffer-based signature is used as it stands rather than wrapped --
    // a second path to the same hash is a second path to get it wrong.
    const std::string key_s{api_key};
    const std::string tok_s{request_token};
    const std::string sec_s{api_secret};
    char checksum[kSha256HexChars + 1]{};
    if (!kite_session_checksum(key_s.c_str(), tok_s.c_str(), sec_s.c_str(),
                               checksum, sizeof checksum)) {
        return std::unexpected(LoginFailure{LoginError::MissingCredentials, {}, {}, 0});
    }

    std::string body;
    body.reserve(api_key.size() + request_token.size() + 48
                 + kSha256HexChars);
    body += "api_key=";        body.append(api_key);
    body += "&request_token="; body.append(request_token);
    body += "&checksum=";      body += checksum;

    const auto res = https_post_form("api.kite.trade", "/session/token", body);
    if (!res) {
        switch (res.error()) {
        case HttpError::ResolveFailed:
            return std::unexpected(LoginFailure{LoginError::ResolveFailed, {}, {}, 0});
        case HttpError::ConnectFailed:
            return std::unexpected(LoginFailure{LoginError::ConnectFailed, {}, {}, 0});
        case HttpError::TlsFailed:
            return std::unexpected(LoginFailure{LoginError::TlsFailed, {}, {}, 0});
        case HttpError::TransportFailed:
        case HttpError::Unknown:
            break;
        }
        return std::unexpected(LoginFailure{LoginError::TransportFailed, {}, {}, 0});
    }

    if (res->status != 200) {
        // Kite answers a spent or expired request_token with 400/403, and puts
        // the actual reason in the body. Carry it out: "Token is invalid or
        // has expired" and "Invalid `checksum`" are the same status code and
        // completely different bugs.
        return std::unexpected(LoginFailure{
            LoginError::Refused,
            detail::json_string_field(res->body, "message"),
            detail::json_string_field(res->body, "error_type"),
            res->status});
    }

    const std::string access = detail::json_string_field(res->body,
                                                         "access_token");
    if (access.empty()) {
        return std::unexpected(LoginFailure{LoginError::MalformedResponse, {}, {}, res->status});
    }

    KiteLoginResult r{};
    r.raw_json = res->body;
    r.user_name = detail::json_string_field(res->body, "user_name");
    r.broker = detail::json_string_field(res->body, "broker");
    r.login_time = detail::json_string_field(res->body, "login_time");
    r.session.issued_at = now;

    // Overflow REFUSES rather than truncating. A truncated access token is a
    // token that looks present and authenticates nothing, which fails later
    // and somewhere else.
    if (!detail::copy_into(r.session.access_token,
                           sizeof r.session.access_token, access)
        || !detail::copy_into(r.session.public_token,
                              sizeof r.session.public_token,
                              detail::json_string_field(res->body,
                                                        "public_token"))
        || !detail::copy_into(r.session.user_id, sizeof r.session.user_id,
                              detail::json_string_field(res->body,
                                                        "user_id"))) {
        return std::unexpected(LoginFailure{LoginError::MalformedResponse, {}, {}, res->status});
    }
    return r;
}

/// A version of the access token safe to print: first six and last four.
///
/// Exists so that a caller wanting to show progress has something to show
/// that is not the credential. Never print `access_token` itself.
[[nodiscard]] inline std::string redact(std::string_view token) {
    if (token.size() <= 12) { return std::string(token.size(), '*'); }
    std::string out;
    out.append(token.substr(0, 6));
    out += "...";
    out.append(token.substr(token.size() - 4));
    return out;
}

} // namespace altair
