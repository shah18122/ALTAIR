// analytics/ewma.hpp -- exponentially weighted mean and variance, decayed in
// TIME rather than in ticks.
//
// P3-06b.
//
// THE WHOLE POINT: alpha IS NOT A CONSTANT.
//
// The EWMA everybody writes is `s += alpha * (x - s)` with a fixed alpha, and
// on a tick stream it is wrong in a way that does not look like a bug. A fixed
// alpha means the estimator's memory is measured in TICKS. Its time constant
// is therefore alpha^-1 multiplied by the average inter-tick interval -- and
// on NSE that interval varies by orders of magnitude between 09:15 and 13:00,
// between NIFTY and a mid-cap, and between a normal Tuesday and expiry day.
//
// So a fixed-alpha EWMA is a measuring instrument whose calibration changes
// with volume. Feed the SAME price path through it at two tick rates and it
// returns two different numbers; `tests/test_ewma.cpp` measures 199.3 against
// 139.3 for a step response over one second. A model trained on that feature
// learns the tick rate, which is a property of the exchange's message bus
// rather than of the market, and then mis-fires whenever liquidity changes.
//
// Here alpha is derived per update from the elapsed time on the TICK:
//
//     alpha = 1 - exp(-dt / tau)
//
// tau is a real time constant in nanoseconds, so the estimator answers "the
// last 200 milliseconds" rather than "the last 40 ticks", and it answers it
// identically at the open and at lunch. This matters more here than in most
// places because CLAUDE.md's Nyquist argument is precisely that order-book
// imbalance decays in 10-200 ms: a feature meant to track that has to have a
// time constant that means something.
//
// TIME COMES OFF THE TICK (rule 7). `update` takes a Timestamp and never
// consults a clock. A replay therefore produces bit-identical output to the
// live path, which is rule 6.
//
// OUT-OF-ORDER IS HANDLED, NOT ASSERTED. P2-04's normaliser deliberately does
// not require monotonic timestamps, so a backwards dt reaches here in normal
// operation. It is refused and counted, never folded in with a negative dt --
// which would produce alpha > 1 and a wildly overshooting estimate.

#pragma once

#include <core/time/timestamp.hpp>
#include <core/types/units.hpp>

#include <cmath>
#include <cstdint>
#include <expected>

namespace altair {

enum class EwmaError : std::uint8_t {
    /// No observation has been folded in yet, so there is nothing to report.
    /// Distinct from a zero value, which is a real estimate.
    Uninitialised,
    /// The tick's timestamp precedes the last one accepted. Refused and
    /// counted: a negative dt gives alpha > 1, which overshoots rather than
    /// averages.
    TimeWentBackwards,
    /// tau <= 0. An exponential average with no memory is not an average.
    NonPositiveTau,
    NotFinite
};

/// Exponentially weighted mean with a time constant.
///
/// SEEDED BY ITS FIRST OBSERVATION rather than by zero. The usual
/// zero-initialised EWMA is biased toward zero for the first few time
/// constants and needs the Adam-style `1 - prod(1-alpha)` correction to undo
/// it. That correction exists because Adam genuinely cannot observe before
/// step zero; here the first tick is a perfectly good estimate of the mean at
/// the moment it arrives, so the estimator starts unbiased and no correction
/// is needed. One less thing to apply twice or forget.
class Ewma {
public:
    /// PRECONDITION on tau: strictly positive. Enforced here so an invalid
    /// object cannot exist, rather than by every `update` re-checking it.
    [[nodiscard]] static std::expected<Ewma, EwmaError>
    create(Duration tau) noexcept {
        if (tau.raw() <= 0) {
            return std::unexpected(EwmaError::NonPositiveTau);
        }
        return Ewma{tau};
    }

    /// Fold in an observation stamped by the TICK. UNIT of x: caller's.
    ///
    /// Returns the updated estimate, so a caller that wants the value after
    /// an update does not have to make a second call and risk interleaving.
    ALTAIR_HOT std::expected<double, EwmaError>
    update(Timestamp ts, double x) noexcept {
        if (!std::isfinite(x)) {
            ++rejected_;
            return std::unexpected(EwmaError::NotFinite);
        }
        if (!have_) {
            // The first observation IS the estimate. See the class note.
            s_ = x;
            last_ = ts;
            have_ = true;
            ++count_;
            return s_;
        }
        const std::int64_t dt = (ts - last_).raw();
        if (dt < 0) {
            ++out_of_order_;
            return std::unexpected(EwmaError::TimeWentBackwards);
        }
        // dt == 0 gives alpha == 0, so a coincident observation contributes
        // nothing. For a TIME-weighted average that is the correct answer, not
        // a special case: weight is elapsed time, and this observation spans
        // none. Counted anyway, because "the estimate did not move" is worth
        // being able to explain.
        if (dt == 0) { ++coincident_; }

        const double alpha = 1.0 - std::exp(-static_cast<double>(dt)
                                            / static_cast<double>(tau_.raw()));
        s_ += alpha * (x - s_);
        last_ = ts;
        ++count_;
        if (!std::isfinite(s_)) { return std::unexpected(EwmaError::NotFinite); }
        return s_;
    }

    [[nodiscard]] std::expected<double, EwmaError> value() const noexcept {
        if (!have_) { return std::unexpected(EwmaError::Uninitialised); }
        return s_;
    }

    [[nodiscard]] bool initialised() const noexcept { return have_; }
    [[nodiscard]] Duration tau() const noexcept { return tau_; }
    /// Timestamp of the last accepted observation. PRECONDITION: initialised().
    [[nodiscard]] Timestamp last() const noexcept { return last_; }
    [[nodiscard]] std::uint64_t count() const noexcept { return count_; }
    /// Ticks refused for arriving before the previous one. Counted so a feed
    /// problem is visible rather than absorbed into the estimate.
    [[nodiscard]] std::uint64_t out_of_order() const noexcept {
        return out_of_order_;
    }
    /// Ticks accepted at exactly the previous timestamp, and therefore given
    /// zero weight.
    [[nodiscard]] std::uint64_t coincident() const noexcept {
        return coincident_;
    }
    [[nodiscard]] std::uint64_t rejected() const noexcept { return rejected_; }

    void reset() noexcept {
        const Duration t = tau_;
        *this = Ewma{t};
    }

private:
    explicit Ewma(Duration tau) noexcept : tau_(tau) {}

    Duration tau_{0};
    Timestamp last_{};
    double s_ = 0.0;
    bool have_ = false;
    std::uint64_t count_ = 0;
    std::uint64_t out_of_order_ = 0;
    std::uint64_t coincident_ = 0;
    std::uint64_t rejected_ = 0;
};

/// Exponentially weighted mean AND variance, sharing one time constant.
///
/// TWO CORRECTIONS, both of which the textbook recursion gets wrong and
/// neither of which announces itself.
///
/// 1. `diff` is taken against the PRE-update mean. Using the post-update mean
///    measures the deviation from a mean that has already moved toward the
///    point being measured, and it is badly biased: against a known variance
///    of 9.0, it returns 6.98 at alpha = 0.095 and 0.65 at alpha = 0.63.
///
/// 2. THE PLAIN RECURSION IS STILL BIASED LOW, even done right. This is the
///    exponential-weights analogue of dividing by n instead of n-1, and it is
///    much larger than the n/(n-1) case because exponential weights are so
///    concentrated. Measured against a true variance of 9.0:
///
///        alpha    uncorrected      corrected
///        0.095    8.52  (-5.3%)    8.97  (-0.3%)
///        0.632    4.82  (-46.4%)   8.97  (-0.3%)
///        0.865    2.14  (-76.2%)   8.97  (-0.3%)
///
///    A 46% low volatility estimate does not look broken. It looks like a
///    quiet market, and it sizes every position in the book too large.
///
/// The correction is the standard reliability factor for weighted samples,
/// `1 / (1 - sum(w^2)/sum(w)^2)`. Because alpha varies per tick here, the two
/// weight sums cannot use their fixed-alpha closed forms and are carried
/// recursively instead:
///
///     W <- (1-alpha) * W + alpha
///     D <- (1-alpha)^2 * D + 2*alpha*(1-alpha)*W        [ D = W^2 - sum(w^2) ]
///
/// D is carried directly rather than formed as `1 - V/W^2`, which cancels
/// catastrophically after a long tick gap; see the note in `update`.
///
/// Seeded at W = 1, D = 0 by the first observation, so one point correctly has
/// no variance rather than a variance of zero. On the second observation the
/// estimator returns
/// (x2-x1)^2 / 2, which is precisely the two-point sample variance; the test
/// asserts that exactly, because it pins the seeding and the correction
/// together in one number.
class EwmaVariance {
public:
    [[nodiscard]] static std::expected<EwmaVariance, EwmaError>
    create(Duration tau) noexcept {
        if (tau.raw() <= 0) {
            return std::unexpected(EwmaError::NonPositiveTau);
        }
        return EwmaVariance{tau};
    }

    ALTAIR_HOT std::expected<double, EwmaError>
    update(Timestamp ts, double x) noexcept {
        if (!std::isfinite(x)) {
            ++rejected_;
            return std::unexpected(EwmaError::NotFinite);
        }
        if (!have_) {
            mean_ = x;
            var_ = 0.0;
            w_ = 1.0;
            d_ = 0.0;       // W^2 - sum(w^2) = 0: one point, no spread
            last_ = ts;
            have_ = true;
            ++count_;
            return mean_;
        }
        const std::int64_t dt = (ts - last_).raw();
        if (dt < 0) {
            ++out_of_order_;
            return std::unexpected(EwmaError::TimeWentBackwards);
        }
        const double alpha = 1.0 - std::exp(-static_cast<double>(dt)
                                            / static_cast<double>(tau_.raw()));
        const double om = 1.0 - alpha;
        const double diff = x - mean_;          // against the PRE-update mean
        mean_ += alpha * diff;
        var_ = om * (var_ + alpha * diff * diff);
        // The reliability denominator D = W^2 - sum(w^2) is carried DIRECTLY,
        // never formed by subtraction. Its recursion follows from expanding
        // W_new^2 - V_new, and every term in it is positive:
        //
        //     D <- (1-alpha)^2 * D + 2*alpha*(1-alpha)*W
        //
        // Computing it as `1 - V/W^2` instead is catastrophic after a long
        // gap: alpha approaches 1, V approaches 1, and the subtraction of two
        // near-equal numbers throws away the answer. At a 20-tau gap that form
        // loses eight significant digits -- and a gap of many time constants is
        // ordinary on an illiquid counter, or on the first ticks after a halt,
        // which is exactly when a vol estimate is most likely to be acted on.
        d_ = om * om * d_ + 2.0 * alpha * om * w_;
        w_ = om * w_ + alpha;
        last_ = ts;
        ++count_;
        if (!std::isfinite(mean_) || !std::isfinite(var_)) {
            return std::unexpected(EwmaError::NotFinite);
        }
        return mean_;
    }

    [[nodiscard]] std::expected<double, EwmaError> mean() const noexcept {
        if (!have_) { return std::unexpected(EwmaError::Uninitialised); }
        return mean_;
    }

    /// Exponentially weighted variance, BIAS CORRECTED. UNIT: the square of
    /// the caller's. This is the one to use.
    [[nodiscard]] std::expected<double, EwmaError> variance() const noexcept {
        if (count_ < 2) { return std::unexpected(EwmaError::Uninitialised); }
        if (!(d_ > 0.0)) {
            // Every effective weight has collapsed onto one observation --
            // a gap of many time constants since the previous tick. There is
            // one point in the window, so there is no variance.
            return std::unexpected(EwmaError::Uninitialised);
        }
        return var_ * w_ * w_ / d_;
    }

    /// The uncorrected recursion, exposed only so the correction can be
    /// measured rather than believed. Do not size on this.
    [[nodiscard]] std::expected<double, EwmaError>
    population_variance() const noexcept {
        if (count_ < 2) { return std::unexpected(EwmaError::Uninitialised); }
        return var_;
    }

    /// Effective sample size, `sum(w)^2 / sum(w^2)`. Falls to 1 after a long
    /// gap and rises toward `2/alpha - 1` in steady state. Worth surfacing:
    /// a vol estimate resting on 1.3 effective observations is not a
    /// measurement, and only this number says so.
    [[nodiscard]] std::expected<double, EwmaError>
    effective_n() const noexcept {
        if (!have_) { return std::unexpected(EwmaError::Uninitialised); }
        // V = W^2 - D. Safe as a subtraction in this direction: D is the
        // small quantity, so nothing cancels.
        const double v = w_ * w_ - d_;
        if (!(v > 0.0)) { return std::unexpected(EwmaError::NotFinite); }
        return w_ * w_ / v;
    }

    [[nodiscard]] std::expected<double, EwmaError> stddev() const noexcept {
        const auto v = variance();
        if (!v) { return std::unexpected(v.error()); }
        return std::sqrt(*v);
    }

    [[nodiscard]] bool initialised() const noexcept { return have_; }
    [[nodiscard]] Duration tau() const noexcept { return tau_; }
    [[nodiscard]] std::uint64_t count() const noexcept { return count_; }
    [[nodiscard]] std::uint64_t out_of_order() const noexcept {
        return out_of_order_;
    }
    [[nodiscard]] std::uint64_t rejected() const noexcept { return rejected_; }

private:
    explicit EwmaVariance(Duration tau) noexcept : tau_(tau) {}

    Duration tau_{0};
    Timestamp last_{};
    double mean_ = 0.0;
    double var_ = 0.0;
    /// W = sum of the effective weights, and D = W^2 - sum(w^2), the
    /// reliability denominator. Both carried recursively because alpha varies
    /// per tick, so the fixed-alpha closed forms (W = 1, V = alpha/(2-alpha))
    /// do not apply. D is carried rather than derived -- see `update`.
    double w_ = 0.0;
    double d_ = 0.0;
    bool have_ = false;
    std::uint64_t count_ = 0;
    std::uint64_t out_of_order_ = 0;
    std::uint64_t rejected_ = 0;
};

/// Half-life of a time constant: the elapsed time over which an observation's
/// weight falls to one half. t_half = tau * ln 2.
///
/// Provided because desks specify EWMAs in half-lives and libraries take taus,
/// and converting in one's head is how a 200 ms feature becomes a 289 ms one.
[[nodiscard]] inline Duration half_life_of(Duration tau) noexcept {
    constexpr double kLn2 = 0.69314718055994530941723212145818;
    return Duration{static_cast<std::int64_t>(
        static_cast<double>(tau.raw()) * kLn2 + 0.5)};
}

/// The inverse: the tau that gives a requested half-life.
[[nodiscard]] inline Duration tau_for_half_life(Duration half_life) noexcept {
    constexpr double kInvLn2 = 1.44269504088896340735992468100189;
    return Duration{static_cast<std::int64_t>(
        static_cast<double>(half_life.raw()) * kInvLn2 + 0.5)};
}

} // namespace altair
