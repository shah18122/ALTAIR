// Tests for models/hmm_regime.hpp -- the HMM stress probability, walk-forward.

#include <models/hmm_regime.hpp>

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

} // namespace

int main() {
    std::printf("HMM regime, walk-forward\n");
    // Calm (0.6%) and stress (2%) in blocks of 60 days.
    std::mt19937 g(5);
    std::normal_distribution<double> z(0.0, 1.0);
    std::vector<std::int64_t> day;
    std::vector<double> ret;
    std::vector<bool> stress;
    for (int t = 0; t < 1500; ++t) {
        const bool s = (t / 60) % 3 == 2;
        day.push_back(18'000 + t);
        ret.push_back((s ? 0.02 : 0.006) * z(g));
        stress.push_back(s);
    }
    RegimeParams p;
    const auto rows = walk_forward_regimes(day, ret, p);
    check(!rows.empty() && rows.front().day == day[p.min_history - 1], "first row at the first fit");
    check(rows.size() == day.size() - (p.min_history - 1), "one row per day after it");

    double ps = 0.0, pc = 0.0;
    std::size_t ns = 0, nc = 0, right = 0;
    for (const auto& r : rows) {
        const auto i = static_cast<std::size_t>(r.day - 18'000);
        (stress[i] ? ps : pc) += r.p_stress;
        ++(stress[i] ? ns : nc);
        if ((r.p_stress > 0.5) == stress[i]) { ++right; }
    }
    std::printf("    mean P(stress): %.2f on stress days, %.2f on calm; %.1f%% classified right\n",
                ps / static_cast<double>(ns), pc / static_cast<double>(nc),
                100.0 * static_cast<double>(right) / static_cast<double>(rows.size()));
    check(ps / static_cast<double>(ns) > 0.7 && pc / static_cast<double>(nc) < 0.3, "stress found where it is");
    check(rows.back().sigma_stress > 2.0 * rows.back().sigma_calm, "the states are sorted: last one is stress");

    // ---- no row changed by data after it -----------------------------------------
    {
        std::size_t diff = 0;
        for (const std::size_t cut : {std::size_t{700}, std::size_t{1111}}) {
            auto other = ret;
            for (std::size_t i = cut + 1; i < other.size(); ++i) { other[i] = 0.05 * z(g); }
            const auto b = walk_forward_regimes(day, other, p);
            for (std::size_t i = 0; i < rows.size() && i < b.size(); ++i) {
                if (rows[i].day > day[cut]) { break; }
                if (rows[i].p_stress != b[i].p_stress || rows[i].p_stress_next != b[i].p_stress_next) { ++diff; }
            }
        }
        check(diff == 0, "leak check: every row up to each cut unchanged by another future");
    }
    // ---- the feed ------------------------------------------------------------------
    {
        const auto f = regime_daily_feed("NIFTY", rows);
        check(f.cols() == 3 && f.rows() == rows.size(), "feed: three columns, one row per day");
        check(f.t.front() == rows.front().day * 86'400 + 15 * 3600 + 30 * 60, "feed stamped at the 15:30 close");
        check(!f.intraday, "feed is daily");
    }
    // ---- too short to fit ----------------------------------------------------------
    {
        std::vector<std::int64_t> d2(day.begin(), day.begin() + 200);
        std::vector<double> r2(ret.begin(), ret.begin() + 200);
        check(walk_forward_regimes(d2, r2, p).empty(), "fewer than min_history returns: no rows");
    }
    std::printf(failures == 0 ? "all passed\n" : "%d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
