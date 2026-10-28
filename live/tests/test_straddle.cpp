// Tests for live/straddle.hpp: the tick-by-tick straddle seller, on a
// simulated session fed tick by tick through the engine.
//
// No check description here may contain the substring "F" "AIL" joined.

#include <live/sim.hpp>
#include <live/straddle.hpp>

#include <cmath>
#include <cstdio>
#include <string>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) { ++failures; }
}

using namespace altair;
using namespace altair::live;

std::vector<LiveInstrument> universe(std::int64_t today) {
    std::vector<LiveInstrument> u;
    const auto add = [&u](std::uint32_t tok, const std::string& sym, const char* under, LiveKind k, double strike,
                          std::int64_t lot, std::int64_t expiry) {
        LiveInstrument i;
        i.token = tok; i.symbol = sym; i.fyers = "NSE:" + sym; i.underlying = under; i.kind = k; i.strike = strike;
        i.lot = lot; i.expiry_day = expiry; i.tick = 0.05;
        u.push_back(i);
    };
    add(kLiveNiftyToken, "NIFTY 50", "NIFTY", LiveKind::Index, 0, 1, 0);
    add(kLiveVixToken, "INDIA VIX", "INDIAVIX", LiveKind::Index, 0, 1, 0);
    add(11, "NIFTY26OCTFUT", "NIFTY", LiveKind::Future, 0, 65, today + 20);
    std::uint32_t tok = 1000;
    for (int k = 23000; k <= 25000; k += 50) {
        add(++tok, "NIFTYW" + std::to_string(k) + "CE", "NIFTY", LiveKind::Call, k, 65, today + 2);
        add(++tok, "NIFTYW" + std::to_string(k) + "PE", "NIFTY", LiveKind::Put, k, 65, today + 2);
    }
    return u;
}

struct Run {
    std::size_t legs_open = 0, sold_calls = 0, sold_puts = 0, bought = 0, trips = 0;
    std::string why_in, finding, why_out, all_findings;
    double call_strike = 0.0, put_strike = 0.0;
    int exit_minute = -1;
    LiveStraddleReading first;
    int entry_minute = -1;
};

Run session(double sigma_day, const LiveStraddleRules& rules = {}, std::vector<double> closes = {}) {
    const std::int64_t today = parse_day("2026-10-01");
    const auto u = universe(today);
    LiveEngine e(u, [](const LiveInstrument&, bool, double qty, double px, std::int64_t) { return 0.0005 * qty * px; });
    LiveStraddleInputs in;
    in.vol = LiveVolInputs{"NIFTY", sigma_day, 0.75, 24000.0};
    in.vix_close = 13.0; in.vix_forecast = 13.2;
    in.rules = rules;
    in.closes = std::move(closes);
    auto owned = std::make_unique<LiveStraddleModel>(in);
    LiveStraddleModel* model = owned.get();
    e.add_model(std::move(owned));
    LiveSimSeeds seeds;
    seeds.nifty = 24000; seeds.banknifty = 54000; seeds.vix = 13;
    const std::int64_t open_ns = (today * 86400 + 9 * 3600 + 15 * 60 - 19800) * 1'000'000'000LL;
    LiveSim sim(u, seeds, 11, open_ns);
    sim.board([&](const LiveInstrument&, const LiveSimEvent& ev) { e.on_quote(ev.quote, open_ns); });
    Run r;
    for (int step = 0; step < 6 * 3600 + 15 * 60; ++step) {
        sim.step(1'000'000'000, [&](const LiveInstrument&, const LiveSimEvent& ev) {
            e.on_quote(ev.quote, sim.now_ns());
            if (ev.trade) e.on_trade(ev.price, ev.price.exchange_ts_ns);
        });
        e.advance(sim.now_ns());
        if (r.entry_minute < 0 && !e.book().flat(model->name())) {
            r.entry_minute = live_minute_of_day(live_ist_minute_index(e.clock_ns()));
            r.first = model->reading();
        }
    }
    for (const auto& f : e.book().fills()) {
        if (f.model != model->name()) continue;
        if (f.partial) continue;   // count each order once, at its last fill
        const bool call = f.symbol.ends_with("CE");
        if (f.side < 0) {
            (call ? r.sold_calls : r.sold_puts) += 1;
            r.why_in = f.reason;
            if (const auto* i = e.instrument(f.token)) (call ? r.call_strike : r.put_strike) = i->strike;
        }
        else r.bought += 1;
    }
    for (const auto& t : e.book().trades()) {
        if (t.model != model->name()) continue;
        ++r.trips;
        r.why_out = t.why_out;
        r.exit_minute = live_minute_of_day(live_ist_minute_index(t.exit_ns));
    }
    r.legs_open = e.book().positions().size();
    for (const auto& n : e.take_decisions()) if (n.model == model->name()) { r.finding = n.text; r.all_findings += n.text + "\n"; }
    return r;
}

}  // namespace

int main() {
    std::printf("straddle seller\n");
    namespace d = live_straddle_detail;
    {
        const double F = 24000, K = 24000, T = 5.0 / 365;
        const double px = d::straddle_price(F, K, T, 0.13);
        check(std::fabs(d::straddle_iv(px, F, K, T) - 0.13) < 1e-6, "straddle IV inverts its own price");
        check(std::isnan(d::straddle_iv(0.5 * std::fabs(F - 23000.0), F, 23000.0, T)), "a price below intrinsic has no IV");
        check(std::fabs(d::years_to(0, 1) - (86400.0 + 36000.0) / (365.0 * 86400.0)) < 1e-12, "expiry is 15:30 IST (10:00 UTC)");
    }
    // Calm forecast (6 % annualised) against options priced near VIX 13: sells.
    const Run calm = session(0.004);
    std::printf("    entry %02d:%02d: %s\n", calm.entry_minute / 60, calm.entry_minute % 60, calm.why_in.c_str());
    check(calm.sold_calls == 1 && calm.sold_puts == 1, "forecast IV above forecast RV: one call and one put sold");
    check(calm.first.ok && calm.first.iv_bid >= calm.first.rv_forecast, "the entry reading had IV (bid) >= RV forecast");
    check(calm.entry_minute >= 9 * 60 + 15 && calm.entry_minute < 9 * 60 + 20, "it sold on the first ticks, not at a fixed minute");
    check(calm.first.call != nullptr && std::fabs(calm.first.strike - calm.first.forward) <= 25.0, "the strike is the one nearest the forward");
    check(calm.why_in.find("forecast IV >= forecast RV") != std::string::npos && calm.why_in.find("VIX") != std::string::npos,
          "the entry carries its reason, with the vols and VIX");
    check(calm.bought == 2 && calm.trips == 2 && calm.legs_open == 0, "both legs bought back by the 15:20 square-off");
    // Wild forecast (32 % annualised): never sells, and says why at 15:10.
    const Run wild = session(0.02);
    check(wild.sold_calls == 0 && wild.sold_puts == 0 && wild.bought == 0, "forecast RV above IV: nothing traded, nothing bought");
    std::printf("    finding: %s\n", wild.finding.c_str());
    check(wild.finding.find("No sale today") != std::string::npos && wild.finding.find("RV forecast") != std::string::npos,
          "a day without a sale records why");

    // ---- the book rules -------------------------------------------------
    const std::int64_t today = parse_day("2026-10-01");
    {
        LiveStraddleRules R;
        R.leg_delta = 0.16;
        const Run s = session(0.004, R);
        std::printf("    strangle: %s\n", s.why_in.c_str());
        check(s.sold_calls == 1 && s.sold_puts == 1 && s.put_strike < s.call_strike && s.put_strike < 24000.0 && s.call_strike > 24000.0,
              "a 16-delta strangle sells one call above and one put below the money");
        check(s.why_in.find("strangle bid") != std::string::npos && s.why_in.find("P(loss at expiry)") != std::string::npos,
              "its reason names the strangle and the book readings");
        check(s.bought == 2 && s.legs_open == 0, "both strangle legs bought back by the square-off");
    }
    {
        LiveStraddleRules R;
        R.events.insert(today);
        const Run s = session(0.004, R);
        check(s.sold_calls + s.sold_puts == 0 && s.all_findings.find("scheduled news") != std::string::npos,
              "a scheduled-news day: nothing sold, and the finding says why");
    }
    {
        LiveStraddleRules R;
        R.monthly_loss_halt = 5000.0;
        R.month_closed_pnl = -6000.0;
        const Run s = session(0.004, R);
        check(s.sold_calls + s.sold_puts == 0 && s.all_findings.find("monthly loss halt") != std::string::npos,
              "past the monthly loss halt: nothing sold");
        R.month_closed_pnl = -1000.0;
        check(session(0.004, R).sold_calls == 1, "inside the monthly limit: it sells as before");
    }
    {
        // Closes whose last five days swing far more than the fifteen before, and fall.
        std::vector<double> c{24000.0};
        for (int k = 1; k <= 60; ++k) c.push_back(c.back() * std::exp(k > 55 ? (k % 2 ? -0.03 : 0.01) : (k % 2 ? 0.002 : -0.0015)));
        LiveStraddleRules R;
        R.calm_trend_only = true;
        check(session(0.004, R, c).sold_calls == 0, "realised vol rising: the calm-only rule sells nothing");
        R.calm_trend_only = false;
        R.uptrend_only = true;
        check(session(0.004, R, c).sold_calls == 0, "below the 50-day average: the uptrend-only rule sells nothing");
        R.uptrend_only = false;
        check(session(0.004, R, c).sold_calls == 1, "the same closes with both rules off: it sells");
    }
    {
        LiveStraddleRules R;
        R.max_risk = 1.0;
        // Early the 3-sd move dwarfs the credit; late in the session it does not.
        const Run s = session(0.004, R);
        check(s.sold_calls == 0 || (s.first.risk <= 1.0 && s.entry_minute > 12 * 60),
              "a one-rupee risk cap: no sale until a 3-sd move over the rest of the day would not lose");
    }
    {
        LiveStraddleRules R;
        R.take_profit = 0.02;
        const Run s = session(0.004, R);
        std::printf("    profit taking: exit %02d:%02d, %s\n", s.exit_minute / 60, s.exit_minute % 60, s.why_out.c_str());
        check(s.trips == 2 && s.why_out.find("profit target") != std::string::npos && s.exit_minute < 15 * 60 + 20,
              "a 2 % profit target buys both legs back before the square-off");
    }
    std::printf("%s\n", failures ? "SOME CHECKS DID NOT PASS" : "all checks passed");
    return failures ? 1 : 0;
}
