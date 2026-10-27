// models/combo_search.hpp on answers known in advance: a rule that pays only
// when one signal is high is found and traded, walk-forward; the placebo
// (outcomes shuffled across sessions) does not find it; a condition compares
// with the median of earlier steps only; an outcome is credited only once it
// is final; changing everything after a cut leaves every pick and trade
// before it bit for bit the same; and the deflation statistics match known
// values.
//
// No check description here may contain the substring "F" "AIL" joined.

#include <models/combo_search.hpp>

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("  %s: %s\n", ok ? "ok  " : "BAD ", what);
    if (!ok) ++failures;
}

using altair::combo::Outcome;
using altair::combo::Step;

constexpr std::int64_t kDay = 86'400;
constexpr std::int64_t kStart = 18'000 * kDay;   // a Monday-ish epoch day, 2019

/// Two groups (slots), two strategies, three signals. Strategy 1 in group 0
/// earns +100 when signal 0 is above zero and -100 when below; everything
/// else is noise around 0.
std::vector<Step> world(std::uint64_t seed, int days) {
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> n01;
    std::vector<Step> out;
    for (int d = 0; d < days; ++d) {
        for (int g = 0; g < 2; ++g) {
            Step s;
            s.t = kStart + d * kDay + (10 + g) * 3600;
            s.group = g;
            s.x = {n01(rng), n01(rng), n01(rng)};
            s.y.resize(2);
            for (int k = 0; k < 2; ++k) {
                double pnl = 30.0 * n01(rng);
                if (g == 0 && k == 1) pnl += s.x[0] > 0 ? 100.0 : -100.0;
                s.y[static_cast<std::size_t>(k)] = Outcome{pnl, s.t + 3 * 3600};
            }
            out.push_back(s);
        }
    }
    return out;
}

double total(const altair::combo::Search& s) {
    double t = 0;
    for (const auto& tr : s.trades()) t += tr.pnl;
    return t;
}

} // namespace

int main() {
    using namespace altair::combo;
    std::printf("altair_trader combination search\n");

    SearchConfig cfg;
    cfg.top_k = 1;
    cfg.warmup_days = 120;
    cfg.min_trades = 20;
    const auto steps = world(7, 700);

    Search s(2, 2, 3, cfg);
    s.run(steps);
    check(!s.picks().empty(), "the book is picked once the warm-up is over");
    {
        const auto& last = s.picks().back();
        const Rule r = s.rules()[last.rules.front()];
        check(last.rules.size() == 1 && r.group == 0 && r.strategy == 1 && r.c1 == 0 && r.c2 < 0,
              "the pick is strategy 1 in slot 0 when signal 0 is above its median, nothing more");
    }
    check(total(s) > 0.0 && s.trades().size() > 100, "walk-forward trading of the pick makes money");
    {
        bool before = true;
        for (const auto& tr : s.trades()) {
            const auto& p = s.picks();
            // the trade's step is after the first pick
            if (steps[tr.step].t < p.front().t) before = false;
        }
        check(before, "no trade before the first pick");
    }

    // placebo: the same search on shuffled outcomes earns about nothing
    {
        double best = -1e18;
        for (std::uint64_t k = 1; k <= 5; ++k) {
            Search p(2, 2, 3, cfg);
            p.run(shuffle_outcomes(steps, k));
            best = std::max(best, total(p));
        }
        check(best < 0.25 * total(s), "on shuffled outcomes no placebo run comes near the real run");
    }

    // thresholds are medians of earlier steps of the same group only
    {
        Search q(2, 2, 3, cfg);
        q.run(steps);
        check(std::isnan(q.thresholds()[0][0]) && std::isnan(q.thresholds()[1][0]),
              "the first step of each group has no threshold yet");
        const double m = q.thresholds()[2][0];   // group 0, after one group-0 step
        check(m == steps[0].x[0], "the second step's threshold is the first step's value");
    }

    // an outcome counts only once final: one huge outcome final far in the future cannot be credited before
    {
        auto late = steps;
        late[0].y[1] = Outcome{1e9, late.back().t + kDay};
        Search q(2, 2, 3, cfg);
        q.run(late);
        bool same = q.picks().size() == s.picks().size();
        for (std::size_t i = 0; same && i < q.picks().size(); ++i) same = q.picks()[i].rules == s.picks()[i].rules;
        check(same, "an outcome final after the last step never moves a pick");
    }

    // leak check: change everything after a cut; picks and trades before it stay
    {
        const std::int64_t cut = steps[steps.size() * 6 / 10].t;
        auto other = steps;
        std::mt19937_64 rng(99);
        std::normal_distribution<double> n01;
        for (auto& st : other) {
            if (st.t >= cut) for (double& v : st.x) v = n01(rng) * 5.0;
            for (auto& o : st.y)
                if (o.known_at >= cut) o.pnl = n01(rng) * 1000.0;
        }
        Search a(2, 2, 3, cfg), b(2, 2, 3, cfg);
        a.run(steps);
        b.run(other);
        bool same = true;
        std::size_t ta = 0, tb = 0;
        for (const auto& tr : a.trades()) if (steps[tr.step].t < cut) ++ta;
        for (const auto& tr : b.trades()) if (other[tr.step].t < cut) ++tb;
        same = ta == tb && ta > 0;
        for (std::size_t i = 0; same && i < ta; ++i)
            same = a.trades()[i].step == b.trades()[i].step && a.trades()[i].rule == b.trades()[i].rule;
        for (std::size_t i = 0; same && i < a.picks().size() && a.picks()[i].t < cut; ++i)
            same = a.picks()[i].rules == b.picks()[i].rules;
        check(same, "everything after a cut changed: every pick and trade before it is the same");
    }

    // depth limits and pair formation
    {
        SearchConfig c1 = cfg;
        c1.depth = 1;
        Search one(2, 2, 3, c1);
        check(one.rules().size() == 2 * 2 * (1 + 6), "depth 1: always plus two conditions per signal");
        Search two(2, 2, 3, cfg);
        check(two.rules().size() == 2 * 2 * (1 + 6 + 12), "depth 2: plus pairs of conditions on different signals");
        SearchConfig c2 = cfg;
        c2.pair_signals = 2;
        Search lim(2, 2, 3, c2);
        check(lim.rules().size() == 2 * 2 * (1 + 6 + 4), "pairs only among the first pair_signals signals");
    }

    // statistics
    check(std::fabs(norm_inv(0.975) - 1.959963985) < 1e-6, "inverse normal at 97.5% is 1.96");
    check(std::fabs(norm_inv(0.001) + 3.090232306) < 1e-6, "inverse normal in the tail");
    check(std::fabs(probabilistic_sharpe(0.0, 0.0, 100, 0, 3) - 0.5) < 1e-12, "PSR of SR equal to its benchmark is 1/2");
    {
        const double e = expected_max_sharpe(1000, 0.01);
        check(e > 0.3 && e < 0.34, "the best of 1000 lucky tries with SR sd 0.1 reaches about 0.32");
    }
    check(Search::month_of(kStart) + 1 == Search::month_of(kStart + 31 * kDay), "calendar months");

    std::printf("%s\n", failures == 0 ? "all checks passed" : "SOME CHECKS ARE BAD");
    return failures == 0 ? 0 : 1;
}
