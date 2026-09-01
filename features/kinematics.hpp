// features/kinematics.hpp -- the kinematics and distribution families.
//
// P5-02. The first two families of feature builders.
//
//   KINEMATICS   velocity, acceleration, jerk, at explicit time scales.
//                Built on analytics/derivatives.hpp (P3-07).
//   DISTRIBUTION mean, variance, skew, excess kurtosis, z-score, Hurst.
//                Built on analytics/rolling.hpp and hurst.hpp (P3-06).
//
// EVERY BUILDER IS A THIN WRAPPER, ON PURPOSE. The mathematics was done and
// tested in Phase 3; if a formula appeared here too there would be two of it,
// and the second copy is the one that goes wrong. What this file adds is the
// three things Phase 3 deliberately did not know about: warmup, absence, and
// the registry's positional contract.
//
// WARMUP IS ENFORCED PER FEATURE, NOT PER VECTOR.
//
// A 50 ms microprice feature is ready long before a 5-minute realised
// volatility. Gating the whole vector on the slowest feature would throw away
// the first five minutes of every session for every fast model; gating nothing
// would emit a 5-minute volatility computed from 8 seconds of data, which is
// not a small version of the feature -- it is a different and much noisier
// one, and a model trained on the real thing will read it as a regime change.
//
// So each builder checks its OWN history and simply does not set its slot when
// it is short. The vector then reports itself incomplete, and P5-01's
// `warmup()` tells a caller when to expect completeness.
//
// EVERYTHING READS TIME OFF THE TICK (rule 7). No builder here consults a
// clock, so a replay produces byte-identical vectors to the live path
// (rule 6) -- which is what makes a backtest a measurement rather than a
// story.

#pragma once

#include <analytics/derivatives.hpp>
#include <analytics/hurst.hpp>
#include <analytics/rolling.hpp>
#include <features/vector.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

enum class BuilderError : std::uint8_t {
    /// Not enough history yet for this feature's own lookback.
    WarmingUp,
    /// The underlying estimator refused -- a constant series, a zero spread.
    Degenerate,
    /// A non-finite input reached the builder.
    NotFinite,
    /// The feature is not registered, or not valid at this horizon.
    NotRegistered
};

/// A rolling price history, shared by every builder in these two families.
///
/// One buffer, not one per feature. Twelve features over the same window would
/// otherwise hold twelve copies of the same prices, and — worse — could drift
/// apart if one of them missed a tick.
template <std::size_t N>
class PriceHistory {
public:
    /// Append a tick. `ts` comes off the tick (rule 7).
    ///
    /// A tick at or before the last one is REFUSED and counted, not folded in:
    /// the derivative builders fit against timestamps, and an out-of-order
    /// point would bend the fit toward a past that already happened.
    [[nodiscard]] std::expected<void, BuilderError>
    push(Timestamp ts, double price) noexcept {
        if (!std::isfinite(price)) {
            ++rejected_;
            return std::unexpected(BuilderError::NotFinite);
        }
        if (size_ > 0 && ts <= newest_ts()) {
            ++out_of_order_;
            return std::unexpected(BuilderError::NotFinite);
        }
        pts_[head_].ts = ts;
        pts_[head_].value = price;
        head_ = (head_ + 1 == N) ? 0 : head_ + 1;
        if (size_ < N) { ++size_; }
        window_.push(price);
        return {};
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::uint64_t out_of_order() const noexcept {
        return out_of_order_;
    }
    [[nodiscard]] std::uint64_t rejected() const noexcept { return rejected_; }

    [[nodiscard]] Timestamp newest_ts() const noexcept {
        return pts_[head_ == 0 ? N - 1 : head_ - 1].ts;
    }
    [[nodiscard]] Timestamp oldest_ts() const noexcept {
        return pts_[size_ == N ? head_ : 0].ts;
    }
    /// How much history is actually held.
    [[nodiscard]] Duration span() const noexcept {
        if (size_ < 2) { return Duration{0}; }
        return newest_ts() - oldest_ts();
    }
    /// Is there at least `need` of history?
    [[nodiscard]] bool warm(Duration need) const noexcept {
        return size_ >= 2 && span().raw() >= need.raw();
    }

    [[nodiscard]] const TimedPoint* points() const noexcept { return pts_; }
    [[nodiscard]] const RollingWindow<N>& window() const noexcept {
        return window_;
    }

private:
    TimedPoint pts_[N] = {};
    RollingWindow<N> window_;
    std::size_t head_ = 0;
    std::size_t size_ = 0;
    std::uint64_t out_of_order_ = 0;
    std::uint64_t rejected_ = 0;
};

// ---------------------------------------------------------------------------
// Kinematics
// ---------------------------------------------------------------------------

/// Velocity, acceleration and jerk at one scale, in paise per second.
///
/// A thin call into P3-07, which fits a local polynomial rather than
/// differencing: on a constant price plus a one-tick bid-ask bounce the
/// two-point difference reports 500 paise/second and the fit reports 0.73.
///
/// `scale` is the window and is REQUIRED. A price has no single velocity, so
/// there is no default here and never will be.
template <std::size_t N>
[[nodiscard]] inline std::expected<Derivatives, BuilderError>
kinematics_at(const PriceHistory<N>& h, Timestamp now, Duration scale,
              int order = 2) noexcept {
    if (!h.warm(scale)) { return std::unexpected(BuilderError::WarmingUp); }
    const auto d = derivatives_at(h.points(), h.size(), now, scale, order);
    if (!d) { return std::unexpected(BuilderError::WarmingUp); }
    return *d;
}

/// Write the kinematics family into a vector at the given slots.
///
/// A slot of `kSkip` (see features/vector.hpp) means the caller did not
/// register that feature, and it is skipped rather than defaulted.

struct KinematicSlots {
    FeatureIndex velocity = kSkip;
    FeatureIndex acceleration = kSkip;
    FeatureIndex jerk = kSkip;
    /// How far the last print sits from its own fitted trend, in units of the
    /// fit's residual spread. A cheap, scale-free "is this print unusual".
    FeatureIndex trend_deviation = kSkip;
};

template <std::size_t N>
[[nodiscard]] inline std::expected<int, BuilderError>
build_kinematics(const PriceHistory<N>& h, Timestamp now, Duration scale,
                 const KinematicSlots& s, FeatureVector& out,
                 int order = 2) noexcept {
    const auto d = kinematics_at(h, now, scale, order);
    if (!d) { return std::unexpected(d.error()); }
    int written = 0;
    auto put = [&](FeatureIndex i, double v) {
        if (i != kSkip && out.set(i, v)) { ++written; }
    };
    put(s.velocity, d->velocity);
    put(s.acceleration, d->acceleration);
    if (order >= 3) { put(s.jerk, d->jerk); }

    if (s.trend_deviation != kSkip && d->residual_sd > 0.0) {
        const auto last = h.window().last();
        if (last) {
            put(s.trend_deviation, (*last - d->value) / d->residual_sd);
        }
    }
    return written;
}

// ---------------------------------------------------------------------------
// Distribution
// ---------------------------------------------------------------------------

struct DistributionSlots {
    FeatureIndex mean = kSkip;
    FeatureIndex stddev = kSkip;
    FeatureIndex skew = kSkip;
    FeatureIndex kurtosis = kSkip;
    FeatureIndex zscore = kSkip;
    FeatureIndex hurst = kSkip;
    /// Hurst's own standard error. Carried as a FEATURE, not folded away:
    /// ROADMAP section 3 wants the error bar to survive to the model, and an
    /// H of 0.58 with a standard error of 0.06 is a random walk.
    FeatureIndex hurst_stderr = kSkip;
};

/// Write the distribution family from the rolling window.
///
/// Each statistic is written ONLY if its own estimator succeeded. A constant
/// series has a variance of zero and NO skewness -- P3-06 refuses it rather
/// than returning zero, and that refusal is carried through as absence rather
/// than being smoothed into a number.
template <std::size_t N>
[[nodiscard]] inline std::expected<int, BuilderError>
build_distribution(const PriceHistory<N>& h, const DistributionSlots& s,
                   FeatureVector& out) noexcept {
    const RollingWindow<N>& w = h.window();
    if (w.size() < 4) { return std::unexpected(BuilderError::WarmingUp); }

    int written = 0;
    auto put = [&](FeatureIndex i, double v) {
        if (i != kSkip && out.set(i, v)) { ++written; }
    };

    if (const auto v = w.mean();            v && s.mean != kSkip)     { put(s.mean, *v); }
    if (const auto v = w.stddev();          v && s.stddev != kSkip)   { put(s.stddev, *v); }
    if (const auto v = w.skewness();        v && s.skew != kSkip)     { put(s.skew, *v); }
    if (const auto v = w.excess_kurtosis(); v && s.kurtosis != kSkip) { put(s.kurtosis, *v); }
    if (s.zscore != kSkip) {
        if (const auto last = w.last()) {
            if (const auto z = w.zscore(*last)) { put(s.zscore, *z); }
        }
    }
    return written;
}

/// Hurst over the window's own contents.
///
/// SEPARATE from `build_distribution` because it needs the raw series rather
/// than the streaming accumulators, costs far more, and -- the real reason --
/// must be fed RETURNS. P3-06c reports 0.96 on a price series and 0.50 on its
/// own increments, so the differencing happens here, once, where it can be
/// seen, rather than being left to a caller to remember.
template <std::size_t N>
[[nodiscard]] inline std::expected<int, BuilderError>
build_hurst(const PriceHistory<N>& h, const DistributionSlots& s,
            FeatureVector& out) noexcept {
    const RollingWindow<N>& w = h.window();
    if (w.size() < kHurstMinSamples + 1) {
        return std::unexpected(BuilderError::WarmingUp);
    }
    static thread_local double diffs[N];
    const std::size_t n = w.size() - 1;
    for (std::size_t i = 0; i < n; ++i) {
        diffs[i] = w.at(i + 1) - w.at(i);       // increments, never levels
    }
    const auto e = hurst_rs(diffs, n);
    if (!e) { return std::unexpected(BuilderError::Degenerate); }

    int written = 0;
    if (s.hurst != kSkip && out.set(s.hurst, e->h)) { ++written; }
    if (s.hurst_stderr != kSkip && out.set(s.hurst_stderr, e->std_error)) {
        ++written;
    }
    return written;
}

} // namespace altair
