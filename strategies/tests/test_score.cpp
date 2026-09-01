// P6-04 acceptance tests for strategies/score.hpp.
//
// Test 1 is the card: ten correlated signals are not ten signals. The test
// builds a real multi-timeframe indicator stack, MEASURES the correlation
// between its members rather than assuming one, and shows what the naive
// independent-signals standard error claims by comparison.
//
// Test 2: size on the lower bound, and a score whose interval straddles zero
// is not a signal.
//
// Test 3: an incomplete regime does not fall back to the unconditioned model.
//
// No check description here may contain the substring FAIL.

#include <strategies/score.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>

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

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
double rupees(std::int64_t p) { return static_cast<double>(p) / 100.0; }

struct Lcg {
    std::uint64_t s;
    double uniform()
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>((s >> 11) & ((1ULL << 53) - 1))
               / static_cast<double>(1ULL << 53);
    }
    double normal()
    {
        const double u1 = uniform();
        const double u2 = uniform();
        return std::sqrt(-2.0 * std::log(u1 + 1e-300))
               * std::cos(6.283185307179586 * u2);
    }
};

} // namespace

using namespace altair;

namespace {

MarketRegime decided_regime()
{
    MarketRegime m{};
    m.trend = TrendRegime::Trending;
    m.vol = VolRegime::Normal;
    m.liquidity = LiquidityRegime::Deep;
    return m;
}

RegimeWeights equal_weights(std::size_t n, double rho)
{
    RegimeWeights w{};
    w.n = n;
    for (std::size_t i = 0; i < n; ++i) { w.w[i] = 1.0; }
    w.mean_correlation = rho;
    w.paise_per_score = 4000.0;     // Rs 40 of expected move per unit score
    w.confidence_k = 1.0;
    return w;
}

constexpr std::size_t kLen = 4000;
constexpr std::size_t kSignals = 10;
double sig_series[kSignals][kLen];

// ── 1 ────────────────────────────────────────────────────────────────────
// THE card.
void ten_correlated_signals_are_not_ten_signals()
{
    std::printf("\n1 ten_correlated_signals_are_not_ten_signals\n");
    // A real multi-timeframe momentum stack: EMAs of the SAME price series at
    // ten neighbouring scales, each expressed as (price - ema) / scale. This
    // is what a typical indicator panel actually is, and its members share
    // their data by construction.
    Lcg g{0x5C02E};
    const int periods[kSignals] = {5, 8, 12, 18, 26, 38, 55, 80, 115, 165};
    Ema emas[kSignals] = {Ema{5},  Ema{8},   Ema{12},  Ema{18}, Ema{26},
                          Ema{38}, Ema{55},  Ema{80},  Ema{115}, Ema{165}};
    double p = 2'400'000.0;
    for (std::size_t t = 0; t < kLen; ++t) {
        p += g.normal() * 2000.0;
        for (std::size_t i = 0; i < kSignals; ++i) {
            const double e = emas[i].update(p);
            sig_series[i][t] = (p - e) / (2000.0 * std::sqrt(
                static_cast<double>(periods[i])));
        }
    }

    const double* ptrs[kSignals];
    for (std::size_t i = 0; i < kSignals; ++i) { ptrs[i] = sig_series[i]; }
    const auto rho = mean_pairwise_correlation(ptrs, kSignals, kLen);
    check(rho.has_value(), "the correlation among the stack is measurable");
    if (!rho) { return; }

    std::printf("    a ten-member EMA momentum stack, periods 5 to 165, on ONE"
                " price series:\n"
                "      MEASURED mean pairwise correlation   %.3f\n"
                "      effective independent signals        %.2f  (of %zu)\n"
                "      naive standard error understates by  %.2fx\n",
                *rho, effective_signals(kSignals, *rho), kSignals,
                correlation_inflation(kSignals, *rho));

    check(*rho > 0.4,
          "the members of an ordinary multi-timeframe stack are strongly"
          " correlated -- they are not ten views of the market, they are one"
          " series filtered ten times");
    check(effective_signals(kSignals, *rho) < 3.0,
          "so their effective number is under three, not ten");

    // What that does to a score.
    Signal s[kSignals];
    for (std::size_t i = 0; i < kSignals; ++i) {
        s[i].z = sig_series[i][kLen - 1];
        s[i].std_error = 1.0;
        s[i].ready = true;
    }
    const auto honest = combine(s, kSignals, equal_weights(kSignals, *rho),
                                decided_regime(), Notional{800});
    const auto naive = combine_assuming_independence(
        s, kSignals, equal_weights(kSignals, *rho), decided_regime(),
        Notional{800});
    check(honest.has_value() && naive.has_value(), "both scores compute");
    if (!honest || !naive) { return; }

    std::printf("      score %.3f:\n"
                "        naive  standard error %.4f -> %.2f sigma\n"
                "        honest standard error %.4f -> %.2f sigma\n",
                honest->score, naive->std_error,
                std::fabs(honest->score) / naive->std_error,
                honest->std_error,
                std::fabs(honest->score) / honest->std_error);

    check(honest->std_error > 2.0 * naive->std_error,
          "the correlation-aware standard error is more than twice the naive"
          " one on this stack");
    check(near(honest->std_error,
               naive->std_error * correlation_inflation(kSignals, *rho), 1e-9),
          "and it is exactly the naive one times sqrt(1 + (n-1)rho), which is"
          " the closed form and not an approximation");
    std::printf("    -> that factor is the difference between a 1.5-sigma"
                " non-signal and a 4-sigma\n       conviction trade. The trade"
                " is sized on the conviction, so an engine that\n       assumes"
                " independence does not merely misreport -- it sizes on the"
                " misreport.\n");

    check(near(effective_signals(kSignals, *rho),
               static_cast<double>(kSignals)
                   / (1.0 + (static_cast<double>(kSignals) - 1.0) * *rho),
               1e-12),
          "n_eff matches n / (1 + (n-1)rho) exactly");
    check(near(effective_signals(4, 0.0), 4.0, 1e-12)
          && near(correlation_inflation(4, 0.0), 1.0, 1e-12),
          "and at zero correlation both reduce to the independent case, so the"
          " correction is a generalisation rather than a fudge");
}

// ── 2 ────────────────────────────────────────────────────────────────────
void a_score_whose_interval_straddles_zero_is_not_a_signal()
{
    std::printf("\n2 a_score_whose_interval_straddles_zero_is_not_a_signal\n");
    const MarketRegime reg = decided_regime();
    const Notional cost{800};           // Rs 8 round trip

    // A modest score with a wide error bar: the point estimate clears the
    // cost, the lower bound does not.
    Signal s[4];
    for (auto& x : s) { x.z = 0.6; x.std_error = 1.0; x.ready = true; }
    RegimeWeights w = equal_weights(4, 0.6);
    w.confidence_k = 1.0;

    const auto r = combine(s, 4, w, reg, cost);
    check(r.has_value(), "the score computes");
    if (!r) { return; }
    std::printf("    score %.2f, %.2f effective signals of 4:\n"
                "      gross expected move   Rs %7.2f\n"
                "      cost                  Rs %7.2f\n"
                "      expected NET          Rs %7.2f   <- point estimate\n"
                "      lower bound (k = 1)   Rs %7.2f   <- what a size uses\n"
                "      fires: %s\n",
                r->score, r->n_effective,
                std::fabs(r->score) * w.paise_per_score / 100.0,
                rupees(cost.raw()), rupees(r->expected_net.raw()),
                rupees(r->lower_bound.raw()), r->fires() ? "yes" : "no");

    check(r->expected_net.raw() > 0,
          "the point estimate of net edge is positive -- an engine reporting"
          " the point estimate would take this trade");
    check(r->lower_bound.raw() < r->expected_net.raw(),
          "the lower bound is below it by k standard errors");
    check(!r->fires(),
          "and the score does NOT fire, because its interval straddles zero:"
          " ROADMAP section 3 sizes on the lower bound, and a signal whose"
          " error bar contains zero is not a signal");
    check(r->direction() == 0,
          "so it reports no direction either -- direction is only meaningful"
          " on a score that fires");

    // Tighten the signals and it fires.
    for (auto& x : s) { x.std_error = 0.15; }
    const auto tight = combine(s, 4, w, reg, cost);
    check(tight.has_value() && tight->fires() && tight->direction() == 1,
          "the same score with tighter inputs fires long, so the bound is a"
          " discriminator and not a blanket refusal");

    // Cost alone can kill it (rule 5).
    const auto expensive = combine(s, 4, w, reg, Notional{300'000});
    check(expensive.has_value() && !expensive->fires()
          && expensive->expected_net.raw() < 0,
          "and a large enough cost turns the same signal negative before it is"
          " ever seen -- there is no gross field on Score, so nothing"
          " downstream can read the pre-cost number by mistake");

    // A short is symmetric.
    for (auto& x : s) { x.z = -0.6; x.std_error = 0.15; }
    const auto shorted = combine(s, 4, w, reg, cost);
    check(shorted.has_value() && shorted->fires()
          && shorted->direction() == -1,
          "a mirror-image negative score fires short");
    check(shorted->expected_net.raw() == tight->expected_net.raw(),
          "with exactly the same net edge -- the edge fields are MAGNITUDES"
          " and direction() carries the sign, which is what stops a large"
          " negative score reading as a large positive edge");
}

// ── 3 ────────────────────────────────────────────────────────────────────
void an_incomplete_regime_does_not_fall_back()
{
    std::printf("\n3 an_incomplete_regime_does_not_fall_back\n");
    Signal s[4];
    for (auto& x : s) { x.z = 1.2; x.std_error = 0.1; x.ready = true; }
    const RegimeWeights w = equal_weights(4, 0.5);

    MarketRegime partial{};
    partial.trend = TrendRegime::Trending;
    partial.vol = VolRegime::Normal;
    // liquidity left Unknown
    const auto r = combine(s, 4, w, partial, Notional{800});
    check(!r && r.error() == ScoreError::RegimeIncomplete,
          "two axes of three decided is REFUSED -- a model conditioned on"
          " regime and one that is not are different models trained on"
          " different samples, so falling back would report one as the other");

    partial.liquidity = LiquidityRegime::Deep;
    check(combine(s, 4, w, partial, Notional{800}).has_value(),
          "and with all three decided it computes");

    // The correlation and the scale have no defaults.
    RegimeWeights bare = w;
    bare.mean_correlation = -1.0;
    check(combine(s, 4, bare, partial, Notional{800}).error()
          == ScoreError::NoCorrelation,
          "a caller who did not supply the correlation is refused: assuming"
          " zero is the bug this file exists around, and assuming anything"
          " else is a different unfounded guess");
    RegimeWeights unscaled = w;
    unscaled.paise_per_score = 0.0;
    check(combine(s, 4, unscaled, partial, Notional{800}).error()
          == ScoreError::NoScale,
          "and without a calibrated score-to-paise scale the cost cannot be"
          " subtracted at all, so rule 5 cannot be satisfied and the engine"
          " says so rather than skipping it");

    // Unready signals are excluded, not zeroed.
    Signal mixed[4];
    mixed[0] = Signal{1.2, 0.1, true};
    mixed[1] = Signal{1.2, 0.1, true};
    mixed[2] = Signal{0.0, 1.0, false};
    mixed[3] = Signal{0.0, 1.0, false};
    const auto m = combine(mixed, 4, w, partial, Notional{800});
    check(m.has_value() && m->n_signals == 2,
          "two ready signals of four are counted as two");
    check(near(m->score, 1.2, 1e-12),
          "and the score is the mean of the READY ones -- treating an unready"
          " signal as a zero would treat 'no opinion' as 'neutral opinion',"
          " and a neutral opinion drags the score toward zero exactly as a real"
          " one would");

    Signal none[4] = {};
    check(combine(none, 4, w, partial, Notional{800}).error()
          == ScoreError::NoSignals,
          "with nothing ready there is no score, rather than a score of zero");
}

} // namespace

int main()
{
    std::printf("altair score engine tests\n");
    ten_correlated_signals_are_not_ten_signals();
    a_score_whose_interval_straddles_zero_is_not_a_signal();
    an_incomplete_regime_does_not_fall_back();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
