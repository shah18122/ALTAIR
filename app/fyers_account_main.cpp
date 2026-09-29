// app/fyers_account_main.cpp -- fetch a validated READ-ONLY FYERS account
// snapshot. No place/modify/cancel endpoint is present in this binary.

#include <broker/account_snapshot_parser.hpp>
#include <broker/fyers_api.hpp>
#include <broker/https_client.hpp>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Session {
    std::string client_id;
    std::string access_token;
};

struct Fetched {
    const char* name{};
    const char* path{};
    unsigned status{};
    std::string body;
};

[[nodiscard]] std::string source_path(const char* relative) {
#ifdef ALTAIR_SOURCE_DIR
    return (std::filesystem::path{ALTAIR_SOURCE_DIR} / relative).string();
#else
    return relative;
#endif
}

[[nodiscard]] std::optional<std::string>
json_string(std::string_view body, std::string_view key) {
    const std::string needle = "\"" + std::string{key} + "\"";
    const std::size_t name = body.find(needle);
    if (name == std::string_view::npos) return std::nullopt;
    const std::size_t colon = body.find(':', name + needle.size());
    if (colon == std::string_view::npos) return std::nullopt;
    const std::size_t quote = body.find('"', colon + 1);
    if (quote == std::string_view::npos) return std::nullopt;
    std::string out;
    bool escaped = false;
    for (std::size_t i = quote + 1; i < body.size(); ++i) {
        const char c = body[i];
        if (escaped) {
            if (c != '"' && c != '\\' && c != '/') return std::nullopt;
            out.push_back(c);
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == '"') {
            return out;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            return std::nullopt;
        } else {
            out.push_back(c);
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<Session> read_session(const std::string& path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) return std::nullopt;
    constexpr std::size_t kMaxSessionBytes = 64 * 1024;
    std::string body;
    body.resize(kMaxSessionBytes + 1);
    in.read(body.data(), static_cast<std::streamsize>(body.size()));
    const auto count = static_cast<std::size_t>(in.gcount());
    if (count == 0 || count > kMaxSessionBytes) return std::nullopt;
    body.resize(count);
    Session session;
    const auto client = json_string(body, "client_id");
    const auto token = json_string(body, "access_token");
    if (!client || !token || client->empty() || token->empty()
        || client->size() > altair::fyers::kClientIdMax
        || token->size() > altair::fyers::kAccessTokenMax) {
        return std::nullopt;
    }
    session.client_id = *client;
    session.access_token = *token;
    return session;
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

/// A section body is embedded verbatim only when it is a successful JSON
/// object; anything else is written as null so the file always parses.
[[nodiscard]] bool embeddable(const Fetched& item) {
    std::string_view body{item.body};
    while (!body.empty() && (body.front() == ' ' || body.front() == '\n'
                             || body.front() == '\r' || body.front() == '\t'))
        body.remove_prefix(1);
    while (!body.empty() && (body.back() == ' ' || body.back() == '\n'
                             || body.back() == '\r' || body.back() == '\t'))
        body.remove_suffix(1);
    return item.status == 200 && body.size() >= 2 && body.front() == '{'
        && body.back() == '}' && altair::broker_view::snapshot_detail::success(body);
}

[[nodiscard]] bool write_snapshot(const std::string& path,
                                  const std::vector<Fetched>& fetched,
                                  const Fetched& tradebook,
                                  std::time_t observed,
                                  const altair::broker_view::AccountSnapshot& snapshot) {
    using altair::broker_view::SnapshotSection;
    using altair::broker_view::SnapshotSectionStatus;
    const auto state_name = [](const SnapshotSection& section) {
        switch (section.status) {
        case SnapshotSectionStatus::Absent:         return "absent";
        case SnapshotSectionStatus::Present:        return "present";
        case SnapshotSectionStatus::Invalid:        return "invalid";
        case SnapshotSectionStatus::TransportError: return "transport_error";
        case SnapshotSectionStatus::HttpError:      return "http_error";
        }
        return "invalid";
    };
    const std::array<const SnapshotSection*, 5> sections{
        &snapshot.profile, &snapshot.funds, &snapshot.positions,
        &snapshot.holdings, &snapshot.orders};
    const std::filesystem::path destination{path};
    std::error_code ec;
    if (!destination.parent_path().empty())
        std::filesystem::create_directories(destination.parent_path(), ec);
    const std::string temporary = path + ".tmp";
    {
        std::ofstream out{temporary, std::ios::binary | std::ios::trunc};
        if (!out) return false;
        out << "{\n  \"schema_version\": 1,\n"
            << "  \"broker\": \"FYERS\",\n"
            << "  \"fetched_at_unix\": " << static_cast<long long>(observed) << ",\n"
            << "  \"service_epoch\": 1,\n"
            << "  \"auth_verified_at_unix\": " << static_cast<long long>(observed) << ",\n"
            << "  \"auth_expires_at_unix\": " << static_cast<long long>(observed + 30) << ",\n"
            << "  \"account_expires_at_unix\": " << static_cast<long long>(observed + 30) << ",\n"
            << "  \"feed_status\": \"disabled\",\n"
            << "  \"account_id\": " << json_quote(snapshot.account_id.data()) << ",\n"
            << "  \"snapshot_complete\": "
            << (altair::broker_view::complete(snapshot) ? "true" : "false") << ",\n";
        for (std::size_t i = 0; i < fetched.size(); ++i) {
            const auto& item = fetched[i];
            out << "  \"" << item.name << "_status\": " << item.status << ",\n"
                << "  \"" << item.name << "_state\": \""
                << state_name(*sections[i]) << "\",\n"
                << "  \"" << item.name << "\": ";
            if (sections[i]->status == SnapshotSectionStatus::Present
                && item.status == 200 && !item.body.empty()) out << item.body;
            else out << "null";
            out << ",\n";
        }
        // GETS trade history and expense report. Not part of the typed
        // snapshot; a failed trade book leaves the account sections intact.
        out << "  \"tradebook_status\": " << tradebook.status << ",\n"
            << "  \"tradebook\": ";
        if (embeddable(tradebook)) out << tradebook.body;
        else out << "null";
        out << "\n}\n";
        if (!out) return false;
    }
    std::filesystem::remove(destination, ec);
    ec.clear();
    std::filesystem::rename(temporary, destination, ec);
    if (ec) {
        std::filesystem::remove(temporary, ec);
        return false;
    }
    return true;
}

void usage(const char* executable) {
    std::printf(
        "  Snapshot the READ-ONLY FYERS account state.\n\n"
        "    %s [--out PATH] [--go]\n\n"
        "    --out PATH   output (default data/fyers_account.json)\n"
        "    --go         perform the five GET requests; otherwise dry-run\n\n"
        "  The helper contains no place, modify, cancel, exit or convert call.\n",
        executable);
}

} // namespace

int main(int argc, char** argv) {
    std::string output = source_path("data/fyers_account.json");
    bool go = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0) { usage(argv[0]); return 0; }
        if (std::strcmp(argv[i], "--go") == 0) { go = true; continue; }
        if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            output = argv[++i];
        }
    }

    std::vector<Fetched> fetched;
    fetched.reserve(altair::fyers::kAccountEndpoints.size());
    std::printf("  FYERS account snapshot -> %s\n\n", output.c_str());
    for (const auto& endpoint : altair::fyers::kAccountEndpoints) {
        fetched.push_back({endpoint.name, endpoint.path, 0, {}});
        std::printf("    GET https://api-t1.fyers.in%s\n", endpoint.path);
    }
    Fetched tradebook{altair::fyers::kTradebookEndpoint.name,
                      altair::fyers::kTradebookEndpoint.path, 0, {}};
    std::printf("    GET https://api-t1.fyers.in%s\n", tradebook.path);
    if (!go) {
        std::printf("\n  DRY RUN. No request was sent and no file was written.\n");
        return 0;
    }

    const std::string session_path = source_path("data/fyers_session.json");
    const auto session = read_session(session_path);
    if (!session) {
        std::printf("\n  no bounded valid FYERS session; link FYERS first.\n");
        return 2;
    }
    const auto authorization = altair::fyers::authorization_header(
        session->client_id.c_str(), session->access_token.c_str());
    if (!authorization) {
        std::printf("\n  FYERS session fields exceed the authorization boundary.\n");
        return 2;
    }

    for (auto& item : fetched) {
        const auto response = altair::https_get_auth(
            "api-t1.fyers.in", item.path, *authorization, "",
            std::chrono::seconds{20});
        if (!response) {
            std::printf("    %-10s TRANSPORT FAILED\n", item.name);
            continue;
        }
        item.status = response->status;
        item.body = response->body;
        std::printf("    %-10s HTTP %u  %zu bytes\n",
                    item.name, item.status, item.body.size());
    }
    if (const auto response = altair::https_get_auth(
            "api-t1.fyers.in", tradebook.path, *authorization, "",
            std::chrono::seconds{20})) {
        tradebook.status = response->status;
        tradebook.body = response->body;
        std::printf("    %-10s HTTP %u  %zu bytes\n",
                    tradebook.name, tradebook.status, tradebook.body.size());
    } else {
        std::printf("    %-10s TRANSPORT FAILED\n", tradebook.name);
    }

    const std::time_t now = std::time(nullptr);
    const auto seconds = static_cast<std::int64_t>(now);
    if (seconds <= 0 || seconds > 9'223'372'036LL) {
        std::printf("\n  local UTC clock is outside the supported range.\n");
        return 1;
    }
    const altair::broker_view::EvidenceWindow observed{
        altair::Timestamp{seconds * 1'000'000'000LL},
        altair::Timestamp{seconds * 1'000'000'000LL + 30'000'000'000LL}};
    const auto response = [&fetched](std::size_t i) {
        return altair::broker_view::ProviderResponse{
            static_cast<int>(fetched[i].status), fetched[i].body};
    };
    const auto typed = altair::broker_view::parse_account_snapshot(
        altair::broker_view::BrokerId::Fyers,
        {altair::broker_view::BrokerId::Fyers, 1, 1}, observed,
        response(0), response(1), response(2), response(3), response(4),
        "fy_id");
    if (!typed) {
        std::printf("\n  FYERS responses did not form a valid typed account snapshot; not writing.\n");
        return 1;
    }
    if (!write_snapshot(output, fetched, tradebook, now, *typed)) {
        std::printf("\n  could not replace %s.\n", output.c_str());
        return 1;
    }
    std::printf("\n  wrote a validated FYERS snapshot for account %s\n",
                typed->account_id.data());
    return 0;
}
