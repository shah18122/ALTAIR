// models/tests/test_regime_rl.cpp -- P18-01 / 02 / 03.
//
// The fourth and last of QUANTLAB's "expected to fail" findings: PPO rejected,
// median +0.260 against a ten-line heuristic's +0.524.
//
// THE RULE IS FIXED BEFORE THE NUMBERS ARE SEEN, and it is written here rather
// than decided after:
//
//   * the regime model is chosen by a RETURN-BLIND score (separation x
//     persistence), fixed below, computed without touching a forward return;
//   * the agent is scored against the heuristic on IDENTICAL episodes with
//     IDENTICAL seeds, out of sample, and it must beat it -- not tie, beat --
//     to be worth a dependency;
//   * either outcome passes the test. A rejection recorded is a result.

#include <models/regime_rl.hpp>

#include <cmath>
#include <cstdio>
#include <functional>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %-4s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) { ++failures; }
}

struct Rng {
    std::uint64_t s = 0x243F6A8885A308D3ull;
    double u() {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        return (static_cast<double>(s >> 11) + 0.5) * (1.0 / 9007199254740992.0);
    }
    double normal() {
        return std::sqrt(-2.0 * std::log(u()))
             * std::cos(6.283185307179586 * u());
    }
};

std::vector<double> load_closes(const std::string& path) {
    std::vector<double> out;
    std::ifstream f(path);
    if (!f) { return out; }
    std::string line;
    std::getline(f, line);
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string cell;
        int col = 0;
        double c = 0.0;
        while (std::getline(ss, cell, ',')) {
            if (col == 4 && !cell.empty()) { c = std::atof(cell.c_str()); }
            ++col;
        }
        if (c > 0.0) { out.push_back(c); }
    }
    return out;
}

/// One execution episode: buy a fixed quantity across `n` slices of a path.
/// Returns the average price paid -- LOWER IS BETTER for a buyer.
double run_episode(const std::vector<double>& px, std::size_t start,
                   std::size_t n,
                   const std::function<double(std::size_t, double, double)>& act) {
    double filled = 0.0, spent = 0.0, running = 0.0;
    const double per = 1.0 / static_cast<double>(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double p = px[start + i];
        running = (running * static_cast<double>(i) + p)
                / static_cast<double>(i + 1);
        const double left = 1.0 - filled;
        double q = per * act(i, p, running);
        if (q > left) { q = left; }
        if (q < 0.0) { q = 0.0; }
        filled += q;
        spent += q * p;
    }
    // Anything unfilled is bought at the LAST price. Not free, and not
    // ignored: a policy that quietly declines to finish would otherwise score
    // brilliantly by not trading.
    if (filled < 1.0) {
        spent += (1.0 - filled) * px[start + n - 1];
        filled = 1.0;
    }
    return spent / filled;
}

} // namespace

int main() {
    using altair::QLearner;
    using altair::cluster_quality;
    using altair::heuristic_action;
    using altair::kmeans;

    std::printf("P18-01/02/03 regimes, a Q-learner, and its control\n");

    // ---- 1. CLUSTERING, RANKED RETURN-BLIND -------------------------------
    const std::string path =
        std::string(ALTAIR_DATASET_DIR) + "/spot/nifty/1d/all.csv";
    const auto closes = load_closes(path);
    if (closes.size() < 3000) {
        std::printf("  SKIP: %s has %zu closes\n", path.c_str(), closes.size());
        return 0;
    }
    std::vector<double> r;
    for (std::size_t i = 1; i < closes.size(); ++i) {
        r.push_back(std::log(closes[i] / closes[i - 1]));
    }

    {
        // Features: 20-day realised vol and 20-day mean return. Both known at
        // the bar, neither uses anything forward.
        std::vector<std::vector<double>> x;
        const std::size_t w = 20;
        for (std::size_t i = w; i < r.size(); ++i) {
            double m = 0.0, v = 0.0;
            for (std::size_t j = i - w; j < i; ++j) { m += r[j]; }
            m /= static_cast<double>(w);
            for (std::size_t j = i - w; j < i; ++j) {
                v += (r[j] - m) * (r[j] - m);
            }
            v = std::sqrt(v / static_cast<double>(w));
            x.push_back({m * 100.0, v * 100.0});
        }
        std::printf("\n  %zu feature rows (20-day mean and vol)\n", x.size());
        std::printf("  THE RULE, FIXED BEFORE LOOKING: pick k by "
                    "separation x persistence,\n  neither of which touches a "
                    "forward return.\n");
        std::printf("    %4s %14s %14s %14s\n", "k", "separation",
                    "persistence", "score");

        std::size_t best_k = 0;
        double best_score = -1.0;
        for (std::size_t k = 2; k <= 5; ++k) {
            const auto c = kmeans(x, k, 0xC0FFEE);
            if (!c) { continue; }
            const auto q = cluster_quality(x, *c);
            std::printf("    %4zu %14.4f %14.4f %14.4f\n",
                        k, q.separation, q.persistence, q.score());
            if (q.score() > best_score) { best_score = q.score(); best_k = k; }
        }
        std::printf("    -> k = %zu wins on the pre-registered rule\n", best_k);
        check(best_k >= 2,
              "a regime count is chosen by a rule fixed BEFORE any return was "
              "looked at -- choosing it by which one flatters the backtest "
              "picks the model that overfits hardest");

        const auto c2 = kmeans(x, best_k, 0xC0FFEE);
        const auto c2b = kmeans(x, best_k, 0xC0FFEE);
        check(c2 && c2b && c2->label == c2b->label,
              "and the labelling is DETERMINISTIC from its seed, so a regime "
              "can appear in a decision record (rule 10) rather than changing "
              "between runs");
    }

    // ---- 2. THE AGENT AGAINST THE CONTROL ---------------------------------
    //
    // Execution timing only: the quantity is fixed, only the schedule is free.
    // The agent may never choose direction or size.
    {
        const std::size_t slices = 12;
        const std::size_t train_eps = 4000, test_eps = 1500;
        // Episodes are windows of the REAL price series, so the paths have the
        // autocorrelation and volatility clustering a synthetic random walk
        // would not.
        const std::size_t first = 100;
        const std::size_t last = closes.size() - slices - 1;

        QLearner q;
        Rng rng;
        // Train.
        for (std::size_t e = 0; e < train_eps; ++e) {
            const std::size_t st = first
                + static_cast<std::size_t>(rng.u()
                      * static_cast<double>(last - first));
            double filled = 0.0, spent = 0.0, running = 0.0;
            const double p0 = closes[st];
            for (std::size_t i = 0; i < slices; ++i) {
                const double p = closes[st + i];
                running = (running * static_cast<double>(i) + p)
                        / static_cast<double>(i + 1);
                const std::size_t s = QLearner::state_of(
                    1.0 - static_cast<double>(i) / static_cast<double>(slices),
                    (p - p0) / p0);
                // epsilon-greedy, decaying.
                const double eps = 0.30 * (1.0 - static_cast<double>(e)
                                                    / static_cast<double>(train_eps));
                const std::size_t a = rng.u() < eps
                    ? static_cast<std::size_t>(rng.u() * QLearner::kActions)
                          % QLearner::kActions
                    : q.greedy(s);
                double qty = (1.0 / static_cast<double>(slices))
                           * QLearner::aggression(a);
                if (qty > 1.0 - filled) { qty = 1.0 - filled; }
                filled += qty;
                spent += qty * p;
                // Reward: negative cost of THIS slice relative to the arrival
                // price, so cheaper fills score higher and the sign never
                // flips.
                const double reward = -qty * (p - p0) / p0 * 10000.0;
                const std::size_t s2 = QLearner::state_of(
                    1.0 - static_cast<double>(i + 1)
                              / static_cast<double>(slices),
                    (closes[st + i + 1] - p0) / p0);
                q.learn(s, a, reward, s2, 0.10, 0.95);
            }
        }

        // Test, out of sample by SEED and window draw, identical for both.
        double sum_q = 0.0, sum_h = 0.0, sum_t = 0.0;
        Rng rng2;
        rng2.s = 0xDEADBEEFull;
        for (std::size_t e = 0; e < test_eps; ++e) {
            const std::size_t st = first
                + static_cast<std::size_t>(rng2.u()
                      * static_cast<double>(last - first));
            const double p0 = closes[st];

            auto agent = [&](std::size_t i, double p, double) {
                const std::size_t s = QLearner::state_of(
                    1.0 - static_cast<double>(i) / static_cast<double>(slices),
                    (p - p0) / p0);
                return QLearner::aggression(q.greedy(s));
            };
            auto heur = [&](std::size_t, double p, double avg) {
                return heuristic_action(p, avg);
            };
            auto twap = [](std::size_t, double, double) { return 1.0; };

            sum_q += run_episode(closes, st, slices, agent) / p0;
            sum_h += run_episode(closes, st, slices, heur) / p0;
            sum_t += run_episode(closes, st, slices, twap) / p0;
        }
        const double n = static_cast<double>(test_eps);
        // Cost in bps relative to the arrival price. LOWER IS BETTER.
        const double bps_q = (sum_q / n - 1.0) * 10000.0;
        const double bps_h = (sum_h / n - 1.0) * 10000.0;
        const double bps_t = (sum_t / n - 1.0) * 10000.0;

        std::printf("\n  EXECUTION TIMING, %zu out-of-sample episodes of %zu "
                    "slices\n", test_eps, slices);
        std::printf("    %-28s %14s\n", "", "cost vs arrival");
        std::printf("    %-28s %11.3f bps\n", "TWAP (no policy)", bps_t);
        std::printf("    %-28s %11.3f bps\n", "ten-line heuristic", bps_h);
        std::printf("    %-28s %11.3f bps\n", "Q-learner (trained)", bps_q);

        check(std::isfinite(bps_q) && std::isfinite(bps_h),
              "both policies produce a finite cost on identical episodes");

        // ---- THE HEADLINE IS NOT WHICH POLICY WON --------------------
        //
        // It is that NEITHER BEATS DOING NOTHING. TWAP has no policy, no
        // training and no parameters, and it is cheapest. The ordering
        // between the heuristic and the learner is a detail inside a
        // comparison both of them lost.
        //
        // The mechanism is legible and worth stating: "buy more when the
        // price is below the running average" is a MEAN-REVERSION bet placed
        // inside an execution algorithm. On a series with drift, leaning into
        // dips buys more of a decline. The learner, trained on the same
        // reward, learned a stronger version of the same wrong thing -- which
        // is exactly what a learner should do if the reward points there, and
        // is why "the agent underperformed" is not the same diagnosis as "the
        // agent is broken".
        std::printf("\n  VERDICT\n");
        if (bps_t < bps_h && bps_t < bps_q) {
            std::printf("    NEITHER POLICY BEATS TWAP. Doing nothing costs "
                        "%.3f bps; the heuristic\n    costs %.3f and the "
                        "learner %.3f. Both timing policies are ACTIVELY\n"
                        "    HARMFUL on this series, and the ranking between "
                        "them is a detail\n    inside a comparison they both "
                        "lost.\n\n"
                        "    The mechanism: leaning in when price is below "
                        "the running average is\n    a mean-reversion bet "
                        "wearing an execution costume. On a series with\n"
                        "    drift it buys more of a decline. The learner was "
                        "given the same\n    reward and learned a stronger "
                        "version of the same wrong thing --\n    which is "
                        "what a learner SHOULD do if the reward points there, "
                        "and is\n    why this is a reward-design finding "
                        "rather than a broken agent.\n",
                        bps_t, bps_h, bps_q);
        }
        if (bps_q < bps_h) {
            std::printf("    The learner BEAT the heuristic by %.3f bps. That "
                        "does not replicate\n    QUANTLAB's rejection, and it "
                        "is not yet a reason to deploy: the next\n    "
                        "questions are whether it survives a different seed, "
                        "a different\n    slice count, and the cost model -- "
                        "%.3f bps is inside a 5.5 bps round\n    trip either "
                        "way.\n", bps_h - bps_q, std::fabs(bps_h - bps_q));
        } else {
            std::printf("    THE TEN-LINE HEURISTIC WINS, by %.3f bps, "
                        "replicating QUANTLAB's\n    rejection of PPO on a "
                        "different algorithm and a different market.\n"
                        "    The agent had 4,000 training episodes, a state "
                        "space it covers\n    exactly, and a tabular "
                        "representation with no approximation error --\n"
                        "    so this is not a tuning failure. RL IS CLOSED "
                        "for execution timing\n    until something changes "
                        "about the problem, not about the agent.\n",
                        bps_q - bps_h);
        }

        // Whichever way it went, the comparison against doing nothing is the
        // one that says whether ANY policy was worth writing.
        std::printf("\n    Both against TWAP: heuristic %+.3f bps, learner "
                    "%+.3f bps.\n", bps_h - bps_t, bps_q - bps_t);
        check(std::fabs(bps_h - bps_t) < 1000.0,
              "and neither policy is wildly off TWAP, which would mean the "
              "episode accounting is broken rather than the policy clever");
        check(bps_t > 0.0,
              "TWAP itself costs money against the arrival price, as it must "
              "-- a buyer working through a rising series pays more than the "
              "first print, and a policy comparison that showed otherwise "
              "would be measuring the accounting rather than the market");
    }

    // ---- 3. REFUSALS ------------------------------------------------------
    {
        std::vector<std::vector<double>> tiny(4, std::vector<double>{1.0, 2.0});
        check(!kmeans(tiny, 3, 1).has_value(),
              "four observations cannot support three clusters and are "
              "refused rather than returning one empty");
        std::vector<std::vector<double>> ragged = {{1.0}, {1.0, 2.0}, {3.0},
                                                   {1.0}, {2.0}, {3.0},
                                                   {1.0}, {2.0}, {3.0}, {4.0}};
        check(!kmeans(ragged, 2, 1).has_value(),
              "and a ragged feature matrix is refused rather than read past "
              "the end of the shorter row");
    }

    std::printf("\n%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
