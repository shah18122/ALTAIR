// app/kite_account_main.cpp -- fetch the READ-ONLY account state into a
// snapshot the UI can render.
//
// P20-01.
//
// WHY THIS IS A SEPARATE BINARY AND NOT A BUTTON IN THE WINDOW.
//
// The obvious design is a "Refresh" button in the Kite panel that calls the
// API. It cannot be built, and the reason is structural rather than
// fastidious: `desktop/` may not link `broker/`. That is enforced by the
// allow-list in `desktop/CMakeLists.txt` (gate 3) and it exists because
// CLAUDE.md's in-process decision gave up blast radius to gain a single
// address space, and kept ONE safety property by construction -- THE UI CANNOT
// TRADE.
//
// A UI that can reach the broker's HTTP surface has that property only by the
// good manners of whoever writes the next panel. So the network half lives
// here, in a tool that runs by hand, and the window reads a FILE.
//
// The cost is real and worth naming: the panel shows a snapshot, not live
// state, and a snapshot can be stale. That is handled the way this project
// handles every other staleness -- the age is on screen, in words, next to the
// number. A funds figure with no timestamp is worse than no funds figure,
// because it will be believed.
//
// EVERY ENDPOINT HERE IS READ-ONLY.
//
//   /user/profile          who the session belongs to
//   /user/margins          funds, per segment
//   /portfolio/positions   what is open today
//   /portfolio/holdings    what is held overnight
//   /orders                today's order book
//
// Place, modify, cancel, GTT, and mutual-fund orders are NOT here and will not
// be. They belong to `oms/`, which is the only directory permitted to place an
// order, and a "convenience" fetch tool that could also send one would be the
// exact hole the allow-list exists to close.

#include <broker/https_client.hpp>
#include <broker/account_snapshot_parser.hpp>
#include <broker/kite_api.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

/// Same shape as kite_fetch_main.cpp's and kite_login_main.cpp's. MSVC
/// deprecates getenv and gate 1 is zero warnings, so the platform split is
/// explicit rather than suppressed with _CRT_SECURE_NO_WARNINGS -- which would
/// silence every other instance of the check too. Copied rather than
/// reinvented so all three Kite tools agree on how a credential is read.
[[nodiscard]] std::string env_or_empty(const char* name) {
#if defined(_MSC_VER)
    char* buf = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&buf, &len, name) != 0 || buf == nullptr) { return {}; }
    std::string out{buf};
    std::free(buf);
    return out;
#else
    const char* v = std::getenv(name);
    return v != nullptr ? std::string{v} : std::string{};
#endif
}

/// Read `access_token` out of data/kite_session.json.
///
/// Its own reader rather than a JSON library: the file holds a LIVE TRADING
/// CREDENTIAL and the smallest amount of code that can see it is the right
/// amount. Never printed, never logged, never put in an error message.
[[nodiscard]] bool read_access_token(const char* path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { return false; }
    const std::string all((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
    const std::string key = "\"access_token\"";
    const auto k = all.find(key);
    if (k == std::string::npos) { return false; }
    const auto q1 = all.find('"', all.find(':', k) + 1);
    if (q1 == std::string::npos) { return false; }
    const auto q2 = all.find('"', q1 + 1);
    if (q2 == std::string::npos) { return false; }
    out = all.substr(q1 + 1, q2 - q1 - 1);
    return !out.empty();
}

struct Fetched {
    const char* name;
    const char* target;
    long status = 0;
    std::string body;
};

void usage(const char* exe) {
    std::printf(
        "  Snapshot the READ-ONLY Kite account state for the UI.\n\n"
        "    %s [--out PATH] [--go]\n\n"
        "    --out PATH   where to write (default data/kite_account.json)\n"
        "    --go         ACTUALLY CALL THE API. Without it this prints the\n"
        "                 requests it would make and exits.\n\n"
        "  Endpoints, all read-only:\n"
        "    /user/profile  /user/margins  /portfolio/positions\n"
        "    /portfolio/holdings  /orders\n\n"
        "  Place, modify, cancel and GTT are DELIBERATELY ABSENT. They belong\n"
        "  to oms/, the only directory permitted to place an order.\n\n"
        "  Needs data/kite_session.json (altair_kite_login writes it) and\n"
        "  ALTAIR_KITE_API_KEY in the environment. The access token is read\n"
        "  from the file and never printed.\n", exe);
}

[[nodiscard]] std::string json_quote(std::string_view text) {
    std::string out{"\""};
    for (const char c : text) {
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

} // namespace

int main(int argc, char** argv) {
    std::string out_path = "data/kite_account.json";
    bool go = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0) { usage(argv[0]); return 0; }
        if (std::strcmp(argv[i], "--go") == 0) { go = true; }
        if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        }
    }

    std::vector<Fetched> want = {
        {"profile",  "/user/profile"},
        {"margins",  "/user/margins"},
        {"positions", "/portfolio/positions"},
        {"holdings", "/portfolio/holdings"},
        {"orders",   "/orders"},
    };

    std::printf("  Kite account snapshot -> %s\n\n", out_path.c_str());
    for (const Fetched& f : want) {
        std::printf("    GET https://api.kite.trade%s\n", f.target);
    }
    if (!go) {
        std::printf("\n  DRY RUN. Nothing was sent and nothing was written.\n"
                    "  Add --go to make these calls for real.\n");
        return 0;
    }

    const std::string api_key = env_or_empty("ALTAIR_KITE_API_KEY");
    if (api_key.empty()) {
        std::printf("\n  ALTAIR_KITE_API_KEY is not set. Refusing.\n");
        return 2;
    }
    std::string access;
    if (!read_access_token("data/kite_session.json", access)) {
        std::printf("\n  no usable data/kite_session.json -- run "
                    "altair_kite_login first.\n");
        return 2;
    }
    const std::string auth =
        std::string("token ") + api_key + ":" + access;

    std::printf("\n");
    bool any_ok = false;
    for (Fetched& f : want) {
        const auto r = altair::https_get_auth(
            "api.kite.trade", f.target, auth, altair::kite::kVersion,
            std::chrono::seconds{20});
        if (!r) {
            std::printf("    %-10s TRANSPORT FAILED\n", f.name);
            continue;
        }
        f.status = r->status;
        f.body = r->body;
        std::printf("    %-10s HTTP %ld  %zu bytes\n",
                    f.name, f.status, f.body.size());
        if (f.status == 200) { any_ok = true; }
        else if (f.status == 403) {
            // The one error worth naming: the token is daily and this is what
            // an expired one looks like. Same message the pill shows.
            std::printf("               token rejected -- Kite sessions are "
                        "daily. Run altair_kite_login.\n");
        }
    }
    if (!any_ok) {
        std::printf("\n  Nothing succeeded; not writing a snapshot.\n"
                    "  An EMPTY snapshot would read as an empty ACCOUNT.\n");
        return 1;
    }

    const auto now = std::time(nullptr);
    const auto seconds = static_cast<std::int64_t>(now);
    if (seconds <= 0 || seconds > 9'223'372'036LL) {
        std::printf("\n  local UTC clock is outside the supported range.\n");
        return 1;
    }
    const altair::broker_view::EvidenceWindow observed{
        altair::Timestamp{seconds * 1'000'000'000LL},
        altair::Timestamp{seconds * 1'000'000'000LL + 30'000'000'000LL}};
    const auto response = [&want](std::size_t i) {
        return altair::broker_view::ProviderResponse{
            static_cast<int>(want[i].status), want[i].body};
    };
    const auto typed = altair::broker_view::parse_account_snapshot(
        altair::broker_view::BrokerId::ZerodhaKite,
        {altair::broker_view::BrokerId::ZerodhaKite, 1, 1}, observed,
        response(0), response(1), response(2), response(3), response(4),
        "user_id");
    if (!typed) {
        std::printf("\n  Kite responses did not form a valid typed account snapshot; not writing.\n");
        return 1;
    }

    // Written temp-then-rename, so the UI never reads a half-written file.
    // The same argument as the warm-restart snapshot in P12-02: a torn file
    // parses.
    const std::string tmp = out_path + ".tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) {
            std::printf("\n  cannot write %s\n", tmp.c_str());
            return 1;
        }
        o << "{\n  \"schema_version\": 1,\n"
          << "  \"broker\": \"ZERODHA_KITE\",\n"
          << "  \"fetched_at_unix\": " << static_cast<long long>(now)
          << ",\n  \"service_epoch\": 1"
          << ",\n  \"auth_verified_at_unix\": " << static_cast<long long>(now)
          << ",\n  \"auth_expires_at_unix\": " << static_cast<long long>(now + 30)
          << ",\n  \"account_expires_at_unix\": " << static_cast<long long>(now + 30)
          << ",\n  \"feed_status\": \"disabled\""
          << ",\n  \"account_id\": "
          << json_quote(typed->account_id.data()) << ",\n";
        for (std::size_t i = 0; i < want.size(); ++i) {
            o << "  \"" << want[i].name << "_status\": " << want[i].status
              << ",\n";
            o << "  \"" << want[i].name << "\": ";
            if (want[i].status == 200 && !want[i].body.empty()) {
                o << want[i].body;
            } else {
                // NULL, not {}. An empty object reads as "no positions";
                // null reads as "not fetched", and those are different facts.
                o << "null";
            }
            o << (i + 1 < want.size() ? ",\n" : "\n");
        }
        o << "}\n";
        if (!o) { return 1; }
    }
    std::remove(out_path.c_str());
    if (std::rename(tmp.c_str(), out_path.c_str()) != 0) {
        std::printf("\n  could not rename %s -> %s\n",
                    tmp.c_str(), out_path.c_str());
        return 1;
    }
    std::printf("\n  wrote %s\n", out_path.c_str());
    std::printf("  The panel shows this with its AGE. A funds figure with no\n"
                "  timestamp is worse than none, because it gets believed.\n");
    return 0;
}
