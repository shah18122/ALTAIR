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

/// What to DO about a refusal, chosen from what Kite actually said.
///
/// THE OLD MESSAGE SENT YOU TO DO THE ONE THING THAT CANNOT HELP.
///
/// `LoginError::Refused` printed "a request_token is SINGLE USE and expires in
/// minutes -- log in again for a fresh one" for every 4xx. That is right for a
/// spent token and WRONG for a bad checksum, and Kite returns both as 403
/// TokenException. `kite_login.hpp` carries `message` and `error_type` out
/// precisely so they can be told apart -- its own header says these are "the
/// same status code and completely different bugs" -- and this function threw
/// that away and guessed.
///
/// Measured cost: three login round trips in one session, each one burning a
/// fresh token, against an api_secret that was never going to work. A wrong
/// instruction is worse than no instruction, because the user follows it.
const char* explain_refusal(const altair::LoginFailure& f)
{
    // Kite's message is the authority. Matching on its text is fragile if
    // Zerodha rewords it, so the fallback below says nothing rather than
    // guessing -- an unrecognised refusal prints Kite's own words and stops.
    if (f.message.find("checksum") != std::string::npos) {
        return "the CHECKSUM did not match, which means your API SECRET does "
               "not go with your API KEY.\n"
               "    The request_token was fine. Logging in again will fail "
               "identically.\n"
               "    Re-copy the API secret from the Kite developer console "
               "(Apps -> your app),\n"
               "    set ALTAIR_KITE_API_SECRET, and open a NEW terminal so it "
               "is inherited.\n"
               "    A Kite secret is 32 characters of a-z and 0-9.";
    }
    if (f.message.find("expired") != std::string::npos
        || f.message.find("invalid") != std::string::npos
        || f.message.find("Invalid") != std::string::npos) {
        return "the request_token is spent or expired. It is SINGLE USE and "
               "lasts minutes --\n    log in again for a fresh one. Retrying "
               "with this one cannot work.";
    }
    return "see Kite's message above -- it names the cause.";
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
        // Deliberately GENERIC. `explain_refusal` below is what a refusal
        // should actually print, because the cause is in Kite's message and
        // not in the error code -- see the note there.
        return "Kite refused the exchange";
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
        // WHAT TO FIX, chosen from Kite's message rather than from the
        // status code. Printed LAST so it is the line still on screen.
        if (f.code == altair::LoginError::Refused) {
            std::printf("\n  WHAT TO FIX: %s\n", explain_refusal(f));
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
