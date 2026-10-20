// app/iv_record_main.cpp -- altair_iv_record: the at-the-money implied vol of
// every minute the terminal streamed, from its session tapes.
//
// The live engine records every session to data/live/tapes/ and keeps the
// last few days (--keep-tapes). This reads each FYERS tape there (SIM and
// replayed prices are skipped: they are not the market), works out each
// minute's ATM IV for NIFTY and BANKNIFTY (app/iv_record.hpp) and writes
//     data/live/iv/<INDEX>/<YYYY-MM-DD>.csv    time,iv,forward,strike,expiry
// which altair_forecast_curriculum reads (--recorded-iv) once it covers
// enough sessions. Run it after the close, at least as often as tapes rotate;
// a day already written is kept unless --force.

#include <app/iv_record.hpp>
#include <live/tape.hpp>
#include <live/universe.hpp>
#include <server/price_payload.hpp>
#include <server/protocol.hpp>
#include <server/quote_payload.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace lv = altair::live;
namespace ir = altair::iv_record;

std::string ist_text(std::int64_t ist) {
    const std::int64_t day = ist / 86'400;
    // civil from days (Howard Hinnant)
    std::int64_t z = day + 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const std::int64_t doe = z - era * 146097;
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::int64_t mp = (5 * doy + 2) / 153;
    const std::int64_t d = doy - (153 * mp + 2) / 5 + 1;
    const std::int64_t m = mp < 10 ? mp + 3 : mp - 9;
    const std::int64_t y = yoe + era * 400 + (m <= 2 ? 1 : 0);
    const std::int64_t s = ist % 86'400;
    char b[40];
    std::snprintf(b, sizeof b, "%04lld-%02lld-%02lldT%02lld:%02lld:%02lld+05:30", static_cast<long long>(y),
                  static_cast<long long>(m), static_cast<long long>(d), static_cast<long long>(s / 3600),
                  static_cast<long long>((s / 60) % 60), static_cast<long long>(s % 60));
    return b;
}

void usage(const char* exe) {
    std::printf(
        "usage: %s [--tapes DIR] [--out DIR] [--rate R] [--force]\n"
        "  --tapes  the engine's session tapes (default data/live/tapes)\n"
        "  --out    where the per-day IV files go (default data/live/iv)\n"
        "  --rate   the rate in Black-76 and in put-call parity (default 0.065)\n"
        "  --force  rewrite days already written\n",
        exe);
}

} // namespace

int main(int argc, char** argv) {
    fs::path tapes = "data/live/tapes", out = "data/live/iv";
    double rate = 0.065;
    bool force = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has = i + 1 < argc;
        if (a == "--tapes" && has) tapes = argv[++i];
        else if (a == "--out" && has) out = argv[++i];
        else if (a == "--rate" && has) rate = std::atof(argv[++i]);
        else if (a == "--force") force = true;
        else if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
        else { usage(argv[0]); return 2; }
    }
    std::error_code ec;
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(tapes, ec)) {
        if (e.path().extension() == ".tape") files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        std::printf("no tapes under %s: the live engine records them while it runs (on by default)\n", tapes.string().c_str());
        return 0;
    }
    std::size_t written = 0, kept = 0, skipped = 0;
    for (const auto& path : files) {
        lv::TapeReader in(path.string());
        lv::TapeRecord rec;
        lv::TapeSections start;
        if (!in.ok() || !in.next(rec) || rec.kind != lv::TapeKind::Start || !lv::tape_unpack(rec.text(), start)) {
            std::printf("  %s: not a session tape, skipped\n", path.filename().string().c_str());
            ++skipped;
            continue;
        }
        const std::string* uni = lv::tape_section(start, "universe.csv");
        if (uni == nullptr) { std::printf("  %s: no universe, skipped\n", path.filename().string().c_str()); ++skipped; continue; }
        std::istringstream us(*uni);
        const auto universe = lv::read_universe_stream(us);
        std::vector<ir::AtmIvRecorder> rec_by;
        const std::vector<std::string> indices{"NIFTY", "BANKNIFTY"};
        for (const auto& n : indices) rec_by.emplace_back(universe, n, rate);
        std::vector<std::uint8_t> buf;
        std::size_t simulated = 0;
        while (in.next(rec)) {
            if (rec.kind == lv::TapeKind::Start) break;
            if (rec.kind == lv::TapeKind::Connected || rec.kind == lv::TapeKind::Disconnected) { buf.clear(); continue; }
            if (rec.kind != lv::TapeKind::Data) continue;
            buf.insert(buf.end(), rec.bytes.begin(), rec.bytes.end());
            std::size_t at = 0;
            while (buf.size() - at >= altair::kFrameHeaderBytes) {
                const auto h = altair::decode_header(buf.data() + at, buf.size() - at);
                if (!h) { buf.clear(); at = 0; break; }
                const std::size_t need = altair::kFrameHeaderBytes + h->payload_len;
                if (buf.size() - at < need) break;
                const std::uint8_t* body = buf.data() + at + altair::kFrameHeaderBytes;
                if (h->topic == altair::kTopicQuote) {
                    const auto q = altair::decode_quote(body, h->payload_len);
                    if (q && q->has(altair::kQuoteSimulated)) ++simulated;
                    else if (q && q->has(altair::kQuoteHasTop) && q->bid > 0 && q->ask > q->bid && !q->has(altair::kQuoteReplay)) {
                        for (auto& r : rec_by) r.on_price(q->token, 0.5 * static_cast<double>(q->bid + q->ask) / 100.0, h->engine_time_ns);
                    }
                } else if (h->topic == altair::kTopicTrades) {
                    const auto d = altair::decode_price(body, h->payload_len);
                    if (d && d->payload.has(altair::kPriceSimulated)) ++simulated;
                    else if (d && !d->payload.has(altair::kPriceReplay)) {
                        const std::int64_t ns = d->payload.exchange_ts_ns > 0 ? d->payload.exchange_ts_ns : h->engine_time_ns;
                        for (auto& r : rec_by) r.on_price(d->payload.token, static_cast<double>(d->payload.last_paise) / 100.0, ns);
                    }
                }
                at += need;
            }
            if (at > 0) buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(at));
        }
        if (simulated > 0) {
            std::printf("  %s: simulated prices, not the market -- skipped\n", path.filename().string().c_str());
            ++skipped;
            continue;
        }
        for (std::size_t k = 0; k < indices.size(); ++k) {
            rec_by[k].finish();
            const auto& mins = rec_by[k].minutes();
            if (mins.empty()) continue;
            const std::string day = ist_text(mins.front().t).substr(0, 10);
            const fs::path dir = out / indices[k];
            fs::create_directories(dir, ec);
            const fs::path file = dir / (day + ".csv");
            if (fs::exists(file) && !force) { ++kept; continue; }
            std::ofstream o(file.string() + ".tmp");
            o << "time,iv,forward,strike,expiry\n";
            for (const auto& m : mins) {
                char line[160];
                std::snprintf(line, sizeof line, "%s,%.6f,%.2f,%.2f,%s\n", ist_text(m.t).c_str(), m.iv, m.forward, m.strike,
                              ist_text(m.expiry_day * 86'400).substr(0, 10).c_str());
                o << line;
            }
            o.close();
            fs::rename(file.string() + ".tmp", file, ec);
            ++written;
            std::printf("  %s %s: %zu minutes, ATM IV %.1f%% to %.1f%%\n", indices[k].c_str(), day.c_str(), mins.size(),
                        100.0 * std::min_element(mins.begin(), mins.end(), [](auto& a, auto& b) { return a.iv < b.iv; })->iv,
                        100.0 * std::max_element(mins.begin(), mins.end(), [](auto& a, auto& b) { return a.iv < b.iv; })->iv);
        }
    }
    std::printf("altair_iv_record: %zu day files written, %zu kept, %zu tapes skipped -> %s\n", written, kept, skipped,
                out.string().c_str());
    return 0;
}
