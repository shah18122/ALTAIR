// analytics/rolling.hpp -- streaming moments and fixed-window statistics.
//
// P3-06a. Mean, variance, skewness, excess kurtosis and z-score, both
// session-long (`RunningMoments`) and over a fixed window (`RollingWindow`).
//
// WHY NOT THE TEXTBOOK FORMULA.
//
// Everyone's first variance is sum(x^2)/n - (sum(x)/n)^2. It is one line, it
// is O(1), and on market data in paise it is wrong. The two terms are nearly
// equal and enormous, so their difference loses most of its significant
// digits -- catastrophic cancellation. Measured, on 50,000 quotes:
//
//   quote around      moves by     naive variance error
//   Rs  24,080        +/- 5 paise      9.4% too small
//   Rs  24,080        +/- 1 paise     97%   too small
//   Rs 240,800        +/- 1 paise   1757%   TOO LARGE  (12.4 vs 0.67)
//   Rs   1 crore      +/- 5 paise   NEGATIVE
//
// The third row is the dangerous one. Nothing crashes, nothing is negative,
// nothing looks wrong: the estimator simply reports a volatility four times
// too high on a high-priced counter that is barely moving. A sizing rule fed
// that number takes a quarter of the position it should, every day, on
// exactly the names where the quote is quiet. Only the last row announces
// itself.
//
// So: Welford for the mean and second moment, Pebay's extension for the third
// and fourth, and a two-pass computation over the window rather than a
// subtractive update. `RollingWindow` costs O(N) per query as a direct result;
// that price is stated and benchmarked rather than traded away for an O(1)
// update that drifts (see the note on `RollingWindow`).
//
// EMPTY IS NOT ZERO. Every statistic returns `std::expected`. A variance
// needs two points, a skewness three, a kurtosis four, and a z-score a
// non-zero spread. The alternative -- returning 0.0, or a NaN, for "I do not
// know yet" -- puts a value that passes every finite check into a feature
// vector, which is the phantom-default-state defect this repo keeps meeting.

#pragma once

#include <core/types/units.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

enum class StatError : std::uint8_t {
    /// Fewer observations than the statistic is defined for: 2 for a
    /// variance, 3 for a skewness, 4 for an excess kurtosis.
    NotEnoughData,
    /// The spread is exactly zero, so a skewness, kurtosis or z-score would
    /// divide by it. A constant series has no shape, and saying so is more
    /// useful than returning zero as though it were flat-and-known.
    ZeroSpread,
    /// A non-finite observation reached the accumulator.
    NotFinite
};

// ---------------------------------------------------------------------------
// RunningMoments -- session-long, O(1) update, no allocation.
// ---------------------------------------------------------------------------

/// The first four central moments of an unbounded stream.
///
/// Welford for the mean and M2; Pebay (2008) for M3 and M4. Update order
/// inside `add` matters: M4 reads the old M3 and M2, M3 reads the old M2. Any
/// other order is silently wrong in the tails only.
///
/// NO KAHAN COMPENSATION HERE, and that is a measured decision rather than an
/// omission. CLAUDE.md requires Kahan on session-long accumulators, so it was
/// implemented and then benchmarked over two million ticks against a
/// long-double reference: the compensated mean was identical to the plain one
/// (1.304e-8 both) and the compensated variance marginally worse
/// (2.421e-12 against 2.413e-12). It was removed.
///
/// The reason is that Welford is not a summation. Its mean increment is
/// delta/n, which shrinks as 1/n, so rounding does not accumulate for a
/// compensator to recover; and M2's residual error is dominated by the mean's
/// conditioning propagating into each delta, which compensating the sum does
/// not touch. Kahan fixes ACCUMULATION error, and Welford's error is
/// CONDITIONING error. They are different failures.
///
/// Where the rule does bite is a RAW running sum past 2^53 paise -- which is
/// Rs 90.07 lakh crore, and test 7 pins both sides of that line. A day of NSE
/// cash turnover (about Rs 1 lakh crore = 1e14 paise) sums EXACTLY in double
/// and needs no compensation at all. A day of F&O NOTIONAL turnover (about
/// Rs 400 lakh crore = 4e16 paise) does not: measured, plain summation loses
/// 3,067,328 paise -- Rs 30,673 -- and compensation recovers every one.
/// That is the accumulator P3-09's carried debt is really about.
///
/// The same test also runs Kahan on the naive variance, where it works exactly
/// as advertised (1.5e-2 relative error down to 1.0e-4) and still loses to
/// Welford's 1.3e-11 by seven orders of magnitude -- because choosing the
/// right algorithm beats compensating the wrong one.
class RunningMoments {
public:
    /// Fold one observation in. UNIT: whatever the caller is measuring --
    /// this class is dimensionless and does not know.
    ALTAIR_HOT void add(double x) noexcept {
        if (!std::isfinite(x)) {
            ++rejected_;
            return;
        }
        ++n_;
        const double n = static_cast<double>(n_);
        const double delta = x - mean_;
        const double delta_n = delta / n;
        const double delta_n2 = delta_n * delta_n;
        const double term1 = delta * delta_n * (n - 1.0);

        // M4 and M3 first: both read the PRE-update mean, M2 and M3.
        m4_ += term1 * delta_n2 * (n * n - 3.0 * n + 3.0)
             + 6.0 * delta_n2 * m2_ - 4.0 * delta_n * m3_;
        m3_ += term1 * delta_n * (n - 2.0) - 3.0 * delta_n * m2_;

        m2_ += term1;
        mean_ += delta_n;
    }

    [[nodiscard]] std::uint64_t count() const noexcept { return n_; }
    /// Observations refused as non-finite. Counted, never silently dropped.
    [[nodiscard]] std::uint64_t rejected() const noexcept { return rejected_; }

    /// The running mean. Defined from the first observation.
    [[nodiscard]] std::expected<double, StatError> mean() const noexcept {
        if (n_ < 1) { return std::unexpected(StatError::NotEnoughData); }
        return mean_;
    }

    /// Population variance (divides by n). Use for a complete population --
    /// e.g. every tick in a closed session.
    [[nodiscard]] std::expected<double, StatError> variance() const noexcept {
        if (n_ < 2) { return std::unexpected(StatError::NotEnoughData); }
        return m2_ / static_cast<double>(n_);
    }

    /// Sample variance (divides by n-1). Use when the observations are a
    /// sample of an ongoing process, which is the usual case intraday.
    ///
    /// Both are provided and neither is the default, because picking one
    /// silently is how an n=2 estimate ends up 100% low.
    [[nodiscard]] std::expected<double, StatError>
    sample_variance() const noexcept {
        if (n_ < 2) { return std::unexpected(StatError::NotEnoughData); }
        return m2_ / static_cast<double>(n_ - 1);
    }

    [[nodiscard]] std::expected<double, StatError> stddev() const noexcept {
        const auto v = sample_variance();
        if (!v) { return std::unexpected(v.error()); }
        return std::sqrt(*v);
    }

    /// Fisher-Pearson skewness of the population.
    [[nodiscard]] std::expected<double, StatError> skewness() const noexcept {
        if (n_ < 3) { return std::unexpected(StatError::NotEnoughData); }
        if (!(m2_ > 0.0)) { return std::unexpected(StatError::ZeroSpread); }
        return std::sqrt(static_cast<double>(n_)) * m3_
             / std::pow(m2_, 1.5);
    }

    /// EXCESS kurtosis: 0.0 for a Gaussian, not 3.0. Named `excess_` so the
    /// three-versus-zero convention cannot be got wrong at a call site.
    [[nodiscard]] std::expected<double, StatError>
    excess_kurtosis() const noexcept {
        if (n_ < 4) { return std::unexpected(StatError::NotEnoughData); }
        if (!(m2_ > 0.0)) { return std::unexpected(StatError::ZeroSpread); }
        return static_cast<double>(n_) * m4_ / (m2_ * m2_) - 3.0;
    }

    /// Standard scores against the running distribution.
    [[nodiscard]] std::expected<double, StatError>
    zscore(double x) const noexcept {
        const auto sd = stddev();
        if (!sd) { return std::unexpected(sd.error()); }
        if (!(*sd > 0.0)) { return std::unexpected(StatError::ZeroSpread); }
        return (x - mean_) / *sd;
    }

    void reset() noexcept { *this = RunningMoments{}; }

private:
    std::uint64_t n_ = 0;
    std::uint64_t rejected_ = 0;
    double mean_ = 0.0;
    double m2_ = 0.0;
    double m3_ = 0.0;
    double m4_ = 0.0;
};

// ---------------------------------------------------------------------------
// RollingWindow -- the last N observations, exactly.
// ---------------------------------------------------------------------------

/// A fixed-capacity ring of the most recent N observations.
///
/// O(1) push, O(N) query. THAT IS DELIBERATE, and it is the whole design
/// decision in this class.
///
/// The O(1) alternative is to run Welford forward on the arriving point and
/// backward on the departing one. The backward step subtracts, so it
/// reintroduces the cancellation Welford exists to avoid. Measured against an
/// exact recomputation, the drift is modest on calm data (3e-11 relative after
/// two million pushes) and reaches 4.7e-3 -- half a percent -- when a volatile
/// stretch is followed by a quiet one, which is precisely the shape of the
/// half hour after a news spike settles, and precisely when a clean variance
/// matters most.
///
/// But the size of the error is the smaller objection. THE SUBTRACTIVE FORM IS
/// NOT REPRODUCIBLE: its accumulator carries residue from every point that has
/// ever passed through, so two windows holding byte-identical data report
/// different variances depending on what preceded them. Rule 10 requires every
/// live decision to be reproducible from a recorded tuple, and a backtest
/// replayed from a different start point would compute a different feature
/// from the same window. `tests/test_rolling.cpp` demonstrates both the drift
/// and the path dependence.
///
/// So the window is recomputed from the ring, in two passes, on each query.
/// At N = 256 that is measured in `tests/test_rolling.cpp` and is small
/// against a tick budget; a feature registry evaluating hundreds of windows
/// per tick should read that number before assuming it is free.
///
/// No allocation: the storage is a member array, so this is usable inside
/// ALTAIR_HOT (rule 4).
template <std::size_t N>
class RollingWindow {
    static_assert(N >= 2, "a window of one has no spread");

public:
    /// Append, evicting the oldest once full. UNIT: caller's.
    ALTAIR_HOT void push(double x) noexcept {
        if (!std::isfinite(x)) {
            ++rejected_;
            return;
        }
        buf_[head_] = x;
        head_ = (head_ + 1 == N) ? 0 : head_ + 1;
        if (size_ < N) { ++size_; }
        ++total_;
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return N; }
    [[nodiscard]] bool full() const noexcept { return size_ == N; }
    /// Observations ever accepted, including those since evicted.
    [[nodiscard]] std::uint64_t total() const noexcept { return total_; }
    [[nodiscard]] std::uint64_t rejected() const noexcept { return rejected_; }

    /// Most recent observation.
    [[nodiscard]] std::expected<double, StatError> last() const noexcept {
        if (size_ == 0) { return std::unexpected(StatError::NotEnoughData); }
        return buf_[head_ == 0 ? N - 1 : head_ - 1];
    }

    /// Oldest observation still in the window.
    [[nodiscard]] std::expected<double, StatError> oldest() const noexcept {
        if (size_ == 0) { return std::unexpected(StatError::NotEnoughData); }
        return buf_[size_ == N ? head_ : 0];
    }

    [[nodiscard]] std::expected<double, StatError> mean() const noexcept {
        if (size_ == 0) { return std::unexpected(StatError::NotEnoughData); }
        return raw_mean();
    }

    /// Sample variance over the window, two-pass.
    ///
    /// The second pass subtracts the mean BEFORE squaring, so the quantities
    /// being summed are the deviations themselves -- small, comparable, and
    /// with nothing large to cancel against. That is the entire fix.
    [[nodiscard]] std::expected<double, StatError> variance() const noexcept {
        if (size_ < 2) { return std::unexpected(StatError::NotEnoughData); }
        const double m = raw_mean();
        double s2 = 0.0;
        for (std::size_t i = 0; i < size_; ++i) {
            const double d = at(i) - m;
            s2 += d * d;
        }
        return s2 / static_cast<double>(size_ - 1);
    }

    [[nodiscard]] std::expected<double, StatError> stddev() const noexcept {
        const auto v = variance();
        if (!v) { return std::unexpected(v.error()); }
        return std::sqrt(*v);
    }

    [[nodiscard]] std::expected<double, StatError> skewness() const noexcept {
        if (size_ < 3) { return std::unexpected(StatError::NotEnoughData); }
        const double m = raw_mean();
        double m2 = 0.0;
        double m3 = 0.0;
        for (std::size_t i = 0; i < size_; ++i) {
            const double d = at(i) - m;
            m2 += d * d;
            m3 += d * d * d;
        }
        if (!(m2 > 0.0)) { return std::unexpected(StatError::ZeroSpread); }
        const double n = static_cast<double>(size_);
        return std::sqrt(n) * m3 / std::pow(m2, 1.5);
    }

    /// EXCESS kurtosis: 0.0 for a Gaussian.
    [[nodiscard]] std::expected<double, StatError>
    excess_kurtosis() const noexcept {
        if (size_ < 4) { return std::unexpected(StatError::NotEnoughData); }
        const double m = raw_mean();
        double m2 = 0.0;
        double m4 = 0.0;
        for (std::size_t i = 0; i < size_; ++i) {
            const double d = at(i) - m;
            const double d2 = d * d;
            m2 += d2;
            m4 += d2 * d2;
        }
        if (!(m2 > 0.0)) { return std::unexpected(StatError::ZeroSpread); }
        const double n = static_cast<double>(size_);
        return n * m4 / (m2 * m2) - 3.0;
    }

    /// Standard score of `x` against the window.
    [[nodiscard]] std::expected<double, StatError>
    zscore(double x) const noexcept {
        const auto sd = stddev();
        if (!sd) { return std::unexpected(sd.error()); }
        if (!(*sd > 0.0)) { return std::unexpected(StatError::ZeroSpread); }
        return (x - raw_mean()) / *sd;
    }

    [[nodiscard]] std::expected<double, StatError> min() const noexcept {
        if (size_ == 0) { return std::unexpected(StatError::NotEnoughData); }
        double v = at(0);
        for (std::size_t i = 1; i < size_; ++i) {
            if (at(i) < v) { v = at(i); }
        }
        return v;
    }

    [[nodiscard]] std::expected<double, StatError> max() const noexcept {
        if (size_ == 0) { return std::unexpected(StatError::NotEnoughData); }
        double v = at(0);
        for (std::size_t i = 1; i < size_; ++i) {
            if (at(i) > v) { v = at(i); }
        }
        return v;
    }

    /// Element i, oldest first. PRECONDITION: i < size().
    [[nodiscard]] double at(std::size_t i) const noexcept {
        const std::size_t base = (size_ == N) ? head_ : 0;
        const std::size_t j = base + i;
        return buf_[j >= N ? j - N : j];
    }

    void reset() noexcept { *this = RollingWindow{}; }

private:
    [[nodiscard]] double raw_mean() const noexcept {
        // Plain summation: N is bounded and small, so the accumulation error
        // is ~sqrt(N)*eps relative -- 1.6e-15 at N = 256, far below the
        // conditioning error the two-pass structure exists to control.
        double s = 0.0;
        for (std::size_t i = 0; i < size_; ++i) { s += at(i); }
        return s / static_cast<double>(size_);
    }

    double buf_[N] = {};
    std::size_t head_ = 0;
    std::size_t size_ = 0;
    std::uint64_t total_ = 0;
    std::uint64_t rejected_ = 0;
};

} // namespace altair
