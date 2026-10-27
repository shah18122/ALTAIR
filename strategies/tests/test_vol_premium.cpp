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

void test_unquoted_strike_still_closes() {
    // The held strike stops trading after the first entry (as NIFTY's deep in
    // the money strikes did in March 2020): the trade must still close at the
    // exit, at intrinsic, and later months must still be sold.
    auto m = market(0.20, 0.10, 0.10, 21);
    const auto quote = m.quote;
    const double held = m.strike_near(0, 0, m.forward(0, *m.expiries.upper_bound(m.day[0])));
    m.quote = [quote, held](std::size_t i, std::int64_t e, double k) {
        return i > 0 && i < 40 && k == held ? VrpQuote{} : quote(i, e, k);
    };
    const auto r = vrp_run(m, policy());
    check(r && r->trades.size() > 20, "an unquoted strike at the exit does not freeze the run");
    check(r && !r->trades.empty() && std::string{r->trades.front().exit_reason} == "unquoted at exit: intrinsic",
          "the unquoted trade is bought back at intrinsic");
}

void test_gamma_desk_numbers() {
    // Gleadall: the hedge band, the locked-in vol and the cost in vols.
    const auto every = vrp_run(market(0.20, 0.10, 0.10, 31), policy());
    auto bp = policy();
    bp.hedge_band = 0.5;
    const auto band = vrp_run(market(0.20, 0.10, 0.10, 31), bp);
    check(every && band && every->trades.size() == band->trades.size(), "a hedge band changes the hedging, not the entries");
    if (!every || !band) { return; }
    std::size_t fe = 0, fb = 0;
    for (const auto& t : every->trades) { fe += t.fills.size(); }
    for (const auto& t : band->trades) { fb += t.fills.size(); }
    std::printf("    hedge orders: every close %zu, half-breakeven band %zu\n", fe, fb);
    check(fb < fe, "the half-breakeven band re-hedges less often than every close");
    for (const auto& t : band->trades) {
        long net = 0;
        for (const auto& f : t.fills) { net += f.lots; }
        if (net != 0) { check(false, "a banded trade leaves the futures hedge open"); return; }
    }
    check(true, "every banded trade closes its futures hedge flat");

    const auto& t = every->trades.front();
    check(t.vega_pt_in > 0.0 && t.breakeven_in > 0.0 && t.gamma_theta_in > 0.0, "entry Greeks recorded on the trade");
    check(std::fabs(t.locked_vol - (t.iv_in - t.gross / t.qty / t.vega_pt_in / 100.0)) < 1e-12
              && (t.gross > 0.0) == (t.locked_vol < t.iv_in),
          "locked-in vol is IV sold minus P&L over vega, below the sale when the trade won");
    const auto& s = every->signals.front();
    check(s.daily_move > 0.0 && s.breakeven_move > 0.0, "each signal carries the implied daily move and breakeven");
    check(s.p_loss_expiry > 0.0 && s.p_loss_expiry < 1.0 && t.p_loss_in > 0.0 && t.p_loss_in < 1.0,
          "signal and trade carry the model's probability of loss at expiry");

    check(std::isnan(s.cone_rank) || (s.cone_rank >= 0.0 && s.cone_rank <= 1.0), "the cone rank is a share or NaN");
    auto hp = policy();
    hp.min_cone_rank = 1.01;   // nothing ranks above every window
    const auto high = vrp_run(market(0.20, 0.10, 0.10, 31), hp);
    check(high && high->trades.empty() && !high->signals.empty(), "a cone-rank floor no IV can reach blocks every entry");

    // A scheduled event inside every holding window vetoes every entry; one
    // dated before the first day vetoes nothing.
    auto em = market(0.20, 0.10, 0.10, 31);
    for (const auto e : em.expiries) { em.events.insert(e); }
    const auto evr = vrp_run(em, policy());
    bool all_vetoed = evr && evr->trades.empty() && !evr->signals.empty();
    if (evr) { for (const auto& sg : evr->signals) { all_vetoed = all_vetoed && (!(sg.iv - sg.rv_fcst >= 0.02) || sg.vetoed); } }
    check(all_vetoed, "news on the expiry day vetoes the straddle that would hold through it");
    auto old = market(0.20, 0.10, 0.10, 31);
    old.events.insert(old.day.front() - 10);
    const auto oldr = vrp_run(old, policy());
    check(oldr && oldr->trades.size() == every->trades.size(), "news already past vetoes nothing");

    // Slippage so large it eats the whole 10-point edge: with costs counted in vols, nothing is sold.
    auto cp = policy();
    cp.slippage_pts = 400.0;
    cp.cost_in_vols = true;
    const auto costly = vrp_run(market(0.20, 0.10, 0.10, 31), cp);
    check(costly && costly->trades.empty() && !costly->signals.empty() && costly->signals.front().cost_vols > 0.10,
          "a round trip that costs more vols than the edge blocks the entry");
    auto neg = policy();
    neg.hedge_band = -0.1;
    check(!vrp_run(market(0.2, 0.1, 0.1, 1, 100), neg).has_value(), "a negative band: refused");
}

} // namespace

int main() {
    std::printf("Volatility premium\n");
    test_premium_is_earned();
    test_no_premium_no_trade();
    test_wrong_forecast_loses();
    test_hedge_cuts_risk();
    test_veto_and_refusals();
    test_unquoted_strike_still_closes();
    test_gamma_desk_numbers();
    std::printf("Volatility premium: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
