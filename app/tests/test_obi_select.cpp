// app/obi_select.hpp: stocks scored off a recorded session tape -- the busy,
// tight, deep one first; a thin one last; one seen too little not scored;
// the indices never candidates; and the config file written so that
// live/obi_underlyings.hpp reads back exactly what was chosen.
//
// No check description here may contain the substring "F" "AIL" joined.

#include <app/obi_select.hpp>
#include <live/obi_underlyings.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("  %s: %s\n", ok ? "ok  " : "BAD ", what);
    if (!ok) ++failures;
}

using namespace altair;
namespace fs = std::filesystem;
namespace os = altair::obi_select;

constexpr std::int64_t kSec = 1'000'000'000LL;

std::vector<std::uint8_t> frame(std::uint32_t topic, std::uint64_t seq, std::int64_t ns, const std::uint8_t* body, std::size_t n) {
    std::vector<std::uint8_t> f(kFrameHeaderBytes + n);
    FrameHeader h;
    h.kind = FrameKind::Delta;
    h.channel = Channel::State;
    h.topic = topic;
    h.seq = seq;
    h.payload_len = static_cast<std::uint32_t>(n);
    h.engine_time_ns = ns;
    h.server_time_ns = ns;
    (void)encode_header(h, f.data(), kFrameHeaderBytes);
    std::copy(body, body + n, f.begin() + static_cast<std::ptrdiff_t>(kFrameHeaderBytes));
    return f;
}

/// 20 minutes of 10:00: BUSY (1-tick spread, 5,000 at the touch, 1,000 shares
/// a second), THIN (4-tick spread, 50 at the touch, 10 a second), RARE (seen
/// for 10 s only) and the NIFTY index (never a candidate).
void write_tape(const fs::path& path) {
    live::TapeWriter w(path.string());
    const std::string uni = std::string(live::kLiveUniverseHeader) + "\n"
                          + "1,NSE:BUSY-EQ,BUSY,BUSY,equity,,0,1,0.05,stocks,0\n"
                          + "2,NSE:THIN-EQ,THIN,THIN,equity,,0,1,0.05,stocks,0\n"
                          + "3,NSE:RARE-EQ,RARE,RARE,equity,,0,1,0.05,stocks,0\n"
                          + "4,BSE:BUSY-A,BUSY,BUSY,equity,,0,1,0.05,stocks,0\n"
                          + "256265,NSE:NIFTY50-INDEX,NIFTY 50,NIFTY,index,,0,1,0.05,indices,0\n";
    w.write(live::TapeKind::Start, 1, live::tape_pack({{"args", "date=2026-11-02\nlatency_ns=0\n"}, {"universe.csv", uni}}));
    w.write(live::TapeKind::Connected, 2);
    const std::int64_t t0 = (live::parse_day("2026-11-02") * 86400 + 10 * 3600 - 19800) * kSec;
    std::uint64_t seq_t = 0, seq_q = 0;
    const auto trade = [&](std::uint32_t tok, std::int64_t px, std::int64_t vol, std::int64_t at) {
        PricePayload p;
        p.token = tok; p.last_paise = px; p.last_qty = 1; p.volume = vol; p.flags |= kPriceHasVolume; p.exchange_ts_ns = at;
        std::vector<std::uint8_t> b(price_frame_bytes(0));
        const auto n = encode_price(p, nullptr, nullptr, b.data(), b.size());
        const auto f = frame(kTopicTrades, ++seq_t, at, b.data(), n ? *n : 0);
        w.write(live::TapeKind::Data, at, f.data(), f.size());
    };
    const auto quote = [&](std::uint32_t tok, std::int64_t bid, std::int64_t ask, std::int64_t qty, std::int64_t at) {
        QuotePayload q;
        q.token = tok; q.flags = kQuoteHasTop; q.bid = bid; q.ask = ask; q.bid_qty = qty; q.ask_qty = qty;
        std::vector<std::uint8_t> b(kQuotePayloadBytes);
        const auto n = encode_quote(q, b.data(), b.size());
        const auto f = frame(kTopicQuote, ++seq_q, at, b.data(), n ? *n : 0);
        w.write(live::TapeKind::Data, at, f.data(), f.size());
    };
    for (std::int64_t s = 0; s < 1200; ++s) {
        const std::int64_t at = t0 + s * kSec;
        trade(1, 100000, 1000 * (s + 1), at);
        quote(1, 100000, 100005, 5000, at + kSec / 2);
        trade(2, 50000, 10 * (s + 1), at);
        quote(2, 50000, 50020, 50, at + kSec / 2);
        quote(256265, 2400000, 2400010, 0, at + kSec / 2);
        quote(4, 99000, 99100, 1, at + kSec / 2);   // the BSE twin: wide and thin, must not count
        if (s < 10) quote(3, 20000, 20005, 1000, at + kSec / 2);
    }
    w.flush();
}

} // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / "altair_obi_select_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "data/live/tapes");
    fs::create_directories(root / "config");
    write_tape(root / "data/live/tapes/2026-11-02-100000.tape");

    std::map<std::string, os::StockDay> days;
    std::string err;
    check(os::collect(altair::tune::find_tapes(root), days, err), "the tape replays");
    check(days.count("NIFTY") == 0, "the index is never a candidate");
    check(days.count("BUSY") == 1 && days.count("THIN") == 1 && days.count("RARE") == 1, "every stock book is seen");
    const auto& busy = days["BUSY"];
    check(busy.spread_ticks.size() >= 1100 && busy.spread_ticks.size() <= 1201, "sampled about once a second");
    check(std::fabs(busy.value - 1199.0 * 1000.0 * 1000.0) < 1.0, "value traded = the volume as it moved x the price");

    const auto ranked = os::score(days, 300);
    check(ranked.size() == 2, "a stock seen for 10 s is not scored");
    check(!ranked.empty() && ranked.front().symbol == "BUSY" && ranked.back().symbol == "THIN",
          "busy, deep and tight ranks first; thin and wide last");
    if (ranked.size() == 2) {
        check(ranked[0].score == 1.0 && ranked[1].score == 0.0, "best on every count scores 1, worst 0");
        check(std::fabs(ranked[0].spread_ticks - 1.0) < 1e-9 && std::fabs(ranked[1].spread_ticks - 4.0) < 1e-9, "median spread in ticks");
        check(std::fabs(ranked[0].depth_rs - 5000.0 * 1000.025) < 1.0, "touch depth in rupees at the mid");
        check(std::fabs(ranked[0].target - 0.40) < 1e-9 && std::fabs(ranked[0].stop - 0.20) < 1e-9 && ranked[0].safety == 0.05,
              "a one-tick stock: target 8 ticks, stop half, safety a tick");
        check(std::fabs(ranked[1].target - 0.80) < 1e-9 && std::fabs(ranked[1].stop - 0.40) < 1e-9, "a four-tick stock: target 4 spreads");

        const fs::path cfg = root / "config/obi_underlyings.csv";
        check(os::write_underlyings(cfg, ranked, "2026-11-02"), "the config file is written");
        const auto back = live::read_obi_underlyings(cfg.string());
        check(back.size() == 2 && back[0].symbol == "BUSY" && std::fabs(back[0].target_pts - 0.40) < 1e-9
                  && std::fabs(back[1].stop_pts - 0.40) < 1e-9 && std::fabs(back[1].safety_pts - 0.05) < 1e-9,
              "the OBI trader reads back exactly what was chosen");
        check(!fs::exists(cfg.string() + ".tmp"), "no temporary file left behind");
    }
    const auto ties = os::pct_ranks({3.0, 1.0, 3.0});
    check(ties[1] == 0.0 && ties[0] == 0.75 && ties[2] == 0.75, "tied values share the mean rank");

    fs::remove_all(root, ec);
    std::printf("%s\n", failures == 0 ? "all obi select checks passed" : "obi select checks did not pass");
    return failures == 0 ? 0 : 1;
}
