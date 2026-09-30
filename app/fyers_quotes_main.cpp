// app/fyers_quotes_main.cpp -- READ-ONLY FYERS market quotes snapshot.
//
// Feeds the GETS screens in the desktop Terminal: underlying spot and option
// LTPs for the Greek market watch, and change / OHLC / previous close for the
// top-movers and index-information tabs. One GET per 50 symbols on
// /data/quotes; no order, socket or account endpoint is present in this
// binary. The desktop launches it and reads what it wrote, the same boundary
// as every other broker helper (desktop/ may not link broker/).
//
// OUTPUT (data/fyers_quotes.json):
//   { "schema_version": 1, "broker": "FYERS", "fetched_at_unix": N,
//     "requested": N, "responses": [ {"status": 200, "body": {...}}, ... ] }
// A response body is embedded verbatim only when it is a successful JSON
// object; otherwise it is null and its HTTP status says why.

#include <broker/fyers_api.hpp>
#include <broker/https_client.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

/// Refuse rather than quietly drop symbols past this (ten requests).
constexpr std::size_t kMaxSymbols = 10 * altair::fyers::kQuotesMaxSymbols;

struct QuoteSession {
    std::string client_id;
    std::string access_token;
};

struct QuoteResponse {
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
quote_json_string(std::string_view body, std::string_view key) {
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

[[nodiscard]] std::optional<QuoteSession> read_quote_session(const std::string& path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) return std::nullopt;
    constexpr std::size_t kMaxSessionBytes = 64 * 1024;
    std::string body;
    body.resize(kMaxSessionBytes + 1);
    in.read(body.data(), static_cast<std::streamsize>(body.size()));
    const auto count = static_cast<std::size_t>(in.gcount());
    if (count == 0 || count > kMaxSessionBytes) return std::nullopt;
    body.resize(count);
    const auto client = quote_json_string(body, "client_id");
    const auto token = quote_json_string(body, "access_token");
    if (!client || !token || client->empty() || token->empty()
        || client->size() > altair::fyers::kClientIdMax
        || token->size() > altair::fyers::kAccessTokenMax) {
        return std::nullopt;
    }
    return QuoteSession{*client, *token};
}

[[nodiscard]] std::string_view trimmed(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r'
                          || s.front() == '\n'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'
                          || s.back() == '\n'))
        s.remove_suffix(1);
    return s;
}

void add_symbol(std::vector<std::string>& out, std::string_view symbol) {
    symbol = trimmed(symbol);
    if (symbol.empty()) return;
    for (const auto& s : out)
        if (s == symbol) return;
    out.emplace_back(symbol);
}

void add_list(std::vector<std::string>& out, std::string_view list) {
    while (!list.empty()) {
        const std::size_t comma = list.find(',');
        add_symbol(out, list.substr(0, comma));
        if (comma == std::string_view::npos) break;
        list.remove_prefix(comma + 1);
    }
}

[[nodiscard]] bool add_file(std::vector<std::string>& out, const std::string& path) {
    std::ifstream in{path};
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        std::string_view view = trimmed(line);
        if (view.empty() || view.front() == '#') continue;
        const std::size_t hash = view.find('#');
        if (hash != std::string_view::npos) view = view.substr(0, hash);
        add_list(out, view);
    }
    return true;
}

[[nodiscard]] bool embeddable(const QuoteResponse& r) {
    const std::string_view body = trimmed(r.body);
    return r.status == 200 && body.size() >= 2 && body.front() == '{' && body.back() == '}'
        && (body.find("\"s\":\"ok\"") != std::string_view::npos
            || body.find("\"s\": \"ok\"") != std::string_view::npos);
}

[[nodiscard]] bool write_quotes(const std::string& path, std::time_t observed,
                                std::size_t requested,
                                const std::vector<QuoteResponse>& responses) {
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
            << "  \"requested\": " << requested << ",\n"
            << "  \"responses\": [\n";
        for (std::size_t i = 0; i < responses.size(); ++i) {
            out << "    {\"status\": " << responses[i].status << ", \"body\": ";
            if (embeddable(responses[i])) out << trimmed(responses[i].body);
            else out << "null";
            out << (i + 1 == responses.size() ? "}\n" : "},\n");
        }
        out << "  ]\n}\n";
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
        "  Snapshot READ-ONLY FYERS market quotes.\n\n"
        "    %s [--symbols A,B,...] [--symbols-file PATH] [--out PATH] [--go]\n\n"
        "    --symbols LIST       comma-separated FYERS tickers (repeatable)\n"
        "    --symbols-file PATH  one ticker per line, '#' comments\n"
        "                         (the desktop passes config/market_watch.txt)\n"
        "    --out PATH           output (default data/fyers_quotes.json)\n"
        "    --go                 perform the GET requests; otherwise dry-run\n\n"
        "  One /data/quotes request per 50 symbols, at most %zu symbols.\n",
        executable, kMaxSymbols);
}

} // namespace

int main(int argc, char** argv) {
    std::string output = source_path("data/fyers_quotes.json");
    std::vector<std::string> symbols;
    bool go = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0) { usage(argv[0]); return 0; }
        if (std::strcmp(argv[i], "--go") == 0) { go = true; continue; }
        if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) { output = argv[++i]; continue; }
        if (std::strcmp(argv[i], "--symbols") == 0 && i + 1 < argc) { add_list(symbols, argv[++i]); continue; }
        if (std::strcmp(argv[i], "--symbols-file") == 0 && i + 1 < argc) {
            const std::string path = argv[++i];
            if (!add_file(symbols, path)) {
                std::printf("  cannot read symbols file %s\n", path.c_str());
                return 2;
            }
            continue;
        }
        std::printf("  unknown argument: %s\n", argv[i]);
        usage(argv[0]);
        return 2;
    }
    if (symbols.empty()) {
        std::printf("  no symbols: pass --symbols or --symbols-file\n");
        return 2;
    }
    if (symbols.size() > kMaxSymbols) {
        std::printf("  %zu symbols requested; the limit is %zu. Nothing was sent.\n",
                    symbols.size(), kMaxSymbols);
        return 2;
    }

    std::vector<std::string> targets;
    for (std::size_t at = 0; at < symbols.size(); at += altair::fyers::kQuotesMaxSymbols) {
        const std::size_t n = symbols.size() - at < altair::fyers::kQuotesMaxSymbols
            ? symbols.size() - at : altair::fyers::kQuotesMaxSymbols;   // RULE 11: chunking, every symbol is sent
        const std::vector<std::string> chunk(symbols.begin() + static_cast<std::ptrdiff_t>(at),
                                             symbols.begin() + static_cast<std::ptrdiff_t>(at + n));
        const auto target = altair::fyers::quotes_target(chunk);
        if (!target) {
            std::printf("  symbol list %zu..%zu cannot form a quotes request; nothing was sent.\n",
                        at + 1, at + n);
            return 2;
        }
        targets.push_back(*target);
    }

    std::printf("  FYERS quotes -> %s\n\n  %zu symbols in %zu request(s)\n",
                output.c_str(), symbols.size(), targets.size());
    for (const auto& t : targets)
        std::printf("    GET https://api-t1.fyers.in%.*s%s\n",
                    static_cast<int>(t.size() > 96 ? 96 : t.size()), t.c_str(),
                    t.size() > 96 ? "..." : "");   // RULE 11: display only, ellipsis marks the cut
    if (!go) {
        std::printf("\n  DRY RUN. No request was sent and no file was written.\n");
        return 0;
    }

    const auto session = read_quote_session(source_path("data/fyers_session.json"));
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

    std::vector<QuoteResponse> responses;
    std::size_t ok = 0;
    for (std::size_t i = 0; i < targets.size(); ++i) {
        if (i > 0) std::this_thread::sleep_for(std::chrono::milliseconds{150});   // FYERS: 10 req/s
        QuoteResponse r{};
        if (const auto response = altair::https_get_auth(
                "api-t1.fyers.in", targets[i], *authorization, "", std::chrono::seconds{20})) {
            r.status = response->status;
            r.body = response->body;
        }
        std::printf("    request %zu: HTTP %u  %zu bytes\n", i + 1, r.status, r.body.size());
        if (embeddable(r)) ++ok;
        responses.push_back(std::move(r));
    }
    if (!write_quotes(output, std::time(nullptr), symbols.size(), responses)) {
        std::printf("\n  could not replace %s.\n", output.c_str());
        return 1;
    }
    std::printf("\n  wrote %zu of %zu quote responses\n", ok, responses.size());
    return ok == 0 ? 1 : 0;
}
