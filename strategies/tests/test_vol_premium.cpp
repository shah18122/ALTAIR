// Tests for strategies/vol_premium.hpp -- the delta-hedged short straddle.

#include <strategies/vol_premium.hpp>

#include <cmath>
#include <cstdio>
#include <random>
#include <string>

namespace {

using namespace altair;

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::printf("FAIL: %s\n", what); }
    else { std::printf("  ok  : %s\n", what); }
}

constexpr double kRate = 0.065;

double b76(bool call, double F, double K, double T, double v) {
    const auto g = black76(call ? OptionRight::Call : OptionRight::Put, Price{std::llround(F * 100.0)},
                           Price{std::llround(K * 100.0)}, Years{T}, Vol{v}, kRate);
    return g ? g->price / 100.0 : std::nan("");
}

/// Weekday sessions from day 20,000; the index follows GBM at `realised`, the
/// options are priced at `implied`, and the forecaster says `forecast`.
VrpMarket market(double implied, double realised, double forecast, unsigned seed, std::size_t n = 750) {
    VrpMarket m;
    std::mt19937 g(seed);
    std::normal_distribution<double> z(0.0, 1.0);
    double s = 20'000.0;
    for (std::int64_t d = 20'000; m.day.size() < n; ++d) {
        if (vrp_detail::weekday(d) >= 5) { continue; }
        m.day.push_back(d);
        m.spot.push_back(s);
        s *= std::exp(-0.5 * realised * realised / 252.0 + realised / std::sqrt(252.0) * z(g));
        if (m.day.size() % 21 == 0) { m.expiries.insert(d); }   // one expiry every 21 sessions
    }
    m.expiries.insert(m.day.back() + 30);
    const auto days = m.day;
    const auto spot = m.spot;
    m.forward = [days, spot](std::size_t i, std::int64_t e) {
        return spot[i] * std::exp(kRate * static_cast<double>(e - days[i]) / 365.0);
    };
    m.strike_near = [](std::size_t, std::int64_t, double level) { return std::round(level / 50.0) * 50.0; };
    m.quote = [days, spot, implied](std::size_t i, std::int64_t e, double k) {
        const double T = static_cast<double>(e - days[i]) / 365.0;
        const double F = spot[i] * std::exp(kRate * T);
        return VrpQuote{b76(true, F, k, T, implied), b76(false, F, k, T, implied)};
    };
    m.rv_forecast = [forecast](std::size_t, std::size_t) { return forecast; };
    return m;
}

VrpPolicy policy() {
    VrpPolicy p;
    p.lot_size = 75.0;
    p.lots = 1;
    p.rate = kRate;
    p.entry_spread = 0.02;
    p.min_sessions = 10;
    p.exit_sessions = 1;
    p.slippage_pts = 0.0;
    return p;
}

double total(const VrpResult& r) {
    double s = 0.0;
    for (const auto& t : r.trades) { s += t.gross; }
    return s;
}

void test_premium_is_earned() {
    const auto r = vrp_run(market(0.20, 0.10, 0.10, 11), policy());
    check(r.has_value() && r->trades.size() > 20, "implied 20 % over a 10 % forecast: the straddle is sold, month after month");
    if (!r) { return; }
    std::size_t wins = 0;
    for (const auto& t : r->trades) { wins += t.gross > 0.0 ? 1 : 0; }
    std::printf("    %zu trades, %zu won, gross %.0f\n", r->trades.size(), wins, total(*r));
    check(total(*r) > 0.0 && wins * 4 > r->trades.size() * 3,
          "and realised 10 % against 20 % implied pays: most months win, the total is positive");
    const auto& t = r->trades.front();
    check(std::fabs(t.iv_in - 0.20) < 1e-3 && std::string{t.exit_reason} == "before expiry",
          "the implied vol is solved back from the quotes, and the trade is closed before expiry");
    long net = 0;
    for (const auto& f : t.fills) { net += f.lots; }
    check(!t.fills.empty() && net == 0, "the futures hedge trades and is flat when the straddle is bought back");
}

void test_no_premium_no_trade() {
    const auto r = vrp_run(market(0.10, 0.20, 0.20, 12), policy());
    check(r && r->trades.empty() && !r->signals.empty(), "implied below the forecast: priced every day, never sold");
}

void test_wrong_forecast_loses() {
    const auto r = vrp_run(market(0.12, 0.25, 0.05, 13), policy());
    check(r && !r->trades.empty() && total(*r) < 0.0,
          "a forecast that says calm when the market is not: sold, and the hedged straddle loses");
}

void test_hedge_cuts_risk() {
    double sd_h = 0.0, sd_n = 0.0;
    auto np = policy();
    np.hedge = false;
    for (unsigned seed = 20; seed < 26; ++seed) {
        const auto h = vrp_run(market(0.15, 0.15, 0.10, seed), policy());
        const auto n = vrp_run(market(0.15, 0.15, 0.10, seed), np);
        if (!h || !n) { continue; }
        for (const auto& t : h->trades) { sd_h += t.gross * t.gross; }
        for (const auto& t : n->trades) { sd_n += t.gross * t.gross; }
    }
    std::printf("    rms P&L a trade: hedged %.0f, unhedged %.0f\n", std::sqrt(sd_h), std::sqrt(sd_n));
    check(sd_h < 0.5 * sd_n, "the daily delta hedge removes most of the straddle's directional risk");
}

void test_veto_and_refusals() {
    auto m = market(0.20, 0.10, 0.10, 14);
    m.veto = [](std::size_t) { return true; };
    const auto r = vrp_run(m, policy());
    check(r && r->trades.empty() && !r->signals.empty() && r->signals.front().vetoed,
          "a veto (a VIX-up forecast) blocks entries and is recorded");
    auto bad = policy();
    bad.exit_sessions = 12;
    check(!vrp_run(market(0.2, 0.1, 0.1, 1, 100), bad).has_value(), "exit later than the entry window: refused");
    VrpMarket empty;
    check(!vrp_run(empty, policy()).has_value(), "a market without prices: refused");
}

} // namespace

int main() {
    std::printf("Volatility premium\n");
    test_premium_is_earned();
    test_no_premium_no_trade();
    test_wrong_forecast_loses();
    test_hedge_cuts_risk();
    test_veto_and_refusals();
    std::printf("Volatility premium: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
