// models/meta_trader.hpp on answers known in advance: a ridge fit from decayed
// sufficient statistics equals the closed form; a stack learns to trust the
// input that carries the move and not the ones that do not; it makes no call
// before it has the outcomes to; a call is made only from what was learned
// before it (the day's own outcome cannot move it); and old days fade.
//
// No check description here may contain the substring "F" "AIL" joined.

#include <models/meta_trader.hpp>

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

} // namespace

int main() {
    using namespace altair::meta;
    std::printf("altair_trader meta-model\n");

    // Closed form: y = 2 + 3 x exactly, no decay, tiny penalty.
    {
        DecayedRidge r(2, 1.0);
        for (int i = 0; i < 50; ++i) {
            const double x = (i % 7) - 3.0;
            r.add({1.0, x}, 2.0 + 3.0 * x);
        }
        std::vector<double> w;
        check(r.solve(1e-9, w) && std::fabs(w[0] - 2.0) < 1e-6 && std::fabs(w[1] - 3.0) < 1e-6,
              "the decayed-statistics ridge recovers y = 2 + 3x");
        std::vector<double> w2;
        check(r.solve(1e6, w2) && std::fabs(w2[1]) < std::fabs(w[1]) * 0.1, "a heavy penalty shrinks the slope towards zero");
        check(std::fabs(r.effective_days() - 50.0) < 1e-9, "without decay every day weighs one");
    }

    // A stack of three inputs: the first carries the move, the others are noise.
    {
        std::mt19937_64 rng(7);
        std::normal_distribution<double> noise(0.0, 60.0);
        std::uniform_real_distribution<double> u(-1.0, 1.0);
        MetaConfig cfg;
        cfg.min_days = 200;
        cfg.score_days = 40;
        MetaModel m(3, cfg);
        int right = 0, called = 0, early = 0;
        for (int day = 0; day < 900; ++day) {
            const std::vector<double> x{u(rng), u(rng), u(rng)};
            const double y = 40.0 * x[0] + noise(rng);
            const MetaCall c = m.predict(x);
            if (day < 200 && c.ready) ++early;
            if (c.ready && day >= 400) {
                ++called;
                right += (c.mu > 0) == (y > 0) ? 1 : 0;
            }
            m.learn(y);
        }
        check(early == 0, "no call is ready before min_days outcomes");
        const auto w = m.weights();
        check(w.size() == 4 && w[1] > 20.0 && std::fabs(w[2]) < 10.0 && std::fabs(w[3]) < 10.0,
              "it trusts the input that carries the move, not the noise");
        const double acc = static_cast<double>(right) / called;
        std::printf("    direction right %.1f %% of %d calls (the signal alone caps it near 63 %%)\n", 100.0 * acc, called);
        check(acc > 0.57, "and calls the direction better than a coin");
    }

    // No lookahead: the day's own outcome does not change the call made for it.
    {
        MetaConfig cfg;
        cfg.min_days = 5;
        cfg.score_days = 1;
        MetaModel a(1, cfg), b(1, cfg);
        for (int d = 0; d < 30; ++d) {
            const std::vector<double> x{(d % 3) - 1.0};
            (void)a.predict(x);
            (void)b.predict(x);
            a.learn(10.0 * x[0]);
            b.learn(10.0 * x[0]);
        }
        const MetaCall ca = a.predict({1.0});
        const MetaCall cb = b.predict({1.0});
        a.learn(+500.0);
        b.learn(-500.0);
        check(ca.ready && ca.mu == cb.mu, "the call is fixed before the outcome: +500 or -500 after it, the same call");
        const MetaCall na = a.predict({1.0});
        const MetaCall nb = b.predict({1.0});
        check(na.mu > nb.mu, "and the outcome moves only the next day's call");
    }

    // Old days fade: a relation that flips is followed.
    {
        MetaConfig cfg;
        cfg.half_life_days = 30;
        cfg.min_days = 50;
        cfg.score_days = 10;
        MetaModel m(1, cfg);
        for (int d = 0; d < 600; ++d) {
            const std::vector<double> x{(d % 5) - 2.0};
            (void)m.predict(x);
            m.learn((d < 300 ? 20.0 : -20.0) * x[0]);
        }
        check(m.predict({1.0}).mu < 0.0, "with a 30-day half-life the flipped relation is the one it follows");
    }

    std::printf("%s\n", failures == 0 ? "all meta-model checks passed" : "meta-model checks did not pass");
    return failures == 0 ? 0 : 1;
}
