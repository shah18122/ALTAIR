// Tests for strategies/pairs_futures.hpp -- the walked-forward futures pair.

#include <strategies/pairs_futures.hpp>

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

using namespace altair;

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::printf("FAIL: %s\n", what); }
    else { std::printf("  ok  : %s\n", what); }
}

struct Rng {
    std::uint64_t s;
    double gauss() {
        double u = 0.0;
        for (int k = 0; k < 12; ++k) {
            s = s * 6364136223846793005ull + 1442695040888963407ull;
            u += static_cast<double>(s >> 11) * (1.0 / 9007199254740992.0);
        }
        return u - 6.0;
    }
};

struct Pair { std::vector<std::int64_t> day; std::vector<double> a, b; };

/// log B a random walk; log A = 0.3 + 1.2 log B + an OU spread (half-life ~5
/// days) when `tied`, an independent random walk otherwise.
Pair make(std::size_t n, bool tied, std::uint64_t seed) {
    Rng r{seed};
    Pair p;
    double lb = std::log(1000.0), e = 0.0, la = std::log(800.0);
    for (std::size_t i = 0; i < n; ++i) {
        lb += 0.01 * r.gauss();
        e = 0.87 * e + 0.004 * r.gauss();
        la = tied ? 0.3 + 1.2 * lb + e : la + 0.01 * r.gauss();
        p.day.push_back(static_cast<std::int64_t>(i));
        p.a.push_back(std::exp(la));
        p.b.push_back(std::exp(lb));
    }
    return p;
}

void test_cointegrated() {
    const auto p = make(1300, true, 3);
    std::set<std::int64_t> rolls;
    for (std::int64_t d = 20; d < 1300; d += 21) { rolls.insert(d); }
    PairsPolicy pol;
    const auto r = pairs_walk_forward(p.day, p.a, p.b, 100.0, 250.0, rolls, pol);
    check(r.has_value(), "the pair walks forward");
    if (!r) { return; }
    std::size_t traded = 0;
    for (const auto& w : r->windows) { traded += w.traded ? 1u : 0u; }
    double gross = 0.0;
    std::size_t at_mean = 0, wins = 0, rolled = 0;
    for (const auto& t : r->trades) {
        gross += t.gross;
        at_mean += t.exit_reason == "mean" ? 1u : 0u;
        wins += t.gross > 0.0 ? 1u : 0u;
        rolled += t.rolls.size();
    }
    std::printf("        %zu of %zu windows traded, %zu trades, %zu at the mean, %zu winners, gross %.0f, %zu rolls\n",
                traded, r->windows.size(), r->trades.size(), at_mean, wins, gross, rolled);
    check(traded >= r->windows.size() - 2, "a truly cointegrated pair passes Engle-Granger in (nearly) every window");
    check(r->trades.size() > 20 && gross > 0.0 && wins > r->trades.size() / 2, "and its spread pays when it reverts");
    check(rolled > 0, "positions held across an expiry are rolled, and say when");
    const auto& t = r->trades.front();
    const double want = t.hedge_beta * 100.0 * t.a_in / (250.0 * t.b_in);
    check(t.lots_a == 1 && t.lots_b == std::max(1L, std::lround(want)), "leg A one lot; leg B the nearest whole number of lots to beta");
    check(std::fabs(r->windows[2].beta - 1.2) < 0.15, "the hedge ratio is estimated, close to the true 1.2");
}

void test_independent() {
    const auto p = make(1300, false, 5);
    const auto r = pairs_walk_forward(p.day, p.a, p.b, 100.0, 250.0, {}, PairsPolicy{});
    std::size_t traded = 0;
    for (const auto& w : r->windows) { traded += w.traded ? 1u : 0u; }
    std::printf("        independent walks: %zu of %zu windows pass\n", traded, r->windows.size());
    check(r.has_value() && traded <= 3u, "two independent random walks are (nearly) never traded");
}

void test_no_look_ahead() {
    auto p = make(1300, true, 11);
    const auto r1 = pairs_walk_forward(p.day, p.a, p.b, 100.0, 250.0, {}, PairsPolicy{});
    const std::size_t j = 800;
    for (std::size_t i = j + 1; i < p.a.size(); ++i) { p.a[i] *= 1.5; }   // everything after j changes
    const auto r2 = pairs_walk_forward(p.day, p.a, p.b, 100.0, 250.0, {}, PairsPolicy{});
    std::size_t same = 0, before = 0;
    for (const auto& t : r1->trades) {
        if (t.exit > j) { continue; }
        ++before;
        for (const auto& u : r2->trades) {
            if (u.entry == t.entry && u.exit == t.exit && u.gross == t.gross) { ++same; break; }
        }
    }
    check(before > 5 && same == before, "a trade closed before a price changed is untouched by it");
}

void test_refusals() {
    const auto p = make(400, true, 1);
    PairsPolicy bad;
    bad.exit_z = 3.0;
    check(!pairs_walk_forward(p.day, p.a, p.b, 100.0, 250.0, {}, bad).has_value(), "an exit beyond the entry is refused");
    check(!pairs_walk_forward(p.day, p.a, p.b, 0.0, 250.0, {}, PairsPolicy{}).has_value(), "no lot size: refused, not guessed");
}

} // namespace

int main() {
    std::printf("Pairs with futures\n");
    test_cointegrated();
    test_independent();
    test_no_look_ahead();
    test_refusals();
    std::printf("Pairs with futures: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
