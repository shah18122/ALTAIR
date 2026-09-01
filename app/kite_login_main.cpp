// app/kite_login_main.cpp -- the C++ Kite login, replacing the Python stopgap.
//
//     altair_kite_login "<the whole redirect URL, or the bare request_token>"
//
// Step 1 of the handshake is a browser login with a Zerodha user id, password
// and TOTP. This program does not ask for those and cannot supply them. It
// prints the login URL, you log in, and you hand back what the redirect
// carried. Step 2 -- the checksum and the POST -- is what this does.
//
// The api key and secret come from the environment and are never printed. The
// access token is written to data/kite_session.json (gitignored) and is shown
// only redacted.

#include <broker/kite_login.hpp>
#include <core/time/timestamp.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace {

/// Accept either a bare request_token or the whole redirect URL, because
/// copying the entire address bar is what actually happens.
std::string request_token_from(std::string_view arg)
{
    const std::size_t q = arg.find("request_token=");
    if (q == std::string_view::npos) { return std::string{arg}; }
    std::string_view rest = arg.substr(q + 14);
    const std::size_t amp = rest.find('&');
    if (amp != std::string_view::npos) { rest = rest.substr(0, amp); }
    return std::string{rest};
}

std::string env_or_empty(const char* name)
{
#if defined(_MSC_VER)
    char* buf = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&buf, &len, name) != 0 || buf == nullptr) { return {}; }
    std::string out{buf};
    std::free(buf);
    return out;
#else
    const char* v = std::getenv(name);
    return v ? std::string{v} : std::string{};
#endif
}

const char* explain(altair::LoginError e)
{
    using altair::LoginError;
    switch (e) {
    case LoginError::MissingCredentials:
        return "ALTAIR_KITE_API_KEY / ALTAIR_KITE_API_SECRET are not set";
    case LoginError::MissingRequestToken:
        return "no request_token in the argument";
    case LoginError::ResolveFailed:
        return "DNS did not resolve api.kite.trade -- check the network";
    case LoginError::ConnectFailed:
        return "could not connect to api.kite.trade -- retryable";
    case LoginError::TlsFailed:
        return "the TLS handshake did not verify. NOT transient: the trust "
               "store or the connection is wrong, and retrying repeats it";
    case LoginError::TransportFailed:
        return "the request or response could not be moved -- RETRYABLE with "
               "the same token, if it has not expired yet";
    case LoginError::Refused:
        return "Kite refused the exchange. A request_token is SINGLE USE and "
               "expires in minutes -- log in again for a fresh one. Retrying "
               "with this one cannot work";
    case LoginError::MalformedResponse:
        return "Kite answered 200 with a body that did not carry an "
               "access_token";
    case LoginError::PersistFailed:
        return "the session file could not be written";
    }
    return "unknown";
}

} // namespace

int main(int argc, char** argv)
{
    using namespace altair;

    const std::string api_key = env_or_empty("ALTAIR_KITE_API_KEY");
    const std::string api_secret = env_or_empty("ALTAIR_KITE_API_SECRET");
    if (api_key.empty() || api_secret.empty()) {
        std::printf("ALTAIR_KITE_API_KEY and ALTAIR_KITE_API_SECRET must be "
                    "set.\nSet them with setx, then open a NEW terminal.\n");
        return 2;
    }

    if (argc < 2) {
        std::printf("\n  Kite login -- step 2 of 2\n");
        std::printf("  %s\n", std::string(58, '-').c_str());
        std::printf("  1. Open this and log in:\n");
        std::printf("       https://kite.zerodha.com/connect/login"
                    "?api_key=%s&v=3\n", api_key.c_str());
        std::printf("  2. Zerodha redirects to your registered callback with"
                    " ?request_token=...\n");
        std::printf("  3. Run:\n");
        std::printf("       %s \"<paste the whole redirect URL>\"\n", argv[0]);
        std::printf("\n  A request_token is SINGLE USE and expires in"
                    " minutes.\n\n");
        return 1;
    }

    const std::string token = request_token_from(argv[1]);
    if (token.empty()) {
        std::printf("  no request_token found in that argument\n");
        return 1;
    }

    const auto now = Timestamp{
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()};

    std::printf("  exchanging a %zu-character request_token...\n",
                token.size());
    const auto r = kite_exchange_token(api_key, api_secret, token, now);
    if (!r) {
        const LoginFailure& f = r.error();
        std::printf("  exchange did not succeed: %s\n", explain(f.code));
        if (f.status != 0) {
            std::printf("  Kite answered HTTP %u\n", f.status);
        }
        // Kite's own words. A spent token, an expired token and a wrong
        // checksum are all HTTP 400 and are three different bugs.
        if (!f.message.empty()) {
            std::printf("  Kite said     : %s\n", f.message.c_str());
        }
        if (!f.error_type.empty()) {
            std::printf("  error_type    : %s\n", f.error_type.c_str());
        }
        return 1;
    }

    // Persist. The token is live from this moment, so a failure to record it
    // is worse than not having it -- it would be working and untracked.
    std::error_code fs_ec;
    std::filesystem::create_directories("data", fs_ec);
    const char* out_path = "data/kite_session.json";
    std::ofstream out{out_path, std::ios::binary | std::ios::trunc};
    if (!out) {
        std::printf("  SESSION OBTAINED BUT NOT WRITTEN to %s -- the token is "
                    "live and unrecorded\n", out_path);
        return 1;
    }
    out << r->raw_json;
    out.close();
    if (!out) {
        std::printf("  SESSION OBTAINED BUT NOT WRITTEN to %s\n", out_path);
        return 1;
    }

    std::printf("\n  SESSION ESTABLISHED\n");
    std::printf("    user_id      : %s\n", r->session.user_id);
    std::printf("    user_name    : %s\n", r->user_name.c_str());
    std::printf("    broker       : %s\n", r->broker.c_str());
    std::printf("    login_time   : %s\n", r->login_time.c_str());
    std::printf("    access_token : %s  (redacted)\n",
                redact(r->session.access_token).c_str());
    std::printf("    written to   : %s  (gitignored)\n", out_path);
    std::printf("\n  Expires tomorrow morning. Re-run after each login.\n\n");
    return 0;
}
