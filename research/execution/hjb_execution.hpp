// research/execution/hjb_execution.hpp -- optimal trade execution by an HJB equation.
//
// P.A. Forsyth, "A Hamilton Jacobi Bellman approach to optimal trade
// execution", Applied Numerical Mathematics 61 (2011) 241-265. Preprint:
// https://cs.uwaterloo.ca/~paforsyt/optimal_trade.pdf
//
// WHAT THIS ADDS TO shortfall.hpp.
//
// Almgren-Chriss (shortfall.hpp) is a closed form for arithmetic Brownian
// prices, LINEAR temporary impact, and a static schedule. Forsyth drops all
// three:
//
//   * the price is geometric, with permanent impact in the drift:
//         dS = (eta + g(v)) S dt + sigma S dZ,     g(v) = kappa_p v
//   * temporary impact and the spread are NONLINEAR in the trading rate v:
//         f(v) = [1 + kappa_s sgn(v)] exp(kappa_t sgn(v) |v|^beta)
//     and cash moves as dB = (r B - v S f(v)) dt
//   * the strategy may react to the price path. The objective is the
//     mean-variance one at the INITIAL time (pre-commitment), turned into
//     the linear-quadratic problem  min E[(B_L - gamma/2)^2]  (eq. 3.6).
//     Sweeping gamma traces the efficient frontier.
//
// THE SOLVER, AS THE PAPER WRITES IT.
//
//   * Similarity reduction (eq. 3.11-3.12): V(xS, xB, a) = x^2 V(S, B, a) and
//     U(xS, xB, a) = x U(S, B, a), so only two cash planes, B = +B* and
//     B = -B*, are solved, each on an S x alpha grid.
//   * Semi-Lagrangian timestepping (eq. 5.8-5.11): for every node and every
//     control in a discrete set, follow the characteristic one step
//         S^ = S exp(g dt)
//         B^ = B exp(r dt) - v S f(v) (exp(r dt) - exp(g dt)) / (r - g)
//         a^ = a + v dt
//     interpolate the previous level there (bilinear in S x alpha, via the
//     similarity reduction for B), take the minimum by linear search, then
//     apply the diffusion  L V = sigma^2 S^2/2 V_SS + eta S V_S  fully
//     implicitly with a positive-coefficient difference (eq. 5.4).
//   * Controls that would leave [0, S_max] x [alpha_min, alpha_max] are not
//     admissible (eq. 5.10). At S = 0 and S = S_max the diffusion is dropped
//     (eq. 4.10).
//   * U = E[B_L] solves the linear PDE with the optimal control frozen
//     (eq. 5.13).
//   * Terminal condition (eq. 2.11-2.12): whatever is left at T is dumped in
//     a final trade of length (dt)_T, so B_L = B + a S f(-a / (dt)_T). The
//     interval is short on purpose; it is the penalty for not finishing.
//
// THE NUMBER TO CHECK AGAINST.
//
// Table 7.1 (one day, sigma = 1, kappa_t = 2e-6, beta = 1, gamma = 200) gives
// expected gain 99.295 and standard deviation 0.7469 (eq. 7.1), with
// E[(B_L - gamma/2)^2] converging 1.668, 1.319, 1.176, 1.095, 1.055 over
// refinement levels 0..4 (Table 7.3). The test reproduces that sequence.
//
// WHAT IT IS NOT.
//
// A research solver, like almgren_chriss(). Nothing here places an order and
// nothing in oms/ calls it to slice a live parent. The parameters (kappa_t,
// kappa_p, beta) are impact estimates for one instrument; the paper's values
// are for a normalised one-share position and are not NSE numbers.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <vector>

namespace altair {

enum class HjbError : std::uint8_t {
    /// sigma, S_max, horizon, liquidation interval or position not positive.
    BadParameter,
    /// The control set does not contain a selling (or buying) rate.
    BadControls,
    /// Fewer than 3 nodes in a direction, or no timesteps.
    BadGrid
};

/// The model of section 2. Field names follow the paper.
struct HjbParams {
    double sigma = 0.0;       ///< volatility, annualised
    double eta = 0.0;         ///< drift of S (the paper's eta, not AC's)
    double r = 0.0;           ///< interest on the cash account
    double kappa_p = 0.0;     ///< permanent impact, g(v) = kappa_p v
    double kappa_s = 0.0;     ///< bid-ask spread factor
    double kappa_t = 0.0;     ///< temporary impact factor
    double beta = 1.0;        ///< temporary impact exponent
    double horizon = 0.0;     ///< T, years
    double s0 = 0.0;          ///< S at t = 0
    double alpha0 = 0.0;      ///< shares held at t = 0 (> 0: a sell program)
    double v_min = 0.0;       ///< slowest (most negative) trading rate
    double v_max = 0.0;       ///< fastest buying rate (0 for a pure sell)
    double s_max = 0.0;       ///< truncated S domain
    double dt_liq = 0.0;      ///< (dt)_T, the final dump interval

    [[nodiscard]] bool valid() const noexcept {
        return sigma > 0.0 && horizon > 0.0 && s0 > 0.0 && alpha0 > 0.0
            && s_max > s0 && dt_liq > 0.0 && beta > 0.0 && kappa_s >= 0.0
            && kappa_s < 1.0 && kappa_t >= 0.0;
    }

    /// f(v): price received (v < 0) or paid (v > 0) per unit of S.
    [[nodiscard]] double f(double v) const noexcept {
        if (v == 0.0) { return 1.0; }
        const double sg = v > 0.0 ? 1.0 : -1.0;
        return (1.0 + kappa_s * sg)
             * std::exp(kappa_t * sg * std::pow(std::fabs(v), beta));
    }
    [[nodiscard]] double g(double v) const noexcept { return kappa_p * v; }

    /// B_L for cash b, price s and a leftover position a dumped over dt_liq.
    [[nodiscard]] double liquidation(double b, double s, double a) const noexcept {
        if (a == 0.0) { return b; }
        return b + a * s * f(-a / dt_liq);
    }
};

/// Table 7.1: the short (one day) sell example.
[[nodiscard]] inline HjbParams hjb_forsyth_short() noexcept {
    HjbParams p;
    p.sigma = 1.0;
    p.horizon = 1.0 / 250.0;
    p.s0 = 100.0;
    p.alpha0 = 1.0;
    p.kappa_t = 2.0e-6;
    p.beta = 1.0;
    p.v_min = -1000.0 / p.horizon;
    p.s_max = 20000.0;
    p.dt_liq = 1.0e-6;
    return p;
}

/// Table 8.1: the long (one month) sell example.
[[nodiscard]] inline HjbParams hjb_forsyth_long() noexcept {
    HjbParams p;
    p.sigma = 0.40;
    p.eta = 0.10;
    p.r = 0.05;
    p.horizon = 1.0 / 12.0;
    p.s0 = 100.0;
    p.alpha0 = 1.0;
    p.kappa_p = 0.01;
    p.kappa_t = 0.069;
    p.kappa_s = 0.01;
    p.beta = 0.5;
    p.v_min = -25.0 / p.horizon;
    p.s_max = 20000.0;
    p.dt_liq = 1.0e-9;
    return p;
}

/// Grid sizes. `level(k)` is the refinement ladder of Table 7.2.
struct HjbGrid {
    std::size_t steps = 25;
    std::size_t s_nodes = 98;
    std::size_t a_nodes = 41;
    std::size_t v_nodes = 30;

    [[nodiscard]] static HjbGrid level(unsigned k) noexcept {
        const std::size_t m = std::size_t{1} << k;
        return HjbGrid{25 * m, 97 * m + 1, 40 * m + 1, 29 * m + 1};
    }
};

/// One point on the efficient frontier, at t = 0.
struct HjbPoint {
    double gamma = 0.0;
    double expected = 0.0;   ///< E[B_L]
    double stdev = 0.0;      ///< sd[B_L]
    double objective = 0.0;  ///< E[(B_L - gamma/2)^2], the value V
};

class HjbSolution;
namespace hjb_detail {
std::expected<HjbSolution, HjbError>
run(const HjbParams&, const HjbGrid&, double, bool,
    const std::vector<double>*) noexcept;
} // namespace hjb_detail

/// The solved problem for one gamma. Keeps the optimal control on the grid
/// (if asked) so the policy can be replayed on simulated paths.
class HjbSolution {
public:
    [[nodiscard]] const HjbPoint& point() const noexcept { return pt_; }
    [[nodiscard]] bool has_policy() const noexcept { return !policy_.empty(); }

    /// The optimal rate at time t (years from the start) in state
    /// (S, B, alpha), by the similarity reduction and bilinear interpolation
    /// of the stored control. Homogeneous of degree zero in (S, B).
    [[nodiscard]] double control(double t, double s, double b,
                                 double a) const noexcept {
        if (policy_.empty()) { return 0.0; }
        const double dt = horizon_ / static_cast<double>(steps_);
        // The level that is applied FROM t forward: tau = T - t.
        long n = static_cast<long>(std::ceil((horizon_ - t) / dt - 1e-9));
        n = std::clamp(n, 1L, static_cast<long>(steps_));
        const auto& lv = policy_[static_cast<std::size_t>(n - 1)];
        const int plane = b >= 0.0 ? 0 : 1;
        const double xi = std::fabs(b) / bstar_;
        const double sp = xi > 0.0 ? s / xi : s_.back();
        return bilinear(lv[static_cast<std::size_t>(plane)],
                        std::min(sp, s_.back()), a);
    }

    friend std::expected<HjbSolution, HjbError>
    hjb_detail::run(const HjbParams&, const HjbGrid&, double, bool,
                    const std::vector<double>*) noexcept;

private:
    using Plane = std::vector<double>;   // s-major: [i * na + j]

    [[nodiscard]] double bilinear(const Plane& f, double s,
                                  double a) const noexcept {
        const std::size_t ns = s_.size(), na = a_.size();
        std::size_t i = static_cast<std::size_t>(
            std::upper_bound(s_.begin(), s_.end(), s) - s_.begin());
        i = std::clamp<std::size_t>(i, 1, ns - 1);
        const double ws = std::clamp((s - s_[i - 1]) / (s_[i] - s_[i - 1]),
                                     0.0, 1.0);
        const double da = a_[1] - a_[0];
        double ja = (a - a_[0]) / da;
        ja = std::clamp(ja, 0.0, static_cast<double>(na - 1));
        std::size_t j = std::min(static_cast<std::size_t>(ja), na - 2);
        const double wa = ja - static_cast<double>(j);
        const auto at = [&](std::size_t ii, std::size_t jj) {
            return f[ii * na + jj];
        };
        return (1 - ws) * ((1 - wa) * at(i - 1, j) + wa * at(i - 1, j + 1))
             + ws * ((1 - wa) * at(i, j) + wa * at(i, j + 1));
    }

    std::vector<double> s_, a_;
    std::vector<std::vector<Plane>> policy_;  // [level][plane]
    double bstar_ = 1.0;
    double horizon_ = 0.0;
    std::size_t steps_ = 0;
    HjbPoint pt_;
};

namespace hjb_detail {

/// Nodes on [0, s_max], clustered at s0 by a sinh stretch. Uniform in the
/// stretched coordinate, so neighbouring spacings change smoothly.
inline std::vector<double> s_grid(double s0, double s_max, std::size_t n) {
    const double d = s0 / 5.0;
    const double c0 = std::asinh(-s0 / d);
    const double c1 = std::asinh((s_max - s0) / d);
    std::vector<double> s(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double x = static_cast<double>(i) / static_cast<double>(n - 1);
        s[i] = s0 + d * std::sinh(c0 + (c1 - c0) * x);
    }
    s.front() = 0.0;
    s.back() = s_max;
    return s;
}

/// Thomas algorithm for one implicit diffusion solve, in place.
inline void tridiag(const std::vector<double>& lo, const std::vector<double>& di,
                    const std::vector<double>& up, std::vector<double>& x,
                    std::vector<double>& work) {
    const std::size_t n = x.size();
    work.assign(n, 0.0);
    double den = di[0];
    x[0] /= den;
    for (std::size_t i = 1; i < n; ++i) {
        work[i] = up[i - 1] / den;
        den = di[i] - lo[i] * work[i];
        x[i] = (x[i] - lo[i] * x[i - 1]) / den;
    }
    for (std::size_t i = n - 1; i-- > 0;) { x[i] -= work[i + 1] * x[i + 1]; }
}

} // namespace hjb_detail

namespace hjb_detail {

/// The solver. With `rates` null it is the HJB of the paper; with a rate per
/// forward step it is the LINEAR PDE for that fixed, deterministic schedule
/// (a TWAP or an Almgren-Chriss trajectory), on the same grid, so the two are
/// compared like for like.
inline std::expected<HjbSolution, HjbError>
run(const HjbParams& p, const HjbGrid& grid, double gamma, bool keep_policy,
    const std::vector<double>* rates) noexcept {
    if (!p.valid() || !(gamma > 0.0)) {
        return std::unexpected(HjbError::BadParameter);
    }
    if (!(p.v_min < 0.0) || p.v_max < 0.0) {
        return std::unexpected(HjbError::BadControls);
    }
    if (grid.steps == 0 || grid.s_nodes < 3 || grid.a_nodes < 3
        || grid.v_nodes < 2) {
        return std::unexpected(HjbError::BadGrid);
    }
    if (rates != nullptr && rates->size() != grid.steps) {
        return std::unexpected(HjbError::BadControls);
    }

    HjbSolution sol;
    const std::size_t ns = grid.s_nodes, na = grid.a_nodes;
    sol.s_ = hjb_detail::s_grid(p.s0, p.s_max, ns);
    sol.a_.resize(na);
    // alpha in [0, alpha0] for a sell; the paper's alpha_min, alpha_max.
    const double a_lo = 0.0, a_hi = p.alpha0;
    for (std::size_t j = 0; j < na; ++j) {
        sol.a_[j] = a_lo + (a_hi - a_lo) * static_cast<double>(j)
                                       / static_cast<double>(na - 1);
    }
    // B* is free; pick the starting cash so t = 0 sits on the minus plane
    // without rescaling.
    const double b0 = -0.5 * gamma * std::exp(-p.r * p.horizon);
    sol.bstar_ = std::fabs(b0);
    sol.horizon_ = p.horizon;
    sol.steps_ = grid.steps;
    const double bstar = sol.bstar_;
    const double dt = p.horizon / static_cast<double>(grid.steps);

    // Discrete controls on [v_min, v_max] with 0 always present. The paper
    // asks only that the largest gap be O(h) (section 5.2); it does not say
    // where the nodes go. A uniform set cannot work at the coarse levels:
    // with v_min = -1000/T and 25 steps, the first nonzero rate moves alpha
    // by more than the whole position in one step, so the only admissible
    // control would be "do nothing". The nodes are therefore cubed toward
    // zero, v = v_end * x^3 for uniform x: the largest gap stays O(h) and
    // the slow rates that matter are resolved.
    std::vector<double> vs;
    vs.reserve(2 * grid.v_nodes + 1);
    const auto add_side = [&](double v_end, std::size_t m) {
        for (std::size_t k = 1; k < m; ++k) {
            const double x = static_cast<double>(k) / static_cast<double>(m - 1);
            vs.push_back(v_end * x * x * x);
        }
    };
    // Split the node budget by the length of each side.
    const double span = p.v_max - p.v_min;
    const std::size_t m_sell = std::max<std::size_t>(
        2, static_cast<std::size_t>(std::lround(
               static_cast<double>(grid.v_nodes) * (-p.v_min) / span)));
    const std::size_t m_buy = p.v_max > 0.0
        ? std::max<std::size_t>(2, grid.v_nodes - m_sell + 1) : 1;
    vs.push_back(0.0);
    add_side(p.v_min, m_sell);
    if (p.v_max > 0.0) { add_side(p.v_max, m_buy); }
    // Per-control constants for the characteristic.
    struct Ctl { double v, sgrow, ffac, astep; };
    std::vector<Ctl> ctl;
    ctl.reserve(vs.size());
    const auto make_ctl = [&](double v) {
        const double gv = p.g(v);
        const double er1 = std::exp(p.r * dt), eg = std::exp(gv * dt);
        const double w = std::fabs(p.r - gv) > 1e-14 ? (er1 - eg) / (p.r - gv)
                                                     : dt * er1;
        return Ctl{v, eg, v * p.f(v) * w, v * dt};
    };
    for (double v : vs) { ctl.push_back(make_ctl(v)); }
    const double er = std::exp(p.r * dt);

    using Plane = HjbSolution::Plane;
    // [0] is B = +B*, [1] is B = -B*.
    Plane V[2], U[2], Vn[2], Un[2], Ctrl[2];
    for (int c = 0; c < 2; ++c) {
        const double b = c == 0 ? bstar : -bstar;
        V[c].resize(ns * na);
        U[c].resize(ns * na);
        Vn[c].resize(ns * na);
        Un[c].resize(ns * na);
        Ctrl[c].resize(ns * na);
        for (std::size_t i = 0; i < ns; ++i) {
            for (std::size_t j = 0; j < na; ++j) {
                const double bl = p.liquidation(b, sol.s_[i], sol.a_[j]);
                V[c][i * na + j] = bl * bl;
                U[c][i * na + j] = bl;
            }
        }
    }

    // W = V - U^2, the conditional variance of B_L, per plane. See lookup.
    Plane W[2];
    const auto refresh_w = [&]() {
        for (int c = 0; c < 2; ++c) {
            W[c].resize(ns * na);
            for (std::size_t k = 0; k < ns * na; ++k) {
                W[c][k] = std::max(0.0, V[c][k] - U[c][k] * U[c][k]);
            }
        }
    };
    refresh_w();

    // Value of the previous level at an arbitrary (S, B, alpha): pick the
    // plane by the sign of B and rescale by the similarity reduction.
    //
    // WHAT IS INTERPOLATED, AND WHY IT IS NOT V.
    //
    // The similarity map sends S' = S B* / |B^| far up the S axis as the cash
    // account nears zero, which is exactly where a liquidation ends. There V
    // is close to (alpha S' f - B*)^2: QUADRATIC in S', and linear
    // interpolation of a quadratic overstates it by about h^2 V''/8 every
    // step. On a 98-node grid that error was larger than the answer: a TWAP
    // with sigma = 1e-4, which has no variance at all, came out with a
    // standard deviation of 7. So the mean U and the variance W = V - U^2
    // are interpolated instead and V is rebuilt as U^2 + W. U is BILINEAR in
    // (S', alpha) for a fixed schedule, so the interpolation is exact for it,
    // and W is small and smooth. Both interpolants are monotone and W stays
    // non-negative. Past S_max both are extended linearly (V_SS -> 0 becomes
    // U_SS, W_SS -> 0), multiplied out so B^ -> 0 stays finite.
    const auto& sg = sol.s_;
    const double da = sol.a_[1] - sol.a_[0];
    struct Hit { double v, u; };
    const auto lookup = [&](double s, double b, double a) -> Hit {
        const int c = b >= 0.0 ? 0 : 1;
        const double xi = std::fabs(b) / bstar;
        double ja = std::clamp((a - a_lo) / da, 0.0, static_cast<double>(na - 1));
        std::size_t j = std::min(static_cast<std::size_t>(ja), na - 2);
        const double wa = ja - static_cast<double>(j);
        const auto col = [&](const Plane& f, std::size_t i) {
            return (1 - wa) * f[i * na + j] + wa * f[i * na + j + 1];
        };
        double u = 0.0, w = 0.0;   // already scaled by xi and xi^2
        if (xi * p.s_max <= s) {
            const double h = sg[ns - 1] - sg[ns - 2];
            const double u1 = col(U[c], ns - 1), u0 = col(U[c], ns - 2);
            const double w1 = col(W[c], ns - 1), w0 = col(W[c], ns - 2);
            const double su = (u1 - u0) / h, sw = (w1 - w0) / h;
            u = xi * (u1 - su * sg[ns - 1]) + su * s;
            w = std::max(0.0, xi * xi * (w1 - sw * sg[ns - 1]) + xi * sw * s);
        } else {
            const double sp = s / xi;
            std::size_t i = static_cast<std::size_t>(
                std::upper_bound(sg.begin(), sg.end(), sp) - sg.begin());
            i = std::clamp<std::size_t>(i, 1, ns - 1);
            const double ws = (sp - sg[i - 1]) / (sg[i] - sg[i - 1]);
            u = xi * ((1 - ws) * col(U[c], i - 1) + ws * col(U[c], i));
            w = xi * xi * ((1 - ws) * col(W[c], i - 1) + ws * col(W[c], i));
        }
        return Hit{u * u + w, u};
    };

    // Implicit diffusion coefficients, fixed for the whole solve.
    std::vector<double> lo(ns, 0.0), di(ns, 1.0), up(ns, 0.0);
    for (std::size_t i = 1; i + 1 < ns; ++i) {
        const double s = sg[i];
        const double hm = s - sg[i - 1], hp = sg[i + 1] - s;
        const double a2 = 0.5 * p.sigma * p.sigma * s * s, b1 = p.eta * s;
        double cl = 2 * a2 / (hm * (hm + hp)) - b1 / (hm + hp);
        double cu = 2 * a2 / (hp * (hm + hp)) + b1 / (hm + hp);
        if (cl < 0.0 || cu < 0.0) {           // upwind the drift
            cl = 2 * a2 / (hm * (hm + hp)) + (b1 < 0 ? -b1 / hm : 0.0);
            cu = 2 * a2 / (hp * (hm + hp)) + (b1 > 0 ? b1 / hp : 0.0);
        }
        lo[i] = -dt * cl;
        up[i] = -dt * cu;
        di[i] = 1.0 + dt * (cl + cu);
    }
    // Rows 0 and ns-1: L = 0 (S = 0, and S = S_max by eq. 4.10).

    if (keep_policy) { sol.policy_.resize(grid.steps); }
    std::vector<double> colv(ns), colu(ns), work;
    const double eps_a = 1e-12 * p.alpha0;

    for (std::size_t n = 0; n < grid.steps; ++n) {
        for (int c = 0; c < 2; ++c) {
            const double b = c == 0 ? bstar : -bstar;
            for (std::size_t i = 0; i < ns; ++i) {
                const double s = sg[i];
                for (std::size_t j = 0; j < na; ++j) {
                    const double a = sol.a_[j];
                    double best = std::numeric_limits<double>::infinity();
                    double best_u = 0.0, best_v = 0.0;
                    const Ctl* arg = nullptr;
                    Ctl fixed{};
                    std::span<const Ctl> set{ctl};
                    if (rates != nullptr) {
                        // Forward step m = steps - 1 - n. A schedule that
                        // would oversell from this node sells what is left.
                        const double v = std::max(
                            (*rates)[grid.steps - 1 - n], -a / dt);
                        fixed = make_ctl(v);
                        set = std::span<const Ctl>{&fixed, 1};
                    }
                    for (const Ctl& k : set) {
                        const double ah = a + k.astep;
                        if (ah < a_lo - eps_a || ah > a_hi + eps_a) { continue; }
                        const double sh = s * k.sgrow;
                        if (sh > p.s_max) { continue; }
                        const double bh = b * er - s * k.ffac;
                        const double val = lookup(sh, bh, ah).v;
                        if (val < best) { best = val; arg = &k; }
                    }
                    if (arg != nullptr) {
                        const double sh = s * arg->sgrow;
                        const double bh = b * er - s * arg->ffac;
                        best_u = lookup(sh, bh, a + arg->astep).u;
                        best_v = arg->v;
                    }
                    Vn[c][i * na + j] = best;
                    Un[c][i * na + j] = best_u;
                    Ctrl[c][i * na + j] = best_v;
                }
            }
        }
        for (int c = 0; c < 2; ++c) {
            for (std::size_t j = 0; j < na; ++j) {
                for (std::size_t i = 0; i < ns; ++i) {
                    colv[i] = Vn[c][i * na + j];
                    colu[i] = Un[c][i * na + j];
                }
                hjb_detail::tridiag(lo, di, up, colv, work);
                hjb_detail::tridiag(lo, di, up, colu, work);
                for (std::size_t i = 0; i < ns; ++i) {
                    V[c][i * na + j] = colv[i];
                    U[c][i * na + j] = colu[i];
                }
            }
            if (keep_policy) { sol.policy_[n].push_back(Ctrl[c]); }
        }
        refresh_w();
    }

    // t = 0, S = s0, B = b0 (the minus plane, xi = 1), alpha = alpha0.
    const Hit h0 = lookup(p.s0, b0, p.alpha0);
    // U0 = E[B_L] - gamma/2 because the starting cash carries the shift.
    sol.pt_.gamma = gamma;
    sol.pt_.objective = h0.v;
    sol.pt_.expected = h0.u + 0.5 * gamma;
    sol.pt_.stdev = std::sqrt(std::max(0.0, h0.v - h0.u * h0.u));
    return sol;
}

} // namespace hjb_detail

/// Solve the HJB for one gamma. `keep_policy` stores the optimal control at
/// every level (steps x 2 x S x alpha doubles) for replay.
[[nodiscard]] inline std::expected<HjbSolution, HjbError>
hjb_solve(const HjbParams& p, const HjbGrid& grid, double gamma,
          bool keep_policy = false) noexcept {
    return hjb_detail::run(p, grid, gamma, keep_policy, nullptr);
}

/// E[B_L], sd and the gamma objective of a FIXED schedule: `rates[m]` is the
/// trading rate over forward step m, m = 0 .. grid.steps - 1.
[[nodiscard]] inline std::expected<HjbPoint, HjbError>
hjb_evaluate(const HjbParams& p, const HjbGrid& grid, double gamma,
             const std::vector<double>& rates) noexcept {
    auto r = hjb_detail::run(p, grid, gamma, false, &rates);
    if (!r) { return std::unexpected(r.error()); }
    return r->point();
}

} // namespace altair
