// P33-01 acceptance tests for models/forecast_scorecard.hpp.
//
// THE CONTROL IS THE TEST.
//
// A judge that always answers "no better than naive" is right about almost
// every financial series and is worthless, because it cannot be wrong. So the
// first thing checked here is that it says BETTER on a series a forecaster
// genuinely can forecast, and WORSE on one where the forecast is deliberately
// bad. Only then does "no better" on a random walk mean anything.
//
// This is the same discipline P24-02 used for cointegration -- a pair whose
// answer is known before the test, and a control that must fail -- and the
// same one P21-03's noise control failed to have until it was fixed.
//
// No check description here may contain the substring FAIL.

#include <models/forecast_scorecard.hpp>

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    if (ok) {
        std::printf("  ok  : %s\n", what);
    } else {
        ++failures;
        std::printf("  FAIL: %s\n", what);
    }
}

/// A pure random walk. Nothing can forecast this; the naive forecast is
/// optimal by construction.
std::vector<double> random_walk(std::size_t n, unsigned seed, double vol_bps)
{
    std::mt19937 rng(seed);
    std::normal_distribution<double> z(0.0, vol_bps);
    std::vector<double> c;
    c.reserve(n);
    double p = 20000.0;
    for (std::size_t i = 0; i < n; ++i) {
        p *= std::exp(z(rng) / 10'000.0);
        c.push_back(p);
    }
    return c;
}

/// A series with REAL, learnable structure: strong negative autocorrelation.
/// Every up move is followed by a down move on average, which lagged returns
/// can see and the random walk cannot.
std::vector<double> mean_reverting(std::size_t n, unsigned seed,
                                   double vol_bps, double phi)
{
    std::mt19937 rng(seed);
    std::normal_distribution<double> z(0.0, vol_bps);
    std::vector<double> c;
    c.reserve(n);
    double p = 20000.0, prev = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double r = phi * prev + z(rng);
        p *= std::exp(r / 10'000.0);
        prev = r;
        c.push_back(p);
    }
    return c;
}

} // namespace

using namespace altair;

int main()
{
    std::printf("P33-01 -- is the price forecast any good?\n");

    SpotSpec spec;
    spec.horizon = 1;
    spec.lags = 8;
    spec.folds = 5;
    spec.cost_bps = 0.0;      // a FORECAST is not a trade; no cost here

    BacktestSpec bt;
    bt.warmup = 1500;
    bt.refit_every = 250;
    bt.max_points = 1500;

    // ── 1. the scorer's arithmetic, on numbers checkable by hand ─────────
    std::printf("\n1 the_arithmetic\n");
    {
        std::vector<ForecastPoint> pts;
        // Anchor 100, model says 101, truth is 102. The model is closer than
        // the naive forecast by construction.
        for (int i = 0; i < 400; ++i) {
            ForecastPoint p{};
            p.anchor = 100.0;
            p.predicted = 101.0;
            p.actual = 102.0;
            p.lo = 100.5;
            p.hi = 101.5;
            pts.push_back(p);
        }
        const auto s = score_forecasts(pts);
        check(s.has_value(), "a record scores");
        if (s) {
            std::printf("    model MAE %.2f bps   naive MAE %.2f bps   "
                        "skill %.4f\n",
                        s->mae_model_bps, s->mae_naive_bps, s->skill);
            check(s->mae_model_bps < s->mae_naive_bps,
                  "the model that is closer has the smaller error");
            check(s->skill > 0.0,
                  "and a positive skill score -- skill is 1 - "
                  "rmse_model/rmse_naive, so zero is exactly as good as "
                  "assuming the price does not move");
            check(s->direction_hit > 0.99,
                  "it called the direction right every time: it said up and "
                  "the price went up");
            check(s->coverage < 0.01,
                  "and the band NEVER contained the truth, which the "
                  "coverage number reports rather than hides -- a band that "
                  "covers nothing is not a confidence interval");
            check(s->mean_error_bps < 0.0,
                  "the mean signed error is negative because the forecast "
                  "was systematically BELOW the truth; a bias is fixable and "
                  "an unbiased wide error is not, so they are reported "
                  "separately");
        }
    }

    // ── 2. THE CONTROL THAT MUST SAY BETTER ──────────────────────────────
    //
    // If the judge cannot find structure that is definitely there, its
    // "no better" on real data means nothing.
    std::printf("\n2 a_series_that_IS_forecastable\n");
    {
        const auto closes = mean_reverting(4000, 0xC0FFEEu, 100.0, -0.45);
        const auto pts =
            backtest_forecasts(closes, {}, spec, bt, 100.0, false);
        check(pts.has_value(), "the walk-forward runs");
        if (pts) {
            const auto s = score_forecasts(*pts);
            if (s) {
                const auto v = judge_forecast(*s);
                std::printf("    %zu forecasts   model RMSE %.2f   naive "
                            "%.2f   skill %+.4f   t %+.2f\n",
                            s->n, s->rmse_model_bps, s->rmse_naive_bps,
                            s->skill, s->t_stat);
                std::printf("    VERDICT: %s\n", verdict_text(v));
                check(s->rmse_model_bps < s->rmse_naive_bps,
                      "on a strongly mean-reverting series the model beats "
                      "the random walk on RMSE");
                check(v == ForecastVerdict::BetterThanNaive,
                      "and the judge says BETTER -- which is the check that "
                      "makes every other verdict in this file mean something");
            }
        }
    }

    // ── 3. THE CONTROL THAT MUST NOT ─────────────────────────────────────
    std::printf("\n3 a_random_walk_cannot_be_forecast\n");
    {
        const auto closes = random_walk(4000, 0x5EEDu, 100.0);
        const auto pts =
            backtest_forecasts(closes, {}, spec, bt, 100.0, false);
        check(pts.has_value(), "the walk-forward runs");
        if (pts) {
            const auto s = score_forecasts(*pts);
            if (s) {
                const auto v = judge_forecast(*s);
                std::printf("    %zu forecasts   model RMSE %.2f   naive "
                            "%.2f   skill %+.4f   t %+.2f\n",
                            s->n, s->rmse_model_bps, s->rmse_naive_bps,
                            s->skill, s->t_stat);
                std::printf("    VERDICT: %s\n", verdict_text(v));
                check(v != ForecastVerdict::BetterThanNaive,
                      "the judge does NOT claim skill on a series that has "
                      "none by construction");
            }
        }
    }

    // ── 4. not enough evidence is its own answer ─────────────────────────
    //
    // A t-statistic on forty forecasts is a number, not evidence, and
    // reporting BETTER off one good month is how a model gets deployed on a
    // fluke.
    std::printf("\n4 too_few_forecasts_is_not_a_verdict\n");
    {
        std::vector<ForecastPoint> few;
        for (int i = 0; i < 40; ++i) {
            ForecastPoint p{};
            p.anchor = 100.0;
            p.predicted = 100.5;
            p.actual = 100.6;
            few.push_back(p);
        }
        const auto s = score_forecasts(few);
        check(s.has_value(), "forty forecasts still score");
        if (s) {
            const auto v = judge_forecast(*s);
            std::printf("    n=%zu  t=%+.2f  VERDICT: %s\n",
                        s->n, s->t_stat, verdict_text(v));
            check(v == ForecastVerdict::NotEnoughEvidence,
                  "and the verdict is NOT ENOUGH EVIDENCE regardless of how "
                  "good the t-statistic looks");
        }
        check(static_cast<int>(ForecastVerdict::NotEnoughEvidence) == 0,
              "NotEnoughEvidence is ordinal ZERO, so an uninitialised verdict "
              "cannot read as an endorsement");
    }

    // ── 5. no look-ahead, asserted structurally ──────────────────────────
    //
    // The claim "every prediction used only earlier bars" is not observable
    // from a good score. It IS observable from this: corrupting the future
    // must not change any forecast that was already made.
    std::printf("\n5 the_future_cannot_reach_the_forecast\n");
    {
        auto closes = mean_reverting(3000, 0xABCDu, 100.0, -0.4);
        BacktestSpec small = bt;
        small.max_points = 300;
        const auto a = backtest_forecasts(closes, {}, spec, small, 100.0, false);

        // Now destroy the last 200 bars. Anything fitted only on earlier data
        // must be unaffected.
        auto wrecked = closes;
        for (std::size_t i = wrecked.size() - 200; i < wrecked.size(); ++i) {
            wrecked[i] *= 3.0;
        }
        const auto b = backtest_forecasts(wrecked, {}, spec, small, 100.0,
                                          false);
        check(a.has_value() && b.has_value(), "both runs complete");
        if (a && b && a->size() == b->size()) {
            std::size_t changed = 0;
            const std::size_t safe = a->size() > 250 ? a->size() - 250 : 0;
            for (std::size_t i = 0; i < safe; ++i) {
                if (std::fabs((*a)[i].predicted - (*b)[i].predicted) > 1e-9) {
                    ++changed;
                }
            }
            std::printf("    %zu of the first %zu forecasts moved when the "
                        "last 200 bars were tripled\n", changed, safe);
            check(changed == 0,
                  "tripling the END of the series changes NO forecast made "
                  "before it -- which is what 'no look-ahead' means and is "
                  "not something a good score can demonstrate");
        }
    }

    std::printf("\n%s -- %d failing check(s)\n",
                failures == 0 ? "PASS" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
