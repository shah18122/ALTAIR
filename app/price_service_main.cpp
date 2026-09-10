// app/price_service_main.cpp -- one process owns the feed.
//
// P37-03. Every live thing in this tree so far is a subprocess that runs,
// writes a JSON file and exits: the tick feed samples for N seconds, the quote
// refresh fetches once, the account panel fetches once, the dataset updater
// tops up and stops. Four programs, four reads of the credential, and not one
// of them is a stream -- so the "Live Grid" has always been a replay and the
// depth ladder has been handed a null book since the day it was written.
//
// This is the stream. It holds ONE Kite connection and republishes every tick
// on a local port as protocol frames. Anything that wants prices connects and
// reads; nothing else needs the credential.
//
// TWO SOURCES, ONE PATH -- HARD RULE 6.
//
//   --go              live from Kite
//   --replay SYM IV   bars from dataset/, at their own timestamps
//
// Both go through the same encoder, the same bus and the same frames. That is
// the rule that says a backtest which does not share the live code path is a
// lie, and it is also what makes this testable at all: the Kite token is a
// daily credential, and a service that could only be exercised with a live one
// would be exercised approximately never.
//
// A REPLAY FRAME SAYS SO. kPriceReplay is set, and engine_time carries the
// bar's ORIGINAL timestamp while server_time carries now -- so a subscriber
// that plots engine time sees 2015 and one that measures latency sees
// something absurd, which is exactly right. A replay must never be mistakable
// for a quiet afternoon.

#include <server/price_bus.hpp>

#include <boost/asio/io_context.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <broker/kite_ticker.hpp>
#include <feed/kite_decoder.hpp>
#include <instruments/contract_spec.hpp>

// FILE SCOPE, NOT THE STACK. SpecStore is 1.63 MB against a 1 MB default
// thread stack on MSVC, and a stack-allocated one crashes with 0xC00000FD --
// a code that names neither the object nor the file. P32-07 paid for this
// once already.
altair::SpecStore g_specs;

namespace {

void usage(const char* exe) {
    std::printf(
        "Altair price service -- one process owns the feed.\n\n"
        "  %s --replay <symbol> <interval> [--port N] [--seconds N]\n"
        "  %s --go [--port N] [--seconds N] [--tokens a,b,c]\n\n"
        "  --replay nifty 1m   serve bars from dataset/spot/<symbol>/<iv>/\n"
        "  --go                serve LIVE from Kite. Needs\n"
        "                      ALTAIR_KITE_API_KEY and data/kite_session.json\n"
        "  --port N            loopback port to publish on (default 7421)\n"
        "  --seconds N         stop after N seconds (default: run until\n"
        "                      interrupted; a replay always stops at the end)\n"
        "  --rate N            replay bars per second (default 200)\n\n"
        "WITHOUT --go OR --replay THIS PRINTS THIS TEXT AND EXITS. The same\n"
        "dry-run guard every other network binary here has: a program that\n"
        "opens a credentialed socket because it was run with no arguments is\n"
        "a program that will one day do it by accident.\n\n"
        "It binds 127.0.0.1 only. This carries live market data derived from\n"
        "a trading credential and protocol.hpp's session layer does not exist\n"
        "yet, so there is nothing to authenticate a remote reader with.\n",
        exe, exe);
}

/// Environment variable, or empty. _dupenv_s on MSVC because getenv is
/// deprecated there and /W4 with warnings-as-errors will not have it.
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
    return v != nullptr ? std::string(v) : std::string();
#endif
}

/// Read `access_token` out of data/kite_session.json.
///
/// A hand-rolled eight-line scan rather than a JSON library, deliberately: the
/// surface that touches a live trading credential should be as small as it can
/// be, and every other binary here does the same. The value is never printed.
[[nodiscard]] bool read_access_token(const char* path, std::string& out) {
    std::ifstream in(path);
    if (!in) { return false; }
    const std::string all((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
    const std::string key = "\"access_token\"";
    const std::size_t at = all.find(key);
    if (at == std::string::npos) { return false; }
    const std::size_t q1 = all.find('"', all.find(':', at) + 1);
    if (q1 == std::string::npos) { return false; }
    const std::size_t q2 = all.find('"', q1 + 1);
    if (q2 == std::string::npos) { return false; }
    out = all.substr(q1 + 1, q2 - q1 - 1);
    return !out.empty();
}

/// Bars from a dataset partition, oldest first.
struct Bar {
    std::int64_t ts_ns = 0;
    double close = 0.0;
    double volume = 0.0;
    bool has_volume = false;
};

/// Parse "2026-09-09T15:29:00+05:30" or "2026-09-09" to ns since epoch.
///
/// Only the fields that exist are read: a daily stamp has no time, and
/// inventing 00:00 for it would be inventing a bar that opened at midnight.
/// The offset is IST throughout the dataset, so it is subtracted rather than
/// parsed -- the day the tree holds a non-IST series this must be revisited,
/// and it will be visible because the stamps will be wrong by hours.
[[nodiscard]] std::int64_t parse_stamp_ns(const std::string& s) {
    if (s.size() < 10) { return 0; }
    std::tm tm{};
    tm.tm_year = std::atoi(s.substr(0, 4).c_str()) - 1900;
    tm.tm_mon = std::atoi(s.substr(5, 2).c_str()) - 1;
    tm.tm_mday = std::atoi(s.substr(8, 2).c_str());
    if (s.size() >= 19 && s[10] == 'T') {
        tm.tm_hour = std::atoi(s.substr(11, 2).c_str());
        tm.tm_min = std::atoi(s.substr(14, 2).c_str());
        tm.tm_sec = std::atoi(s.substr(17, 2).c_str());
    }
    // Days since epoch by civil-from-days (Howard Hinnant's algorithm), so
    // there is no dependence on the host time zone -- std::mktime would apply
    // whatever the machine is set to, which is how a dataset becomes a
    // function of the laptop that read it.
    const int y = tm.tm_year + 1900;
    const int m = tm.tm_mon + 1;
    const int d = tm.tm_mday;
    const int yy = y - (m <= 2 ? 1 : 0);
    const int era = (yy >= 0 ? yy : yy - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(yy - era * 400);
    const unsigned doy =
        static_cast<unsigned>((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const std::int64_t days =
        static_cast<std::int64_t>(era) * 146097
        + static_cast<std::int64_t>(doe) - 719468;
    const std::int64_t secs = days * 86400 + tm.tm_hour * 3600
                              + tm.tm_min * 60 + tm.tm_sec
                              - (5 * 3600 + 30 * 60);   // IST -> UTC
    return secs * 1'000'000'000LL;
}

[[nodiscard]] std::vector<Bar> load_bars(const std::string& dir) {
    std::vector<Bar> out;
    std::vector<std::string> files;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (e.path().extension() == ".csv") { files.push_back(e.path().string()); }
    }
    std::sort(files.begin(), files.end());
    for (const std::string& f : files) {
        std::ifstream in(f);
        if (!in) { continue; }
        std::string line;
        std::getline(in, line);                       // header
        while (std::getline(in, line)) {
            std::istringstream ss(line);
            std::string cell;
            int col = 0;
            Bar b;
            std::string stamp;
            while (std::getline(ss, cell, ',')) {
                if (col == 0) { stamp = cell; }
                if (col == 4 && !cell.empty()) { b.close = std::atof(cell.c_str()); }
                if (col == 5 && !cell.empty()) {
                    // ABSENT IS NOT ZERO. An index reports no volume at all
                    // and the ingested files leave the field EMPTY for
                    // exactly that reason -- a zero here would be a measured
                    // zero and the wire would carry it as one.
                    b.volume = std::atof(cell.c_str());
                    b.has_volume = true;
                }
                ++col;
            }
            b.ts_ns = parse_stamp_ns(stamp);
            if (b.close > 0.0 && b.ts_ns > 0) { out.push_back(b); }
        }
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    std::string mode, sym, iv;
    unsigned short port = 7421;
    int seconds = 0;
    int rate = 200;
    // NIFTY 50, NIFTY BANK, INDIA VIX -- the three the ticker has always
    // defaulted to. Overridable, because P38 needs an option chain here.
    std::vector<std::uint32_t> sub_tokens{256265u, 260105u, 264969u};

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--replay" && i + 2 < argc) {
            mode = "replay";
            sym = argv[i + 1];
            iv = argv[i + 2];
            i += 2;
        } else if (a == "--go") {
            mode = "live";
        } else if (a == "--port" && i + 1 < argc) {
            port = static_cast<unsigned short>(std::atoi(argv[++i]));
        } else if (a == "--seconds" && i + 1 < argc) {
            seconds = std::atoi(argv[++i]);
        } else if (a == "--tokens" && i + 1 < argc) {
            sub_tokens.clear();
            std::istringstream ts(argv[++i]);
            std::string one;
            while (std::getline(ts, one, ',')) {
                if (!one.empty()) {
                    sub_tokens.push_back(
                        static_cast<std::uint32_t>(std::atoll(one.c_str())));
                }
            }
        } else if (a == "--rate" && i + 1 < argc) {
            rate = std::max(1, std::atoi(argv[++i]));
        } else if (a == "--help" || a == "-h") {
            usage(argv[0]);
            return 0;
        }
    }

    if (mode.empty()) {
        usage(argv[0]);
        return 0;
    }

    boost::asio::io_context io;
    altair::PriceBus bus(io, port);
    if (!bus.ok()) {
        std::printf("could not bind 127.0.0.1:%u -- %s\n",
                    static_cast<unsigned>(port),
                    bus.error().message().c_str());
        return 1;
    }
    std::printf("altair price service on 127.0.0.1:%u  [%s]\n",
                static_cast<unsigned>(bus.port()), mode.c_str());
    std::fflush(stdout);

    const auto started = std::chrono::steady_clock::now();
    const auto expired = [&] {
        if (seconds <= 0) { return false; }
        return std::chrono::steady_clock::now() - started
               >= std::chrono::seconds(seconds);
    };

    if (mode == "replay") {
        const std::string dir =
            std::string(ALTAIR_DATASET_DIR) + "/spot/" + sym + "/" + iv;
        const std::vector<Bar> bars = load_bars(dir);
        if (bars.empty()) {
            std::printf("no bars under %s\n", dir.c_str());
            return 1;
        }
        std::printf("replaying %zu bars from %s at %d/s\n", bars.size(),
                    dir.c_str(), rate);
        std::fflush(stdout);

        // The token is looked up nowhere: a replay of a spot series is
        // published under the series' own canonical token so a subscriber
        // sees the same instrument it would see live.
        const std::uint32_t token =
            sym == "nifty" ? 256265u
            : sym == "banknifty" ? 260105u
            : sym == "indiavix" ? 264969u : 0u;

        const auto per = std::chrono::microseconds(1'000'000 / rate);
        auto next = std::chrono::steady_clock::now();
        std::size_t sent = 0;
        for (const Bar& b : bars) {
            if (expired()) { break; }
            altair::PricePayload p;
            p.token = token;
            p.last_paise = static_cast<std::int64_t>(b.close * 100.0 + 0.5);
            p.exchange_ts_ns = b.ts_ns;
            p.flags = altair::kPriceReplay;
            if (b.has_volume) {
                p.flags |= altair::kPriceHasVolume;
                p.volume = static_cast<std::int64_t>(b.volume);
            }
            bus.publish(altair::kTopicTrades, p, nullptr, nullptr, b.ts_ns);
            ++sent;
            next += per;
            std::this_thread::sleep_until(next);
            bus.poll();
        }
        std::printf("replay done: %zu frames published, %llu delivered, "
                    "%llu coalesced, %zu subscriber(s)\n",
                    sent,
                    static_cast<unsigned long long>(bus.frames_sent()),
                    static_cast<unsigned long long>(bus.coalesced()),
                    bus.clients());
        return 0;
    }

    // ---- live ------------------------------------------------------------
    const std::string api_key = env_or_empty("ALTAIR_KITE_API_KEY");
    std::string access;
    if (!read_access_token("data/kite_session.json", access)) {
        std::printf("no usable data/kite_session.json -- link a token first\n");
        return 2;
    }
    if (api_key.empty()) {
        std::printf("ALTAIR_KITE_API_KEY is not set\n");
        return 2;
    }

    // The decoder refuses an unknown token rather than inventing a price
    // scale, so every subscription is registered first. Scale 100 is what
    // equity and F&O ARE; lot and tick are deliberately 1 and unused, because
    // rule 1 says those come from the point-in-time spec store and this
    // program does not load one -- a lot size of 1 is obviously wrong in a way
    // that a plausible 75 typed here would not be.
    for (std::uint32_t t : sub_tokens) {
        altair::ContractSpec cs{};
        cs.token[static_cast<std::size_t>(altair::FeedSource::Kite)] = t;
        std::snprintf(cs.symbol, sizeof(cs.symbol), "TOKEN%u", t);
        cs.lot_size = altair::LotSize{1};
        cs.tick_size = altair::Price{1};
        cs.price_scale = 100;
        if (!g_specs.add(cs)) {
            std::printf("could not register token %u\n", t);
            return 2;
        }
    }

    constexpr std::size_t kMaxTicks = 512;
    constexpr std::size_t kMaxDepth = 512;
    std::vector<altair::Tick> ticks(kMaxTicks);
    std::vector<altair::DepthUpdate> depths(kMaxDepth);
    std::uint32_t seq = 0;
    std::size_t frames = 0, published = 0, unknown = 0, bad = 0;

    const auto on_frame = [&](const unsigned char* raw, std::size_t len) {
        ++frames;
        // THE CLOCK IS READ HERE AND PASSED IN, never inside the decoder. A
        // decoder that reads a wall clock cannot be replayed.
        const auto now = std::chrono::system_clock::now();
        const altair::Timestamp recv{
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                now.time_since_epoch()).count()};
        const auto r = altair::decode_kite_frame(
            raw, len, g_specs, recv, seq, ticks.data(), kMaxTicks,
            depths.data(), kMaxDepth);
        if (!r) { ++bad; return; }
        unknown += r->unknown_token;

        for (std::size_t i = 0; i < r->ticks; ++i) {
            const altair::Tick& t = ticks[i];
            // A TICK CARRIES CANONICAL IDENTITY, NOT A BROKER TOKEN. The
            // token is recovered HERE, at the one boundary that legitimately
            // knows both: SpecStore::add assigns ids densely from zero in
            // insertion order, so id i is sub_tokens[i]. Same boundary
            // kite_ticker_main.cpp uses, for the same reason.
            const auto idx = static_cast<std::size_t>(t.id);
            if (idx >= sub_tokens.size()) { continue; }

            altair::PricePayload p;
            p.token = sub_tokens[idx];
            p.last_paise = t.last.raw();
            p.last_qty = t.last_qty.raw();
            p.exchange_ts_ns = t.exchange_ts.ns_since_epoch();
            // ABSENCE IS NOT ZERO. An index reports no volume and no open
            // interest at all, and the decoder leaves them zero; publishing
            // them with the presence bit set would turn "this instrument has
            // no such field" into "it measured zero".
            if (t.volume.raw() > 0) {
                p.flags |= altair::kPriceHasVolume;
                p.volume = t.volume.raw();
            }
            if (t.oi > 0) {
                p.flags |= altair::kPriceHasOi;
                p.oi = t.oi;
            }
            if ((t.flags & static_cast<std::uint16_t>(
                     altair::TickFlag::NoExchangeTs)) != 0) {
                p.flags |= altair::kPriceNoExchTs;
            }
            bus.publish(altair::kTopicTrades, p, nullptr, nullptr,
                        p.exchange_ts_ns != 0 ? p.exchange_ts_ns
                                              : recv.ns_since_epoch());
            ++published;
        }

        for (std::size_t i = 0; i < r->depths; ++i) {
            const altair::DepthUpdate& d = depths[i];
            const auto idx = static_cast<std::size_t>(d.id);
            if (idx >= sub_tokens.size()) { continue; }
            altair::PricePayload p;
            p.token = sub_tokens[idx];
            p.exchange_ts_ns = d.exchange_ts.ns_since_epoch();
            const std::uint16_t levels = static_cast<std::uint16_t>(
                std::min<std::size_t>(altair::kMaxDepthLevels,
                                      std::max(d.bid_levels, d.ask_levels)));
            if (levels == 0) { continue; }
            p.flags |= altair::kPriceHasBook;
            p.depth_levels = levels;
            altair::PriceLevel bids[altair::kMaxDepthLevels]{};
            altair::PriceLevel asks[altair::kMaxDepthLevels]{};
            for (std::uint16_t k = 0; k < levels; ++k) {
                bids[k].price_paise = d.bid[k].px.raw();
                bids[k].qty = d.bid[k].qty.raw();
                bids[k].orders = d.bid[k].orders;
                asks[k].price_paise = d.ask[k].px.raw();
                asks[k].qty = d.ask[k].qty.raw();
                asks[k].orders = d.ask[k].orders;
            }
            bus.publish(altair::kTopicBook, p, bids, asks,
                        p.exchange_ts_ns != 0 ? p.exchange_ts_ns
                                              : recv.ns_since_epoch());
            ++published;
        }
        bus.poll();
    };

    std::printf("subscribing %zu token(s), full mode\n",
                sub_tokens.size());
    std::fflush(stdout);

    const auto stats = altair::kite_ticker_run(
        api_key, access, sub_tokens, altair::TickerMode::Full, on_frame,
        expired);

    std::printf("frames %zu, ticks+books published %zu, unknown token %zu, "
                "undecodable %zu\n",
                frames, published, unknown, bad);
    if (!stats) {
        std::printf("ticker ended: %s\n",
                    altair::ticker_error_text(stats.error()));
        return 1;
    }
    std::printf("delivered %llu frame(s) to subscribers, %llu coalesced\n",
                static_cast<unsigned long long>(bus.frames_sent()),
                static_cast<unsigned long long>(bus.coalesced()));
    return 0;
}
