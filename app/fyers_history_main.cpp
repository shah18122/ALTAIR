// app/fyers_history_main.cpp -- read-only FYERS history and 1m/5m audit fetch.
#include <app/dataset_merge.hpp>
#include <app/fyers_env_session.hpp>
#include <app/price_text.hpp>
#include <broker/fyers_api.hpp>
#include <broker/fyers_historical.hpp>
#include <broker/https_client.hpp>

#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace {
struct Session { std::string client; std::string access; };

[[nodiscard]] std::string source_path(const char* relative) {
#ifdef ALTAIR_SOURCE_DIR
    return (std::filesystem::path{ALTAIR_SOURCE_DIR} / relative).string();
#else
    return relative;
#endif
}

[[nodiscard]] std::optional<std::string> json_string(std::string_view body,
                                                      std::string_view key) {
    const std::string needle = "\"" + std::string{key} + "\"";
    const auto name = body.find(needle);
    const auto colon = name == std::string_view::npos ? name : body.find(':', name + needle.size());
    const auto first = colon == std::string_view::npos ? colon : body.find('"', colon + 1);
    const auto last = first == std::string_view::npos ? first : body.find('"', first + 1);
    if (last == std::string_view::npos || last == first + 1) return std::nullopt;
    return std::string{body.substr(first + 1, last - first - 1)};
}

[[nodiscard]] std::optional<Session> read_session(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    constexpr std::size_t cap = 64 * 1024;
    std::string body(cap + 1, '\0');
    in.read(body.data(), static_cast<std::streamsize>(body.size()));
    const auto size = static_cast<std::size_t>(in.gcount());
    if (size == 0 || size > cap) return std::nullopt;
    body.resize(size);
    const auto client = json_string(body, "client_id");
    const auto access = json_string(body, "access_token");
    if (!client || !access || client->size() > altair::fyers::kClientIdMax
        || access->size() > altair::fyers::kAccessTokenMax) return std::nullopt;
    return Session{*client, *access};
}

[[nodiscard]] bool append_price(std::string& output, double value) {
    char buffer[altair::dataset::kPriceTextMax]{};
    const auto size = altair::dataset::format_price(value, buffer, sizeof(buffer));
    if (!size) return false;
    output.append(buffer, *size);
    return true;
}

[[nodiscard]] bool csv(const std::vector<altair::RawCandle>& candles,
                       std::string& output) {
    output = "time,open,high,low,close,volume\n";
    for (const auto& candle : candles) {
        output += altair::format_ist(candle.ts_ns);
        for (const double price : {candle.open, candle.high, candle.low, candle.close}) {
            output.push_back(',');
            if (!append_price(output, price)) return false;
        }
        output.push_back(',');
        char volume[32]{};
        const auto encoded = std::to_chars(volume, volume + sizeof(volume),
            static_cast<std::uint64_t>(candle.volume));
        if (encoded.ec != std::errc{}) return false;
        output.append(volume, encoded.ptr);
        output.push_back('\n');
    }
    return true;
}

void usage(const char* executable) {
    std::printf(
        "FYERS read-only OHLCV audit fetch (dry-run by default).\n\n"
        "  %s --symbol NSE:NIFTY50-INDEX --resolution 1 --from YYYY-MM-DD\n"
        "     --to YYYY-MM-DD --out PATH [--continuous] [--force] [--go]\n\n"
        "  %s --symbol NSE:SBIN-EQ --from YYYY-MM-DD --to YYYY-MM-DD\n"
        "     --out-1m PATH --out-5m PATH [--go]\n\n"
        "The first form fetches one documented FYERS resolution. The audit form\n"
        "fetches separate 1m and 5m files. Provider day limits are chunked, and\n"
        "chunks are sent 350 ms apart (FYERS allows 200 requests a minute).\n"
        "--continuous sets cont_flag=1: a futures symbol returns the stitched\n"
        "near-month series instead of that one contract.\n"
        "Existing files are never overwritten unless --force is explicit.\n",
        executable, executable);
}
} // namespace

int main(int argc, char** argv) {
    std::string symbol, from, to, out1, out5, resolution, out_path;
    bool go = false, force = false, continuous = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        auto value = [&](std::string& target) {
            if (i + 1 >= argc) return false;
            target = argv[++i];
            return true;
        };
        if (arg == "--symbol") { if (!value(symbol)) return 2; }
        else if (arg == "--from") { if (!value(from)) return 2; }
        else if (arg == "--to") { if (!value(to)) return 2; }
        else if (arg == "--out-1m") { if (!value(out1)) return 2; }
        else if (arg == "--out-5m") { if (!value(out5)) return 2; }
        else if (arg == "--resolution") { if (!value(resolution)) return 2; }
        else if (arg == "--out") { if (!value(out_path)) return 2; }
        else if (arg == "--force") force = true;
        else if (arg == "--continuous") continuous = true;
        else if (arg == "--go") go = true;
        else if (arg == "--help" || arg == "-h") { usage(argv[0]); return 0; }
        else { usage(argv[0]); return 2; }
    }
    const bool single = !resolution.empty() || !out_path.empty();
    const bool audit = !out1.empty() || !out5.empty();
    if (symbol.empty() || from.empty() || to.empty() || single == audit
        || (single && (resolution.empty() || out_path.empty()))
        || (audit && (out1.empty() || out5.empty() || out1 == out5))) {
        usage(argv[0]); return 2;
    }
    const auto print_requests = [&](std::string_view iv) -> bool {
        const auto chunks = altair::fyers_history::chunk_requests(iv, from, to);
        if (!chunks) return false;
        for (const auto& chunk : *chunks) {
            const auto request = altair::fyers_history::uri(
                symbol, iv, chunk.from, chunk.to, continuous);
            if (!request) return false;
            std::printf("GET https://api-t1.fyers.in%s\n", request->c_str());
        }
        return true;
    };
    if ((single && !print_requests(resolution))
        || (audit && (!print_requests("1") || !print_requests("5")))) {
        std::printf("invalid symbol, date range or resolution\n"); return 2;
    }
    if (!go) {
        std::printf("DRY RUN. No request sent and no file written.\n");
        return 0;
    }
    if (!force && ((single && std::filesystem::exists(out_path))
        || (audit && (std::filesystem::exists(out1) || std::filesystem::exists(out5))))) {
        std::printf("refusing to overwrite an existing audit file\n");
        return 1;
    }
    auto session = read_session(source_path("data/fyers_session.json"));
    if (!session) {   // a headless host: the day's session from the environment (app/fyers_env_session.hpp)
        if (const auto env = altair::fyers_env::from_environment()) { session = Session{env->client, env->access}; }
    }
    if (!session) {
        std::printf("no bounded valid FYERS session: link first, or set ALTAIR_FYERS_CLIENT_ID and ALTAIR_FYERS_ACCESS_TOKEN\n");
        return 2;
    }
    const auto authorization = altair::fyers::authorization_header(
        session->client.c_str(), session->access.c_str());
    if (!authorization) return 2;
    const auto fetch = [&](std::string_view iv)
        -> std::optional<std::vector<altair::RawCandle>> {
        const auto chunks = altair::fyers_history::chunk_requests(iv, from, to);
        if (!chunks) return std::nullopt;
        std::vector<altair::RawCandle> all;
        bool first_chunk = true;
        for (const auto& chunk : *chunks) {
            if (!first_chunk) std::this_thread::sleep_for(std::chrono::milliseconds{350});
            first_chunk = false;
            const auto request = altair::fyers_history::uri(
                symbol, iv, chunk.from, chunk.to, continuous);
            if (!request) return std::nullopt;
            const auto response = altair::https_get_auth("api-t1.fyers.in", *request,
                *authorization, "", std::chrono::seconds{30});
            if (!response) {
                std::printf("FYERS history transport failed for %s to %s\n",
                            chunk.from.c_str(), chunk.to.c_str());
                return std::nullopt;
            }
            if (response->status != 200) {
                const auto message = json_string(response->body, "message");
                std::printf("FYERS history HTTP %u for %s to %s%s%s\n",
                            response->status, chunk.from.c_str(), chunk.to.c_str(),
                            message ? ": " : "", message ? message->c_str() : "");
                return std::nullopt;
            }
            const auto parsed = altair::fyers_history::parse(response->body);
            if (!parsed || parsed->empty()) {
                const auto message = json_string(response->body, "message");
                std::printf("FYERS history response refused validation%s%s\n",
                            message ? ": " : "", message ? message->c_str() : "");
                return std::nullopt;
            }
            for (const auto& candle : *parsed) {
                if (all.empty() || candle.ts_ns > all.back().ts_ns) all.push_back(candle);
                else if (candle.ts_ns != all.back().ts_ns) {
                    std::printf("FYERS returned out-of-order candles across chunks\n");
                    return std::nullopt;
                }
            }
        }
        return all;
    };
    if (single) {
        const auto bars = fetch(resolution);
        if (!bars) {
            std::printf("history fetch failed; nothing written\n");
            return 1;
        }
        std::string output;
        if (!csv(*bars, output)
            || !altair::dataset::replace_file_checked(out_path, output)) return 1;
        std::printf("wrote %zu FYERS bars at resolution %s\n",
                    bars->size(), resolution.c_str());
        return 0;
    }
    const auto bars1 = fetch("1");
    const auto bars5 = fetch("5");
    if (!bars1 || !bars5) {
        std::printf("history response failed validation; nothing written\n");
        return 1;
    }
    std::string csv1, csv5;
    if (!csv(*bars1, csv1) || !csv(*bars5, csv5)) return 1;
    const auto write1 = altair::dataset::replace_file_checked(out1, csv1);
    if (!write1) return 1;
    const auto write5 = altair::dataset::replace_file_checked(out5, csv5);
    if (!write5) {
        std::printf("5m write failed; 1m audit file remains at %s\n", out1.c_str());
        return 1;
    }
    const std::string provenance =
        "{\"provider\":\"FYERS\",\"symbol\":\"" + symbol
        + "\",\"from\":\"" + from + "\",\"to\":\"" + to
        + "\",\"adjustment\":\"provider_raw\",\"closed_bars_only\":true}\n";
    (void)altair::dataset::replace_file_checked(out1 + ".provenance.json", provenance);
    (void)altair::dataset::replace_file_checked(out5 + ".provenance.json", provenance);
    std::printf("wrote %zu one-minute and %zu five-minute FYERS bars; source data untouched\n",
                bars1->size(), bars5->size());
    return 0;
}
