// strategies/pairs_futures.hpp -- a pair hedged with futures, walked forward.
//
// THE METHOD, AND WHERE EACH PART COMES FROM (research/papers/index.md):
//   * Formation and trading windows that roll forward, nothing estimated on
//     the days it trades (Gatev, Goetzmann & Rouwenhorst 2006).
//   * The relationship is log A = alpha + beta log B + e, tested with
//     Engle-Granger at MacKinnon's critical values (Engle & Granger 1987;
//     strategies/cointegration.hpp). A window whose residual is not stationary
//     at 5 % is NOT traded -- a high correlation is not a reason (Vidyamurthy
//     2004; Krauss 2017).
//   * The spread's Ornstein-Uhlenbeck half-life sets the time stop and rejects
//     a spread too slow to revert inside the window (Elliott, van der Hoek &
//     Malcolm 2005; Avellaneda & Lee 2010).
//   * Enter at |z| >= entry, exit at |z| <= exit, stop at |z| >= stop; z uses
//     the formation window's mean and sd, frozen.
//
// THE HEDGE IS IN WHOLE LOTS. Leg A is one lot; leg B is the whole number of
// lots nearest beta x (A's notional) / (one lot of B's notional), at least one.
// The hedge actually held is therefore not exactly beta -- both are reported.
// Lot sizes come from the instrument master, never a literal.
//
// FUTURES ARE PRICED AS THE UNDERLYING. The dataset has no continuous stock
// futures; a near-month future tracks its underlying to within the basis,
// which this ignores. Each monthly expiry a position is held across costs a
// roll -- both legs closed and reopened -- which the caller prices.

#pragma once

#include <strategies/cointegration.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace altair {

struct PairsPolicy {
    std::size_t formation = 250;   ///< trading days the relationship is estimated on
    std::size_t trading = 60;      ///< days it is then traded, frozen
    double entry_z = 2.0;
    double exit_z = 0.5;
    double stop_z = 4.0;
    double max_half_life = 30.0;   ///< days; a slower spread is not traded
    std::size_t adf_lags = 1;
};

struct PairsWindow {
    std::size_t start = 0, end = 0;   ///< trading rows [start, end)
    bool traded = false;
    std::string reason;               ///< why not, when not
    double alpha = 0.0, beta = 0.0, mu = 0.0, sd = 0.0;
    double adf_t = 0.0, critical = 0.0, half_life = 0.0, correlation = 0.0;
};

struct PairsTrade {
    std::size_t window = 0, entry = 0, exit = 0;   ///< row indices
    int side = 0;                                   ///< +1 long A / short B, -1 the reverse
    double z_entry = 0.0, z_exit = 0.0;
    long lots_a = 0, lots_b = 0;
    double a_in = 0.0, a_out = 0.0, b_in = 0.0, b_out = 0.0;
    double hedge_beta = 0.0;                        ///< the window's beta
    double held_ratio = 0.0;                        ///< B's notional / A's notional actually held
    double gross = 0.0;                             ///< rupees
    std::vector<std::size_t> rolls;                 ///< rows at whose close the position was rolled
    std::string exit_reason;                        ///< mean, stop, time, window end
};

struct PairsResult {
    std::vector<PairsWindow> windows;
    std::vector<PairsTrade> trades;
};

enum class PairsError : std::uint8_t { BadPolicy, BadSeries };

[[nodiscard]] inline const char* pairs_error_text(PairsError e) noexcept {
    return e == PairsError::BadPolicy ? "the window lengths and z thresholds are inconsistent"
                                      : "the two series must be the same length, positive and finite";
}

/// Walk the pair forward. `day[i]` is row i's trading day (any day count);
/// `roll_days` are the futures expiries.
[[nodiscard]] inline std::expected<PairsResult, PairsError>
pairs_walk_forward(std::span<const std::int64_t> day, std::span<const double> a, std::span<const double> b,
                   double lot_a, double lot_b, const std::set<std::int64_t>& roll_days, const PairsPolicy& p) {
    if (p.formation < 30 || p.formation > kMaxPairWindow || p.trading == 0 || !(p.entry_z > p.exit_z)
        || !(p.exit_z >= 0.0) || !(p.stop_z > p.entry_z) || !(p.max_half_life > 0.0) || !(lot_a > 0.0)
        || !(lot_b > 0.0)) {
        return std::unexpected(PairsError::BadPolicy);
    }
    const std::size_t n = a.size();
    if (b.size() != n || day.size() != n) { return std::unexpected(PairsError::BadSeries); }
    for (std::size_t i = 0; i < n; ++i) {
        if (!(a[i] > 0.0) || !(b[i] > 0.0) || !std::isfinite(a[i]) || !std::isfinite(b[i])) {
            return std::unexpected(PairsError::BadSeries);
        }
    }
    PairsResult out;
    std::vector<double> ly(n), lx(n);
    for (std::size_t i = 0; i < n; ++i) { ly[i] = std::log(a[i]); lx[i] = std::log(b[i]); }

    for (std::size_t s = p.formation; s < n; s += p.trading) {
        PairsWindow w;
        w.start = s;
        w.end = std::min(n, s + p.trading);
        const double* y = ly.data() + (s - p.formation);
        const double* x = lx.data() + (s - p.formation);
        const auto eg = engle_granger(y, x, p.formation, p.adf_lags);
        if (!eg) {
            w.reason = "Engle-Granger refused";
            out.windows.push_back(w);
            continue;
        }
        w.alpha = eg->hedge.alpha;
        w.beta = eg->hedge.beta;
        w.adf_t = eg->residual_test.t_stat;
        w.critical = eg->critical_5pct;
        w.correlation = eg->level_correlation;
        std::vector<double> e(p.formation);
        double m = 0.0;
        for (std::size_t k = 0; k < p.formation; ++k) { e[k] = y[k] - w.alpha - w.beta * x[k]; m += e[k]; }
        m /= static_cast<double>(p.formation);
        double ss = 0.0;
        for (const double v : e) { ss += (v - m) * (v - m); }
        w.mu = m;
        w.sd = std::sqrt(ss / static_cast<double>(p.formation - 1));
        const auto hl = half_life(e.data(), p.formation);
        w.half_life = hl ? *hl : std::numeric_limits<double>::infinity();
        if (!eg->cointegrated) { w.reason = "not cointegrated at 5 %"; }
        else if (!(w.beta > 0.0)) { w.reason = "hedge ratio not positive"; }
        else if (!hl || *hl > p.max_half_life) { w.reason = "half-life too long"; }
        else if (!(w.sd > 0.0)) { w.reason = "spread has no variance"; }
        else { w.traded = true; }
        const std::size_t wi = out.windows.size();
        out.windows.push_back(w);
        if (!w.traded) { continue; }

        const std::size_t max_hold = static_cast<std::size_t>(std::ceil(2.0 * w.half_life));
        bool open = false;
        PairsTrade t;
        for (std::size_t i = w.start; i < w.end; ++i) {
            const double z = (ly[i] - w.alpha - w.beta * lx[i] - w.mu) / w.sd;
            if (!open) {
                if (i + 1 < w.end && std::fabs(z) >= p.entry_z && std::fabs(z) < p.stop_z) {
                    t = PairsTrade{};
                    t.window = wi;
                    t.entry = i;
                    t.side = z > 0.0 ? -1 : 1;   // spread rich: short A, long B
                    t.z_entry = z;
                    t.lots_a = 1;
                    const double want = w.beta * lot_a * a[i] / (lot_b * b[i]);
                    t.lots_b = std::max(1L, std::lround(want));
                    t.a_in = a[i];
                    t.b_in = b[i];
                    t.hedge_beta = w.beta;
                    t.held_ratio = static_cast<double>(t.lots_b) * lot_b * b[i] / (lot_a * a[i]);
                    open = true;
                }
                continue;
            }
            const bool last = i + 1 == w.end;
            const char* why = std::fabs(z) <= p.exit_z ? "mean"
                            : std::fabs(z) >= p.stop_z ? "stop"
                            : i - t.entry >= max_hold ? "time"
                            : last ? "window end" : nullptr;
            if (why == nullptr) {
                if (roll_days.contains(day[i])) { t.rolls.push_back(i); }   // held through this expiry's close
                continue;
            }
            t.exit = i;
            t.z_exit = z;
            t.a_out = a[i];
            t.b_out = b[i];
            t.exit_reason = why;
            t.gross = static_cast<double>(t.side)
                    * (static_cast<double>(t.lots_a) * lot_a * (t.a_out - t.a_in)
                       - static_cast<double>(t.lots_b) * lot_b * (t.b_out - t.b_in));
            out.trades.push_back(t);
            open = false;
        }
    }
    return out;
}

} // namespace altair
