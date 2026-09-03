// desktop/data/fits.hpp -- fitting the models the available data can actually
// fit, on that data.
//
// P11Q-07.
//
// TWO MODELS OF THIRTEEN, AND THE PANEL SAYS WHICH TWO.
//
// The Models page lists thirteen models and reports eleven as trainable only
// on synthetic data. That is a claim, and a claim on a dashboard should be
// something the program can demonstrate rather than a string somebody typed.
//
// This module fits the two that CAN be fitted from `dataset/` -- the Markov
// regime chain (P8-13) on 8,755 daily NIFTY returns, and the India VIX AR(1)
// (P10-07) on ~3,000 India VIX bars -- and returns the numbers. Everything
// else stays a string, because everything else has no data.
//
// THE VIX FIT IS A CHECK ON A SYNTHETIC FINDING.
//
// P10-07 measured, on synthetic data, that a LEVEL-space VIX model carries one
// residual standard deviation across regimes where a LOG-space one scales with
// the level -- and that the usual complaint about level models forecasting a
// negative VIX is false. Both of those were measured where the truth was known
// by construction.
//
// India VIX is real, and this refits both spaces on it. Whether the synthetic
// finding survives contact with the real series is exactly the kind of thing
// that should be discovered by running it rather than assumed because the test
// passed. `VixFit` carries both spaces' residual spread by regime so the two
// can be compared on screen.
//
// NOTHING HERE IS A FORECAST ANYBODY SHOULD TRADE.
//
// A fit on the whole history is an in-sample fit. It says the model can be
// estimated and what the parameters look like; it says nothing about
// out-of-sample behaviour, and the walk-forward split that would is P11Q-08.
// `FitResult::in_sample` is true on everything this module produces and the
// panel renders it, so the distinction is on screen rather than in a comment.

#pragma once

#include "bar_csv.hpp"

#include <models/markov.hpp>
#include <strategies/vix_forecast.hpp>

#include <QString>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace altair::ui {

struct MarkovFit {
    bool ok = false;
    QString error;
    std::size_t returns = 0;
    std::size_t transitions = 0;
    std::size_t states = 0;
    std::uint32_t thinnest_cell = 0;
    std::size_t empty_cells = 0;
    /// How many state labels change when the quantile boundaries come from the
    /// past only. The look-ahead, as a count.
    std::size_t relabelled = 0;
    double relabelled_pct = 0.0;
    double chi_square = 0.0;
    double chi_square_shuffled = 0.0;
    double critical_5pct = 0.0;
    bool rejects = false;
    bool shuffled_rejects = false;
    std::vector<double> stationary_dist;
};

/// Fit the Markov chain on a loaded bar series.
///
/// `warmup` is how many returns accumulate before any state is assigned. It is
/// an argument rather than a constant because it is a modelling choice: too
/// short and the early boundaries are noise, too long and the fit throws away
/// history.
[[nodiscard]] inline MarkovFit
fit_markov(const std::vector<Candle>& bars, std::size_t states = 5,
           std::size_t warmup = 500) {
    MarkovFit f;
    if (bars.size() < warmup + states * 4) {
        f.error = QStringLiteral("need at least %1 bars, have %2")
                      .arg(warmup + states * 4)
                      .arg(bars.size());
        return f;
    }
    std::vector<double> closes;
    closes.reserve(bars.size());
    for (const Candle& c : bars) {
        closes.push_back(static_cast<double>(c.close));
    }
    const std::vector<double> r = log_returns(closes);
    f.returns = r.size();
    f.states = states;

    const auto honest = fit_expanding(r, states, warmup);
    if (!honest) {
        f.error = QStringLiteral("expanding fit failed");
        return f;
    }
    f.transitions = honest->transitions;
    f.thinnest_cell = honest->min_count();
    f.empty_cells = honest->empty_cells();
    f.stationary_dist = stationary(*honest);

    // The look-ahead, measured rather than described.
    std::vector<double> all = r;
    const auto full = quantile_boundaries(all, states);
    const auto seq = expanding_states(r, states, warmup);
    if (full && seq) {
        for (std::size_t i = 0; i < seq->size(); ++i) {
            if (full->classify(r[warmup + i]) != (*seq)[i]) {
                ++f.relabelled;
            }
        }
        f.relabelled_pct = seq->empty()
                             ? 0.0
                             : 100.0 * static_cast<double>(f.relabelled)
                                   / static_cast<double>(seq->size());
    }

    const IndependenceTest t = independence_chi_square(*honest);
    f.chi_square = t.chi_square;
    f.critical_5pct = t.critical_5pct;
    f.rejects = t.rejects_independence;

    // THE CONTROL. Same marginal distribution, no temporal structure. Without
    // it a large chi-square is just a large sample.
    std::vector<double> shuffled = r;
    std::uint64_t rng = 0x9E3779B97F4A7C15ULL;
    for (std::size_t i = shuffled.size(); i > 1; --i) {
        rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
        const std::size_t j = static_cast<std::size_t>((rng >> 33) % i);
        std::swap(shuffled[i - 1], shuffled[j]);
    }
    if (const auto ctrl = fit_expanding(shuffled, states, warmup)) {
        const IndependenceTest c = independence_chi_square(*ctrl);
        f.chi_square_shuffled = c.chi_square;
        f.shuffled_rejects = c.rejects_independence;
    }

    f.ok = true;
    return f;
}

// ---------------------------------------------------------------------------
// India VIX
// ---------------------------------------------------------------------------

struct VixFit {
    bool ok = false;
    QString error;
    std::size_t observations = 0;
    double level_a = 0.0, level_b = 0.0, level_sd = 0.0, level_half_life = 0.0;
    double log_a = 0.0, log_b = 0.0, log_sd = 0.0, log_half_life = 0.0;
    /// Residual spread of the LEVEL model in each regime. P10-07's finding was
    /// that one fitted sd covers both badly; this is the same measurement on
    /// the real series.
    double level_sd_quiet = 0.0;
    double level_sd_stressed = 0.0;
    std::size_t quiet_n = 0;
    std::size_t stressed_n = 0;
    double quiet_threshold = 0.0;
    double stressed_threshold = 0.0;
    /// Coverage of a nominal 95.4% two-sigma band, by regime and space.
    double level_cover_quiet = 0.0, level_cover_stressed = 0.0;
    double log_cover_quiet = 0.0, log_cover_stressed = 0.0;
};

/// Fit the India VIX AR(1) in both spaces and compare their intervals.
///
/// The thresholds are QUANTILES of the series rather than the 15 and 22 that
/// P10-07's synthetic test used. Hard-coded levels are a claim about where
/// "quiet" and "stressed" sit, and India VIX does not sit where a synthetic
/// series does -- using them would measure the fixture rather than the market.
[[nodiscard]] inline VixFit fit_vix_both_spaces(const std::vector<Candle>& bars,
                                                double quiet_q = 0.25,
                                                double stressed_q = 0.75) {
    VixFit f;
    if (bars.size() < 200) {
        f.error = QStringLiteral("need at least 200 bars, have %1")
                      .arg(bars.size());
        return f;
    }
    std::vector<double> vix;
    vix.reserve(bars.size());
    for (const Candle& c : bars) {
        // Paise back to index points. VIX is quoted in volatility points, and
        // the loader stored them scaled by 100 like any other price.
        const double v = static_cast<double>(c.close) / 100.0;
        if (v > 0.0) {
            vix.push_back(v);
        }
    }
    f.observations = vix.size();
    if (vix.size() < 200) {
        f.error = QStringLiteral("too few positive observations");
        return f;
    }

    const auto level = fit_vix(vix.data(), vix.size(), VixSpace::Level);
    const auto logm = fit_vix(vix.data(), vix.size(), VixSpace::Log);
    if (!level || !logm) {
        f.error = QStringLiteral("fit failed");
        return f;
    }
    f.level_a = level->a; f.level_b = level->b;
    f.level_sd = level->residual_sd; f.level_half_life = level->half_life;
    f.log_a = logm->a; f.log_b = logm->b;
    f.log_sd = logm->residual_sd; f.log_half_life = logm->half_life;

    std::vector<double> sorted = vix;
    std::sort(sorted.begin(), sorted.end());
    f.quiet_threshold =
        sorted[static_cast<std::size_t>(quiet_q * (sorted.size() - 1))];
    f.stressed_threshold =
        sorted[static_cast<std::size_t>(stressed_q * (sorted.size() - 1))];

    // Residual spread by regime, LEVEL space.
    double q_sum = 0.0, s_sum = 0.0;
    for (std::size_t i = 0; i + 1 < vix.size(); ++i) {
        const double e = vix[i + 1] - level->a - level->b * vix[i];
        if (vix[i] <= f.quiet_threshold) {
            q_sum += e * e;
            ++f.quiet_n;
        } else if (vix[i] >= f.stressed_threshold) {
            s_sum += e * e;
            ++f.stressed_n;
        }
    }
    f.level_sd_quiet = f.quiet_n > 0 ? std::sqrt(q_sum / f.quiet_n) : 0.0;
    f.level_sd_stressed =
        f.stressed_n > 0 ? std::sqrt(s_sum / f.stressed_n) : 0.0;

    // Coverage of a nominal two-sigma band, both spaces, both regimes.
    const auto cover = [&](const VixModel& m, bool quiet) {
        std::size_t inside = 0, total = 0;
        for (std::size_t i = 0; i + 1 < vix.size(); ++i) {
            const bool in_regime = quiet ? vix[i] <= f.quiet_threshold
                                         : vix[i] >= f.stressed_threshold;
            if (!in_regime) {
                continue;
            }
            const auto fc = forecast_vix(m, vix[i], 1);
            if (!fc) {
                continue;
            }
            double lo = 0.0, hi = 0.0;
            if (m.space == VixSpace::Log) {
                const double lf = std::log(*fc);
                lo = std::exp(lf - 2.0 * m.residual_sd);
                hi = std::exp(lf + 2.0 * m.residual_sd);
            } else {
                lo = *fc - 2.0 * m.residual_sd;
                hi = *fc + 2.0 * m.residual_sd;
            }
            if (vix[i + 1] >= lo && vix[i + 1] <= hi) {
                ++inside;
            }
            ++total;
        }
        return total > 0 ? 100.0 * static_cast<double>(inside)
                             / static_cast<double>(total)
                         : 0.0;
    };
    f.level_cover_quiet = cover(*level, true);
    f.level_cover_stressed = cover(*level, false);
    f.log_cover_quiet = cover(*logm, true);
    f.log_cover_stressed = cover(*logm, false);

    f.ok = true;
    return f;
}

} // namespace altair::ui
