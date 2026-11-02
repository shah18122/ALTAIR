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

/// One session tape: two RELIANCE listings, a gap of 5.5 bp at 10:00:01 that
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
    quote(2, 99935, 99945, t0 + kSec);        // NSE bid 1000.00 over BSE ask 999.45: 5.5 bp
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
              && arb.points[arb.defaults].at("cooldown_s") == 5.0,
          "the arbitrage grid holds today's settings (2 bp, 5 s) among its 15");
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
    check(p.at("min_profit_bps") == 1.0 && p.at("cooldown_s") == 5.0 && p.count("unknown") == 0,
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
        if (arb.points[i].at("cooldown_s") != 5.0) continue;
        if (arb.points[i].at("min_profit_bps") == 1.0) in1 = i;
        if (arb.points[i].at("min_profit_bps") == 3.0) in3 = i;
    }
    check(d.trips[in1] == 1 && d.net[in1] > 0.0 && d.trips[in3] == 0 && d.net[in3] == 0.0,
          "5.5 bp clears the two fills' 3 bp + 1, not + 3: each point trades as its rule says (one netted pair)");
    check(d.trips[arb.defaults] == 1, "the defaults (2 bp) take it too");
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

    // ---- the OBI traders ----------------------------------------------------------------
    const auto og = tn::obi_grids({{"BANKNIFTY", 25.0, 15.0, 2.0}, {"NYKAA", 0.80, 0.40, 0.05}});
    check(og.size() == 16, "16 OBI grids: two underlyings x four trade types x futures / option sale");
    bool defaults_are_today = true, keys_ok = true;
    for (const auto& g : og) {
        live::ObiTraderRules r = tn::obi_base_rules(g.key.find("nykaa") != std::string::npos ? live::ObiUnderlying{"NYKAA", 0.80, 0.40, 0.05}
                                                                                           : live::ObiUnderlying{"BANKNIFTY", 25.0, 15.0, 2.0},
                                                    live::ObiType::A, live::ObiMode::Futures);
        const live::ObiTraderRules before = r;
        tn::apply_obi_params(r, g.points[g.defaults]);
        defaults_are_today = defaults_are_today && r.obi_entry == before.obi_entry && r.min_persist_ns == before.min_persist_ns
                          && r.target_pts == before.target_pts && r.stop_pts == before.stop_pts && r.max_hold_ns == before.max_hold_ns
                          && r.option_target_pct == before.option_target_pct && r.passive_options == before.passive_options;
        keys_ok = keys_ok && g.make_all != nullptr && g.key.rfind("obi_", 0) == 0;
    }
    check(defaults_are_today, "every OBI grid's defaults are today's rules exactly");
    check(keys_ok, "each builds its own hub and trader");
    const tn::Grid* aopt = nullptr;
    for (const auto& g : og) if (g.key == "obi_banknifty_A_opt") aopt = &g;
    check(aopt != nullptr && aopt->points.size() == 54 && aopt->model == "OBI A option-sell BANKNIFTY",
          "obi_banknifty_A_opt: threshold x hold x option target x passive = 54 settings, judged on its own model");
    {
        live::ObiTraderRules r = tn::obi_base_rules({"BANKNIFTY", 25.0, 15.0, 2.0}, live::ObiType::A, live::ObiMode::SellOptions);
        tn::apply_obi_params(r, {{"obi_entry", 0.4}, {"persist_ms", 1000.0}, {"target_scale", 1.5}, {"passive", 0.0}});
        check(r.obi_entry == 0.4 && r.min_persist_ns == 1'000'000'000 && r.target_pts == 37.5 && r.stop_pts == 22.5 && !r.passive_options,
              "a tuned file sets the threshold, the hold, scales target and stop together, and can turn resting off");
    }
    {
        const auto qg = tn::quoter_grids({{"BANKNIFTY", 25.0, 15.0, 2.0}, {"NYKAA", 0.80, 0.40, 0.05}});
        check(qg.size() == 2 && qg[0].key == "obiq_banknifty" && qg[0].points.size() == 27 && qg[0].model == "OBI quoter BANKNIFTY"
                  && qg[0].make_all != nullptr,
              "the option quoter: one grid an underlying, pull level x target x strikes out = 27 settings");
        live::ObiQuoterRules q, d;
        tn::apply_quoter_params(q, qg[0].points[qg[0].defaults]);
        check(q.pull_obi == d.pull_obi && q.target_pct == d.target_pct && q.strikes_otm == d.strikes_otm,
              "the quoter grid's defaults are today's rules exactly");
        tn::apply_quoter_params(q, {{"pull_obi", 0.5}, {"strikes_otm", 3.0}});
        check(q.pull_obi == 0.5 && q.strikes_otm == 3 && q.target_pct == d.target_pct, "a tuned file sets only its own keys");
    }
    {
        // The engine runs an OBI grid through the same tape pass as the others.
        std::vector<tn::DayRun> runs;
        check(tn::run_tape_group(root / "2026-10-05-100000.tape", {&arb, &og.front()}, cost, runs, err) && runs.size() == 2
                  && runs[0].trips[arb.defaults] == 1 && runs[1].net.size() == og.front().points.size(),
              "several grids share one pass of a tape; each gets its own day");
        check(runs[0].rec[in1].trade_net.size() == 1 && runs[0].rec[in1].markout_n[0] == 2 && runs[0].rec[in1].markout_n[2] == 0,
              "every round trip, and each entry leg's markout once its horizon has passed (the 60 s one has not)");
        tn::FeedLag lag;
        std::vector<tn::DayRun> r2;
        check(tn::run_tape_group(root / "2026-10-05-100000.tape", {&arb}, cost, r2, err, 1'000'000'000, &lag) && lag.hist.count() >= 3,
              "the feed lag is measured off the trade prints");
        check(r2[0].trips[in1] == 0 || r2[0].net[in1] != runs[0].net[in1], "a slower latency changes what the strategy gets");
    }
    {
        fs::create_directories(root / "data/store/tapes/2026-10-07");
        fs::create_directories(root / "data/live/tapes");
        std::ofstream(root / "data/store/tapes/2026-10-07/2026-10-07-091000.tape") << "x";
        std::ofstream(root / "data/live/tapes/2026-10-06-091000.tape") << "x";
        const auto t = tn::find_tapes(root);
        check(t.size() == 2 && t[0].filename() == "2026-10-06-091000.tape" && t[1].filename() == "2026-10-07-091000.tape",
              "the engine's tapes and the recorder's are both found, oldest first");
    }

    // ---- the stats sheet ----------------------------------------------------------------
    {
        live::TradeRecord r;
        r.trade_net = {100, -50, 200, -25};
        r.hold_s = {10, 20, 30, 40};
        r.daily_net = {50, 175, -100, 0};
        r.gross = 300;
        r.expenses = 75;
        r.markout_sum = {-40, 20, 60};
        r.markout_n = {4, 4, 4};
        const auto st = live::stats_sheet(r);
        check(st.trades == 4 && st.hit_rate == 0.5 && st.expectancy == 56.25 && st.profit_factor == 4.0,
              "hit rate, expectancy and profit factor per round trip");
        check(st.max_drawdown == 100.0 && st.net == 125.0 && std::fabs(st.cost_share - 0.25) < 1e-12 && st.avg_hold_s == 25.0,
              "drawdown on the cumulative daily net, costs as a share of gross, the average hold");
        check(st.markout[0] == -10.0 && st.markout[2] == 15.0 && std::isfinite(st.sharpe), "markouts a trade, and a daily Sharpe");
        live::TradeRecord one;
        one.daily_net = {10};
        check(!std::isfinite(live::stats_sheet(one).sharpe), "one day has no Sharpe: NaN, never a made-up zero");
    }

    fs::remove_all(root);
    std::printf("%s\n", failures == 0 ? "all tuning checks passed" : "tuning checks did not pass");
    return failures == 0 ? 0 : 1;
}
