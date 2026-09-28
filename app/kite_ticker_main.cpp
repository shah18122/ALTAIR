// app/kite_ticker_main.cpp -- the live tick feed, end to end.
//
// P32-07.
//
//     altair_kite_ticker [--tokens 256265,260105] [--mode full]
//                        [--seconds 30] [--out PATH] [--store PATH] --go
//
// WHAT THIS CLOSES.
//
// `feed/kite_decoder.hpp` has turned Kite's binary frames into Tick and
// DepthUpdate structs since P2-02 -- every offset read out of Zerodha's own
// client, forty checks behind it -- and until now nothing had ever handed it a
// frame that came off a socket. Every "live" number in this project has come
// from a replay file. This is the first program in the tree that receives a
// tick from an exchange.
//
// THE PIPELINE IS THE REAL ONE, WHICH IS THE POINT OF RULE 6.
//
//     socket -> kite_ticker.hpp -> kite_decode_frame -> Tick -> TickStoreWriter
//
// The decoder and the store are the same ones the replayer uses. If the live
// path and the replay path ever diverge the backtest is a lie, so there is
// deliberately no second decoder and no "live-only" struct anywhere here.
//
// WHY IT IS A SEPARATE BINARY.
//
// Same reason as altair_kite_login, altair_kite_fetch, altair_kite_account and
// altair_kite_quote: a program that reads a live trading credential should not
// be inside the process that holds positions, and `desktop/` may not link
// `broker/` at all. The UI reads what this writes.
//
// IT SUBSCRIBES AND IT LISTENS. THAT IS THE WHOLE VOCABULARY.
//
// The Kite ticker protocol can carry order updates in the other direction;
// this program never writes anything but `subscribe` and `mode`, has no order
// vocabulary, and could not place one if asked. `oms/` remains the only
// directory that can.
//
// --go, FOR THE SAME REASON EVERY OTHER FETCHER HAS IT.
//
// Without it this prints what it would subscribe to and exits. A tick feed is
// harmless to open, but a habit of programs that reach the network the moment
// they are run is not, and the guard is the habit.

#include <broker/kite_ticker.hpp>
#include <feed/kite_decoder.hpp>
#include <feed/tick_store.hpp>
#include <instruments/contract_spec.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

[[nodiscard]] const char* arg_value(int argc, char** argv, const char* flag) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], flag) == 0) { return argv[i + 1]; }
    }
    return nullptr;
}

[[nodiscard]] bool has_flag(int argc, char** argv, const char* flag) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], flag) == 0) { return true; }
    }
    return false;
}

[[nodiscard]] std::string env_or_empty(const char* name) {
#if defined(_WIN32)
    char* buf = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&buf, &len, name) != 0 || buf == nullptr) { return {}; }
    std::string v(buf);
    std::free(buf);
    return v;
#else
    const char* v = std::getenv(name);
    return v == nullptr ? std::string{} : std::string{v};
#endif
}

/// Read `access_token` out of data/kite_session.json.
///
/// Its own reader rather than a JSON library, for the reason
/// kite_account_main.cpp gives: the file holds a LIVE TRADING CREDENTIAL and
/// the smallest amount of code that can see it is the right amount. Never
/// printed, never logged, never put in an error message.
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

void usage(const char* exe) {
    std::printf(
        "  The LIVE TICK FEED. Subscribes to Kite's WebSocket, decodes with\n"
        "  the same feed/kite_decoder.hpp the replayer uses, and writes.\n\n"
        "    %s [--tokens A,B,C] [--mode ltp|quote|full]\n"
        "        [--seconds N] [--out PATH] [--store PATH] --go\n\n"
        "    --tokens   instrument tokens (default 256265,260105,264969 --\n"
        "               NIFTY 50, NIFTY BANK, INDIA VIX)\n"
        "    --mode     ltp | quote | full   (default full: only full\n"
        "               carries the five depth levels, and depth is the\n"
        "               reason for a tick feed rather than a quote poll)\n"
        "    --seconds  how long to listen (default 30)\n"
        "    --out      status JSON for the UI (default data/kite_ticks.json)\n"
        "    --store    binary tick store to append (default none)\n"
        "    --go       ACTUALLY CONNECT. Without it this prints what it\n"
        "               would subscribe to and exits.\n\n"
        "  Needs ALTAIR_KITE_API_KEY and data/kite_session.json.\n"
        "  Subscribes and listens. It has no order vocabulary.\n",
        exe);
}

/// NOT ON THE STACK, AND app/instruments_demo.cpp ALREADY MEASURED WHY.
///
/// SpecStore holds 8,192 ContractSpecs and is about 1.63 MB. The default
/// Windows stack is 1 MB, so a local one overflows before the program does
/// anything -- and the failure is exit code 0xC00000FD with no message naming
/// the object, the file or the line. File scope, exactly as instruments_demo
/// does it, and for exactly the reason its comment gives.
altair::SpecStore g_specs;

} // namespace

int main(int argc, char** argv)
{
    if (has_flag(argc, argv, "--help")) { usage(argv[0]); return 0; }

    const char* toks_s = arg_value(argc, argv, "--tokens");
    const char* mode_s = arg_value(argc, argv, "--mode");
    const char* secs_s = arg_value(argc, argv, "--seconds");
    const char* out_s = arg_value(argc, argv, "--out");
    const char* store_s = arg_value(argc, argv, "--store");
    const bool go = has_flag(argc, argv, "--go");

    std::vector<std::uint32_t> tokens;
    {
        const std::string all = toks_s != nullptr
                                    ? std::string{toks_s}
                                    : std::string{"256265,260105,264969"};
        std::size_t at = 0;
        while (at <= all.size()) {
            const auto comma = all.find(',', at);
            const std::string one =
                all.substr(at, comma == std::string::npos ? std::string::npos
                                                          : comma - at);
            if (!one.empty()) {
                // The WHOLE field, as a number that fits a Kite token.
                // atoll read "256265x" as 256265 and a value above 2^32
                // wrapped to a different instrument (C14-016). A bad token
                // refuses the command rather than being dropped from the
                // subscription unnoticed.
                std::uint64_t v = 0;
                bool ok = one.size() <= 10;
                for (const char ch : one) {
                    if (ch < '0' || ch > '9') {
                        ok = false;
                        break;
                    }
                    v = v * 10 + static_cast<std::uint64_t>(ch - '0');
                }
                if (!ok || v == 0 || v > 4'294'967'295ULL) {
                    std::printf("  --tokens: \"%s\" is not an instrument "
                                "token\n", one.c_str());
                    return 2;
                }
                tokens.push_back(static_cast<std::uint32_t>(v));
            }
            if (comma == std::string::npos) { break; }
            at = comma + 1;
        }
    }
    if (tokens.empty()) {
        std::printf("  --tokens listed nothing usable\n");
        return 2;
    }

    altair::TickerMode mode = altair::TickerMode::Full;
    if (mode_s != nullptr) {
        if (std::strcmp(mode_s, "ltp") == 0) {
            mode = altair::TickerMode::Ltp;
        } else if (std::strcmp(mode_s, "quote") == 0) {
            mode = altair::TickerMode::Quote;
        } else if (std::strcmp(mode_s, "full") != 0) {
            std::printf("  --mode must be ltp, quote or full\n");
            return 2;
        }
    }
    const int seconds = secs_s != nullptr ? std::atoi(secs_s) : 30;
    if (seconds <= 0 || seconds > 3600) {
        std::printf("  --seconds must be 1..3600\n");
        return 2;
    }
    const std::string out_path =
        out_s != nullptr ? out_s : "data/kite_ticks.json";

    std::printf("\n  Kite ticker: %zu instrument(s), mode %s, %d s\n",
                tokens.size(), altair::ticker_mode_name(mode), seconds);
    std::printf("    wss://%s/  (credential redacted)\n",
                altair::kKiteTickerHost);
    for (std::uint32_t t : tokens) { std::printf("      %u\n", t); }

    if (!go) {
        std::printf(
            "\n  DRY RUN. No socket was opened.\n"
            "  Add --go to connect for real.\n\n");
        return 0;
    }

    const std::string api_key = env_or_empty("ALTAIR_KITE_API_KEY");
    if (api_key.empty()) {
        std::printf("\n  ALTAIR_KITE_API_KEY is not set.\n");
        return 3;
    }
    std::string access;
    if (!read_access_token("data/kite_session.json", access)) {
        std::printf("\n  no usable data/kite_session.json -- run "
                    "altair_kite_login. Kite tokens are DAILY.\n");
        return 3;
    }

    // ---- the pipeline ------------------------------------------------------
    //
    // The decoder writes into caller-owned arrays and never allocates: it is
    // ALTAIR_HOT and runs on every tick. The specs are needed because the
    // decoder refuses a token it does not know rather than guessing a scale --
    // D2 in P2-02.
    // THE SPECS ARE MINIMAL AND THAT IS A STATED LIMITATION, NOT A GUESS.
    //
    // The decoder needs a spec per token because it refuses an unknown token
    // rather than inventing a price scale (D2 in P2-02). What it uses from the
    // spec is the SCALE, and 100 units per rupee is not a guess for these
    // instruments -- it is what equity and F&O are, which is exactly why rule
    // 3 holds for them and does not for currency derivatives.
    //
    // Lot size and tick size are set to 1 and are NOT used by anything here.
    // They are deliberately not invented from a plausible-looking number: rule
    // 1 says they come from the point-in-time spec store, this program does
    // not load one, and a lot size of 1 is obviously wrong in a way that a
    // lot size of 75 typed here would not be. Nothing downstream of this
    // binary sizes anything.
    for (std::uint32_t t : tokens) {
        altair::ContractSpec cs{};
        cs.token[static_cast<std::size_t>(altair::FeedSource::Kite)] = t;
        std::snprintf(cs.symbol, sizeof(cs.symbol), "TOKEN%u", t);
        cs.lot_size = altair::LotSize{1};
        cs.tick_size = altair::Price{1};
        cs.price_scale = 100;      // equity / F&O: the wire value IS paise
        if (!g_specs.add(cs)) {
            std::printf("  could not register token %u\n", t);
            return 2;
        }
    }

    constexpr std::size_t kMaxTicks = 512;
    constexpr std::size_t kMaxDepth = 512;
    std::vector<altair::Tick> ticks(kMaxTicks);
    std::vector<altair::DepthUpdate> depths(kMaxDepth);

    std::size_t total_ticks = 0, total_depth = 0, bad = 0, unknown = 0;
    // The decoder stamps every tick with a monotonically increasing sequence
    // number, which is the `tick_seqno` rule 10 wants in the reproducibility
    // tuple. It is the DECODER's counter, not a wall clock, so a replay of the
    // same bytes produces the same seqnos.
    std::uint32_t seq = 0;
    std::int64_t last_price_paise[8]{};
    std::uint32_t last_token[8]{};
    std::size_t seen_tokens = 0;

    altair::TickStoreWriter writer;
    bool storing = false;
    if (store_s != nullptr) {
        const auto today = std::chrono::system_clock::now();
        const altair::Timestamp session_date{
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                today.time_since_epoch()).count()};
        storing = writer.open(store_s, session_date).has_value();
        if (!storing) {
            std::printf("  could not open the tick store at %s -- continuing "
                        "without it\n", store_s);
        }
    }

    const auto on_frame = [&](const unsigned char* p, std::size_t n) {
        // THE CLOCK IS READ HERE AND PASSED IN, not read inside the decoder.
        // A decoder that reads a wall clock cannot be replayed.
        const auto now = std::chrono::system_clock::now();
        const altair::Timestamp recv{
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                now.time_since_epoch()).count()};

        const auto r = altair::decode_kite_frame(
            p, n, g_specs, recv, seq, ticks.data(), kMaxTicks, depths.data(),
            kMaxDepth);
        if (!r) { ++bad; return; }
        total_ticks += r->ticks;
        total_depth += r->depths;
        unknown += r->unknown_token;

        for (std::size_t i = 0; i < r->ticks; ++i) {
            const altair::Tick& t = ticks[i];
            if (storing) { (void)writer.write(t); }

            // A TICK CARRIES CANONICAL IDENTITY, NOT A BROKER TOKEN, AND
            // THAT IS D1 RATHER THAN AN INCONVENIENCE.
            //
            // Kite's instrument_token and XTS's ExchangeInstrumentID are
            // different number spaces for the same contract, so `Tick` holds
            // an InstrumentId and nothing downstream ever sees a token. The
            // status file this program writes is read by the UI, which knows
            // Kite tokens and not our ids -- so the token is recovered HERE,
            // at the one boundary that legitimately knows both, by the order
            // the specs were added in. SpecStore::add assigns ids densely
            // from zero in insertion order, so id i is tokens[i].
            const auto idx = static_cast<std::size_t>(t.id);
            if (idx >= tokens.size()) { continue; }
            const std::uint32_t tok = tokens[idx];

            bool found = false;
            for (std::size_t k = 0; k < seen_tokens; ++k) {
                if (last_token[k] == tok) {
                    last_price_paise[k] = t.last.raw();
                    found = true;
                    break;
                }
            }
            if (!found && seen_tokens < 8) {
                last_token[seen_tokens] = tok;
                last_price_paise[seen_tokens] = t.last.raw();
                ++seen_tokens;
            }
        }
    };

    const auto never_stop = [] { return false; };

    const auto st = altair::kite_ticker_run(
        api_key, access, tokens, mode, on_frame, never_stop,
        std::chrono::seconds{seconds});

    if (!st) {
        std::printf("\n  FEED FAILED: %s\n",
                    altair::ticker_error_text(st.error()));
        return 4;
    }

    std::printf("\n  %zu binary frame(s), %zu heartbeat(s), %zu text frame(s), "
                "%zu bytes\n"
                "  %zu tick(s), %zu depth update(s), %zu undecodable, "
                "%zu unknown token\n",
                st->binary_frames, st->heartbeats, st->text_frames, st->bytes,
                total_ticks, total_depth, bad, unknown);

    // A FEED THAT ONLY HEARTBEATS IS CONNECTED AND SILENT, and that reads
    // exactly like a healthy one in any counter that lumps the two together.
    // It is also what a subscription looks like outside market hours, which is
    // the common case and deserves to be said rather than diagnosed.
    if (st->binary_frames == 0) {
        std::printf(
            "\n  CONNECTED, AND NOTHING TICKED. The socket opened, the\n"
            "  subscription was accepted, and only heartbeats arrived. That\n"
            "  is what a closed market looks like -- it is not a failure and\n"
            "  it is not a healthy feed either.\n");
    }

    // ---- the status file the UI reads --------------------------------------
    //
    // Temp then rename, so a reader never sees half a file. Same as the quote
    // and account snapshots.
    {
        const auto now = std::chrono::system_clock::now();
        const long long at =
            std::chrono::duration_cast<std::chrono::seconds>(
                now.time_since_epoch()).count();
        const std::string tmp = out_path + ".tmp";
        std::ofstream f(tmp, std::ios::binary);
        if (f) {
            f << "{\n  \"fetched_at_unix\": " << at
              << ",\n  \"seconds\": " << seconds
              << ",\n  \"mode\": \"" << altair::ticker_mode_name(mode)
              << "\",\n  \"binary_frames\": " << st->binary_frames
              << ",\n  \"heartbeats\": " << st->heartbeats
              << ",\n  \"text_frames\": " << st->text_frames
              << ",\n  \"bytes\": " << st->bytes
              << ",\n  \"ticks\": " << total_ticks
              << ",\n  \"depth_updates\": " << total_depth
              << ",\n  \"undecodable\": " << bad
              << ",\n  \"unknown_token\": " << unknown
              << ",\n  \"last\": [";
            for (std::size_t k = 0; k < seen_tokens; ++k) {
                if (k != 0) { f << ","; }
                f << "\n    {\"token\": " << last_token[k]
                  << ", \"last_price_paise\": " << last_price_paise[k] << "}";
            }
            f << "\n  ]\n}\n";
            f.close();
            std::remove(out_path.c_str());
            if (std::rename(tmp.c_str(), out_path.c_str()) == 0) {
                std::printf("  wrote %s\n", out_path.c_str());
            }
        }
    }

    if (storing) { (void)writer.close(); }
    return 0;
}
