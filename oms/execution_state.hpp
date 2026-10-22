// oms/execution_state.hpp -- the VRC Execution State Model and its tools.
//
// D. Verma, "Market Impact Models for Small Funds: Practical Execution Alpha,
// Intraday Liquidity Dynamics, and the Microstructure of Trading Cost",
// Verma Research Capital whitepaper, 2026-01-11 (SSRN 6301920). Equation
// numbers below are the paper's.
//
// WHAT IS HERE, BY SECTION.
//
//   2.1  square-root impact                       esm_sqrt_impact      (1)
//   3.1  intraday U-shaped spread                 esm_intraday_spread  (9)
//   3.2  effective depth within c bps             esm_effective_depth  (10)
//   3.3  short-term volatility ratio              esm_vol_ratio        (11)
//   4.1  Execution Quality Score                  esm_score            (12)
//   4.2  regime and size scaling                  esm_regime, Table 1
//   4.3  regime-conditioned cost                  esm_cost             (13)
//   5.2  passive vs aggressive                    esm_passive_*        (15-16)
//   6.3  rebalancing frequency                    esm_rebalance_freq   (19)
//   7.1  option execution cost                    esm_option_cost      (20)
//   7.3  pre-event spread blow-out                esm_event_spread     (22)
//   8.3  exit-asymmetry sizing                    esm_exit_adjusted    (25)
//   9.3  abstain when edge < cost                 esm_abstain          (29)
//   10.1 Kelly net of cost                        esm_kelly_net        (31)
//   10.2 participation cap                        esm_liquidity_cap    (32)
//   11.2 opportunistic execution algorithm        esm_oea_child        (33)
//
// WHERE THE PAPER IS WRONG OR UNDER-SPECIFIED, AND WHAT IS DONE INSTEAD.
//
//   * Eq. 3 says the permanent-impact cost is gamma sum x_k^2 = gamma X^2.
//     The sum of squares is not X^2, and the Almgren-Chriss permanent cost
//     is gamma X^2 / 2 (independent of the split, which is the point the
//     paper is making). Eq. 6 writes the optimal trade with sinh where the
//     derivative of the sinh trajectory has cosh, and the text calls the
//     result "trading equal quantities". Neither is used: Almgren-Chriss is
//     shortfall.hpp, which has it right.
//   * Eq. 33 normalises the child weight by the sum of (2 - EQS_s)^beta over
//     the REMAINING intervals, which are in the future. As written it cannot
//     be computed. The causal version used here replaces each future EQS by
//     its expectation, 1 by construction (every input is standardised so 1
//     is the historical average), so each future interval contributes 1.
//   * Section 4.2 gives the boundaries 0.8, 1.2, 1.6 without saying which
//     side they fall on. Here the lower bound is inclusive: 0.8 is Normal,
//     1.2 is Thin, 1.6 is Stressed.
//   * Section 11.2 executes only when EQS < 1.0, while Table 1 lets the
//     Normal regime (up to 1.2) trade at 90%. The OEA follows section 11.2,
//     the sizing helper follows Table 1; both are the paper's.
//
// WHAT THE TEST FOUND ABOUT THE SLICER.
//
// Against TWAP under the paper's own eq. 13 cost, on simulated days whose
// EQS drifts between regimes, the OEA saves about 0.34 bp on an order of
// 0.05% of ADV and LOSES about 1.2 bp on the paper's own section 5.1 order
// (2.9% of ADV). Waiting for good windows saves spread; concentrating size
// in them costs concave impact, and half the days end in a forced finish.
// It is a small-order tool, which is what the paper says it is for.
//
// WHAT IS NOT HERE.
//
// Calibration. eta(regime), xi, the weights and the thresholds are VRC's,
// fitted on VRC's trades (US equities, futures and options). They are
// defaults to be refitted from ALTAIR's own fills, not NSE facts.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace altair {

// ---------------------------------------------------------------------------
// 2.1, 3.1-3.3 -- inputs
// ---------------------------------------------------------------------------

/// Eq. 1: I(Q) = eta sigma sqrt(Q / V), a fraction of price. `sigma` is the
/// volatility over the horizon V is measured on (daily with daily volume).
[[nodiscard]] inline double esm_sqrt_impact(double eta, double sigma, double q,
                                            double v) noexcept {
    if (!(v > 0.0) || q <= 0.0) { return 0.0; }
    return eta * sigma * std::sqrt(q / v);
}

/// Eq. 9: S(t) = Smin + (Sopen - Smin) e^{-a1 t} + (Sclose - Smin) e^{-a2 (T - t)}.
struct EsmSpreadCurve {
    double s_min = 0.0, s_open = 0.0, s_close = 0.0;
    double a1 = 0.0, a2 = 0.0;
    double session = 0.0;   ///< T, same unit as t

    [[nodiscard]] double at(double t) const noexcept {
        return s_min + (s_open - s_min) * std::exp(-a1 * t)
             + (s_close - s_min) * std::exp(-a2 * (session - t));
    }
};
[[nodiscard]] inline double esm_intraday_spread(const EsmSpreadCurve& c,
                                                double t) noexcept {
    return c.at(t);
}

/// One price level of a book side.
struct EsmLevel {
    double price = 0.0;
    double qty = 0.0;
};

/// Eq. 10: quantity quoted within c basis points of the mid.
[[nodiscard]] inline double esm_effective_depth(std::span<const EsmLevel> side,
                                                double mid, double c_bps) noexcept {
    const double band = c_bps * mid / 10000.0;
    double d = 0.0;
    for (const EsmLevel& l : side) {
        if (std::fabs(l.price - mid) <= band) { d += l.qty; }
    }
    return d;
}

/// Eq. 11: current short-term realised vol over its time-of-day median.
[[nodiscard]] inline double esm_vol_ratio(double sigma_now,
                                          double sigma_median) noexcept {
    return sigma_median > 0.0 ? sigma_now / sigma_median : 0.0;
}

/// Section 3.3: above this VolRatio non-urgent executions are deferred.
inline constexpr double kEsmDeferVolRatio = 1.5;

// ---------------------------------------------------------------------------
// 4 -- the Execution State Model
// ---------------------------------------------------------------------------

/// The four standardised inputs (section 4.1). 1 is the historical average
/// for that instrument and time of day.
struct EsmInputs {
    double rel_spread = 1.0;   ///< S_t / S-bar
    double rel_depth = 1.0;    ///< D_eff,t / D-bar_eff (HIGHER is better)
    double vol_ratio = 1.0;    ///< eq. 11
    double ofi = 0.0;          ///< signed order-flow imbalance, standardised
};

/// Eq. 12 weights, VRC's calibration.
struct EsmWeights {
    double spread = 0.30, depth = 0.25, vol = 0.25, ofi = 0.20;
};

/// Eq. 12: EQS = wS S~ + wD (2 - D~) + wV VolRatio + wOFI |OFI|.
/// Every term rises as conditions worsen; 1 is an average day.
[[nodiscard]] inline double esm_score(const EsmInputs& in,
                                      const EsmWeights& w = {}) noexcept {
    return w.spread * in.rel_spread + w.depth * (2.0 - in.rel_depth)
         + w.vol * in.vol_ratio + w.ofi * std::fabs(in.ofi);
}

enum class EsmRegime : std::uint8_t { Liquid, Normal, Thin, Stressed };

/// Table 1.
[[nodiscard]] inline EsmRegime esm_regime(double eqs) noexcept {
    if (eqs < 0.8) { return EsmRegime::Liquid; }
    if (eqs < 1.2) { return EsmRegime::Normal; }
    if (eqs < 1.6) { return EsmRegime::Thin; }
    return EsmRegime::Stressed;
}

/// Table 1: share of the target size to trade in each regime.
[[nodiscard]] inline double esm_size_fraction(EsmRegime r) noexcept {
    switch (r) {
        case EsmRegime::Liquid: return 1.0;
        case EsmRegime::Normal: return 0.9;
        case EsmRegime::Thin: return 0.7;
        case EsmRegime::Stressed: return 0.4;
    }
    return 0.4;
}

/// Eq. 13 parameters. eta per regime and xi are calibrated per instrument
/// (section 4.3); the defaults rise with the regime and are placeholders.
struct EsmCostParams {
    double eta[4] = {0.5, 0.6, 0.8, 1.2};
    double xi = 0.0;   ///< book-walking cost, from the slope beyond level 1
};

/// Eq. 13: C = S_t/2 + eta(regime) sigma_t sqrt(Q/V) + 1[Q > D] xi (Q-D)^2 / D.
/// `spread` and the result are fractions of price; `depth` is D_eff.
[[nodiscard]] inline double esm_cost(double q, double v, double spread,
                                     double sigma, double depth, EsmRegime r,
                                     const EsmCostParams& p = {}) noexcept {
    double c = 0.5 * spread
             + p.eta[static_cast<int>(r)] * esm_sqrt_impact(1.0, sigma, q, v);
    if (q > depth && depth > 0.0) { c += p.xi * (q - depth) * (q - depth) / depth; }
    return c;
}

// ---------------------------------------------------------------------------
// 5.2, 6.3, 7, 8.3, 9.3, 10 -- decisions
// ---------------------------------------------------------------------------

/// Eq. 15: E[benefit of posting] = S/2 P(fill) - dp (1 - P(fill)).
[[nodiscard]] inline double esm_passive_benefit(double spread, double p_fill,
                                                double adverse) noexcept {
    return 0.5 * spread * p_fill - adverse * (1.0 - p_fill);
}

/// Eq. 16: post passively when P(fill) > dp / (dp + S/2).
[[nodiscard]] inline bool esm_passive_preferred(double spread, double p_fill,
                                                double adverse) noexcept {
    const double den = adverse + 0.5 * spread;
    return den > 0.0 && p_fill > adverse / den;
}

/// Eq. 19: f* = f_signal min(1, sqrt(rho sigma^2 / (eta f_signal qbar^2))).
[[nodiscard]] inline double esm_rebalance_freq(double f_signal, double rho,
                                               double sigma, double eta,
                                               double qbar) noexcept {
    const double den = eta * f_signal * qbar * qbar;
    if (!(den > 0.0)) { return f_signal; }
    return f_signal * std::min(1.0, std::sqrt(rho * sigma * sigma / den));
}

/// Eq. 20: C = S_IV vega / 2 + kappa_IV D / (vega OI).
[[nodiscard]] inline double esm_option_cost(double spread_iv, double vega,
                                            double kappa_iv, double delta_value,
                                            double open_interest) noexcept {
    double c = 0.5 * spread_iv * vega;
    if (vega > 0.0 && open_interest > 0.0) {
        c += kappa_iv * delta_value / (vega * open_interest);
    }
    return c;
}

/// Eq. 22: S_IV(tau) = S-bar_IV (1 + psi e^{-mu tau}).
[[nodiscard]] inline double esm_event_spread(double normal, double psi, double mu,
                                             double tau_event) noexcept {
    return normal * (1.0 + psi * std::exp(-mu * tau_event));
}

/// Eq. 25: Q_adj = Q (mu - C_entry - E[C_exit | adverse]) / (mu - 2 C-bar).
/// Zero when the adjusted edge is gone; the naive size if the naive edge is.
[[nodiscard]] inline double esm_exit_adjusted(double q_naive, double mu,
                                              double c_entry, double c_exit_adverse,
                                              double c_avg) noexcept {
    const double den = mu - 2.0 * c_avg;
    if (!(den > 0.0)) { return 0.0; }
    return q_naive * std::max(0.0, (mu - c_entry - c_exit_adverse) / den);
}

/// Eq. 29: do not trade when the expected edge is below the round trip.
[[nodiscard]] inline bool esm_abstain(double mu, double c_entry,
                                      double c_exit) noexcept {
    return mu < c_entry + c_exit;
}

/// Eq. 31: f* = (mu - C_total) / sigma^2, floored at zero.
[[nodiscard]] inline double esm_kelly_net(double mu, double c_total,
                                          double sigma) noexcept {
    if (!(sigma > 0.0)) { return 0.0; }
    return std::max(0.0, (mu - c_total) / (sigma * sigma));
}

/// Section 10.2: VRC's participation caps.
inline constexpr double kEsmMaxParticipationEquity = 0.05;
inline constexpr double kEsmMaxParticipationIndexFut = 0.02;

/// Eq. 32: Q_max = V tau_max p_max.
[[nodiscard]] inline double esm_liquidity_cap(double adv, double days,
                                              double p_max) noexcept {
    return std::max(0.0, adv * days * p_max);
}

// ---------------------------------------------------------------------------
// 11.2 -- the opportunistic execution algorithm
// ---------------------------------------------------------------------------

/// Section 11.2: trade only below this EQS.
inline constexpr double kEsmOeaMaxScore = 1.0;

/// Eq. 33, made causal (see the header): the child for this interval, given
/// the remaining quantity, the current EQS, the number of intervals AFTER
/// this one before T_max, and beta. At the last interval everything left
/// goes, aggressively, whatever the regime.
[[nodiscard]] inline double esm_oea_child(double remaining, double eqs,
                                          std::size_t intervals_after,
                                          double beta) noexcept {
    if (remaining <= 0.0) { return 0.0; }
    if (intervals_after == 0) { return remaining; }
    if (!(eqs < kEsmOeaMaxScore)) { return 0.0; }
    const double now = std::pow(std::max(0.0, 2.0 - eqs), beta);
    // E[(2 - EQS_s)^beta] at E[EQS_s] = 1 is 1 per future interval.
    const double w = now / (now + static_cast<double>(intervals_after));
    return remaining * w;
}

} // namespace altair
