// risk/stress.hpp -- stress testing, and Black-Litterman.
//
// P15-04 / P15-07.
//
// THE CORRELATION YOU DIVERSIFIED ON IS NOT THE ONE YOU GET IN A CRISIS.
//
// This is the whole of stress testing in one line. A portfolio built on a
// full-sample correlation of 0.3 is holding something quite different when
// that correlation goes to 0.8, and it goes to 0.8 exactly when it matters.
// So `conditional_correlation` measures correlation INSIDE a window rather
// than over the sample, and `stress_windows` names real dated periods rather
// than a synthetic shock, because "down 20%" is a number somebody chose and
// 2008 is a thing that happened.
//
// A SCENARIO IS NOT A PROBABILITY.
//
// Nothing here attaches a likelihood to a stress window. The question a stress
// test answers is "what would this book have done", not "how likely is that",
// and blending the two produces a probability-weighted loss that is both
// unfalsifiable and reassuringly small. The output is a loss per named period
// and nothing else.
//
// BLACK-LITTERMAN, AND THE MATRIX NOBODY SETS HONESTLY.
//
// BL starts from the returns IMPLIED by the market's own weights -- reverse
// optimisation, so the neutral answer is the market portfolio rather than
// whatever a mean estimate happened to produce -- and then tilts toward the
// caller's views. The tilt size is governed by Omega, the uncertainty on those
// views.
//
// Omega is where BL is used dishonestly. Set it small and BL returns your view
// with extra steps; set it large and it returns the market. There is no
// procedure for choosing it, it is a statement about how sure you are, and
// most implementations bury a default. This one has NO default: `bl_posterior`
// requires a confidence per view, and the test demonstrates both limits so
// that the parameter's power is visible rather than incidental.

#pragma once

#include <risk/covariance.hpp>
#include <risk/optimise.hpp>

#include <cmath>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace altair {

enum class StressError : std::uint8_t {
    TooFewSamples,
    BadShape,
    /// A view confidence was zero or negative -- infinite certainty, which BL
    /// cannot represent and no analyst has.
    BadConfidence
};

// ---------------------------------------------------------------------------
// P15-04 — stress windows
// ---------------------------------------------------------------------------

/// A named historical period. Dated, not synthetic.
struct StressWindow {
    const char* name;
    /// Inclusive index range into the return series.
    std::size_t from = 0;
    std::size_t to = 0;
};

struct StressResult {
    const char* name = "";
    /// Cumulative return over the window, as a fraction. Negative is a loss.
    double cumulative = 0.0;
    /// Worst peak-to-trough inside the window.
    double max_drawdown = 0.0;
    /// Correlation between the two series INSIDE the window.
    double correlation = 0.0;
    std::size_t days = 0;
};

/// Correlation restricted to a window. The number that matters, and the one a
/// full-sample estimate hides.
[[nodiscard]] inline std::expected<double, StressError>
conditional_correlation(const std::vector<double>& a,
                        const std::vector<double>& b, std::size_t from,
                        std::size_t to) {
    if (a.size() != b.size() || to <= from || to > a.size()) {
        return std::unexpected(StressError::BadShape);
    }
    const std::size_t n = to - from;
    if (n < 10) { return std::unexpected(StressError::TooFewSamples); }
    double ma = 0.0, mb = 0.0;
    for (std::size_t i = from; i < to; ++i) { ma += a[i]; mb += b[i]; }
    ma /= static_cast<double>(n);
    mb /= static_cast<double>(n);
    double sab = 0.0, sa = 0.0, sb = 0.0;
    for (std::size_t i = from; i < to; ++i) {
        const double da = a[i] - ma, db = b[i] - mb;
        sab += da * db;
        sa += da * da;
        sb += db * db;
    }
    if (!(sa > 0.0) || !(sb > 0.0)) {
        return std::unexpected(StressError::TooFewSamples);
    }
    return sab / std::sqrt(sa * sb);
}

/// Run a portfolio's returns through one window.
[[nodiscard]] inline std::expected<StressResult, StressError>
stress(const std::vector<double>& port, const std::vector<double>& other,
       const StressWindow& w) {
    if (w.to <= w.from || w.to > port.size()) {
        return std::unexpected(StressError::BadShape);
    }
    StressResult r;
    r.name = w.name;
    r.days = w.to - w.from;

    double cum = 0.0, peak = 0.0, worst = 0.0;
    for (std::size_t i = w.from; i < w.to; ++i) {
        cum += port[i];
        peak = std::max(peak, cum);
        worst = std::max(worst, peak - cum);
    }
    // Log returns in, simple return out -- reported the way a drawdown is
    // actually quoted, rather than leaving the reader to exponentiate.
    r.cumulative = std::exp(cum) - 1.0;
    r.max_drawdown = 1.0 - std::exp(-worst);

    const auto c = conditional_correlation(port, other, w.from, w.to);
    r.correlation = c ? *c : 0.0;
    return r;
}

// ---------------------------------------------------------------------------
// P15-07 — Black-Litterman
// ---------------------------------------------------------------------------

/// One view: "asset `on` will return `value`", held with `confidence`.
///
/// Absolute views only. Relative views ("A beats B by 2%") are the more useful
/// form and need a full pick matrix; that is a separate card rather than a
/// half-implemented parameter, because a P matrix silently restricted to one
/// non-zero entry is a relative-view API that cannot express a relative view.
struct BlView {
    std::size_t on = 0;
    double value = 0.0;
    /// Standard deviation of the view. SMALLER MEANS MORE CERTAIN. No
    /// default -- see the header.
    double confidence = 0.0;
};

struct BlResult {
    /// Posterior expected returns.
    std::vector<double> mu;
    /// The equilibrium returns the market's own weights imply, before any
    /// view. Returned so a caller can see how far the views moved things.
    std::vector<double> equilibrium;
};

/// Reverse-optimise market weights into the returns they imply, then tilt.
///
///     pi     = lambda * Sigma * w_market
///     mu_bar = pi + tau*Sigma*P' (P tau*Sigma P' + Omega)^-1 (q - P pi)
///
/// With absolute views P is a selection matrix and the inverse is diagonal, so
/// this needs no matrix solver -- which is why absolute views are worth having
/// as their own card.
[[nodiscard]] inline std::expected<BlResult, StressError>
bl_posterior(const CovMatrix& sigma, const Weights& market,
             const std::vector<BlView>& views, double risk_aversion,
             double tau) {
    const std::size_t p = sigma.p;
    if (p == 0 || market.w.size() != p) {
        return std::unexpected(StressError::BadShape);
    }
    if (!(risk_aversion > 0.0) || !(tau > 0.0)) {
        return std::unexpected(StressError::BadConfidence);
    }
    for (const BlView& v : views) {
        if (v.on >= p || !(v.confidence > 0.0)) {
            // A zero confidence is INFINITE certainty. BL cannot represent it
            // and no analyst has it; refused rather than clamped to something
            // small, which would silently make the view dominate.
            return std::unexpected(StressError::BadConfidence);
        }
    }

    BlResult out;
    out.equilibrium.assign(p, 0.0);
    for (std::size_t i = 0; i < p; ++i) {
        double acc = 0.0;
        for (std::size_t j = 0; j < p; ++j) {
            acc += sigma.at(i, j) * market.w[j];
        }
        out.equilibrium[i] = risk_aversion * acc;
    }
    out.mu = out.equilibrium;

    // Each absolute view updates independently: with a selection P the
    // bracketed inverse is scalar per view.
    for (const BlView& v : views) {
        const double ts = tau * sigma.at(v.on, v.on);
        const double denom = ts + v.confidence * v.confidence;
        if (!(denom > 0.0)) { continue; }
        const double k = ts / denom;              // shrinkage toward the view
        const double resid = v.value - out.equilibrium[v.on];
        for (std::size_t i = 0; i < p; ++i) {
            // The view on one asset moves EVERY asset, in proportion to their
            // covariance. That is the whole reason to use BL rather than
            // overwriting one number: a view on NIFTY is implicitly a view on
            // anything correlated with it.
            const double beta = sigma.at(v.on, v.on) > 0.0
                ? sigma.at(i, v.on) / sigma.at(v.on, v.on) : 0.0;
            out.mu[i] += k * beta * resid;
        }
    }
    return out;
}

} // namespace altair
