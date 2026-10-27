// Tests for app/training_store.hpp (tapes decoded; dataset/ + FYERS merged at one
// cutoff) and app/activity_log.hpp (one log, no secrets).

#include <app/activity_log.hpp>
#include <app/training_store.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace st = altair::store;
namespace lv = altair::live;
namespace act = altair::activity;

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::printf("FAIL: %s\n", what); }
    else { std::printf("  ok  : %s\n", what); }
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
void spit(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}

// 2026-10-26 09:15:00 IST in UTC ns.
constexpr std::int64_t kDay = 20'752;   // 2026-10-26
constexpr std::int64_t k0915 = (kDay * 86'400 + 9 * 3600 + 15 * 60 - 19'800) * 1'000'000'000LL;

std::vector<std::uint8_t> frame(altair::FrameKind kind, std::uint64_t seq, std::uint32_t topic,
                                const altair::PricePayload& p, const altair::PriceLevel* bids = nullptr,
                                const altair::PriceLevel* asks = nullptr) {
    std::vector<std::uint8_t> out(altair::kFrameHeaderBytes + altair::price_frame_bytes(p.depth_levels));
    altair::FrameHeader h;
    h.kind = kind;
    h.channel = altair::Channel::State;
    h.seq = seq;
    h.topic = topic;
    h.payload_len = static_cast<std::uint32_t>(altair::price_frame_bytes(p.depth_levels));
    h.engine_time_ns = p.exchange_ts_ns;
    (void)altair::encode_header(h, out.data(), out.size());
    (void)altair::encode_price(p, bids, asks, out.data() + altair::kFrameHeaderBytes, out.size() - altair::kFrameHeaderBytes);
    return out;
}

altair::PricePayload trade(std::uint32_t token, double px, std::int64_t qty, std::int64_t vol, std::int64_t at_ns,
                           std::uint16_t extra = 0) {
    altair::PricePayload p;
    p.token = token;
    p.flags = static_cast<std::uint16_t>(altair::kPriceHasVolume | extra);
    p.last_paise = std::llround(px * 100.0);
    p.last_qty = qty;
    p.volume = vol;
    p.exchange_ts_ns = at_ns;
    return p;
}

void test_activity_log(const fs::path& root) {
    std::printf("activity log\n");
    check(act::redact("login ok access_token=abc.def.ghi rest") == "login ok access_token=<redacted> rest", "access_token value masked");
    check(act::redact("{\"refresh_token\":\"xyz\"}").find("xyz") == std::string::npos, "JSON refresh_token masked");
    check(act::redact("Authorization: Bearer QWERTY") .find("QWERTY") == std::string::npos, "Bearer token masked");
    check(act::redact("spinning up; pinned 3 cores") == "spinning up; pinned 3 cores", "words that contain 'pin' are left alone");
    check(act::redact("two\nlines") == "two lines", "newlines cannot forge a second log line");
    check(act::ist_stamp(0) == "1970-01-01T05:30:00.000+05:30", "stamps are IST");
    check(act::ist_day(18'629'999LL) == "1970-01-01" && act::ist_day(66'600'000LL) == "1970-01-02", "the IST day turns at 18:30 UTC");
    check(act::write(root, "test", "INFO", "hello secret=hunter2"), "a line is written");
    const fs::path f = root / "data" / "logs" / ("altair-" + act::ist_day(act::now_ms()) + ".log");
    const std::string body = slurp(f);
    check(body.find("[test] INFO hello secret=<redacted>") != std::string::npos && body.find("hunter2") == std::string::npos,
          "the file holds the redacted line");
    spit(root / "data" / "logs" / "altair-2001-01-01.log", "old\n");
    check(act::prune(root, 30) == 1 && !fs::exists(root / "data" / "logs" / "altair-2001-01-01.log") && fs::exists(f),
          "prune removes only logs older than the window");
}

void test_decode(const fs::path& root) {
    std::printf("decode a tape\n");
    std::vector<lv::LiveInstrument> u(4);
    u[0].token = lv::kLiveNiftyToken; u[0].fyers = "NSE:NIFTY50-INDEX"; u[0].symbol = "NIFTY 50"; u[0].underlying = "NIFTY";
    u[1].token = 11; u[1].fyers = "NSE:NIFTY26OCTFUT"; u[1].symbol = "NIFTY26OCTFUT"; u[1].underlying = "NIFTY";
    u[1].kind = lv::LiveKind::Future; u[1].expiry_day = kDay + 1;
    u[2].token = 12; u[2].fyers = "NSE:NIFTY26NOVFUT"; u[2].symbol = "NIFTY26NOVFUT"; u[2].underlying = "NIFTY";
    u[2].kind = lv::LiveKind::Future; u[2].expiry_day = kDay + 29;
    u[3].token = 13; u[3].fyers = "NSE:NIFTY2610625000CE"; u[3].symbol = "NIFTY2610625000CE"; u[3].underlying = "NIFTY";
    u[3].kind = lv::LiveKind::Call; u[3].expiry_day = kDay + 1; u[3].strike = 25'000;
    const fs::path ufile = root / "uni.csv";
    (void)lv::write_universe(ufile.string(), u);

    const fs::path tape = root / "tapes" / "2026-10-26-091400.tape";
    fs::create_directories(tape.parent_path());
    {
        lv::TapeWriter w(tape.string());
        w.write(lv::TapeKind::Start, 1, lv::tape_pack({{"args", "date=2026-10-26\n"}, {"universe.csv", slurp(ufile)}}));
        w.write(lv::TapeKind::Connected, 2);
        std::vector<std::uint8_t> all;
        const auto add = [&all](const std::vector<std::uint8_t>& f) { all.insert(all.end(), f.begin(), f.end()); };
        using K = altair::FrameKind;
        // Snapshot of the future: the day's volume before the session's first print.
        add(frame(K::Snapshot, 1, altair::kTopicTrades, trade(11, 25'100, 0, 1'000, k0915 - 60'000'000'000LL)));
        add(frame(K::Delta, 2, altair::kTopicTrades, trade(11, 25'110, 75, 1'075, k0915 + 1'000'000'000LL)));
        add(frame(K::Delta, 3, altair::kTopicTrades, trade(11, 25'130, 75, 1'150, k0915 + 20'000'000'000LL)));
        add(frame(K::Delta, 4, altair::kTopicTrades, trade(11, 25'090, 150, 1'300, k0915 + 59'000'000'000LL)));
        // seq 5 never arrives: one trade missed.
        add(frame(K::Delta, 6, altair::kTopicTrades, trade(11, 25'120, 75, 1'500, k0915 + 61'000'000'000LL)));
        add(frame(K::Delta, 7, altair::kTopicTrades, trade(lv::kLiveNiftyToken, 25'050.5, 0, 0, k0915 + 2'000'000'000LL, 0)));
        add(frame(K::Delta, 8, altair::kTopicTrades, trade(13, 120, 75, 75, k0915 + 3'000'000'000LL)));
        // A SIM frame: dropped. An unknown token: counted.
        add(frame(K::Delta, 9, altair::kTopicTrades, trade(11, 1, 1, 1, k0915 + 4'000'000'000LL, altair::kPriceSimulated)));
        add(frame(K::Delta, 10, altair::kTopicTrades, trade(999, 10, 1, 1, k0915 + 5'000'000'000LL)));
        add(frame(K::Delta, 10, altair::kTopicTrades, trade(11, 1, 1, 1, k0915 + 5'000'000'000LL)));   // a duplicate seq: skipped
        // A 50-level book.
        altair::PriceLevel bids[50], asks[50];
        for (int k = 0; k < 50; ++k) {
            bids[k] = {2'511'000 - 5 * k, 75 * (k + 1), static_cast<std::uint32_t>(k + 1), 0};
            asks[k] = {2'511'500 + 5 * k, 75 * (k + 1), static_cast<std::uint32_t>(k + 1), 0};
        }
        altair::PricePayload b;
        b.token = 11; b.flags = altair::kPriceHasBook; b.depth_levels = 50; b.exchange_ts_ns = k0915 + 6'000'000'000LL;
        add(frame(K::Delta, 1, altair::kTopicBook, b, bids, asks));
        // Split the bytes across two records: a frame may straddle them.
        const std::size_t cut = all.size() / 2 + 7;
        w.write(lv::TapeKind::Data, 3, all.data(), cut);
        w.write(lv::TapeKind::Data, 4, all.data() + cut, all.size() - cut);
        w.write(lv::TapeKind::Disconnected, 5, "stopping");
        (void)w.flush();
    }
    st::TapeDecoder dec(root / "ticks");
    check(dec.read(tape), "the tape reads");
    check(dec.close_ticks(), "the tick files close cleanly");
    const auto& s = dec.stats();
    check(s.trades == 6 && s.snapshots == 1, "six prints and one snapshot");
    check(s.missed[altair::kTopicTrades] == 1, "the sequence gap is counted");
    check(s.simulated == 1 && s.unknown_token == 1, "SIM dropped; an unknown token counted");
    check(s.books == 1 && s.deep_books == 1, "the 50-level book is kept");
    const auto& fut = dec.bars().at("NSE:NIFTY26OCTFUT");
    check(fut.size() == 2, "two minutes of the future");
    const auto& m0 = fut.begin()->second;
    check(m0.o == 2'511'000 && m0.h == 2'513'000 && m0.l == 2'509'000 && m0.c == 2'509'000 && m0.trades == 3, "OHLC of the minute");
    const std::string bars = dec.bars_csv();
    check(bars.find("2026-10-26T09:15:00+05:30,NSE:NIFTY26OCTFUT,future,NIFTY,2026-10-27,,25110,25130,25090,25090,300,,3,fyers_tape")
              != std::string::npos, "minute volume is the change in the day's volume since the snapshot");
    check(bars.find("2026-10-26T09:16:00+05:30,NSE:NIFTY26OCTFUT,future,NIFTY,2026-10-27,,25120,25120,25120,25120,200,,1")
              != std::string::npos, "the next minute's volume runs on from the last");
    const auto series = dec.series_csv();
    check(series.contains("fut/nifty") && series.contains("spot/nifty") && series.size() == 2,
          "the index and only the NEAREST future go to the dataset-shaped tree");
    check(series.at("spot/nifty").find("09:15:00+05:30,25050.5,25050.5,25050.5,25050.5,,,fyers_tape") != std::string::npos,
          "an index carries no volume (absent, not zero)");
    const std::string books = slurp(root / "ticks" / "books.csv");
    check(books.find(",NSE:NIFTY26OCTFUT,50,25110:75:1;") != std::string::npos && books.find("25117.45:3750:50,fyers_live") != std::string::npos,
          "books.csv holds all 50 levels a side");
    check(slurp(root / "ticks" / "trades.csv").find("2026-10-26T09:15:01.000+05:30,NSE:NIFTY26OCTFUT,25110,75,1075") != std::string::npos,
          "trades.csv stamps exchange time in IST");
}

void test_snapshot(const fs::path& root) {
    std::printf("snapshot\n");
    const fs::path ds = root / "dataset", fy = root / "fyers", tp = root / "tape";
    spit(ds / "spot/nifty/1m/2026-10.csv",
         "time,open,high,low,close,volume\n"
         "2026-10-23T09:15:00+05:30,100,101,99,100.5,\n"      // FYERS agrees to the decimal: dataset row stays
         "2026-10-23T09:16:00+05:30,100,101,99,100.5,\n"      // FYERS differs by ~50 bp: FYERS wins
         "2026-10-23T09:17:00+05:30,100,101,99,100.5,\n"      // FYERS's bar is impossible: dataset stays
         "2026-10-23T09:18:01+05:30,100,101,99,100,\n"        // seconds floored to 09:18
         "2026-10-27T09:15:00+05:30,100,101,99,100,\n");      // after the cutoff: dropped
    spit(fy / "spot/nifty/1m/2026-10-23.csv",
         "time,open,high,low,close,volume\n"
         "2026-10-23T03:45:00Z,100,101,99,100.5001,\n"        // = 09:15 IST, within 1 bp
         "2026-10-23T09:16:00+05:30,100,101.5,99,101,\n"
         "2026-10-23T09:17:00+05:30,100,101,99,105,\n"        // close above high
         "2026-10-23T09:19:00+05:30,100,101,99,100,\n");      // only FYERS has it
    spit(tp / "spot/nifty/1m/2026-10-23.csv",
         "time,open,high,low,close,volume,oi,source\n"
         "2026-10-23T09:19:00+05:30,1,1,1,1,,,fyers_tape\n"   // fill-only: FYERS's row stays
         "2026-10-23T09:20:00+05:30,100,100,100,100,,,fyers_tape\n");
    spit(ds / "spot/nifty/1d/all.csv", "time,open,high,low,close,volume\n2026-10-23,1,2,0.5,1.5,\n2026-10-26,1,2,0.5,1.5,\n");
    spit(fy / "spot/nifty/1d/2026-10-23.csv", "time,open,high,low,close,volume\n2026-10-23T00:00:00+05:30,1,2,0.5,1.5,123\n");
    spit(ds / "spot/nifty/_superseded_1m/x.csv", "time,open,high,low,close\n");   // ignored: not a timeframe
    spit(root / "bhav/fo23OCT2026bhav.csv", "x\n");
    spit(root / "bhav/BhavCopy_NSE_FO_0_0_0_20261027_F_0000.csv", "x\n");       // after the cutoff
    spit(root / "iv/NIFTY/2026-10-23.csv", "time,iv\n");
    spit(root / "iv/NIFTY/2026-10-27.csv", "time,iv\n");

    st::SnapshotOptions o;
    o.sources = {{"dataset", ds, false}, {"fyers", fy, false}, {"tape", tp, true}};
    o.cutoff_day = *st::parse_day("2026-10-26");
    o.out = root / "snap";
    o.bhavcopy = root / "bhav";
    o.iv = root / "iv";
    const auto rep = st::build_snapshot(o);
    check(rep.error.empty(), "the snapshot builds");
    check(rep.series.size() == 2, "two series discovered; _superseded_* ignored");
    const std::string m = slurp(o.out / "spot/nifty/1m/2026-10.csv");
    check(m.find("2026-10-23T09:15:00+05:30,100,101,99,100.5,,,dataset") != std::string::npos, "within 1 bp the dataset row stays");
    check(m.find("2026-10-23T09:16:00+05:30,100,101.5,99,101,,,fyers") != std::string::npos, "beyond 1 bp FYERS wins");
    check(m.find("2026-10-23T09:17:00+05:30,100,101,99,100.5,,,dataset") != std::string::npos, "an impossible bar loses");
    check(m.find("2026-10-23T09:18:00+05:30,100,101,99,100,,,dataset") != std::string::npos, "stamps are floored to the minute");
    check(m.find("09:19:00+05:30,100,101,99,100,,,fyers") != std::string::npos, "a fill-only source never replaces");
    check(m.find("09:20:00+05:30,100,100,100,100,,,tape") != std::string::npos, "a fill-only source fills a gap");
    check(m.find("2026-10-27") == std::string::npos, "nothing after the cutoff");
    const std::string d = slurp(o.out / "spot/nifty/1d/all.csv");
    check(d.find("2026-10-23,1,2,0.5,1.5,,,dataset") != std::string::npos && d.find("2026-10-26,1,2,0.5,1.5,,,dataset") != std::string::npos,
          "daily bars key on the date, whatever the stamp");
    const std::string cx = slurp(o.out / "_conflicts.csv");
    check(cx.find("spot/nifty/1m,2026-10-23T09:16:00+05:30,fyers,101,dataset,100.5,") != std::string::npos && rep.conflicts == 1,
          "the one real conflict is listed with both closes");
    check(fs::exists(o.out / "_inputs/bhavcopy/fo23OCT2026bhav.csv") && !fs::exists(o.out / "_inputs/bhavcopy/BhavCopy_NSE_FO_0_0_0_20261027_F_0000.csv"),
          "bhavcopy is cut at the same day");
    check(fs::exists(o.out / "_inputs/iv/NIFTY/2026-10-23.csv") && !fs::exists(o.out / "_inputs/iv/NIFTY/2026-10-27.csv"),
          "recorded IV is cut at the same day");
    check(slurp(o.out / "_snapshot.json").find("\"cutoff\": \"2026-10-26\"") != std::string::npos, "the manifest names the cutoff");
    check(slurp(o.out / "_provenance.csv").find("spot/nifty/1m,2026-10-23T09:16:00+05:30,2026-10-23T09:16:00+05:30,fyers,1") != std::string::npos,
          "provenance runs say which source each stretch came from");
    const auto again = st::build_snapshot(o);
    check(!again.error.empty(), "a snapshot is never rebuilt over");
    o.out = root / "snap2";
    const auto twin = st::build_snapshot(o);
    check(twin.error.empty() && twin.id == rep.id, "same inputs, same cutoff: same id");
    spit(fy / "spot/nifty/1m/2026-10-23.csv", slurp(fy / "spot/nifty/1m/2026-10-23.csv") + "2026-10-23T09:21:00+05:30,1,1,1,1,\n");
    o.out = root / "snap3";
    const auto changed = st::build_snapshot(o);
    check(changed.error.empty() && changed.id != rep.id, "a changed input changes the id");
    check(st::bhavcopy_day("fo05JUL2024bhav.csv") == st::parse_day("2024-07-05")
              && st::bhavcopy_day("BhavCopy_NSE_FO_0_0_0_20240708_F_0000.csv") == st::parse_day("2024-07-08")
              && !st::bhavcopy_day("readme.csv"),
          "both bhavcopy name formats are dated");
}

} // namespace


altair::QuotePayload top(std::uint32_t token, std::int64_t bid, std::int64_t ask) {
    altair::QuotePayload q;
    q.token = token;
    q.flags = altair::kQuoteHasTop;
    q.bid = bid; q.ask = ask; q.bid_qty = 75; q.ask_qty = 75;
    return q;
}

std::vector<std::uint8_t> quote_frame(std::uint64_t seq, const altair::QuotePayload& q, std::int64_t at_ns) {
    std::vector<std::uint8_t> out(altair::kFrameHeaderBytes + altair::kQuotePayloadBytes);
    altair::FrameHeader h;
    h.kind = altair::FrameKind::Delta;
    h.channel = altair::Channel::State;
    h.seq = seq;
    h.topic = altair::kTopicQuote;
    h.payload_len = static_cast<std::uint32_t>(altair::kQuotePayloadBytes);
    h.engine_time_ns = at_ns;
    (void)altair::encode_header(h, out.data(), out.size());
    (void)altair::encode_quote(q, out.data() + altair::kFrameHeaderBytes, out.size() - altair::kFrameHeaderBytes);
    return out;
}

// Field `col` (0-based) of the first line of `csv` that contains `key`.
std::string field(const std::string& csv, const std::string& key, int col) {
    const auto at = csv.find(key);
    if (at == std::string::npos) return "<missing>";
    const auto bol = csv.rfind('\n', at) + 1;
    const std::string line = csv.substr(bol, csv.find('\n', at) - bol);
    std::size_t from = 0;
    for (int i = 0; i < col; ++i) {
        from = line.find(',', from);
        if (from == std::string::npos) return "<missing>";
        ++from;
    }
    return line.substr(from, line.find(',', from) - from);
}
double fnum(const std::string& s) { return s.empty() || s[0] == '<' ? -1.0 : std::atof(s.c_str()); }

void test_vol(const fs::path& root) {
    std::printf("IV, VIX and RV a minute\n");
    // A flat 15-vol chain on a 25,000 forward, two expiries, strikes 20,000..30,000 by 100.
    constexpr double kVol = 0.15, kRate = 0.065, kF = 25'000.0;
    const std::int64_t near = kDay + 10, next = kDay + 38;
    std::vector<lv::LiveInstrument> u;
    lv::LiveInstrument in;
    in.token = lv::kLiveNiftyToken; in.fyers = "NSE:NIFTY50-INDEX"; in.symbol = "NIFTY 50"; in.underlying = "NIFTY";
    u.push_back(in);
    in.token = 7; in.fyers = "NSE:INDIAVIX-INDEX"; in.symbol = "INDIA VIX"; in.underlying = "INDIAVIX";
    u.push_back(in);
    std::uint32_t tok = 100;
    for (const std::int64_t e : {near, next})
        for (int k = 20'000; k <= 30'000; k += 100)
            for (const bool call : {true, false}) {
                lv::LiveInstrument o;
                o.token = tok++;
                o.symbol = "NIFTY" + std::to_string(e) + std::to_string(k) + (call ? "CE" : "PE");
                o.fyers = "NSE:" + o.symbol;
                o.underlying = "NIFTY";
                o.kind = call ? lv::LiveKind::Call : lv::LiveKind::Put;
                o.expiry_day = e;
                o.strike = k;
                u.push_back(o);
            }
    const fs::path ufile = root / "uni_vol.csv";
    (void)lv::write_universe(ufile.string(), u);
    const auto u2 = [&] { std::ifstream f(ufile); return lv::read_universe_stream(f); }();
    check(u2.size() == u.size(), "the chain universe round-trips");

    const fs::path tape = root / "tapes_vol" / "2026-10-26-091400.tape";
    fs::create_directories(tape.parent_path());
    const std::int64_t m0915_end = kDay * 86'400 + 9 * 3600 + 16 * 60;   // IST seconds
    std::string trade_only;
    {
        lv::TapeWriter w(tape.string());
        w.write(lv::TapeKind::Start, 1, lv::tape_pack({{"args", "date=2026-10-26\n"}, {"universe.csv", slurp(ufile)}}));
        w.write(lv::TapeKind::Connected, 2);
        std::vector<std::uint8_t> all;
        const auto add = [&all](const std::vector<std::uint8_t>& f) { all.insert(all.end(), f.begin(), f.end()); };
        using K = altair::FrameKind;
        std::uint64_t tseq = 1, qseq = 1;
        add(frame(K::Delta, tseq++, altair::kTopicTrades, trade(lv::kLiveNiftyToken, 25'000, 0, 0, k0915 + 10'000'000'000LL, 0)));
        add(frame(K::Delta, tseq++, altair::kTopicTrades, trade(7, 14.5, 0, 0, k0915 + 11'000'000'000LL, 0)));
        for (const auto& o : u) {
            if (o.kind != lv::LiveKind::Call && o.kind != lv::LiveKind::Put) continue;
            // Priced at the end of the minute the reading is for.
            const double T = static_cast<double>(o.expiry_day * 86'400 + 15 * 3600 + 30 * 60 - m0915_end) / (365.0 * 86'400.0);
            const auto g = altair::black76(o.kind == lv::LiveKind::Call ? altair::OptionRight::Call : altair::OptionRight::Put,
                                           altair::Price{std::llround(kF * 100)}, altair::Price{std::llround(o.strike * 100)},
                                           altair::Years{T}, altair::Vol{kVol}, kRate);
            const std::int64_t P = std::llround(g->price);
            if (P < 2) continue;   // a worthless wing shows no bid
            if (o.expiry_day == near && o.strike == 25'500 && o.kind == lv::LiveKind::Call) {
                trade_only = o.fyers;   // priced only by a trade
                add(frame(K::Delta, tseq++, altair::kTopicTrades,
                          trade(o.token, static_cast<double>(P) / 100.0, 75, 75, k0915 + 30'000'000'000LL)));
                continue;
            }
            add(quote_frame(qseq++, top(o.token, P - 1, P + 1), k0915 + 30'000'000'000LL));
        }
        add(frame(K::Delta, tseq++, altair::kTopicTrades, trade(lv::kLiveNiftyToken, 25'025, 0, 0, k0915 + 70'000'000'000LL, 0)));
        add(frame(K::Delta, tseq++, altair::kTopicTrades, trade(lv::kLiveNiftyToken, 25'000, 0, 0, k0915 + 130'000'000'000LL, 0)));
        w.write(lv::TapeKind::Data, 3, all.data(), all.size());
        (void)w.flush();
    }
    st::TapeDecoder dec;
    check(dec.read(tape), "the chain tape reads");
    const auto v = dec.vol_views();
    const std::string near_key = "2026-10-26T09:16:00+05:30,NIFTY,25000,14.5,2026-11-05,";
    const std::string next_key = "2026-10-26T09:16:00+05:30,NIFTY,25000,14.5,2026-12-03,";
    check(std::fabs(fnum(field(v.vol, near_key, 6)) - kF) < 0.5, "the parity forward is the chain's forward");
    check(std::fabs(fnum(field(v.vol, near_key, 8)) - kVol) < 1e-3 && std::fabs(fnum(field(v.vol, next_key, 8)) - kVol) < 1e-3,
          "ATM IV of each expiry is the chain's vol");
    check(std::fabs(fnum(field(v.vol, near_key, 9)) - kVol) < 5e-3, "the strip's model-free vol agrees on a flat smile");
    check(std::fabs(fnum(field(v.vol, near_key, 11)) - 100.0 * kVol) < 0.3, "the 30-day chain VIX is 15 points");
    check(field(v.vol, near_key, 12).empty() && field(v.vol, near_key, 14).empty(), "no RV before there are returns");
    const std::string carry = "2026-10-26T09:17:00+05:30,NIFTY,25025,,2026-11-05,";
    check(field(v.vol, carry, 6) != "<missing>", "a minute with no fresh quote carries the last minute's");
    const double r = std::log(25'025.0 / 25'000.0);
    const std::string late = "2026-10-26T09:18:00+05:30,NIFTY,25000,,,";
    check(std::fabs(fnum(field(v.vol, late, 14)) - std::sqrt(r * r * 375.0 * 252.0)) < 1e-6 && field(v.vol, late, 12).empty(),
          "session RV from the index's minute returns; the 15-minute window waits for 15 returns");
    check(field(v.chain, "2026-10-26T09:16:00+05:30," + trade_only + ",", 7) == "last", "a trade-only option is priced at its last");
    const std::string atm_ce = "2026-10-26T09:16:00+05:30,NSE:NIFTY" + std::to_string(near) + "25000CE,";
    check(std::fabs(fnum(field(v.chain, atm_ce, 9)) - kVol) < 1e-3 && std::fabs(fnum(field(v.chain, atm_ce, 10)) - 0.5) < 0.05
              && field(v.chain, atm_ce, 7) == "mid", "each option's IV and delta, from its mid");
    check(v.chain_rows > 300 && v.vol_rows == 5, "every priced option a minute; one vol row per expiry a minute");
}

int main() {
    const fs::path root = fs::temp_directory_path() / ("altair_store_test_" + std::to_string(act::now_ms()));
    fs::create_directories(root);
    test_activity_log(root);
    test_decode(root);
    test_vol(root);
    test_snapshot(root);
    std::error_code ec;
    fs::remove_all(root, ec);
    std::printf(failures == 0 ? "all passed\n" : "%d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
