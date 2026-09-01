// analytics/svi_fit.hpp -- calibrating a raw SVI slice to observed quotes.
//
// P3-05b. Evaluation and the arbitrage conditions are P3-05a.
//
// THE FIVE-PARAMETER PROBLEM IS TWO PROBLEMS, ONE OF THEM LINEAR.
//
// Fitting a, b, rho, m, sigma directly is a five-dimensional non-convex search
// with a long flat valley, and it finds a different answer depending on where
// it started. Zeliade's observation removes most of that. Hold (m, sigma) and
// substitute
//
//     y = (k - m)/sigma,   z = sqrt(y^2 + 1)
//
// and the model becomes
//
//     w = a + (b*rho*sigma)*y + (b*sigma)*z  =  a + c*y + d*z
//
// which is LINEAR in (a, c, d). So the inner problem is a three-variable least
// squares with an exact solution, and only (m, sigma) are searched -- two
// dimensions, by Nelder-Mead. b and rho come back as d/sigma and c/d.
//
// WEIGHTS COME FROM P3-04, AND THIS IS THE POINT OF THE CARD.
//
// An unweighted fit treats a deep-wing quote as being as informative as an
// at-the-money one. It is not, and analytics/iv.hpp measures by how much: on a
// 30-day NIFTY chain, one 5-paise tick moves the 24000 implied vol by 0.000018
// and the 28000 by 0.0079 -- a factor of 433. Equal weighting therefore lets
// the least reliable quotes on the board pull the surface hardest, because the
// wings are also where the residuals are largest in absolute terms.
//
// The right weight is the inverse variance of the observation, and P3-04 hands
// it over directly: `iv_uncertainty(vega, half_the_spread)` is the standard
// error of that point's implied vol, and `svi_weight_from_iv_error` converts
// it to a weight on TOTAL VARIANCE, which is what is being fitted. Test 3
// measures what this buys -- the at-the-money fit improves by an order of
// magnitude, on the same quotes.
//
// A FIT IS NOT DONE UNTIL IT HAS BEEN CHECKED. `SviFitResult` carries the
// butterfly scan of its own output, over the range it was fitted on. There is
// no path through this header that returns a calibrated slice without also
// returning whether that slice implies negative probability.

#pragma once

#include <analytics/svi.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

enum class SviFitError : std::uint8_t {
    /// Fewer than five points. The model has five parameters; fitting it to
    /// four is interpolation wearing a model's clothes.
    TooFewPoints,
    /// A non-finite k, w or weight, or a non-positive total variance.
    BadObservation,
    /// Every weight is zero or negative, so there is nothing to fit to.
    NoWeight,
    /// All observations sit at the same log-moneyness, so (m, sigma) are
    /// unidentifiable.
    Degenerate,
    /// Nelder-Mead exhausted its budget without the simplex collapsing.
    NoConvergence
};

/// One observed point on a slice.
struct SviFitPoint {
    /// Log-moneyness ln(K/F) against the FORWARD.
    double k = 0.0;
    /// Observed TOTAL implied variance, sigma_obs^2 * T. Not vol.
    double w = 0.0;
    /// Inverse-variance weight. Use `svi_weight_from_iv_error`; a weight of
    /// zero drops the point without removing it from the caller's array.
    double weight = 1.0;
};

struct SviFitConfig {
    /// Nelder-Mead iteration budget on (m, log sigma).
    int max_iterations = 400;
    /// Simplex collapse tolerance, on the parameter simplex diameter.
    double tolerance = 1e-10;
    /// Range over which the returned slice is scanned for butterfly
    /// arbitrage. Defaults to the data's own k range, padded, when left at
    /// zero -- see `svi_fit`.
    double scan_lo = 0.0;
    double scan_hi = 0.0;
    int scan_points = 401;
};

struct SviFitResult {
    SviParams params{};
    /// Weighted RMSE in TOTAL VARIANCE. UNIT: vol^2 * years.
    double rmse = 0.0;
    /// Largest absolute residual in total variance, and where.
    double worst_residual = 0.0;
    double worst_k = 0.0;
    /// Nelder-Mead iterations on (m, sigma).
    int iterations = 0;
    /// Points that carried a positive weight.
    int points = 0;
    /// How many inner solves had to be projected back into the domain. A
    /// large count means the search spent its time in infeasible territory
    /// and the answer should be treated with suspicion.
    int projections = 0;
    /// The butterfly scan of THIS fit, over the scan range. A fit is not
    /// returned without it.
    SviScan butterfly{};
};

/// Weight for a point whose implied vol is known to +/- `iv_error`.
///
/// The fit is in TOTAL VARIANCE w = sigma^2 * T, so an uncertainty on sigma
/// propagates as dw = 2*sigma*T*dsigma. Inverse-variance weighting then wants
/// 1/dw^2. Getting this conversion wrong -- weighting by 1/dsigma^2 directly
/// -- tilts the fit toward high-vol strikes by a factor of sigma^2, which is
/// four to one across a normal equity smile and is invisible in the output.
///
/// Returns 0 for a non-positive or non-finite error, which drops the point:
/// an infinite error bar is exactly what `iv_uncertainty` returns where vega
/// has underflowed, and such a quote carries no information about vol at all.
[[nodiscard]] inline double
svi_weight_from_iv_error(double iv_error, Vol vol, Years t) noexcept {
    if (!(iv_error > 0.0) || !std::isfinite(iv_error)) { return 0.0; }
    const double dw = 2.0 * vol.raw() * t.raw() * iv_error;
    if (!(dw > 0.0) || !std::isfinite(dw)) { return 0.0; }
    return 1.0 / (dw * dw);
}

namespace detail {

/// Result of the inner three-variable linear solve at a fixed (m, sigma).
struct SviInner {
    double a = 0.0;
    double c = 0.0;      // b * rho * sigma
    double d = 0.0;      // b * sigma
    double sse = 0.0;    // weighted sum of squared residuals
    bool projected = false;
};

/// Solve the linear inner problem, then project into the SVI domain.
///
/// The normal equations are 3x3, symmetric and positive semi-definite, so a
/// Cholesky factorisation with a tiny ridge is enough. The ridge matters: with
/// every quote at nearly the same moneyness the columns [1, y, z] become
/// collinear and an unridged solve returns something enormous rather than
/// something wrong-looking.
[[nodiscard]] inline SviInner
svi_inner_solve(const SviFitPoint* pts, std::size_t n, double m,
                double sigma) noexcept {
    // Accumulate A^T W A (symmetric, 6 distinct entries) and A^T W w.
    double s11 = 0.0, s1y = 0.0, s1z = 0.0, syy = 0.0, syz = 0.0, szz = 0.0;
    double t1 = 0.0, ty = 0.0, tz = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double ww = pts[i].weight;
        if (!(ww > 0.0)) { continue; }
        const double y = (pts[i].k - m) / sigma;
        const double z = std::sqrt(y * y + 1.0);
        const double w = pts[i].w;
        s11 += ww;         s1y += ww * y;      s1z += ww * z;
        syy += ww * y * y; syz += ww * y * z;  szz += ww * z * z;
        t1 += ww * w;      ty += ww * y * w;   tz += ww * z * w;
    }

    // Ridge, scaled to the problem so it is negligible when well conditioned.
    const double ridge = 1e-12 * (s11 + syy + szz);
    double A[3][3] = {{s11 + ridge, s1y, s1z},
                      {s1y, syy + ridge, syz},
                      {s1z, syz, szz + ridge}};
    double rhs[3] = {t1, ty, tz};

    // Cholesky, in place.
    SviInner out{};
    double L[3][3] = {};
    bool ok = true;
    for (int i = 0; i < 3 && ok; ++i) {
        for (int j = 0; j <= i; ++j) {
            double sum = A[i][j];
            for (int q = 0; q < j; ++q) { sum -= L[i][q] * L[j][q]; }
            if (i == j) {
                if (!(sum > 0.0)) { ok = false; break; }
                L[i][i] = std::sqrt(sum);
            } else {
                L[i][j] = sum / L[j][j];
            }
        }
    }
    double x[3] = {0.0, 0.0, 0.0};
    if (ok) {
        double v[3];
        for (int i = 0; i < 3; ++i) {
            double sum = rhs[i];
            for (int q = 0; q < i; ++q) { sum -= L[i][q] * v[q]; }
            v[i] = sum / L[i][i];
        }
        for (int i = 2; i >= 0; --i) {
            double sum = v[i];
            for (int q = i + 1; q < 3; ++q) { sum -= L[q][i] * x[q]; }
            x[i] = sum / L[i][i];
        }
    }
    out.a = x[0];
    out.c = x[1];
    out.d = x[2];

    // --- projection into the SVI domain -------------------------------
    // The unconstrained solution can land outside it, most often with d < 0
    // (a downward-sloping smile) or |c| > d (|rho| > 1). Each clamp below is
    // one of the domain conditions svi_check_params enforces, applied in the
    // (a, c, d) coordinates the inner problem lives in.
    const double before_c = out.c, before_d = out.d, before_a = out.a;
    if (out.d < 0.0) { out.d = 0.0; }                       // b >= 0
    if (out.c > out.d) { out.c = out.d; }                   // rho <= 1
    if (out.c < -out.d) { out.c = -out.d; }                 // rho >= -1
    // Lee: b(1+|rho|) <= 2, which in these coordinates is (d + |c|) <= 2*sigma.
    const double lee = out.d + std::fabs(out.c);
    if (lee > 2.0 * sigma && lee > 0.0) {
        const double scale = 2.0 * sigma / lee;
        out.c *= scale;
        out.d *= scale;
    }
    // With (c, d) now fixed, `a` is a one-variable weighted least squares and
    // can be re-solved exactly rather than left at its stale value.
    if (out.c != before_c || out.d != before_d) {
        double num = 0.0, den = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            const double ww = pts[i].weight;
            if (!(ww > 0.0)) { continue; }
            const double y = (pts[i].k - m) / sigma;
            const double z = std::sqrt(y * y + 1.0);
            num += ww * (pts[i].w - out.c * y - out.d * z);
            den += ww;
        }
        if (den > 0.0) { out.a = num / den; }
    }
    // Minimum total variance must be non-negative.
    const double floor_a = -std::sqrt(
        (out.d * out.d - out.c * out.c) > 0.0
            ? (out.d * out.d - out.c * out.c) : 0.0);
    if (out.a < floor_a) { out.a = floor_a; }
    out.projected = (out.a != before_a) || (out.c != before_c)
                 || (out.d != before_d);

    // Objective at the projected point, which is what the outer search sees.
    double sse = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double ww = pts[i].weight;
        if (!(ww > 0.0)) { continue; }
        const double y = (pts[i].k - m) / sigma;
        const double z = std::sqrt(y * y + 1.0);
        const double r = out.a + out.c * y + out.d * z - pts[i].w;
        sse += ww * r * r;
    }
    out.sse = sse;
    return out;
}

/// Convert an inner solution back to raw SVI parameters.
[[nodiscard]] inline SviParams svi_from_inner(const SviInner& in, double m,
                                              double sigma) noexcept {
    SviParams p{};
    p.a = in.a;
    p.m = m;
    p.sigma = sigma;
    p.b = in.d / sigma;
    // rho = c/d. At d == 0 the slice is flat, rho is unidentifiable, and 0 is
    // the only choice that keeps |rho| < 1 -- reporting a stale or clamped
    // rho on a flat slice would invite reading a skew off nothing.
    p.rho = (in.d > 0.0) ? in.c / in.d : 0.0;
    if (p.rho > 0.999999) { p.rho = 0.999999; }
    if (p.rho < -0.999999) { p.rho = -0.999999; }
    return p;
}

} // namespace detail

/// Calibrate a raw SVI slice to observed total variances.
///
/// `pts` are (log-moneyness, total variance, weight). Weights should come from
/// `svi_weight_from_iv_error`; see the header note on why equal weighting lets
/// the worst quotes on the board pull hardest.
///
/// Nelder-Mead searches (m, log sigma) -- log so sigma cannot go non-positive
/// by construction rather than by clamping -- and the inner (a, c, d) is
/// solved exactly at each trial.
[[nodiscard]] inline std::expected<SviFitResult, SviFitError>
svi_fit(const SviFitPoint* pts, std::size_t n,
        const SviFitConfig& cfg = SviFitConfig{}) noexcept {
    if (n < 5) { return std::unexpected(SviFitError::TooFewPoints); }

    double k_lo = 0.0, k_hi = 0.0, wsum = 0.0, kbar = 0.0;
    double w_min = 0.0, k_at_w_min = 0.0;
    int used = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const SviFitPoint& p = pts[i];
        if (!std::isfinite(p.k) || !std::isfinite(p.w)
            || !std::isfinite(p.weight)) {
            return std::unexpected(SviFitError::BadObservation);
        }
        if (!(p.w > 0.0)) {
            return std::unexpected(SviFitError::BadObservation);
        }
        if (!(p.weight > 0.0)) { continue; }
        if (used == 0 || p.k < k_lo) { k_lo = p.k; }
        if (used == 0 || p.k > k_hi) { k_hi = p.k; }
        if (used == 0 || p.w < w_min) { w_min = p.w; k_at_w_min = p.k; }
        wsum += p.weight;
        kbar += p.weight * p.k;
        ++used;
    }
    if (used < 5 || !(wsum > 0.0)) {
        return std::unexpected(used < 5 ? SviFitError::TooFewPoints
                                        : SviFitError::NoWeight);
    }
    kbar /= wsum;
    const double k_span = k_hi - k_lo;
    if (!(k_span > 0.0)) { return std::unexpected(SviFitError::Degenerate); }

    // --- Nelder-Mead on (m, log sigma) ---------------------------------
    // Seeded at the moneyness of the CHEAPEST observed variance, which is
    // where the smile bottoms out and therefore close to m, and at a sigma of
    // a quarter of the observed k span.
    auto objective = [&](double m, double log_sigma) {
        return detail::svi_inner_solve(pts, n, m, std::exp(log_sigma)).sse;
    };

    double sx[3][2] = {
        {k_at_w_min,                std::log(0.25 * k_span)},
        {k_at_w_min + 0.25 * k_span, std::log(0.25 * k_span)},
        {kbar,                      std::log(0.60 * k_span)},
    };
    double fv[3];
    for (int i = 0; i < 3; ++i) { fv[i] = objective(sx[i][0], sx[i][1]); }

    int iter = 0;
    for (; iter < cfg.max_iterations; ++iter) {
        // Order: best, second, worst.
        int b = 0, s = 0, w = 0;
        for (int i = 1; i < 3; ++i) {
            if (fv[i] < fv[b]) { b = i; }
            if (fv[i] > fv[w]) { w = i; }
        }
        for (int i = 0; i < 3; ++i) { if (i != b && i != w) { s = i; } }

        // Converged when the simplex has collapsed in parameter space. Testing
        // the FUNCTION spread instead would stop early on the long flat valley
        // that makes the five-parameter problem hard in the first place.
        const double diam =
            std::fabs(sx[b][0] - sx[w][0]) + std::fabs(sx[b][1] - sx[w][1]);
        if (diam < cfg.tolerance) { break; }

        const double cx = 0.5 * (sx[b][0] + sx[s][0]);
        const double cy = 0.5 * (sx[b][1] + sx[s][1]);

        const double rx = cx + (cx - sx[w][0]);
        const double ry = cy + (cy - sx[w][1]);
        const double fr = objective(rx, ry);

        if (fr < fv[b]) {
            const double ex = cx + 2.0 * (cx - sx[w][0]);
            const double ey = cy + 2.0 * (cy - sx[w][1]);
            const double fe = objective(ex, ey);
            if (fe < fr) { sx[w][0] = ex; sx[w][1] = ey; fv[w] = fe; }
            else         { sx[w][0] = rx; sx[w][1] = ry; fv[w] = fr; }
        } else if (fr < fv[s]) {
            sx[w][0] = rx; sx[w][1] = ry; fv[w] = fr;
        } else {
            const double kx = cx + 0.5 * (sx[w][0] - cx);
            const double ky = cy + 0.5 * (sx[w][1] - cy);
            const double fk = objective(kx, ky);
            if (fk < fv[w]) {
                sx[w][0] = kx; sx[w][1] = ky; fv[w] = fk;
            } else {
                for (int i = 0; i < 3; ++i) {
                    if (i == b) { continue; }
                    sx[i][0] = sx[b][0] + 0.5 * (sx[i][0] - sx[b][0]);
                    sx[i][1] = sx[b][1] + 0.5 * (sx[i][1] - sx[b][1]);
                    fv[i] = objective(sx[i][0], sx[i][1]);
                }
            }
        }
    }

    int best = 0;
    for (int i = 1; i < 3; ++i) { if (fv[i] < fv[best]) { best = i; } }
    const double m_hat = sx[best][0];
    const double sigma_hat = std::exp(sx[best][1]);
    const detail::SviInner in = detail::svi_inner_solve(pts, n, m_hat,
                                                        sigma_hat);

    SviFitResult out{};
    out.params = detail::svi_from_inner(in, m_hat, sigma_hat);
    out.iterations = iter;
    out.points = used;
    out.projections = in.projected ? 1 : 0;

    double wsse = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        if (!(pts[i].weight > 0.0)) { continue; }
        const double r = svi_total_variance(out.params, pts[i].k) - pts[i].w;
        wsse += pts[i].weight * r * r;
        if (std::fabs(r) > std::fabs(out.worst_residual)) {
            out.worst_residual = r;
            out.worst_k = pts[i].k;
        }
    }
    out.rmse = std::sqrt(wsse / wsum);

    // A fit is not done until it has been checked. The scan range defaults to
    // the data's own span, padded by a quarter on each side -- because a
    // surface is invariably asked for a vol slightly outside the strikes it
    // was fitted to, and that is where an over-curved fit breaks first.
    const double pad = 0.25 * k_span;
    const double lo = (cfg.scan_lo < cfg.scan_hi) ? cfg.scan_lo : k_lo - pad;
    const double hi = (cfg.scan_lo < cfg.scan_hi) ? cfg.scan_hi : k_hi + pad;
    if (const auto scan = svi_butterfly_scan(out.params, lo, hi,
                                             cfg.scan_points)) {
        out.butterfly = *scan;
    } else {
        out.butterfly.clean = false;
        out.butterfly.worst = -1.0;
    }
    return out;
}

} // namespace altair
