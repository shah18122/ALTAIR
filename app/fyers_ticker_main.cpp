// app/fyers_ticker_main.cpp -- the LIVE FYERS market-data feed.
//
//     altair_fyers_ticker [--symbols NSE:NIFTY50-INDEX,NSE:SBIN-EQ]
//                         [--depth] [--lite] [--seconds 30] [--channel 11]
//                         [--out PATH] [--jsonl PATH] [--stamp] [--store PATH]
//                         [--source NAME] [--reconnect 3] --go
//
// The pipeline, and every stage of it is also what the official SDK does:
//
//     data/fyers_session.json (from altair_fyers_login)
//       -> hsm_key out of the access token          feed/fyers_hsm.hpp
//       -> POST /data/symbol-token -> HSM topics     feed/fyers_hsm.hpp
//       -> wss://socket.fyers.in/hsm/v1-5/prod       broker/fyers_data_socket.hpp
//       -> binary frames -> SDK on_message JSON      feed/fyers_hsm.hpp
//       -> FyersDataAdapter -> Tick / DepthUpdate    feed/fyers_adapter.hpp
//       -> TickStoreWriter (--store) and a status JSON for the UI (--out)
//
// Read-only: no order endpoint is reachable from this binary. Without --go it
// prints what it would do and exits, like every other fetcher in app/.
#include <broker/fyers_api.hpp>
#include <broker/fyers_data_socket.hpp>
#include <broker/https_client.hpp>
#include <feed/fyers_adapter.hpp>
#include <feed/fyers_hsm.hpp>
#include <feed/tick_store.hpp>
#include <instruments/contract_spec.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace hsm = altair::fyers_hsm;

[[nodiscard]] std::string source_path(const char* relative) {
#ifdef ALTAIR_SOURCE_DIR
    return (std::filesystem::path{ALTAIR_SOURCE_DIR} / relative).string();
#else
    return relative;
#endif
}

[[nodiscard]] const char* arg_value(int argc, char** argv, const char* flag) {
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], flag) == 0) return argv[i + 1];
    return nullptr;
}

[[nodiscard]] bool has_flag(int argc, char** argv, const char* flag) {
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], flag) == 0) return true;
    return false;
}

/// A top-level JSON string member. Its own reader, as in the other FYERS
/// helpers: the file holds a live credential and is never printed.
[[nodiscard]] std::optional<std::string> json_string(std::string_view body,
                                                     std::string_view key) {
    const std::string needle = "\"" + std::string{key} + "\"";
    const std::size_t name = body.find(needle);
    if (name == std::string_view::npos) return std::nullopt;
    const std::size_t colon = body.find(':', name + needle.size());
    if (colon == std::string_view::npos) return std::nullopt;
    const std::size_t quote = body.find('"', colon + 1);
    if (quote == std::string_view::npos) return std::nullopt;
    const std::size_t end = body.find('"', quote + 1);
    if (end == std::string_view::npos) return std::nullopt;
    return std::string{body.substr(quote + 1, end - quote - 1)};
}

struct Session {
    std::string client_id;
    std::string access_token;
};

[[nodiscard]] std::optional<Session> read_session(const std::string& path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) return std::nullopt;
    constexpr std::size_t kMaxSessionBytes = 64 * 1024;
    std::string body(kMaxSessionBytes + 1, '\0');
    in.read(body.data(), static_cast<std::streamsize>(body.size()));
    const auto count = static_cast<std::size_t>(in.gcount());
    if (count == 0 || count > kMaxSessionBytes) return std::nullopt;
    body.resize(count);
    auto client = json_string(body, "client_id");
    auto token = json_string(body, "access_token");
    if (!client || !token || client->empty() || token->empty()
        || client->size() > altair::fyers::kClientIdMax
        || token->size() > altair::fyers::kAccessTokenMax)
        return std::nullopt;
    return Session{std::move(*client), std::move(*token)};
}

/// FYERS symbols are "EXCH:NAME[-SERIES]" in upper case, digits, '&', '_'.
[[nodiscard]] bool plausible_symbol(std::string_view s) {
    if (s.size() < 5 || s.size() > 64 || s.find(':') == std::string_view::npos) return false;
    for (char c : s) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ':'
                     || c == '-' || c == '&' || c == '_';
        if (!ok) return false;
    }
    return true;
}

[[nodiscard]] std::vector<std::string> split_symbols(const char* list) {
    std::vector<std::string> out;
    std::string cur;
    for (const char* p = list; ; ++p) {
        if (*p == ',' || *p == '\0') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
            if (*p == '\0') break;
        } else if (*p != ' ') {
            cur.push_back(*p);
        }
    }
    return out;
}

void usage(const char* self) {
    std::printf(
        "\n  The LIVE FYERS data feed (official HSM socket protocol).\n\n"
        "    %s [--symbols A,B] [--depth] [--lite] [--seconds N]\n"
        "        [--channel N] [--out PATH] [--jsonl PATH] [--stamp] [--store PATH]\n"
        "        [--source NAME] [--reconnect N] --go\n\n"
        "    --symbols    FYERS symbols (default NSE:NIFTY50-INDEX,\n"
        "                 NSE:NIFTYBANK-INDEX,NSE:SBIN-EQ)\n"
        "    --depth      also subscribe 5-level market depth (not for indices)\n"
        "    --lite       LTP-only lite mode\n"
        "    --seconds    how long to listen, 1..86400 (default 30)\n"
        "    --channel    socket channel 1..30 (default 11, as the SDK)\n"
        "    --out        status JSON for the UI (default data/fyers_ticks.json)\n"
        "    --jsonl      append every update as the SDK's on_message JSON\n"
        "    --stamp      prefix each --jsonl line with \"recv_ms\" (receive time, epoch ms):\n"
        "                 depth updates carry no time of their own (altair_depth_study needs it)\n"
        "    --store      binary tick store to append (default none)\n"
        "    --source     client name sent at auth (default %.*s)\n"
        "    --reconnect  reconnect attempts after a drop (default 3)\n"
        "    --go         ACTUALLY CONNECT. Without it this is a dry run.\n\n"
        "  Needs data/fyers_session.json: run altair_fyers_login first.\n\n",
        self, static_cast<int>(hsm::kDefaultSource.size()), hsm::kDefaultSource.data());
}

altair::SpecStore g_specs;
altair::FyersDataAdapter<hsm::kMaxSymbols, 4096, 1024> g_adapter{g_specs, hsm::kDefaultChannel};

} // namespace

int main(int argc, char** argv) {
    if (has_flag(argc, argv, "--help")) { usage(argv[0]); return 0; }

    const char* symbols_s = arg_value(argc, argv, "--symbols");
    const char* seconds_s = arg_value(argc, argv, "--seconds");
    const char* channel_s = arg_value(argc, argv, "--channel");
    const char* out_s = arg_value(argc, argv, "--out");
    const char* jsonl_s = arg_value(argc, argv, "--jsonl");
    const char* store_s = arg_value(argc, argv, "--store");
    const char* source_s = arg_value(argc, argv, "--source");
    const char* reconnect_s = arg_value(argc, argv, "--reconnect");
    const bool depth = has_flag(argc, argv, "--depth");
    const bool stamp = has_flag(argc, argv, "--stamp");
    const bool lite = has_flag(argc, argv, "--lite");
    const bool go = has_flag(argc, argv, "--go");

    const auto symbols = split_symbols(
        symbols_s ? symbols_s : "NSE:NIFTY50-INDEX,NSE:NIFTYBANK-INDEX,NSE:SBIN-EQ");
    if (symbols.empty() || symbols.size() > hsm::kMaxSymbols) {
        std::printf("  --symbols must list 1..%zu symbols\n", hsm::kMaxSymbols);
        return 2;
    }
    for (const auto& s : symbols) {
        if (!plausible_symbol(s)) {
            std::printf("  --symbols: \"%s\" is not a FYERS symbol (EXCH:NAME-SERIES)\n", s.c_str());
            return 2;
        }
    }
    const long seconds = seconds_s ? std::strtol(seconds_s, nullptr, 10) : 30;
    if (seconds < 1 || seconds > 86400) { std::printf("  --seconds must be 1..86400\n"); return 2; }
    const long channel_l = channel_s ? std::strtol(channel_s, nullptr, 10) : hsm::kDefaultChannel;
    if (channel_l < 1 || channel_l > 30) { std::printf("  --channel must be 1..30\n"); return 2; }
    const auto channel = static_cast<std::uint8_t>(channel_l);
    const long reconnects = reconnect_s ? std::strtol(reconnect_s, nullptr, 10) : 3;
    if (reconnects < 0 || reconnects > 20) { std::printf("  --reconnect must be 0..20\n"); return 2; }
    const std::string source = source_s ? source_s : std::string{hsm::kDefaultSource};
    if (source.empty() || source.size() > 64) { std::printf("  --source must be 1..64 chars\n"); return 2; }
    const std::string out_path = out_s ? out_s : source_path("data/fyers_ticks.json");

    std::printf("  FYERS live data feed\n"
                "    wss://%s%s  (channel %u, %s mode%s)\n",
                altair::kFyersDataSocketHost, altair::kFyersDataSocketPath,
                static_cast<unsigned>(channel), lite ? "lite" : "full",
                depth ? " + depth" : "");
    for (const auto& s : symbols) std::printf("    %s\n", s.c_str());
    std::printf("    for %ld s -> %s\n", seconds, out_path.c_str());
    if (!go) {
        std::printf("\n  DRY RUN. No request was sent and no socket was opened.\n"
                    "  Add --go to connect for real.\n");
        return 0;
    }

    // ---- credential -> hsm_key ---------------------------------------------
    const auto session = read_session(source_path("data/fyers_session.json"));
    if (!session) {
        std::printf("\n  no valid FYERS session; run altair_fyers_login first.\n");
        return 2;
    }
    const auto now_unix = static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    const auto hsm_key = hsm::hsm_key_from_token(session->access_token, now_unix);
    if (!hsm_key) {
        std::printf("\n  %s\n", hsm::error_text(hsm_key.error()));
        return 2;
    }

    // ---- symbols -> HSM topics (POST /data/symbol-token) -------------------
    std::string body = "{\"symbols\":[";
    for (std::size_t i = 0; i < symbols.size(); ++i) {
        if (i) body.push_back(',');
        body += "\"" + symbols[i] + "\"";
    }
    body += "]}";
    // The SDK authorizes this call with the bare token; FYERS' REST convention
    // is "APPID:token". Try the SDK's form first, then the REST form.
    const auto bare = std::string{hsm::bare_token(session->access_token)};
    const auto rest_auth = altair::fyers::authorization_header(
        session->client_id.c_str(), session->access_token.c_str());
    std::optional<hsm::SymbolTokens> tokens;
    std::string rejection;
    for (int attempt = 0; attempt < 2 && !tokens; ++attempt) {
        const std::string auth = attempt == 0 ? bare : (rest_auth ? *rest_auth : std::string{});
        if (auth.empty()) break;
        const auto response = altair::https_post_json(
            hsm::kSymbolTokenHost, hsm::kSymbolTokenPath, body,
            std::chrono::seconds{20}, auth);
        if (!response) { rejection = "symbol-token request: transport failed"; continue; }
        auto parsed = hsm::parse_symbol_tokens(response->body);
        if (parsed) { tokens = std::move(*parsed); break; }
        rejection = "symbol-token request: HTTP " + std::to_string(response->status)
                  + ", " + hsm::error_text(parsed.error());
    }
    if (!tokens) { std::printf("\n  %s\n", rejection.c_str()); return 3; }
    for (const auto& bad : tokens->invalid)
        std::printf("    FYERS does not know %s -- skipped\n", bad.c_str());

    hsm::HsmSession decoder{lite};
    std::vector<std::string> topics;
    std::vector<std::string> registered;   // SpecStore id i is registered[i]
    for (const auto& [symbol, fytoken] : tokens->valid) {
        // Same stated limitation as altair_kite_ticker: the scale (100 per
        // rupee) is right for NSE/BSE equity, F&O and indices; lot and tick
        // are placeholders nothing downstream of this binary sizes with.
        std::uint32_t cookie = hsm::kNoCookie;
        if (symbol.size() > altair::kMaxSymbolLen) {
            std::printf("    %s is longer than the spec store's symbol field -- skipped\n",
                        symbol.c_str());
        } else {
            altair::ContractSpec cs{};
            std::memcpy(cs.symbol, symbol.c_str(), symbol.size() + 1);
            cs.lot_size = altair::LotSize{1};
            cs.tick_size = altair::Price{1};
            cs.price_scale = 100;
            cs.valid_from = altair::Timestamp{1};
            cs.valid_to = altair::Timestamp::max();
            cs.snapshot_at = altair::Timestamp{1};
            if (const auto id = g_specs.add(cs)) {
                registered.push_back(symbol);
                // Resolved ONCE here: updates carry the id as the topic's
                // cookie, so nothing is looked up by name on the hot path.
                cookie = static_cast<std::uint32_t>(*id);
            }
        }
        for (int pass = 0; pass < (depth ? 2 : 1); ++pass) {
            const auto type = pass == 0 ? hsm::DataType::SymbolUpdate : hsm::DataType::DepthUpdate;
            auto topic = hsm::topic_for(symbol, fytoken, type);
            if (topic.empty()) continue;
            decoder.map_topic(topic, symbol, cookie);
            topics.push_back(std::move(topic));
            (void)g_adapter.subscribe(symbol, pass == 0 ? altair::FyersDataMode::SymbolUpdate
                                                        : altair::FyersDataMode::DepthUpdate);
        }
    }
    if (topics.empty()) { std::printf("\n  nothing subscribable\n"); return 3; }

    std::vector<hsm::Bytes> subscribe_frames;
    for (std::size_t i = 0; i < topics.size(); i += hsm::kSubscribeChunk) {
        // RULE 11: chunking, not truncation -- the loop sends every topic in
        // frames of at most kSubscribeChunk, as the SDK does.
        const std::size_t n = std::min(hsm::kSubscribeChunk, topics.size() - i);
        const auto frame = hsm::topics_frame(
            true, std::span<const std::string>{topics.data() + i, n}, channel,
            session->client_id.size() + 1 + session->access_token.size(), source.size());
        if (!frame) { std::printf("\n  %s\n", hsm::error_text(frame.error())); return 3; }
        subscribe_frames.push_back(*frame);
    }

    // ---- outputs -----------------------------------------------------------
    std::ofstream jsonl;
    if (jsonl_s) {
        jsonl.open(jsonl_s, std::ios::app | std::ios::binary);
        if (!jsonl) std::printf("  could not open %s -- continuing without it\n", jsonl_s);
    }
    altair::TickStoreWriter writer;
    bool storing = false;
    if (store_s) {
        const altair::Timestamp session_date{std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()};
        storing = writer.open(store_s, session_date).has_value();
        if (!storing) std::printf("  could not open the tick store at %s -- continuing without it\n", store_s);
    }

    std::size_t messages = 0, ticks = 0, depth_updates = 0, unknown_topic = 0, acks = 0;
    std::size_t frames = 0, bytes = 0, reconnect_count = 0;
    bool authenticated = false, auth_rejected = false;
    std::vector<std::int64_t> last_paise(registered.size(), -1);
    std::uint64_t epoch = 0;

    std::string json_line;
    altair::FyersFields fields{};
    const auto emit = [&](const hsm::HsmUpdate& u) {
        ++messages;
        const altair::Timestamp recv{std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()};
        // JSON is only produced for the recording; the feed path is typed.
        if (jsonl) {
            hsm::render_sdk_json(u, lite, json_line);
            if (stamp) {   // opt-in: the SDK's own JSON has no receive time
                json_line.insert(1, "\"recv_ms\":" + std::to_string(recv.ns_since_epoch() / 1'000'000) + ",");
            }
            jsonl << json_line << '\n';
        }
        if (u.cookie != hsm::kNoCookie
            && hsm::to_fyers_fields(u, static_cast<altair::InstrumentId>(u.cookie), fields))
            (void)g_adapter.on_fields(fields, recv, epoch);
        altair::FeedEnvelope<altair::Tick> t{};
        while (g_adapter.try_pop(t)) {
            ++ticks;
            if (storing) (void)writer.write(t.value);
            const auto idx = static_cast<std::size_t>(t.value.id);
            if (idx < last_paise.size()) last_paise[idx] = t.value.last.raw();
        }
        altair::FeedEnvelope<altair::DepthUpdate> d{};
        while (g_adapter.try_pop(d)) ++depth_updates;
    };
    const auto on_frame = [&](const std::uint8_t* p, std::size_t n,
                              std::vector<altair::FyersFrame>& replies) -> bool {
        ++frames;
        bytes += n;
        const auto r = decoder.on_frame(p, n, emit);
        unknown_topic += r.unknown_topic;
        if (r.ack) { replies.push_back(r.ack_bytes); ++acks; }
        switch (r.event) {
        case hsm::HsmEvent::AuthOk:
            authenticated = true;
            std::printf("    authenticated; subscribing %zu topic(s)\n", topics.size());
            for (const auto& f : subscribe_frames) replies.push_back(f);
            break;
        case hsm::HsmEvent::AuthFailed:
            auth_rejected = true;
            std::printf("    FYERS refused the socket token: %s\n", hsm::event_text(r.event));
            return false;
        case hsm::HsmEvent::Subscribed:
        case hsm::HsmEvent::ModeOk:
            std::printf("    %s\n", hsm::event_text(r.event));
            break;
        case hsm::HsmEvent::SubscribeFailed:
        case hsm::HsmEvent::ModeFailed:
        case hsm::HsmEvent::Malformed:
            std::printf("    %s\n", hsm::event_text(r.event));
            break;
        default:
            break;
        }
        return true;
    };

    // ---- connect, with bounded reconnects ----------------------------------
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{seconds};
    std::optional<altair::FyersSocketError> last_error;
    for (long attempt = 0; attempt <= reconnects; ++attempt) {
        const auto left = std::chrono::duration_cast<std::chrono::seconds>(
            deadline - std::chrono::steady_clock::now());
        if (left.count() < 1) break;
        decoder.reset();
        epoch = g_adapter.connected();
        g_adapter.subscriptions_sent();
        if (attempt > 0) ++reconnect_count;
        const auto st = altair::fyers_data_socket_run(
            {hsm::auth_frame(*hsm_key, source), hsm::mode_frame(lite, channel)},
            on_frame, hsm::ping_frame(), std::chrono::seconds{hsm::kPingSeconds}, left);
        g_adapter.disconnected(epoch);
        if (st) {
            last_error.reset();
            if (!st->closed_by_server || st->interrupted) break;
            std::printf("    FYERS closed the connection\n");
        } else {
            last_error = st.error();
            std::printf("    feed error: %s\n", altair::fyers_socket_error_text(st.error()));
        }
        if (auth_rejected) break;
        // RULE 11: safe-side clamp -- back-off is capped at 8 s and skipped
        // when it would outlast the run, so the caller's timeout never fires.
        const long backoff = std::min<long>(8, 1L << std::min<long>(attempt, 3));
        if (deadline - std::chrono::steady_clock::now() <= std::chrono::seconds{backoff + 1}) break;
        std::this_thread::sleep_for(std::chrono::seconds{backoff});
    }

    const auto& stats = g_adapter.stats();
    std::printf("\n  %zu frame(s), %zu bytes, %zu update(s): %zu tick(s), %zu depth update(s)\n"
                "  %zu decode error(s), %zu unknown topic(s), %zu ack(s), %zu reconnect(s)\n",
                frames, bytes, messages, ticks, depth_updates,
                static_cast<std::size_t>(stats.decode_errors), unknown_topic, acks, reconnect_count);
    if (authenticated && messages == 0)
        std::printf("\n  CONNECTED, AND NOTHING TICKED. Authenticated and subscribed, but no\n"
                    "  update arrived. Outside market hours that is expected.\n");

    // ---- status file for the UI: temp then rename --------------------------
    {
        std::error_code dir_ec;
        const auto parent = std::filesystem::path{out_path}.parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent, dir_ec);
        const std::string tmp = out_path + ".tmp";
        std::ofstream f(tmp, std::ios::binary);
        if (f) {
            f << "{\n  \"fetched_at_unix\": " << now_unix
              << ",\n  \"seconds\": " << seconds
              << ",\n  \"mode\": \"" << (lite ? "lite" : "full") << (depth ? "+depth" : "")
              << "\",\n  \"channel\": " << static_cast<unsigned>(channel)
              << ",\n  \"authenticated\": " << (authenticated ? "true" : "false")
              << ",\n  \"frames\": " << frames
              << ",\n  \"bytes\": " << bytes
              << ",\n  \"updates\": " << messages
              << ",\n  \"ticks\": " << ticks
              << ",\n  \"depth_updates\": " << depth_updates
              << ",\n  \"decode_errors\": " << stats.decode_errors
              << ",\n  \"unknown_topic\": " << unknown_topic
              << ",\n  \"reconnects\": " << reconnect_count
              << ",\n  \"error\": \""
              << (last_error ? altair::fyers_socket_error_text(*last_error) : "") << "\""
              << ",\n  \"last\": [";
            bool first = true;
            for (std::size_t i = 0; i < registered.size(); ++i) {
                if (last_paise[i] < 0) continue;
                f << (first ? "" : ",") << "\n    {\"symbol\": \"" << registered[i]
                  << "\", \"last_price_paise\": " << last_paise[i] << "}";
                first = false;
            }
            f << "\n  ]\n}\n";
            f.close();
            std::remove(out_path.c_str());
            if (std::rename(tmp.c_str(), out_path.c_str()) == 0)
                std::printf("  wrote %s\n", out_path.c_str());
        }
    }
    if (storing) (void)writer.close();
    if (auth_rejected) return 4;
    if (last_error && messages == 0) return 4;
    return 0;
}
