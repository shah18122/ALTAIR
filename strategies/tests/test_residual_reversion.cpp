// Tests for strategies/residual_reversion.hpp -- Avellaneda-Lee residual reversion.

#include <strategies/residual_reversion.hpp>

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

namespace {

using namespace altair;

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::printf("FAIL: %s\n", what); }
    else { std::printf("  ok  : %s\n", what); }
}

/// N stocks over T days: r_i = beta_i r_m + (sector shock) + d(residual level).
/// `b` is the residual level's daily AR(1) coefficient: 0.85 reverts within
/// days, 1.0 is a random walk (nothing to revert).
RrPanel panel(std::size_t N, std::size_t T, double b, unsigned seed, double sector_vol = 0.0) {
    std::mt19937 g(seed);
    std::normal_distribution<double> z(0.0, 1.0);
    RrPanel p;
    p.market.resize(T);
    p.ret.assign(N, std::vector<double>(T));
    p.sector.resize(N);
    std::vector<double> beta(N), level(N, 0.0);
    for (std::size_t i = 0; i < N; ++i) {
        beta[i] = 0.7 + 0.6 * static_cast<double>(i % 5) / 4.0;
        p.sector[i] = static_cast<int>(i % 4);
    }
    for (std::size_t t = 0; t < T; ++t) {
        p.day.push_back(static_cast<std::int64_t>(20'000 + t));
        p.market[t] = 0.01 * z(g);
        double shock[4];
        for (double& s : shock) { s = sector_vol * z(g); }
        for (std::size_t i = 0; i < N; ++i) {
            const double next = b * level[i] + 0.01 * z(g);
            const double d = next - level[i];
            level[i] = next;
            p.ret[i][t] = beta[i] * p.market[t] + shock[p.sector[i]] + d;
        }
    }
    return p;
}

double total(const RrResult& r) {
    double s = 0.0;
    for (const auto& d : r.daily) { s += d.ret; }
    return s;
}

void test_reverting_residuals_pay() {
    const auto p = panel(30, 800, 0.85, 1);
    const auto r = rr_run(p, RrPolicy{});
    check(r.has_value() && r->trades.size() > 200, "reverting residuals: many trades across the universe");
    if (!r) { return; }
    std::size_t wins = 0;
    for (const auto& t : r->trades) { wins += t.ret > 0.0 ? 1 : 0; }
    std::printf("    %zu trades, %.1f %% won, total %.3f per unit\n", r->trades.size(),
                100.0 * static_cast<double>(wins) / static_cast<double>(r->trades.size()), total(*r));
    check(total(*r) > 0.0 && wins * 10 > r->trades.size() * 6, "and the hedged book makes money, most trades winning");
    double hold = 0.0;
    for (const auto& t : r->trades) { hold += static_cast<double>(t.exit - t.entry); }
    check(hold / static_cast<double>(r->trades.size()) < 15.0, "positions are short-lived, as fast reversion implies");
}

void test_random_walk_residuals_do_not() {
    double sum = 0.0, abs_sum = 0.0;
    std::size_t trades = 0;
    for (unsigned seed = 10; seed < 16; ++seed) {
        const auto r = rr_run(panel(30, 600, 1.0, seed), RrPolicy{});
        if (!r) { continue; }
        sum += total(*r);
        for (const auto& t : r->trades) { abs_sum += std::fabs(t.ret); }
        trades += r->trades.size();
    }
    std::printf("    random-walk residuals: %zu trades, total %.3f against %.3f of absolute P&L\n", trades, sum, abs_sum);
    check(std::fabs(sum) < 0.1 * abs_sum, "random-walk residuals: no systematic profit");
}

void test_no_look_ahead() {
    auto p = panel(20, 500, 0.85, 3);
    const auto a = rr_run(p, RrPolicy{});
    for (auto& r : p.ret) { for (std::size_t t = 301; t < r.size(); ++t) { r[t] = -r[t] * 3.0; } }
    for (std::size_t t = 301; t < p.market.size(); ++t) { p.market[t] = 0.05; }
    const auto b = rr_run(p, RrPolicy{});
    bool same = a && b;
    std::size_t compared = 0;
    if (same) {
        for (const auto& ta : a->trades) {
            if (ta.exit > 300) { continue; }   // closed before the future changed
            bool found = false;
            for (const auto& tb : b->trades) {
                if (tb.stock == ta.stock && tb.entry == ta.entry && tb.exit == ta.exit && tb.ret == ta.ret) { found = true; break; }
            }
            same = same && found;
            ++compared;
        }
        for (std::size_t t = 0; t <= 300; ++t) { same = same && a->daily[t].ret == b->daily[t].ret; }
    }
    check(same && compared > 20, "decisions and P&L up to day t do not change when everything after it does");
}

void test_sector_factor() {
    // A strong shared sector shock with no reversion of its own: without the
    // sector factor it leaks into every residual; with it, it is hedged out.
    const auto p = panel(32, 700, 0.85, 4, 0.02);
    RrPolicy with, without;
    without.sector_factor = false;
    const auto a = rr_run(p, with), b = rr_run(p, without);
    if (!a || !b) { check(false, "sector runs"); return; }
    double va = 0, vb = 0;
    for (const auto& d : a->daily) { va += d.ret * d.ret; }
    for (const auto& d : b->daily) { vb += d.ret * d.ret; }
    std::printf("    daily P&L rms: with sector %.4f, without %.4f; totals %.3f / %.3f\n", std::sqrt(va / 700), std::sqrt(vb / 700),
                total(*a), total(*b));
    check(total(*a) > total(*b), "the leave-one-out sector factor hedges a shared sector shock out of the residual");
}

void test_refusals() {
    RrPolicy bad;
    bad.exit_long = 2.0;
    check(!rr_run(panel(5, 100, 0.9, 5), bad).has_value(), "an exit beyond the entry is refused");
    RrPanel broken = panel(5, 100, 0.9, 5);
    broken.ret[2].pop_back();
    check(!rr_run(broken, RrPolicy{}).has_value(), "a ragged panel is refused");
}

} // namespace

int main() {
    std::printf("Residual reversion\n");
    test_reverting_residuals_pay();
    test_random_walk_residuals_do_not();
    test_no_look_ahead();
    test_sector_factor();
    test_refusals();
    std::printf("Residual reversion: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
