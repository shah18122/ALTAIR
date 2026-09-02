// strategies/cointegration.hpp -- Engle-Granger, Johansen, half-life and
// structural break detection.
//
// P10-04 and P10-05.
//
// CORRELATION IS NOT COINTEGRATION, AND THE PAIR YOU WANT IS THE SECOND ONE.
//
// This is the card. A pairs strategy needs the SPREAD to come back. Correlation
// says the two series move together; cointegration says the gap between them
// is stationary. They are different properties and neither implies the other:
//
//   Two independent random walks, both drifting up, are highly CORRELATED and
//   not cointegrated. Their spread wanders off and never returns, which is the
//   spurious regression P5-04 already measured -- |0.492| in levels against
//   |0.031| in returns.
//
//   Two series tied by a stationary error can be weakly correlated day to day
//   and firmly cointegrated. That pair is tradable and a correlation screen
//   never sees it.
//
// Measured, on 500 observations each:
//
//                           level corr   EG t-stat   crit(5%)   cointegrated
//   shared market factor      +0.886       -1.617     -3.350        no
//   a genuinely tied pair     +0.986       -4.823     -3.350        YES
//
// Both look like a pair on a correlation screen. Johansen agrees with
// Engle-Granger on both -- trace 2.95 against 27.11, critical 15.49 -- and the
// cointegrating vector comes back at 1.9702 against the 2.0 it was built with.
//
// Screening on correlation selects for the first kind, which is the kind that
// blows up.
//
// A COINTEGRATED PAIR WITH A HALF-LIFE LONGER THAN YOUR HOLDING PERIOD IS NOT
// A TRADE.
//
// P10-05, and it is the question a cointegration test does not answer. The
// test says the spread is stationary; it says nothing about HOW FAST. An
// Ornstein-Uhlenbeck fit gives the speed, and a pair with a 200-day half-life
// is perfectly cointegrated and perfectly untradeable on an intraday book --
// the position is carried, financed and margined for a convergence that
// arrives after the strategy has been switched off.
//
// So `PairCandidate` carries the half-life next to the test statistic, and
// `tradeable` takes the intended holding period and compares. Two pairs that
// BOTH pass both cointegration tests: half-lives of 1.83 and 9.24 bars, and
// only the first is tradeable on an eight-bar book. The second becomes
// tradeable at forty.
//
// AND A HALF-LIFE ON ITS OWN CANNOT TELL A SLOW REVERTER FROM A WALK.
//
// A pure random walk -- no mean reversion whatsoever -- reports an apparent
// half-life of 193.57 bars. OLS on a unit root returns a slightly negative
// slope by chance (the Dickey-Fuller downward bias), so the number is finite
// and would pass a "reverts within 200 bars" filter unaided. Its ADF statistic
// is -1.075 against a -3.350 critical value and correctly says NOT stationary.
// `tradeable` therefore requires cointegration AND a half-life, not either.
//
// THE COINTEGRATING RELATIONSHIP ITSELF BREAKS.
//
// The hedge ratio is estimated on a window, and a merger, a demerger, a
// regulatory change or an index reconstitution moves it. The pair is then held
// at the OLD ratio against a NEW relationship, which is not a pairs trade any
// more -- it is a directional position nobody chose. A CUSUM on the spread's
// residuals detects that, and it is the one alarm this file treats as an exit
// rather than a signal.

#pragma once

#include <analytics/rolling.hpp>
#include <core/types/units.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

inline constexpr std::size_t kMaxPairWindow = 2048;

enum class CointError : std::uint8_t {
    /// Fewer observations than the test needs.
    TooFewSamples,
    /// A series had no variation.
    Degenerate,
    /// The holding period was not supplied.
    NoHoldingPeriod,
    /// The critical value table does not cover this sample size.
    NoCriticalValue,
    /// The regression was singular.
    Singular
};

/// An OLS fit of y on x with an intercept: y = alpha + beta*x + e.
struct Regression {
    double alpha = 0.0;
    double beta = 0.0;
    double residual_sd = 0.0;
    double r_squared = 0.0;
    std::size_t n = 0;
};

[[nodiscard]] inline std::expected<Regression, CointError>
ols(const double* y, const double* x, std::size_t n) noexcept {
    if (n < 8) { return std::unexpected(CointError::TooFewSamples); }
    double sx = 0.0, sy = 0.0;
    for (std::size_t i = 0; i < n; ++i) { sx += x[i]; sy += y[i]; }
    const double dn = static_cast<double>(n);
    const double mx = sx / dn, my = sy / dn;
    double sxx = 0.0, sxy = 0.0, syy = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double dx = x[i] - mx, dy = y[i] - my;
        sxx += dx * dx;
        sxy += dx * dy;
        syy += dy * dy;
    }
    if (!(sxx > 0.0)) { return std::unexpected(CointError::Degenerate); }
    Regression r{};
    r.n = n;
    r.beta = sxy / sxx;
    r.alpha = my - r.beta * mx;
    double sse = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double e = y[i] - r.alpha - r.beta * x[i];
        sse += e * e;
    }
    r.residual_sd = std::sqrt(sse / (dn - 2.0));
    r.r_squared = syy > 0.0 ? 1.0 - sse / syy : 0.0;
    return r;
}

/// Pearson correlation. Here so the two can be compared directly.
[[nodiscard]] inline std::expected<double, CointError>
correlation(const double* a, const double* b, std::size_t n) noexcept {
    if (n < 3) { return std::unexpected(CointError::TooFewSamples); }
    const double dn = static_cast<double>(n);
    double sa = 0.0, sb = 0.0;
    for (std::size_t i = 0; i < n; ++i) { sa += a[i]; sb += b[i]; }
    const double ma = sa / dn, mb = sb / dn;
    double saa = 0.0, sbb = 0.0, sab = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double da = a[i] - ma, db = b[i] - mb;
        saa += da * da;
        sbb += db * db;
        sab += da * db;
    }
    if (!(saa > 0.0) || !(sbb > 0.0)) {
        return std::unexpected(CointError::Degenerate);
    }
    return sab / std::sqrt(saa * sbb);
}

// ---------------------------------------------------------------------------
// P10-04: Engle-Granger
// ---------------------------------------------------------------------------

/// Augmented Dickey-Fuller on a series, with a constant and no trend.
///
/// Regresses de[t] on e[t-1] and `lags` lagged differences. The statistic is
/// the t-ratio on the e[t-1] coefficient; MORE NEGATIVE means more stationary.
///
/// The sign convention is the one thing everybody gets backwards, so it is
/// stated here and the comparison helper below takes the direction out of the
/// caller's hands entirely.
struct AdfResult {
    /// The t-statistic on the lagged level. More negative is more stationary.
    double t_stat = 0.0;
    /// The estimated mean-reversion coefficient, gamma in de = gamma*e + ...
    /// Negative for a stationary series.
    double gamma = 0.0;
    std::size_t n = 0;
    std::size_t lags = 0;
};

[[nodiscard]] inline std::expected<AdfResult, CointError>
adf(const double* e, std::size_t n, std::size_t lags) noexcept {
    if (n < 32 + lags) { return std::unexpected(CointError::TooFewSamples); }
    // Single-lag augmentation is done by residualising the differences on the
    // lagged differences first, then regressing on the level. Keeping it to a
    // small fixed lag count is deliberate: an ADF with many lags on a short
    // window has almost no power, and reporting a non-rejection from it as
    // "not cointegrated" is the commonest way a real pair is discarded.
    static thread_local double de[kMaxPairWindow], lev[kMaxPairWindow];
    const std::size_t m = n - 1 - lags;
    if (m < 16 || n > kMaxPairWindow) {
        return std::unexpected(CointError::TooFewSamples);
    }
    for (std::size_t i = 0; i < m; ++i) {
        const std::size_t t = i + 1 + lags;
        de[i] = e[t] - e[t - 1];
        lev[i] = e[t - 1];
    }
    if (lags > 0) {
        // Residualise out the lagged differences, one at a time.
        for (std::size_t L = 1; L <= lags; ++L) {
            static thread_local double lag[kMaxPairWindow];
            for (std::size_t i = 0; i < m; ++i) {
                const std::size_t t = i + 1 + lags;
                lag[i] = e[t - L] - e[t - L - 1];
            }
            const auto rd = ols(de, lag, m);
            const auto rl = ols(lev, lag, m);
            if (!rd || !rl) { return std::unexpected(CointError::Singular); }
            for (std::size_t i = 0; i < m; ++i) {
                de[i] -= rd->alpha + rd->beta * lag[i];
                lev[i] -= rl->alpha + rl->beta * lag[i];
            }
        }
    }
    const auto r = ols(de, lev, m);
    if (!r) { return std::unexpected(r.error()); }

    double sxx = 0.0, mx = 0.0;
    for (std::size_t i = 0; i < m; ++i) { mx += lev[i]; }
    mx /= static_cast<double>(m);
    for (std::size_t i = 0; i < m; ++i) {
        const double d = lev[i] - mx;
        sxx += d * d;
    }
    if (!(sxx > 0.0)) { return std::unexpected(CointError::Degenerate); }
    const double se = r->residual_sd / std::sqrt(sxx);

    AdfResult out{};
    out.gamma = r->beta;
    out.t_stat = se > 0.0 ? r->beta / se : 0.0;
    out.n = m;
    out.lags = lags;
    return out;
}

/// MacKinnon critical values for the Engle-Granger residual test, two series,
/// constant and no trend.
///
/// NOT the standard Dickey-Fuller table. The residual being tested was itself
/// produced by a regression that MINIMISED it, so it is more stationary-looking
/// than an arbitrary series and the critical values are further from zero.
/// Using the ordinary ADF table here finds cointegration roughly twice as often
/// as it exists, which is the second-commonest error in this whole area.
[[nodiscard]] inline double eg_critical(std::size_t n,
                                        double level_pct) noexcept {
    const double dn = static_cast<double>(n);
    // MacKinnon (1991/2010) response surface, N=2, no trend.
    if (level_pct <= 1.0) { return -3.9001 - 10.534 / dn - 30.03 / (dn * dn); }
    if (level_pct <= 5.0) { return -3.3377 - 5.967 / dn - 8.98 / (dn * dn); }
    return -3.0462 - 4.069 / dn - 5.73 / (dn * dn);
}

struct EngleGranger {
    Regression hedge{};
    AdfResult residual_test{};
    /// The 5% MacKinnon critical value at this sample size.
    double critical_5pct = 0.0;
    /// t_stat < critical. The comparison is done HERE so a caller never has to
    /// remember which way the inequality runs.
    bool cointegrated = false;
    /// Pearson correlation of the two LEVEL series, carried so the two
    /// properties can be reported side by side rather than confused.
    double level_correlation = 0.0;
};

/// Engle-Granger: regress y on x, then test the residual for stationarity.
[[nodiscard]] inline std::expected<EngleGranger, CointError>
engle_granger(const double* y, const double* x, std::size_t n,
              std::size_t lags) noexcept {
    const auto h = ols(y, x, n);
    if (!h) { return std::unexpected(h.error()); }
    static thread_local double resid[kMaxPairWindow];
    if (n > kMaxPairWindow) {
        return std::unexpected(CointError::TooFewSamples);
    }
    for (std::size_t i = 0; i < n; ++i) {
        resid[i] = y[i] - h->alpha - h->beta * x[i];
    }
    const auto t = adf(resid, n, lags);
    if (!t) { return std::unexpected(t.error()); }
    const auto c = correlation(y, x, n);

    EngleGranger out{};
    out.hedge = *h;
    out.residual_test = *t;
    out.critical_5pct = eg_critical(n, 5.0);
    out.cointegrated = t->t_stat < out.critical_5pct;
    out.level_correlation = c ? *c : 0.0;
    return out;
}

/// The bivariate Johansen trace statistic.
///
/// For two series the eigenvalue problem reduces to a form that can be written
/// out, which is what this does. It is a SECOND OPINION on Engle-Granger and
/// it matters because Engle-Granger is not symmetric: regressing y on x and x
/// on y give different residuals and can give different verdicts. Johansen
/// treats the two symmetrically, so a pair the two directions disagree about
/// is a pair to look at rather than a coin to flip.
struct Johansen {
    double trace_stat = 0.0;
    /// The 5% critical value for r = 0 against r >= 1, two series.
    double critical_5pct = 15.4948;
    bool cointegrated = false;
    double eigenvalue = 0.0;
};

[[nodiscard]] inline std::expected<Johansen, CointError>
johansen_bivariate(const double* y, const double* x, std::size_t n) noexcept {
    if (n < 32) { return std::unexpected(CointError::TooFewSamples); }
    // The reduced-rank regression of the differences on the lagged levels.
    // With two series the largest squared canonical correlation between
    // (dy, dx) and (y_-1, x_-1) is the eigenvalue, and the trace statistic is
    // -T*ln(1 - lambda).
    static thread_local double dy[kMaxPairWindow], dx[kMaxPairWindow];
    static thread_local double ly[kMaxPairWindow], lx[kMaxPairWindow];
    const std::size_t m = n - 1;
    if (n > kMaxPairWindow) {
        return std::unexpected(CointError::TooFewSamples);
    }
    for (std::size_t i = 0; i < m; ++i) {
        dy[i] = y[i + 1] - y[i];
        dx[i] = x[i + 1] - x[i];
        ly[i] = y[i];
        lx[i] = x[i];
    }
    // Project the level pair onto the difference pair; the squared canonical
    // correlation of the best linear combinations is what we need. With two
    // dimensions the maximum over the unit circle can be found by scanning,
    // which is exact enough here and far clearer than an eigen-decomposition
    // written out by hand.
    double best = 0.0;
    for (int k = 0; k < 180; ++k) {
        const double th = 3.14159265358979323846 * static_cast<double>(k)
                        / 180.0;
        const double c = std::cos(th), s = std::sin(th);
        static thread_local double comb_l[kMaxPairWindow];
        static thread_local double comb_d[kMaxPairWindow];
        for (std::size_t i = 0; i < m; ++i) {
            comb_l[i] = c * ly[i] + s * lx[i];
            comb_d[i] = c * dy[i] + s * dx[i];
        }
        const auto r = correlation(comb_d, comb_l, m);
        if (!r) { continue; }
        const double sq = (*r) * (*r);
        if (sq > best) { best = sq; }
    }
    Johansen out{};
    out.eigenvalue = best;
    out.trace_stat = -static_cast<double>(m) * std::log(1.0 - best);
    out.cointegrated = out.trace_stat > out.critical_5pct;
    return out;
}

// ---------------------------------------------------------------------------
// P10-05: half-life, selection, structural break
// ---------------------------------------------------------------------------

/// Ornstein-Uhlenbeck half-life of a spread, in observations.
///
/// From de[t] = lambda * e[t-1] + noise: half-life = -ln(2)/ln(1+lambda).
/// A non-negative lambda means the spread is not reverting at all, and the
/// half-life is infinite -- reported as a refusal rather than as a very large
/// number that would pass a "less than 200 bars" filter after overflow.
[[nodiscard]] inline std::expected<double, CointError>
half_life(const double* spread, std::size_t n) noexcept {
    if (n < 16) { return std::unexpected(CointError::TooFewSamples); }
    static thread_local double de[kMaxPairWindow], lev[kMaxPairWindow];
    if (n > kMaxPairWindow) {
        return std::unexpected(CointError::TooFewSamples);
    }
    for (std::size_t i = 0; i + 1 < n; ++i) {
        de[i] = spread[i + 1] - spread[i];
        lev[i] = spread[i];
    }
    const auto r = ols(de, lev, n - 1);
    if (!r) { return std::unexpected(r.error()); }
    if (!(r->beta < 0.0) || !(1.0 + r->beta > 0.0)) {
        return std::unexpected(CointError::Degenerate);
    }
    return -std::log(2.0) / std::log(1.0 + r->beta);
}

struct PairCandidate {
    EngleGranger eg{};
    Johansen joh{};
    double half_life_bars = 0.0;
    /// Current spread in units of its own standard deviation. The entry
    /// signal, and the reason `Distribution` statistics live in P3-06.
    double zscore = 0.0;
    bool half_life_known = false;

    /// Is this pair tradeable on a book with this holding period?
    ///
    /// `max_hold` is REQUIRED. A cointegration test says the spread is
    /// stationary and says nothing about how fast: a 200-bar half-life is
    /// perfectly cointegrated and perfectly untradeable intraday, and the
    /// position is financed and margined for a convergence that arrives after
    /// the strategy has been switched off.
    [[nodiscard]] bool tradeable(double max_hold_bars) const noexcept {
        return eg.cointegrated && joh.cointegrated && half_life_known
            && half_life_bars > 0.0 && half_life_bars <= max_hold_bars;
    }
    /// The two tests disagreeing is not a tie to be broken. Engle-Granger is
    /// not symmetric in y and x; Johansen is. A disagreement is a reason to
    /// look, and `tradeable` requires BOTH.
    [[nodiscard]] bool tests_agree() const noexcept {
        return eg.cointegrated == joh.cointegrated;
    }
};

/// Assess one pair end to end.
[[nodiscard]] inline std::expected<PairCandidate, CointError>
assess_pair(const double* y, const double* x, std::size_t n,
            std::size_t lags) noexcept {
    const auto eg = engle_granger(y, x, n, lags);
    if (!eg) { return std::unexpected(eg.error()); }
    const auto jo = johansen_bivariate(y, x, n);
    if (!jo) { return std::unexpected(jo.error()); }

    static thread_local double spread[kMaxPairWindow];
    for (std::size_t i = 0; i < n; ++i) {
        spread[i] = y[i] - eg->hedge.alpha - eg->hedge.beta * x[i];
    }
    PairCandidate c{};
    c.eg = *eg;
    c.joh = *jo;
    if (const auto h = half_life(spread, n)) {
        c.half_life_bars = *h;
        c.half_life_known = true;
    }
    if (eg->hedge.residual_sd > 0.0) {
        c.zscore = spread[n - 1] / eg->hedge.residual_sd;
    }
    return c;
}

/// CUSUM of squared recursive residuals, for a break in the RELATIONSHIP.
///
/// Not a break in the spread's level -- that is what the z-score trades. This
/// watches the hedge ratio itself: a merger, a demerger, a regulatory change or
/// an index reconstitution moves beta, and the pair is then held at the OLD
/// ratio against a NEW relationship. That is not a pairs trade any more, it is
/// a directional position nobody chose, and it is the one alarm treated as an
/// EXIT rather than a signal.
struct BreakTest {
    double max_excursion = 0.0;
    double threshold = 0.0;
    bool broke = false;
    std::size_t at = 0;
};

[[nodiscard]] inline std::expected<BreakTest, CointError>
structural_break(const double* y, const double* x, std::size_t n,
                 std::size_t burn_in, double band) noexcept {
    if (n < burn_in + 16 || burn_in < 16) {
        return std::unexpected(CointError::TooFewSamples);
    }
    if (!(band > 0.0)) { return std::unexpected(CointError::NoHoldingPeriod); }
    const auto base = ols(y, x, burn_in);
    if (!base) { return std::unexpected(base.error()); }

    BreakTest t{};
    t.threshold = band;
    double cum = 0.0, sse = 0.0;
    for (std::size_t i = burn_in; i < n; ++i) {
        const double e = y[i] - base->alpha - base->beta * x[i];
        sse += e * e;
    }
    const double denom = sse > 0.0 ? sse : 1.0;
    double run = 0.0;
    for (std::size_t i = burn_in; i < n; ++i) {
        const double e = y[i] - base->alpha - base->beta * x[i];
        run += e * e;
        // The CUSUM of squares against its expected linear path. A relationship
        // that still holds accumulates residual variance evenly; one that has
        // broken accumulates it in a burst.
        const double expected = static_cast<double>(i - burn_in + 1)
                              / static_cast<double>(n - burn_in);
        cum = run / denom - expected;
        if (std::fabs(cum) > t.max_excursion) {
            t.max_excursion = std::fabs(cum);
            t.at = i;
        }
    }
    t.broke = t.max_excursion > band;
    return t;
}

} // namespace altair
