// app/fyers_login_main.cpp -- FYERS v3 browser login and token exchange.
//
// The desktop never reads a FYERS secret or access token. It launches this
// short-lived helper, which reads credentials from the environment, opens the
// official OAuth URL, validates the returned auth_code, and writes only the
// redacted session metadata to data/fyers_session.json. No order endpoint is
// present in this binary.

#include <broker/fyers_api.hpp>
#include <broker/https_client.hpp>
#include <broker/credential_store.hpp>
#include <broker/oauth_attempt.hpp>

#include <chrono>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>

namespace {

std::string env_or_empty(const char* name) {
#if defined(_MSC_VER)
    char* buf = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&buf, &len, name) != 0 || buf == nullptr) return {};
    std::string out{buf};
    std::free(buf);
    return out;
#else
    const char* value = std::getenv(name);
    return value == nullptr ? std::string{} : std::string{value};
#endif
}

std::string vault_or_env(std::string_view account, const char* environment) {
    const auto stored = altair::broker::load_credential(
        {"altair.fyers", std::string{account}});
    return stored ? *stored : env_or_empty(environment);
}

std::optional<std::string> read_bounded_stdin() {
    constexpr std::size_t kMaxRedirectBytes = 16 * 1024;
    std::string input;
    input.reserve(1024);
    for (int next = std::getc(stdin); next != EOF; next = std::getc(stdin)) {
        if (input.size() == kMaxRedirectBytes) return std::nullopt;
        if (next == '\n') break;
        input.push_back(static_cast<char>(next));
    }
    while (!input.empty()
           && (input.back() == '\r' || input.back() == '\n')) {
        input.pop_back();
    }
    return input;
}

std::string source_path(const char* relative) {
#ifdef ALTAIR_SOURCE_DIR
    return (std::filesystem::path{ALTAIR_SOURCE_DIR} / relative).string();
#else
    return relative;
#endif
}

std::string url_encode(std::string_view value) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size() * 2);
    for (const unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

std::optional<std::string> json_string(std::string_view body,
                                       std::string_view key) {
    const std::string needle = "\"" + std::string{key} + "\"";
    const std::size_t name = body.find(needle);
    if (name == std::string_view::npos) return std::nullopt;
    const std::size_t colon = body.find(':', name + needle.size());
    if (colon == std::string_view::npos) return std::nullopt;
    std::size_t at = colon + 1;
    while (at < body.size() && std::isspace(static_cast<unsigned char>(body[at]))) ++at;
    if (at >= body.size() || body[at] != '"') return std::nullopt;
    ++at;
    std::string out;
    bool escaped = false;
    for (; at < body.size(); ++at) {
        const char c = body[at];
        if (escaped) {
            switch (c) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            default: return std::nullopt;
            }
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == '"') {
            return out;
        } else {
            out.push_back(c);
        }
    }
    return std::nullopt;
}

std::string json_quote(std::string_view value) {
    std::string out{"\""};
    for (const char c : value) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out.push_back(c); break;
        }
    }
    out.push_back('"');
    return out;
}

std::string random_state() {
    std::random_device rd;
    std::mt19937_64 rng{static_cast<std::uint64_t>(rd())
                        ^ (static_cast<std::uint64_t>(rd()) << 32)};
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (int i = 0; i < 4; ++i) out << std::setw(16) << rng();
    return out.str();
}

std::string now_utc() {
    const auto now = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char out[32]{};
    std::strftime(out, sizeof(out), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return out;
}

std::int64_t now_unix_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

bool write_text(const std::string& path, std::string_view text) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path{path}.parent_path(), ec);
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    if (!out) return false;
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(out);
}

std::optional<std::string> read_text(const std::string& path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) return std::nullopt;
    return std::string{std::istreambuf_iterator<char>{in},
                       std::istreambuf_iterator<char>{}};
}

std::optional<altair::broker::OAuthAttempt>
read_attempt(const std::string& path) {
    const auto text = read_text(path);
    if (!text) return std::nullopt;
    std::istringstream in{*text};
    std::string version;
    std::string issued;
    altair::broker::OAuthAttempt attempt;
    if (!std::getline(in, version) || version != "ALTAIR-FYERS-OAUTH/1"
        || !std::getline(in, issued) || !std::getline(in, attempt.state)
        || !std::getline(in, attempt.callback)) {
        return std::nullopt;
    }
    const auto parsed = std::from_chars(
        issued.data(), issued.data() + issued.size(), attempt.issued_unix_seconds);
    if (parsed.ec != std::errc{} || parsed.ptr != issued.data() + issued.size())
        return std::nullopt;
    attempt.lifetime_seconds = 600;
    return attempt;
}

const char* redirect_error(altair::broker::OAuthRedirectError error) {
    using altair::broker::OAuthRedirectError;
    switch (error) {
    case OAuthRedirectError::InvalidAttempt: return "saved login attempt is invalid";
    case OAuthRedirectError::Expired: return "login attempt expired; start a new login";
    case OAuthRedirectError::CallbackMismatch: return "redirect callback does not match the registered URL";
    case OAuthRedirectError::Cancelled: return "login was cancelled or rejected";
    case OAuthRedirectError::MissingCode: return "redirect has no auth_code";
    case OAuthRedirectError::MissingState: return "redirect has no OAuth state";
    case OAuthRedirectError::StateMismatch: return "OAuth state mismatch";
    case OAuthRedirectError::MalformedEncoding: return "redirect query encoding is malformed";
    }
    return "invalid redirect";
}

const char* explain(altair::HttpError e) {
    using altair::HttpError;
    switch (e) {
    case HttpError::ResolveFailed: return "DNS resolution failed";
    case HttpError::ConnectFailed: return "TLS host connection failed";
    case HttpError::TlsFailed: return "TLS verification failed";
    case HttpError::TransportFailed: return "HTTPS exchange failed";
    case HttpError::Unknown: return "unknown HTTPS failure";
    }
    return "unknown";
}

void usage(const std::string& client, const std::string& redirect) {
    const std::string state = random_state();
    if (redirect.find_first_of("?#\r\n") != std::string::npos) {
        std::printf("registered FYERS redirect must be an exact URL without query or fragment\n");
        return;
    }
    const std::string attempt = "ALTAIR-FYERS-OAUTH/1\n"
        + std::to_string(now_unix_seconds()) + "\n" + state + "\n"
        + redirect + "\n";
    if (!write_text(source_path("data/fyers_login_state.txt"), attempt)) {
        std::printf("could not save the one-time OAuth state; refusing login\n");
        return;
    }
    const std::string encoded_redirect = url_encode(redirect);
    char url[altair::fyers::kLoginUrlMax]{};
    const auto built = altair::fyers::login_url(
        client.c_str(), encoded_redirect.c_str(), state.c_str(), url, sizeof(url));
    if (!built) {
        std::printf("could not build the FYERS login URL\n");
        return;
    }
    std::printf("\n  FYERS login -- step 1 of 2\n");
    std::printf("  1. Open this URL in your browser:\n       %s\n", url);
    std::printf("  2. Log in and approve the app. FYERS redirects to:\n       %s?auth_code=...&state=...\n", redirect.c_str());
    std::printf("  3. Run this helper again with the whole redirect URL.\n");
    std::printf("\n  The state is one-time and the auth_code is short-lived.\n\n");
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && (std::string_view{argv[1]} == "--help"
                      || std::string_view{argv[1]} == "-h")) {
        std::printf("FYERS browser login helper (credential-boundary process).\n"
                    "Reads the OS vault, opens no socket until a redirect is supplied,\n"
                    "and never exposes secrets or access tokens in output.\n"
                    "Usage: altair_fyers_login [--stdin | REDIRECT_URL]\n"
                    "The desktop uses --stdin for the one-time redirect.\n");
        return 0;
    }
    const std::string client = vault_or_env("client-id", "ALTAIR_FYERS_CLIENT_ID");
    const std::string secret = vault_or_env("app-secret", "ALTAIR_FYERS_SECRET");
    const std::string configured_redirect =
        vault_or_env("redirect-uri", "ALTAIR_FYERS_REDIRECT_URI");
    const std::string redirect = configured_redirect.empty()
        ? std::string{"https://altair.thesmitshah.com/fyers/callback"}
        : configured_redirect;

    if (client.empty() || secret.empty()) {
        std::printf("FYERS app credentials are not configured in the OS vault "
                    "or this process environment.\n");
        return 2;
    }
    if (argc < 2) {
        usage(client, redirect);
        return 0;
    }

    std::string input;
    if (std::string_view{argv[1]} == "--stdin") {
        const auto piped = read_bounded_stdin();
        if (!piped || piped->empty()) {
            std::printf("a non-empty FYERS redirect of at most 16 KiB is required on stdin\n");
            return 1;
        }
        input = *piped;
    } else {
        // Kept for deliberate command-line use. The desktop always selects
        // --stdin so its one-time auth_code never appears in a process list.
        input = argv[1];
    }
    const std::string attempt_path = source_path("data/fyers_login_state.txt");
    const auto attempt = read_attempt(attempt_path);
    if (!attempt) {
        std::printf("no active FYERS login attempt; open a new login first\n");
        return 1;
    }
    const auto code = altair::broker::validate_oauth_redirect(
        input, *attempt, now_unix_seconds(), "auth_code");
    if (!code) {
        std::printf("FYERS redirect refused: %s\n", redirect_error(code.error()));
        if (code.error() == altair::broker::OAuthRedirectError::Expired
            || code.error() == altair::broker::OAuthRedirectError::Cancelled) {
            std::error_code ignored;
            std::filesystem::remove(attempt_path, ignored);
        }
        return 1;
    }
    // Consume locally before the network exchange. A second process cannot
    // race the same one-time code through Altair.
    std::error_code ignored;
    std::filesystem::remove(attempt_path, ignored);

    char hash[altair::kSha256HexChars + 1]{};
    if (!altair::fyers::app_id_hash(client.c_str(), secret.c_str(), hash, sizeof(hash))) {
        std::printf("could not build the FYERS appIdHash\n");
        return 1;
    }
    const std::string body = "{\"grant_type\":\"authorization_code\",\"appIdHash\":"
                           + json_quote(hash) + ",\"code\":" + json_quote(*code) + "}";
    std::printf("  exchanging the FYERS auth_code (%zu chars)...\n", code->size());
    const auto response = altair::https_post_json(
        "api-t1.fyers.in", "/api/v3/validate-authcode", body);
    if (!response) {
        std::printf("  exchange did not complete: %s\n", explain(response.error()));
        return 1;
    }
    const auto status = json_string(response->body, "s");
    const auto access = json_string(response->body, "access_token");
    if (response->status < 200 || response->status >= 300 || !access || access->empty()
        || !status || *status != "ok") {
        const auto message = json_string(response->body, "message");
        std::printf("  FYERS refused the exchange (HTTP %u)%s\n",
                    response->status,
                    message ? (std::string{" — "} + *message).c_str() : "");
        return 1;
    }

    const auto refresh = json_string(response->body, "refresh_token").value_or("");
    const auto authorization = altair::fyers::authorization_header(
        client.c_str(), access->c_str());
    if (!authorization) {
        std::printf("  token obtained but its fields exceed the authorization boundary\n");
        return 1;
    }
    const auto profile = altair::https_get_auth(
        "api-t1.fyers.in", "/api/v3/profile", *authorization, "");
    const auto profile_status = profile
        ? json_string(profile->body, "s") : std::optional<std::string>{};
    const auto profile_id = profile
        ? json_string(profile->body, "fy_id") : std::optional<std::string>{};
    if (!profile || profile->status < 200 || profile->status >= 300
        || !profile_status || *profile_status != "ok"
        || !profile_id || profile_id->empty()) {
        std::printf("  token obtained but FYERS profile verification failed; session not saved\n");
        return 1;
    }
    const std::string logged_at = now_utc();
    const std::string session =
        "{\"status\":\"success\",\"data\":{" 
        "\"broker\":\"FYERS\",\"client_id\":" + json_quote(client)
        + ",\"profile_id\":" + json_quote(*profile_id)
        + ",\"logged_at\":" + json_quote(logged_at)
        + "},\"access_token\":" + json_quote(*access)
        + ",\"refresh_token\":" + json_quote(refresh) + "}\n";
    const std::string path = source_path("data/fyers_session.json");
    if (!write_text(path, session)) {
        std::printf("  token obtained but session could not be written to %s\n", path.c_str());
        return 1;
    }
    std::printf("\n  FYERS SESSION ESTABLISHED\n");
    std::printf("    client_id    : %s\n", client.c_str());
    std::printf("    access_token : %zu chars (redacted)\n", access->size());
    std::printf("    written to   : %s (gitignored)\n", path.c_str());
    std::printf("\n  The access token is a daily artifact; relink after expiry.\n\n");
    return 0;
}
