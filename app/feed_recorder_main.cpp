// app/feed_recorder_main.cpp -- altair_feed_recorder: the whole FYERS stream, to disk, all session.
//
//     altair_feed_recorder [--port 7421] [--root DIR] [--out DIR] [--until HH:MM]
//                          [--seconds N] [--min-free-gb N] --go
//
// altair_price_service owns the FYERS connection and republishes every trade,
// quote and book (5 levels from the HSM socket, 50 from the TBT socket for the
// instruments FYERS allows) on 127.0.0.1:7421. This is one more reader of that
// bus, with no other job: every byte it receives goes into a session tape
// (live/tape.hpp), the same format the live engine writes, so altair_iv_record,
// altair_tune, altair_exec_study and `altair_store decode` read it unchanged.
//
// WHY A SEPARATE PROCESS. The engine records only while the desktop runs it.
// This runs from a Windows scheduled task (ops/install_tasks.ps1) whether the
// desktop is open or not, and it decides nothing, so it cannot be wrong about
// anything but bytes.
//
//     data/store/tapes/<YYYY-MM-DD>/<YYYY-MM-DD>-<HHMMSS>.tape     (git-ignored)
//
// A tape's first record names its universe (data/live/universe.csv, which the
// price service writes when it starts): the bus carries tokens, not names, and
// the token-to-contract pairing changes every day. When the price service
// restarts with a different universe (a new ATM, a new expiry), the recorder
// starts a NEW tape at the next connect, so each tape is self-describing.
//
// A SIM or replay feed (altair_price_service --sim / --replay: its frames carry
// the flag) is not recorded: it is not the market. Each connection is judged by its first
// priced frame.
//
// It never reads a credential: it only listens on loopback. It stops at
// --until (IST, default 15:45), on Ctrl+C, when a write fails, or when the
// disk falls below --min-free-gb (default 2). Every start, roll, stop and a
// half-hourly progress line go to the activity log (app/activity_log.hpp).
#include <app/live_feed_reader.hpp>   // first: Boost.Asio settles the Windows header order

#include <app/activity_log.hpp>
#include <live/tape.hpp>
#include <server/price_payload.hpp>
#include <server/protocol.hpp>
#include <server/quote_payload.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace lv = altair::live;
namespace act = altair::activity;

std::atomic<bool> g_stop{false};
extern "C" void on_stop(int) { g_stop.store(true); }

[[nodiscard]] std::int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
[[nodiscard]] std::int64_t wall_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

[[nodiscard]] int parse_hhmm(const std::string& s) {
    if (s.size() != 5 || s[2] != ':') return -1;
    const int h = std::atoi(s.substr(0, 2).c_str()), m = std::atoi(s.substr(3, 2).c_str());
    if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
    return h * 60 + m;
}

[[nodiscard]] std::string file_text(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return in ? std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()) : std::string();
}

[[nodiscard]] std::string hhmmss_ist(std::int64_t unix_s) {
    const std::int64_t s = ((unix_s + act::kIstOffsetSec) % 86400 + 86400) % 86400;
    char b[8];
    std::snprintf(b, sizeof b, "%02lld%02lld%02lld", static_cast<long long>(s / 3600),
                  static_cast<long long>((s / 60) % 60), static_cast<long long>(s % 60));
    return b;
}

void usage(const char* exe) {
    std::printf(
        "  Record everything altair_price_service publishes, to session tapes.\n\n"
        "    %s [--port 7421] [--root DIR] [--out DIR] [--until HH:MM] [--seconds N]\n"
        "        [--min-free-gb N] --go\n\n"
        "    --port         the price service's loopback port (default 7421)\n"
        "    --root         the ALTAIR tree (default: the source tree it was built from)\n"
        "    --out          where tapes go (default <root>/data/store/tapes; a folder a day)\n"
        "    --until        stop at this IST time (default 15:45)\n"
        "    --seconds      stop after N seconds instead (testing)\n"
        "    --min-free-gb  stop recording below this much free disk (default 2)\n"
        "    --go           record. Without it this prints what it would do and exits.\n\n"
        "  Start the feed first (altair_price_service --live --go), or let\n"
        "  ops\\record_session.ps1 do both. It waits for the feed and reconnects.\n",
        exe);
}

} // namespace

int main(int argc, char** argv) {
    unsigned short port = 7421;
#ifdef ALTAIR_SOURCE_DIR
    fs::path root = ALTAIR_SOURCE_DIR;
#else
    fs::path root = ".";
#endif
    fs::path out;
    int until = 15 * 60 + 45;
    long seconds = 0;
    double min_free_gb = 2.0;
    bool go = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has = i + 1 < argc;
        if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
        if (a == "--go") { go = true; continue; }
        if (a == "--port" && has) {
            const long p = std::strtol(argv[++i], nullptr, 10);
            if (p < 1 || p > 65535) { std::printf("--port must be 1..65535\n"); return 2; }
            port = static_cast<unsigned short>(p);
            continue;
        }
        if (a == "--root" && has) { root = argv[++i]; continue; }
        if (a == "--out" && has) { out = argv[++i]; continue; }
        if (a == "--until" && has) {
            until = parse_hhmm(argv[++i]);
            if (until < 0) { std::printf("--until must be HH:MM\n"); return 2; }
            continue;
        }
        if (a == "--seconds" && has) {
            seconds = std::strtol(argv[++i], nullptr, 10);
            if (seconds < 1 || seconds > 86400) { std::printf("--seconds must be 1..86400\n"); return 2; }
            continue;
        }
        if (a == "--min-free-gb" && has) { min_free_gb = std::atof(argv[++i]); continue; }
        usage(argv[0]);
        return 2;
    }
    if (out.empty()) out = root / "data" / "store" / "tapes";

    const std::int64_t start_unix = unix_now();
    const std::string day = act::ist_day(start_unix * 1000);
    const std::int64_t day_start = (start_unix + act::kIstOffsetSec) / 86400 * 86400 - act::kIstOffsetSec;
    const std::int64_t deadline = seconds > 0 ? start_unix + seconds : day_start + static_cast<std::int64_t>(until) * 60;
    std::printf("  feed recorder: 127.0.0.1:%u -> %s\n  until %s IST (%lld s)\n", static_cast<unsigned>(port),
                (out / day).string().c_str(), act::ist_stamp(deadline * 1000).substr(11, 5).c_str(),
                static_cast<long long>(deadline - start_unix));
    if (!go) {
        std::printf("\n  DRY RUN: nothing was opened. Add --go to record.\n");
        return 0;
    }
    if (deadline <= start_unix) {
        std::printf("  already past --until: nothing to record\n");
        act::write(root, "recorder", "INFO", "started after --until; nothing recorded");
        return 0;
    }
    std::signal(SIGINT, on_stop);
    std::signal(SIGTERM, on_stop);

    std::error_code ec;
    const fs::path dir = out / day;
    fs::create_directories(dir, ec);
    if (ec) {
        act::write(root, "recorder", "ERROR", "cannot create " + dir.string() + ": " + ec.message());
        std::printf("  cannot create %s\n", dir.string().c_str());
        return 2;
    }
    act::write(root, "recorder", "INFO", "started: port " + std::to_string(port) + ", tapes in " + dir.string()
                                             + ", until " + act::ist_stamp(deadline * 1000).substr(11, 5) + " IST");

    std::optional<lv::TapeWriter> tape;
    std::string tape_path, tape_universe;
    std::uint64_t total_bytes = 0, connects = 0, tapes_opened = 0;
    int code = 0;
    const auto close_tape = [&](const char* why) {
        if (!tape) return;
        (void)tape->flush();
        act::write(root, "recorder", "INFO", std::string("tape closed (") + why + "): " + tape_path + ", "
                                                 + std::to_string(tape->bytes_written() / (1024 * 1024)) + " MB");
        tape.reset();
    };
    // A new tape whenever the universe the price service streams has changed.
    const auto ensure_tape = [&]() -> bool {
        const std::string uni = file_text(root / "data" / "live" / "universe.csv");
        if (tape && uni == tape_universe) return true;
        close_tape("universe changed");
        tape_path = (dir / (day + "-" + hhmmss_ist(unix_now()) + ".tape")).string();
        tape.emplace(tape_path);
        if (!tape->ok()) {
            act::write(root, "recorder", "ERROR", "cannot write " + tape_path);
            tape.reset();
            return false;
        }
        tape_universe = uni;
        // args "date=" is what altair_iv_record and altair_tune read off a tape.
        const lv::TapeSections start{{"args", "date=" + day + "\nsource=altair_feed_recorder\n"},
                                     {"universe.csv", uni}};
        tape->write(lv::TapeKind::Start, wall_ns(), lv::tape_pack(start));
        ++tapes_opened;
        act::write(root, "recorder", uni.empty() ? "WARN" : "INFO",
                   "tape opened: " + tape_path + (uni.empty() ? " (no data/live/universe.csv yet: tokens only)" : ""));
        return tape->ok();
    };

    // Each connection is judged by its first priced frame: SIM is not recorded.
    // Until then its bytes wait in `pending` (a few KB in practice).
    enum class Conn { None, Undecided, Recording, Skipping };
    Conn conn = Conn::None;
    std::vector<std::uint8_t> pending;
    std::int64_t pending_at = 0;
    const auto decide = [&]() -> std::optional<bool> {   // true: record; false: SIM or replay
        std::size_t at = 0;
        while (pending.size() - at >= altair::kFrameHeaderBytes) {
            const auto h = altair::decode_header(pending.data() + at, pending.size() - at);
            if (!h) return true;   // unframed: record it, the decoder will say so
            const std::size_t need = altair::kFrameHeaderBytes + h->payload_len;
            if (pending.size() - at < need) break;
            const std::uint8_t* body = pending.data() + at + altair::kFrameHeaderBytes;
            if (h->topic == altair::kTopicTrades || h->topic == altair::kTopicBook) {
                if (const auto d = altair::decode_price(body, h->payload_len)) return !d->payload.has(altair::kPriceSimulated) && !d->payload.has(altair::kPriceReplay);
            } else if (h->topic == altair::kTopicQuote) {
                if (const auto q = altair::decode_quote(body, h->payload_len)) return !q->has(altair::kQuoteSimulated) && !q->has(altair::kQuoteReplay);
            }
            at += need;
        }
        if (pending.size() > (64u << 20)) return true;
        return std::nullopt;
    };
    altair::live_feed::FeedReader reader{port};
    auto last_flush = std::chrono::steady_clock::now();
    auto last_report = last_flush, last_disk = last_flush - std::chrono::minutes(2);
    bool waiting_logged = false;
    while (!g_stop.load() && unix_now() < deadline) {
        for (auto& ev : reader.take(std::chrono::milliseconds(200))) {
            const std::int64_t at = wall_ns();
            if (ev.kind == altair::live_feed::FeedEvent::Connected) {
                ++connects;
                waiting_logged = false;
                conn = Conn::Undecided;
                pending.clear();
                pending_at = at;
                continue;
            }
            if (ev.kind == altair::live_feed::FeedEvent::Disconnected) {
                if (conn == Conn::Recording && tape) {
                    tape->write(lv::TapeKind::Disconnected, at, ev.note);
                    act::write(root, "recorder", "WARN", "price service went away: " + ev.note);
                }
                conn = Conn::None;
                pending.clear();
                continue;
            }
            if (conn == Conn::Skipping || conn == Conn::None) continue;
            if (conn == Conn::Undecided) {
                pending.insert(pending.end(), ev.bytes.begin(), ev.bytes.end());
                const auto verdict = decide();
                if (!verdict) continue;
                if (!*verdict) {
                    conn = Conn::Skipping;
                    pending.clear();
                    act::write(root, "recorder", "INFO", "the price service is running SIM or a replay: not recorded");
                    continue;
                }
                if (!ensure_tape()) { code = 3; g_stop.store(true); break; }
                tape->write(lv::TapeKind::Connected, pending_at, nullptr, 0);
                tape->write(lv::TapeKind::Data, at, pending.data(), pending.size());
                total_bytes += pending.size();
                pending.clear();
                conn = Conn::Recording;
                act::write(root, "recorder", "INFO", "recording the live feed");
                continue;
            }
            tape->write(lv::TapeKind::Data, at, ev.bytes.data(), ev.bytes.size());
            total_bytes += ev.bytes.size();
        }
        if (tape && !tape->ok()) {
            act::write(root, "recorder", "ERROR", "TAPE WRITE FAILED: " + tape_path + "; recording stopped");
            code = 3;
            break;
        }
        const auto now = std::chrono::steady_clock::now();
        if (!reader.connected() && !waiting_logged && connects == 0 && now - last_flush > std::chrono::seconds(30)) {
            act::write(root, "recorder", "WARN", "waiting for altair_price_service on port " + std::to_string(port));
            waiting_logged = true;
        }
        if (now - last_flush >= std::chrono::seconds(1)) {
            last_flush = now;
            if (tape) (void)tape->flush();   // a failure shows in ok(), checked above
        }
        if (now - last_disk >= std::chrono::minutes(1)) {
            last_disk = now;
            const auto space = fs::space(dir, ec);
            if (!ec && static_cast<double>(space.available) < min_free_gb * 1024.0 * 1024.0 * 1024.0) {
                act::write(root, "recorder", "ERROR", "disk below " + std::to_string(min_free_gb).substr(0, 4)
                                                          + " GB free; recording stopped");
                code = 4;
                break;
            }
        }
        if (now - last_report >= std::chrono::minutes(30)) {
            last_report = now;
            act::write(root, "recorder", "INFO", "recording: " + std::to_string(total_bytes / (1024 * 1024)) + " MB so far, "
                                                     + std::to_string(connects) + " connect(s), "
                                                     + std::to_string(reader.throttled()) + " throttle(s)");
        }
    }
    close_tape(g_stop.load() ? "stopped" : "end of session");
    act::write(root, "recorder", code == 0 ? "INFO" : "ERROR",
               "finished: " + std::to_string(total_bytes / (1024 * 1024)) + " MB in " + std::to_string(tapes_opened)
                   + " tape(s), " + std::to_string(connects) + " connect(s), exit " + std::to_string(code));
    std::printf("  %llu MB in %llu tape(s), %llu connect(s)\n", static_cast<unsigned long long>(total_bytes / (1024 * 1024)),
                static_cast<unsigned long long>(tapes_opened), static_cast<unsigned long long>(connects));
    return code;
}
