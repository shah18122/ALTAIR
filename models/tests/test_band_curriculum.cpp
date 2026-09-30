// Tests for models/band_curriculum.hpp -- range forecasts on the curriculum.

#include <models/band_curriculum.hpp>

#include <cmath>
#include <cstdio>
#include <string>

namespace {

using namespace altair;

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::printf("FAIL: %s\n", what); }
    else { std::printf("  ok  : %s\n", what); }
}

/// Volatility that clusters: r_t = sigma_t * e_t, GARCH(1,1) with
/// persistence 0.95, and a weekday profile (Mondays twice as volatile).
CurriculumTrack garch_track(std::size_t days, std::uint64_t seed = 5) {
    CurriculumTrack tr;
    tr.name = "garch";
    tr.p = 2;
    tr.feature_names = {"|r| last", "noise"};
    tr.seq_cols = {0, 1};
    tr.tradable = false;
    std::uint64_t s = seed;
    const auto gauss = [&s]() {
        double u = 0.0;
        for (int k = 0; k < 12; ++k) {
            s = curriculum_detail::mix(s);
            u += static_cast<double>(s >> 11) * (1.0 / 9007199254740992.0);
        }
        return u - 6.0;
    };
    const double omega = 2e-6, alpha = 0.10, beta = 0.85;
    double v = omega / (1.0 - alpha - beta), last = 0.0, price = 100.0;
    for (std::size_t i = 0; i < days; ++i) {
        const std::uint16_t slot = static_cast<std::uint16_t>(i % 5);
        v = omega + alpha * last * last + beta * v;
        const double e = std::sqrt(v) * gauss();          // the GARCH innovation
        const double r = e * (slot == 0 ? 2.0 : 1.0);     // Mondays scaled outside the recursion
        tr.x.push_back(std::fabs(last));
        tr.x.push_back(gauss());
        tr.t.push_back(static_cast<std::int64_t>(i) * 86'400);
        tr.t_out.push_back(static_cast<std::int64_t>(i + 1) * 86'400);
        tr.day.push_back(static_cast<std::int32_t>(i));
        tr.slot.push_back(static_cast<std::uint16_t>((i + 1) % 5));   // the outcome's weekday
        tr.anchor.push_back(price);
        price *= std::exp(r);
        tr.actual.push_back(price);
        last = e;
    }
    tr.season = 1;
    return tr;
}

std::size_t index_of(const BandRun& run, const char* name) {
    for (std::size_t m = 0; m < run.models.size(); ++m) { if (run.models[m] == name) return m; }
    return run.models.size();
}

void test_scoring() {
    const double a = 0.2;
    check(std::fabs(band_detail::interval_score(0.01, 0.005, a) - 0.02) < 1e-15, "a hit costs only the width");
    check(std::fabs(band_detail::interval_score(0.01, -0.02, a) - (0.02 + 10.0 * 0.01)) < 1e-15,
          "a miss costs the width plus 2/alpha times the excess");
}

void test_bands() {
    const auto tr = garch_track(1600);
    auto models = band_default_models();
    const auto run = band_run(tr, models);
    check(run.has_value(), "every band model runs");
    if (!run) { return; }
    check(run->lookahead_refusals == 0, "no band model reads the future");
    const auto after = run->stages[4].test_begin;   // past the in-sample calibration stages
    const auto tally = [&](const char* name) { return band_tally(tr, *run, index_of(*run, name), after, tr.rows()); };
    const auto flat = tally("Constant sigma (GBM)");
    const auto garch = tally("GARCH(1,1)");
    const auto ewma = tally("EWMA (0.94)");
    const auto ens = tally("Vol ensemble");
    std::printf("        coverage: constant %.3f  GARCH %.3f  EWMA %.3f  ensemble %.3f\n", flat.coverage(),
                garch.coverage(), ewma.coverage(), ens.coverage());
    std::printf("        interval score bp: constant %.1f  GARCH %.1f  EWMA %.1f\n", flat.mean_score_bp(),
                garch.mean_score_bp(), ewma.mean_score_bp());
    check(std::fabs(flat.coverage() - kBandCoverage) < 0.05 && std::fabs(garch.coverage() - kBandCoverage) < 0.05,
          "calibration on past errors holds coverage near 80 %");
    check(garch.mean_score_bp() < flat.mean_score_bp() && ewma.mean_score_bp() < flat.mean_score_bp(),
          "on clustered volatility GARCH and EWMA beat the constant band's interval score");
    check(ens.made > 0 && std::fabs(ens.coverage() - kBandCoverage) < 0.06, "the vol ensemble is calibrated too");
    check(run->notes[index_of(*run, "Best band so far")].back().find("follows") != std::string::npos,
          "the best band so far follows a model with a record");
    // A constant band cannot be narrower where volatility is high; GARCH's is.
    check(garch.hi_made > 0 && flat.hi_made > 0, "the wide-half split is populated");
    const auto seasonal = tally("Seasonal vol (time of day)");
    std::printf("        seasonal: coverage %.3f  score %.1f bp  width %.1f bp (constant %.1f bp)\n",
                seasonal.coverage(), seasonal.mean_score_bp(), seasonal.mean_width_bp(), flat.mean_width_bp());
    check(seasonal.mean_score_bp() < garch.mean_score_bp(),
          "knowing Mondays are twice as volatile beats GARCH, which does not");

    // Changing one outcome moves no band set before it was known.
    auto moved = tr;
    const std::size_t j = run->stages[5].test_begin + 7;
    moved.actual[j] = moved.anchor[j] * 1.2;
    auto models2 = band_default_models();
    const auto run2 = band_run(moved, models2);
    bool same = run2.has_value();
    for (std::size_t m = 0; same && m < run->models.size(); ++m) {
        for (std::size_t i = run->first_row; i <= j; ++i) {
            const float a = run->half[m][i - run->first_row], b = run2->half[m][i - run2->first_row];
            if (!(a == b || (std::isnan(a) && std::isnan(b)))) { same = false; break; }
        }
    }
    check(same, "an outcome cannot change any band set before it was known");
}

} // namespace

int main() {
    std::printf("Band curriculum\n");
    test_scoring();
    test_bands();
    std::printf("Band curriculum: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
