// The tuner (app/live_tune.hpp) on answers known in advance: the grids hold
// today's settings; a settings file overrides only its own keys; a tape is
// replayed through every grid point in one pass and each point trades as its
// rule says; walk-forward picks each day's setting from the days before it;
// and nothing replaces the defaults on too few days, on a setting that did not
// beat them out of sample, or on one that does not survive the Romano-Wolf
// adjustment.
//
// No check description here may contain the substring "F" "AIL" joined.

#include <app/live_tune.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
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
namespace tn = altair::tune;

constexpr std::int64_t kSec = 1'000'000'000LL;

/// A bus frame, as the price service sends it.
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

/// One session tape: two RELIANCE listings, a gap of 10.5 bp at 10:00:01 that
/// closes at 10:00:30.
void write_tape(const fs::path& path, const std::string& day, bool sim) {
    live::TapeWriter w(path.string());
    const std::string uni = std::string(live::kLiveUniverseHeader) + "\n"
                          + "1,NSE:RELIANCE-EQ,RELIANCE,RELIANCE,equity,,0,1,0.05,stocks,0\n"
                          + "2,BSE:RELIANCE-A,RELIANCE,RELIANCE,equity,,0,1,0.05,stocks,0\n";
    w.write(live::TapeKind::Start, 1, live::tape_pack({{"args", "date=" + day + "\nlatency_ns=0\n"}, {"universe.csv", uni}}));
    w.write(live::TapeKind::Connected, 2);
    const std::int64_t t0 = (live::parse_day(day) * 86400 + 10 * 3600 - 19800) * kSec;
    std::uint64_t seq_t = 0, seq_q = 0;
    const auto trade = [&](std::uint32_t tok, std::int64_t px, std::int64_t at) {
        PricePayload p;
        p.token = tok; p.last_paise = px; p.last_qty = 1; p.exchange_ts_ns = at;
        if (sim) p.flags |= kPriceSimulated;
        std::vector<std::uint8_t> b(price_frame_bytes(0));
        const auto n = encode_price(p, nullptr, nullptr, b.data(), b.size());
        const auto f = frame(kTopicTrades, ++seq_t, at, b.data(), n ? *n : 0);
        w.write(live::TapeKind::Data, at, f.data(), f.size());
    };
    const auto quote = [&](std::uint32_t tok, std::int64_t bid, std::int64_t ask, std::int64_t at) {
        QuotePayload q;
        q.token = tok; q.flags = kQuoteHasTop | (sim ? kQuoteSimulated : 0); q.bid = bid; q.ask = ask; q.bid_qty = 100; q.ask_qty = 100;
        std::vector<std::uint8_t> b(kQuotePayloadBytes);
        const auto n = encode_quote(q, b.data(), b.size());
        const auto f = frame(kTopicQuote, ++seq_q, at, b.data(), n ? *n : 0);
        w.write(live::TapeKind::Data, at, f.data(), f.size());
    };
    trade(1, 100000, t0);
    trade(2, 100000, t0);
    quote(1, 100000, 100010, t0 + kSec);
    quote(2, 99885, 99895, t0 + kSec);        // NSE bid 1000.00 over BSE ask 998.95: 10.5 bp
    trade(1, 100000, t0 + 30 * kSec);
    quote(2, 99945, 99955, t0 + 30 * kSec);   // they meet
    quote(1, 99945, 99955, t0 + 30 * kSec);
    w.flush();
}

tn::DayRun synthetic(const std::string& day, std::vector<double> net) {
    tn::DayRun d;
    d.day = day;
    d.net = net;
    d.gross = net;
    d.trips.assign(net.size(), 1);
    return d;
}

} // namespace

int main() {
    std::printf("tuning on recorded sessions\n");
    const auto arb = tn::arbitrage_grid();
    const auto ohl = tn::ohl_grid();
    const auto oa = tn::option_arb_grid();
    check(arb.points.size() == 15 && arb.points[arb.defaults].at("min_profit_bps") == 2.0
              && arb.points[arb.defaults].at("max_hold_min") == 30.0,
          "the arbitrage grid holds today's settings (2 bp, 30 minutes) among its 15");
    check(ohl.points.size() == 36 && ohl.points[ohl.defaults].at("stop_pct") == 0.5 && ohl.points[ohl.defaults].at("trail_pct") == 0.25,
          "the OHL grid holds 0.5 % / 1.5 % / 0.25 % among its 36");
    check(oa.points.size() == 15 && oa.points[oa.defaults].at("margin_bp") == 3.0, "the option arbitrage grid holds 3 bp");

    const fs::path root = fs::temp_directory_path() / ("altair_tune_" + std::to_string(std::rand()));
    fs::remove_all(root);
    fs::create_directories(root / "config/model_params");
    {
        std::ofstream f(root / "config/model_params/arbitrage.toml");
        f << "# tuned\nmin_profit_bps = 1   # a comment\nunknown = 3\n";
    }
    const auto p = tn::tuned_or_default(arb, root);
    check(p.at("min_profit_bps") == 1.0 && p.at("max_hold_min") == 30.0 && p.count("unknown") == 0,
          "a settings file overrides its own keys only; the rest stay the defaults");
    check(tn::tuned_or_default(ohl, root) == ohl.points[ohl.defaults], "a strategy with no file runs on its defaults");

    // ---- one tape, every grid point ---------------------------------------------------
    const live::LiveCostFn cost = [](const live::LiveInstrument&, bool, double q, double px, std::int64_t) { return 0.00015 * q * px; };
    write_tape(root / "2026-10-05-100000.tape", "2026-10-05", false);
    tn::DayRun d;
    std::string err;
    check(tn::run_tape(root / "2026-10-05-100000.tape", arb, cost, d, err) && d.day == "2026-10-05" && !d.simulated
              && d.net.size() == arb.points.size(),
          "a tape replays through every point of the grid in one pass");
    std::size_t in1 = 0, in3 = 0;
    for (std::size_t i = 0; i < arb.points.size(); ++i) {
        if (arb.points[i].at("max_hold_min") != 30.0) continue;
        if (arb.points[i].at("min_profit_bps") == 1.0) in1 = i;
        if (arb.points[i].at("min_profit_bps") == 3.0) in3 = i;
    }
    check(d.trips[in1] == 2 && d.net[in1] > 0.0 && d.trips[in3] == 0 && d.net[in3] == 0.0,
          "10.5 bp clears 6 (expenses) + 2 (spreads) + 1, not + 3: each point trades as its rule says");
    check(d.trips[arb.defaults] == 2, "the defaults (2 bp) take it too");
    write_tape(root / "sim.tape", "2026-10-06", true);
    tn::DayRun s;
    check(tn::run_tape(root / "sim.tape", arb, cost, s, err) && s.simulated, "a SIM tape says so (and is left out unless asked)");

    // ---- walk-forward -----------------------------------------------------------------------
    // Three settings; 1 is the defaults. Setting 2 makes 100 a day, give or take; the defaults lose 10.
    std::vector<tn::DayRun> days;
    const double wobble[] = {5, -3, 8, -6, 2, 4, -1, 7};
    for (int i = 0; i < 8; ++i)
        days.push_back(synthetic("2026-09-" + std::to_string(10 + i), {0.0, -10.0 + wobble[i] * 0.1, 100.0 + wobble[i]}));
    const auto w = tn::walk_forward(days, 3, 1);
    check(w.test_days.size() == 6 && w.chosen.front() == 2 && w.final_choice == 2,
          "each test day takes the setting best on the days before it");
    check(w.oos_total > w.defaults_total && w.accept && w.p_rw < tn::kMaxP,
          "8 days, better out of sample, positive, and through Romano-Wolf: accepted");
    const std::vector<tn::DayRun> few(days.begin(), days.begin() + 4);
    const auto w4 = tn::walk_forward(few, 3, 1);
    check(!w4.accept && w4.why.find("at least") != std::string::npos, "4 days are not enough, however good");
    std::vector<tn::DayRun> noisy;
    const double swing[] = {300, -280, 250, -310, 290, -260, 270, -240};
    for (int i = 0; i < 8; ++i) noisy.push_back(synthetic("2026-09-" + std::to_string(10 + i), {0.0, 1.0, 5.0 + swing[i]}));
    const auto wn = tn::walk_forward(noisy, 3, 1);
    check(!wn.accept, "a setting whose days swing either way is not accepted");
    std::vector<tn::DayRun> plain;
    for (int i = 0; i < 8; ++i) plain.push_back(synthetic("2026-09-" + std::to_string(10 + i), {-5.0, 50.0, 10.0}));
    const auto wp = tn::walk_forward(plain, 3, 1);
    check(!wp.accept && wp.final_choice == 1 && wp.why.find("already") != std::string::npos, "when the defaults are best they stay");

    check(tn::write_params(root, arb, arb.points[2], w, 8, "2026-10-06")
              && tn::read_params(root / "config/model_params/arbitrage.toml") == arb.points[2],
          "an accepted setting is written so the engine reads it back exactly");

    fs::remove_all(root);
    std::printf("%s\n", failures == 0 ? "all tuning checks passed" : "tuning checks did not pass");
    return failures == 0 ? 0 : 1;
}
