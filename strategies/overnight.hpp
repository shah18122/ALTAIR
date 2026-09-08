// strategies/overnight.hpp -- hold the index between the close and the open,
// and sit out the session.
//
// P22-01.
//
// WHY THIS EXISTS.
//
// P21-03 decomposed eleven years of NIFTY and found the session hours were a
// net drag while every rupee of the index's move arrived overnight. That is a
// decomposition, not a strategy. This is the strategy, and building it is the
// only way to find out whether the decomposition was tradeable or merely true.
//
// READ THIS BEFORE READING A SINGLE NUMBER THIS FILE PRODUCES.
//
// The gross edge here is large and highly significant, and it is ALSO the most
// fragile result in this project, because the whole of it lives inside an
// execution assumption that this code cannot test:
//
//   1. YOU CANNOT BUY AT THE PRINTED CLOSE. NSE's closing price is derived
//      from the last half hour, and the index close is computed from
//      constituent closes. What you can actually do is trade the future at
//      15:29:59 at whatever is quoted then.
//
//   2. YOU CANNOT SELL AT THE PRINTED OPEN. The 09:15 print is the outcome of
//      the pre-open call auction. You can submit into it; you cannot choose
//      your fill.
//
//   3. AND THE DATA SAYS EXACTLY WHAT THAT COSTS. The first five minutes of
//      the session -- 09:15 to 09:20 -- average -4.07 bps on this history.
//      The gap starts filling immediately. If you miss the open and get out
//      five minutes later, you lose nearly forty per cent of the edge, and
//      `Exit::FirstBar` exists to measure precisely that.
//
// So the honest form of the question is not "does the overnight gap pay" but
// "does it still pay once you cannot have the two prices the backtest used".
// Both exits are computed, always, side by side.
//
// THE ARITHMETIC IS AN IDENTITY, NOT A FIT.
//
// Nothing here is estimated. Given the mean overnight return m, the mean
// close-to-close return h, and an all-in round-trip cost RT:
//
//     net over buy-and-hold, per session  =  (m - RT) - h  =  |session drag| - RT
//
// because m - h is exactly minus the session return. So the strategy beats
// holding the index if and only if the round trip costs less than the session
// drag it avoids. There is no parameter in that sentence, which is why it is
// the sentence to remember.
//
// AND THE PAYOFF IS LEFT-SKEWED, WHICH IS THE OTHER HALF OF THE RISK.
//
// Two thirds of nights are up, and the MEDIAN night pays more than the MEAN
// night. That combination has one meaning: most nights pay a little and a few
// take a lot back. The strategy is short gap risk -- it is selling overnight
// insurance and collecting the premium -- and `nights_per_bad_night()` exists
// so that fact is a number on the page rather than an inference from a Sharpe.
//
// COST IS QUOTED AS AN ALL-IN ROUND TRIP HERE, NOT PER UNIT OF TURNOVER.
//
// `momentum.hpp` charges `cost_bps` per unit of turnover, where a 0 -> 1 entry
// is one unit; a full in-and-out is therefore TWO units. This strategy does
// exactly one in-and-out per night, so quoting the round trip directly removes
// a factor of two that is easy to drop and expensive to drop. For NIFTY
// futures the round trip is dominated by STT at 0.05% on the sell side alone
// -- see the sweep in the acceptance test rather than any constant here, since
// rule 1 forbids a rate literal and `config/charges.toml` owns the real ones.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <expected>
#include <limits>
#include <string>
#include <vector>

namespace altair {

/// One trading session, reduced to the three prices this strategy can act on.
struct SessionBar {
    /// First traded price of the session -- the pre-open auction result.
    double open = 0.0;
    /// Last traded price of the session.
    double close = 0.0;
    /// Close of the FIRST intraday bar. The price you get if you could not
    /// hit the auction and had to exit a few minutes in. Absence is not zero:
    /// leave it at 0.0 and `Exit::FirstBar` is refused rather than silently
    /// falling back to `open`.
    double first_bar_close = 0.0;
    /// YYYY-MM-DD, optional. Carried only so a result can be broken down by
    /// period; nothing in the strategy reads it, and a strategy that read a
    /// date would be reading a wall clock, which rule 7 forbids.
    std::string day;
};

enum class OvernightError : std::uint8_t {
    TooFewSessions,
    BadParameter,
    /// `Exit::FirstBar` was asked for on data that has no first-bar price.
    MissingFirstBar,
    /// A non-positive price, which would make a log return meaningless.
    BadPrice
};

/// Where the morning exit happens.
enum class Exit : std::uint8_t {
    /// The pre-open auction print. The optimistic assumption.
    Open,
    /// The close of the first intraday bar -- 09:20 on five-minute data. What
    /// you get if the auction did not fill you, and the measurement that
    /// decides whether this strategy is real.
    FirstBar
};

struct OvernightSpec {
    Exit exit = Exit::Open;
    /// Take the night only when the session that PRECEDED it returned less
    /// than this, in bps. Infinity takes every night.
    ///
    /// This is the one lever, and it is a lever on the CONDITIONAL MEAN, not
    /// on frequency -- see `breakeven_rt_bps` below for why trading less often
    /// cannot by itself make the strategy affordable.
    double session_below_bps = std::numeric_limits<double>::infinity();
    /// Position size, in units of the index.
    double size = 1.0;

    [[nodiscard]] bool valid() const noexcept {
        return size > 0.0 && !std::isnan(session_below_bps);
    }
};

struct OvernightResult {
    std::size_t sessions = 0;
    /// Nights the filter actually took.
    std::size_t taken = 0;

    /// Mean overnight return across the TAKEN nights, bps. The strategy's
    /// gross edge per trade.
    double mean_taken_bps = 0.0;
    /// Its standard deviation and t-statistic, across taken nights.
    double sd_taken_bps = 0.0;
    double t_taken = 0.0;

    /// Mean close-to-close return across ALL sessions, bps. Buy-and-hold, the
    /// control, which pays no meaningful turnover because it trades once.
    double hold_bps = 0.0;
    /// Mean session (open-to-close) return across all sessions, bps.
    double session_bps = 0.0;

    /// Fraction of taken nights that were positive.
    double hit_rate = 0.0;

    /// THE SHAPE OF THE PAYOFF, not just its centre.
    ///
    /// These exist because a 67% hit rate and a positive mean describe two
    /// completely different strategies depending on the skew, and this one is
    /// LEFT-skewed: its median return is HIGHER than its mean, meaning most
    /// nights pay a little and a few take a lot back. That is the signature of
    /// selling insurance, and a report that gave only mean, t and hit rate
    /// would have described a strategy that picks up pennies without saying
    /// what the steamroller looks like.
    double median_bps = 0.0;
    double p05_bps = 0.0;
    double p95_bps = 0.0;
    double worst_bps = 0.0;

    /// How many average nights one 5th-percentile night gives back.
    ///
    /// The single most legible risk number here: "a bad night costs N good
    /// ones" is a sentence a person can weigh, and a Sharpe is not.
    [[nodiscard]] double nights_per_bad_night() const noexcept {
        return mean_taken_bps > 0.0 ? -p05_bps / mean_taken_bps : 0.0;
    }

    /// Gross return per SESSION -- not per trade -- so it is comparable with
    /// `hold_bps`. Selective specs are flat on the nights they skip and this
    /// number says so.
    [[nodiscard]] double gross_per_session_bps() const noexcept {
        return sessions > 0
            ? mean_taken_bps * static_cast<double>(taken)
                  / static_cast<double>(sessions)
            : 0.0;
    }

    /// Net per session at an all-in round-trip cost of `rt` bps.
    [[nodiscard]] double net_at(double rt) const noexcept {
        return sessions > 0
            ? (mean_taken_bps - rt) * static_cast<double>(taken)
                  / static_cast<double>(sessions)
            : 0.0;
    }

    /// Net per session OVER buy-and-hold. The number that decides anything.
    [[nodiscard]] double excess_at(double rt) const noexcept {
        return net_at(rt) - hold_bps;
    }

    /// The round trip at which the strategy stops making money at all.
    ///
    /// It is exactly the mean overnight return of the taken nights, and it is
    /// INDEPENDENT OF HOW MANY NIGHTS ARE TAKEN: trading half as often halves
    /// the gross and halves the cost, leaving the ratio alone. A filter can
    /// only help by raising the conditional mean, never by trading less.
    [[nodiscard]] double breakeven_rt_bps() const noexcept {
        return mean_taken_bps;
    }

    /// The round trip at which it stops beating buy-and-hold.
    ///
    /// Unlike the one above, this DOES depend on how often the strategy
    /// trades, and that asymmetry is the point: skipping a night means being
    /// flat, and being flat on a drifting index forgoes the drift. Selectivity
    /// is not free.
    [[nodiscard]] double breakeven_rt_vs_hold_bps() const noexcept {
        return taken > 0
            ? mean_taken_bps - hold_bps * static_cast<double>(sessions)
                                   / static_cast<double>(taken)
            : -std::numeric_limits<double>::infinity();
    }
};

/// The three return series a session decomposes into, in bps.
///
/// `overnight[i] + session[i] == close_to_close[i]` exactly, by construction,
/// and the acceptance test asserts it. That identity is what makes the
/// benchmark arithmetic above an identity rather than an approximation.
struct SessionReturns {
    std::vector<double> overnight;        // previous close -> this open
    std::vector<double> first_bar;        // previous close -> this 09:20
    std::vector<double> session;          // this open -> this close
    std::vector<double> close_to_close;   // previous close -> this close
};

[[nodiscard]] inline std::expected<SessionReturns, OvernightError>
session_returns(const std::vector<SessionBar>& bars) {
    if (bars.size() < 100) {
        return std::unexpected(OvernightError::TooFewSessions);
    }
    SessionReturns out;
    const std::size_t n = bars.size() - 1;
    out.overnight.reserve(n);
    out.first_bar.reserve(n);
    out.session.reserve(n);
    out.close_to_close.reserve(n);
    for (std::size_t i = 1; i < bars.size(); ++i) {
        const SessionBar& p = bars[i - 1];
        const SessionBar& b = bars[i];
        if (!(p.close > 0.0) || !(b.open > 0.0) || !(b.close > 0.0)) {
            return std::unexpected(OvernightError::BadPrice);
        }
        out.overnight.push_back(10000.0 * std::log(b.open / p.close));
        out.session.push_back(10000.0 * std::log(b.close / b.open));
        out.close_to_close.push_back(10000.0 * std::log(b.close / p.close));
        out.first_bar.push_back(
            b.first_bar_close > 0.0
                ? 10000.0 * std::log(b.first_bar_close / p.close)
                : std::nan(""));
    }
    return out;
}

/// Run the overnight strategy over a session series.
///
/// CAUSALITY. The decision for night i is taken at the close of session i,
/// from session i's own open-to-close return, which is complete at that
/// moment. Night i's outcome is not consulted, and the acceptance test proves
/// it by perturbing the outcome and checking the decision did not move.
[[nodiscard]] inline std::expected<OvernightResult, OvernightError>
run_overnight(const std::vector<SessionBar>& bars, const OvernightSpec& spec) {
    if (!spec.valid()) { return std::unexpected(OvernightError::BadParameter); }
    const auto r = session_returns(bars);
    if (!r) { return std::unexpected(r.error()); }

    const std::vector<double>& night =
        spec.exit == Exit::Open ? r->overnight : r->first_bar;
    if (spec.exit == Exit::FirstBar) {
        for (const double v : night) {
            if (std::isnan(v)) {
                return std::unexpected(OvernightError::MissingFirstBar);
            }
        }
    }

    OvernightResult out;
    out.sessions = night.size();

    double sum_hold = 0.0, sum_sess = 0.0;
    for (std::size_t i = 0; i < night.size(); ++i) {
        sum_hold += r->close_to_close[i];
        sum_sess += r->session[i];
    }
    const double n = static_cast<double>(night.size());
    out.hold_bps = sum_hold / n;
    out.session_bps = sum_sess / n;

    // The filter reads session i's OWN return, which ends at the close, i.e.
    // strictly before night i begins. `session[i]` is that return; night i is
    // `night[i]`. Same index, different halves of the day.
    std::vector<double> taken;
    taken.reserve(night.size());
    std::size_t wins = 0;
    for (std::size_t i = 0; i < night.size(); ++i) {
        if (!(r->session[i] < spec.session_below_bps)) { continue; }
        taken.push_back(night[i] * spec.size);
        if (night[i] > 0.0) { ++wins; }
    }
    out.taken = taken.size();
    if (taken.empty()) { return out; }

    double s = 0.0;
    for (const double v : taken) { s += v; }
    const double k = static_cast<double>(taken.size());
    out.mean_taken_bps = s / k;
    out.hit_rate = static_cast<double>(wins) / k;

    if (taken.size() > 1) {
        double s2 = 0.0;
        for (const double v : taken) {
            const double d = v - out.mean_taken_bps;
            s2 += d * d;
        }
        out.sd_taken_bps = std::sqrt(s2 / (k - 1.0));
        const double se = out.sd_taken_bps / std::sqrt(k);
        out.t_taken = se > 0.0 ? out.mean_taken_bps / se : 0.0;

        std::vector<double> sorted = taken;
        std::sort(sorted.begin(), sorted.end());
        const auto at = [&sorted](double q) {
            const double idx = q * static_cast<double>(sorted.size() - 1);
            const std::size_t lo = static_cast<std::size_t>(idx);
            const std::size_t hi = std::min(lo + 1, sorted.size() - 1);
            const double f = idx - static_cast<double>(lo);
            return sorted[lo] * (1.0 - f) + sorted[hi] * f;
        };
        out.median_bps = at(0.50);
        out.p05_bps = at(0.05);
        out.p95_bps = at(0.95);
        out.worst_bps = sorted.front();
    }
    return out;
}

/// One calendar period's worth of the decomposition.
struct PeriodStat {
    std::string label;
    std::size_t n = 0;
    double overnight_bps = 0.0;
    double overnight_t = 0.0;
    double session_bps = 0.0;
    double hold_bps = 0.0;
    /// The round trip this period could have borne against buy-and-hold.
    /// Identical to `-session_bps` by the identity, and printed anyway
    /// because seeing the two agree in every row is the check.
    [[nodiscard]] double breakeven_vs_hold_bps() const noexcept {
        return overnight_bps - hold_bps;
    }
};

/// Break the decomposition down by calendar year.
///
/// IS IT ONE EPISODE, AND IS IT STILL HAPPENING? Those are the two questions a
/// full-sample mean cannot answer and the two that decide whether a result is
/// worth acting on. The returns are computed ONCE over the whole series and
/// then grouped, so no year loses its first night to a slice boundary.
///
/// Requires `SessionBar::day`; sessions without one are skipped rather than
/// bucketed into a year they might not belong to.
[[nodiscard]] inline std::expected<std::vector<PeriodStat>, OvernightError>
by_year(const std::vector<SessionBar>& bars) {
    const auto r = session_returns(bars);
    if (!r) { return std::unexpected(r.error()); }

    std::vector<std::string> labels;
    std::vector<std::vector<double>> on, se, cc;
    for (std::size_t i = 1; i < bars.size(); ++i) {
        if (bars[i].day.size() < 4) { continue; }
        const std::string y = bars[i].day.substr(0, 4);
        if (labels.empty() || labels.back() != y) {
            labels.push_back(y);
            on.emplace_back();
            se.emplace_back();
            cc.emplace_back();
        }
        on.back().push_back(r->overnight[i - 1]);
        se.back().push_back(r->session[i - 1]);
        cc.back().push_back(r->close_to_close[i - 1]);
    }

    std::vector<PeriodStat> out;
    out.reserve(labels.size());
    for (std::size_t j = 0; j < labels.size(); ++j) {
        if (on[j].size() < 30) { continue; }
        PeriodStat ps;
        ps.label = labels[j];
        ps.n = on[j].size();
        const double n = static_cast<double>(ps.n);
        double a = 0.0, b = 0.0, c = 0.0;
        for (std::size_t i = 0; i < on[j].size(); ++i) {
            a += on[j][i];
            b += se[j][i];
            c += cc[j][i];
        }
        ps.overnight_bps = a / n;
        ps.session_bps = b / n;
        ps.hold_bps = c / n;
        double s2 = 0.0;
        for (const double v : on[j]) {
            const double d = v - ps.overnight_bps;
            s2 += d * d;
        }
        const double se_mean = std::sqrt(s2 / (n - 1.0)) / std::sqrt(n);
        ps.overnight_t = se_mean > 0.0 ? ps.overnight_bps / se_mean : 0.0;
        out.push_back(ps);
    }
    return out;
}

} // namespace altair
