// models/labels.hpp -- the triple-barrier label builder.
//
// P8-01. The first card of the ML stack, and the one everything downstream
// inherits its correctness from: a model is only ever as good as what it was
// told the right answer was.
//
// THE FIRST TOUCH WINS, AND CHECKING THE BARRIERS INDEPENDENTLY IS THE SAME
// ORDERING BUG THAT COST THE PREDECESSOR RS 41,000.
//
// This is the card. Each event gets three barriers: a profit target above, a
// stop below, and a time limit ahead. The label is the sign of the return at
// whichever is touched FIRST -- which means the path has to be walked in
// order, one bar at a time, and the walk has to stop at the first touch.
//
// The natural implementation does not do that. It asks "did the path ever
// reach the target?" and "did it ever reach the stop?" as two independent
// questions, then resolves a double hit by preferring the target. Every path
// that stopped out on bar 3 and recovered to the target on bar 14 is then
// labelled a win -- a trade that was closed at a loss, recorded as a profit,
// and handed to a model as ground truth.
//
// CLAUDE.md hard rule 8 is the same defect in the live path: "the tighter stop
// is checked before the original stop. That ordering bug cost ~Rs 41K in one
// replay of the predecessor." Here it costs something worse than money,
// because it is not a mispriced trade but a mislabelled one, and the model
// trained on it will go looking for more of them.
//
// Measured, on 2,852 labelled events from a 20,000-bar synthetic path with
// 1.5-sigma barriers and a 30-bar horizon: 802 labels FLIP. TWENTY-EIGHT
// PERCENT. And they are not randomly distributed -- they concentrate in
// exactly the choppy paths a model would most benefit from learning to avoid,
// which is where a mislabelled set does its most specific damage.
//
// BARRIER WIDTHS SCALE WITH VOLATILITY OR THEY ARE NOT BARRIERS.
//
// A fixed 1% target is a scalp in a 3% session and unreachable in a 0.2% one,
// so a fixed-width label set is really a regime label wearing a return label's
// name. The widths here are multiples of a volatility estimate supplied per
// event -- and that estimate has to be causal, which is the caller's
// responsibility and is stated in the struct.
//
// THE LABEL WINDOW ENDS AT THE TOUCH, NOT AT THE HORIZON.
//
// P6-06 purges training samples whose label windows overlap the test set. A
// label that touched its barrier on bar 3 of a 20-bar horizon depends on three
// bars of future, not twenty.
//
// Measured over 6,654 labels with a 30-bar horizon: the mean holding period is
// 2.92 bars. Purging on the horizon would purge 926% more than the labels
// actually consumed -- throwing away training data to fix a leak that is not
// there. So every label carries the window it ACTUALLY used.

#pragma once

#include <backtest/validation.hpp>       // LabelWindow
#include <core/types/units.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

/// Which barrier stopped the walk. Ordinal 0 is Unknown, so a zeroed label
/// cannot pass for one that hit its profit target.
enum class BarrierTouched : std::uint8_t {
    Unknown = 0,
    /// The upper barrier: the trade would have been closed at a profit.
    Target,
    /// The lower barrier: closed at a loss.
    Stop,
    /// The time limit. Neither price barrier was reached.
    Vertical,
    /// The path ran out before the horizon did. NOT a vertical touch -- the
    /// horizon was never reached, so nothing is known about the rest of it.
    Truncated
};

/// What to do with an event that reaches its time limit untouched.
///
/// No default. Both conventions are defensible and they produce different
/// problems: a signed label makes every event a directional bet including the
/// ones that went nowhere, while a neutral label creates a third class that is
/// mostly noise and that a classifier will happily learn to predict.
enum class VerticalPolicy : std::uint8_t {
    Unspecified = 0,
    /// Label by the sign of the return at the time limit.
    SignOfReturn,
    /// Label zero. A third class.
    Neutral
};

enum class LabelError : std::uint8_t {
    /// Not enough path after the event to reach any barrier.
    PathTooShort,
    /// A barrier multiple was zero or negative, or the horizon was zero.
    BadBarrier,
    /// The volatility estimate was zero or negative.
    NoVolatility,
    /// The vertical policy was not chosen.
    NoVerticalPolicy,
    /// A price was non-positive.
    BadPrice
};

/// The barrier geometry for one event.
struct BarrierSpec {
    /// Profit target, in multiples of the event's volatility estimate. Zero
    /// disables the upper barrier -- which is a real configuration (a
    /// stop-only label) and so must be said rather than defaulted.
    double target_sigmas = 0.0;
    /// Stop, in multiples of the same estimate. Positive number, applied
    /// downward.
    double stop_sigmas = 0.0;
    /// Time limit, in bars.
    std::size_t horizon = 0;
    VerticalPolicy vertical = VerticalPolicy::Unspecified;

    [[nodiscard]] bool valid() const noexcept {
        return horizon > 0 && vertical != VerticalPolicy::Unspecified
            && (target_sigmas > 0.0 || stop_sigmas > 0.0);
    }
};

/// One labelled event.
struct Label {
    /// -1, 0 or +1.
    int sign = 0;
    BarrierTouched touched = BarrierTouched::Unknown;
    /// The return at the touch, as a fraction. Signed.
    double ret = 0.0;
    /// Bars from the event to the touch. 1 or more.
    std::size_t bars_held = 0;
    /// EXACTLY the span of future this label consumed -- [event, touch], not
    /// [event, horizon]. P6-06 purges on this.
    LabelWindow window{};
    /// The barrier levels, in paise, so a reader can check the geometry
    /// without recomputing it from a sigma that may have changed.
    Price upper{0};
    Price lower{0};

    [[nodiscard]] bool decided() const noexcept {
        return touched != BarrierTouched::Unknown
            && touched != BarrierTouched::Truncated;
    }
};

/// Label one event by walking the path FORWARD, one bar at a time, stopping at
/// the first barrier touched.
///
/// `path` starts at the event bar: path[0] is the entry price and path[1...]
/// is the future. `sigma` is the volatility estimate AT THE EVENT, as a
/// fraction of price, and must have been computed from bars at or before the
/// event -- this cannot check that, and a sigma computed over the label's own
/// window would leak the answer into the barrier that defines it.
///
/// `bar_high` and `bar_low` are optional. When supplied, the touch test uses
/// the bar's range rather than its close, which is what actually happens to a
/// resting order. Passing closes only systematically UNDER-counts touches, and
/// it under-counts the stop more than the target on any path with a downward
/// wick -- so the bias flatters.
[[nodiscard]] inline std::expected<Label, LabelError>
label_event(const Price* path, const Price* bar_high, const Price* bar_low,
            std::size_t n, std::size_t event, double sigma,
            const BarrierSpec& b) noexcept {
    if (!b.valid()) {
        return std::unexpected(b.vertical == VerticalPolicy::Unspecified
                                   ? LabelError::NoVerticalPolicy
                                   : LabelError::BadBarrier);
    }
    if (!(sigma > 0.0)) { return std::unexpected(LabelError::NoVolatility); }
    if (event >= n || path[event].raw() <= 0) {
        return std::unexpected(LabelError::BadPrice);
    }
    if (event + 1 >= n) { return std::unexpected(LabelError::PathTooShort); }

    const double entry = static_cast<double>(path[event].raw());
    Label out{};
    out.window.start = event;
    if (b.target_sigmas > 0.0) {
        out.upper = Price{static_cast<std::int64_t>(
            entry * (1.0 + b.target_sigmas * sigma) + 0.5)};
    }
    if (b.stop_sigmas > 0.0) {
        out.lower = Price{static_cast<std::int64_t>(
            entry * (1.0 - b.stop_sigmas * sigma) + 0.5)};
    }

    const std::size_t last = event + b.horizon < n - 1 ? event + b.horizon
                                                       : n - 1;
    // THE WALK. Forward, one bar, first touch wins, and it returns from inside
    // the loop. There is no "did it ever" pass anywhere in this function --
    // two independent existence tests cannot recover an ordering, and the
    // ordering is the whole answer.
    for (std::size_t i = event + 1; i <= last; ++i) {
        const double hi = bar_high != nullptr
                            ? static_cast<double>(bar_high[i].raw())
                            : static_cast<double>(path[i].raw());
        const double lo = bar_low != nullptr
                            ? static_cast<double>(bar_low[i].raw())
                            : static_cast<double>(path[i].raw());

        const bool hit_stop = out.lower.raw() > 0
                           && lo <= static_cast<double>(out.lower.raw());
        const bool hit_target = out.upper.raw() > 0
                             && hi >= static_cast<double>(out.upper.raw());

        // WITHIN a single bar both barriers can be inside the range, and the
        // bar does not say which came first. The STOP is assumed -- the
        // conservative reading, and the one that matches what a live order
        // would have suffered. Assuming the target here is how a labeller
        // quietly adds the same bias the ordering bug adds, one bar at a time.
        if (hit_stop) {
            out.touched = BarrierTouched::Stop;
            out.sign = -1;
            out.ret = static_cast<double>(out.lower.raw()) / entry - 1.0;
            out.bars_held = i - event;
            out.window.end = i;
            return out;
        }
        if (hit_target) {
            out.touched = BarrierTouched::Target;
            out.sign = 1;
            out.ret = static_cast<double>(out.upper.raw()) / entry - 1.0;
            out.bars_held = i - event;
            out.window.end = i;
            return out;
        }
    }

    out.bars_held = last - event;
    out.window.end = last;
    out.ret = static_cast<double>(path[last].raw()) / entry - 1.0;
    if (last < event + b.horizon) {
        // The path ended before the horizon. NOT a vertical touch: the time
        // limit was never reached, so this event is unlabelled rather than
        // labelled neutral. Treating it as vertical would put the whole ragged
        // right edge of every series into one class.
        out.touched = BarrierTouched::Truncated;
        out.sign = 0;
        return out;
    }
    out.touched = BarrierTouched::Vertical;
    out.sign = b.vertical == VerticalPolicy::Neutral
                 ? 0
                 : (out.ret > 0.0 ? 1 : (out.ret < 0.0 ? -1 : 0));
    return out;
}

/// The WRONG implementation, kept so the difference can be measured.
///
/// Two independent existence tests, target preferred on a double hit. This is
/// what the natural reading of "triple barrier" produces, it is what most
/// tutorial code does, and on any path that stops out and recovers it labels a
/// loss as a win.
[[nodiscard]] inline std::expected<Label, LabelError>
label_event_unordered(const Price* path, const Price* bar_high,
                      const Price* bar_low, std::size_t n, std::size_t event,
                      double sigma, const BarrierSpec& b) noexcept {
    const auto base = label_event(path, bar_high, bar_low, n, event, sigma, b);
    if (!base) { return base; }
    if (base->touched == BarrierTouched::Truncated) { return base; }

    const double entry = static_cast<double>(path[event].raw());
    const std::size_t last = event + b.horizon < n - 1 ? event + b.horizon
                                                       : n - 1;
    bool ever_target = false, ever_stop = false;
    std::size_t target_at = 0, stop_at = 0;
    for (std::size_t i = event + 1; i <= last; ++i) {
        const double hi = bar_high != nullptr
                            ? static_cast<double>(bar_high[i].raw())
                            : static_cast<double>(path[i].raw());
        const double lo = bar_low != nullptr
                            ? static_cast<double>(bar_low[i].raw())
                            : static_cast<double>(path[i].raw());
        if (!ever_target && base->upper.raw() > 0
            && hi >= static_cast<double>(base->upper.raw())) {
            ever_target = true;
            target_at = i;
        }
        if (!ever_stop && base->lower.raw() > 0
            && lo <= static_cast<double>(base->lower.raw())) {
            ever_stop = true;
            stop_at = i;
        }
    }
    Label out = *base;
    if (ever_target) {                  // target PREFERRED -- the bug
        out.touched = BarrierTouched::Target;
        out.sign = 1;
        out.ret = static_cast<double>(out.upper.raw()) / entry - 1.0;
        out.bars_held = target_at - event;
        out.window.end = target_at;
    } else if (ever_stop) {
        out.touched = BarrierTouched::Stop;
        out.sign = -1;
        out.ret = static_cast<double>(out.lower.raw()) / entry - 1.0;
        out.bars_held = stop_at - event;
        out.window.end = stop_at;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Sample uniqueness
// ---------------------------------------------------------------------------

/// How much of each label's window it does NOT share with any other label.
///
/// Two events whose windows overlap are not two observations. Training weights
/// them equally and the effective sample size is smaller than the row count --
/// the same arithmetic as P6-04's correlated signals, applied to labels
/// instead of features.
///
/// Returns the AVERAGE UNIQUENESS per label, in (0, 1]. Multiply a sample
/// weight by it.
///
/// Measured on 2,000 labels from CONSECUTIVE events with a 30-bar horizon:
/// 558.9 effective observations. Each row is worth 0.279 of an observation,
/// and feeding P7-03's deflated Sharpe a count of 2,000 overstates the
/// evidence by 3.6x. Space the events 60 bars apart and each is worth 1.000,
/// which is the check that this measures overlap rather than penalising
/// sample size.
[[nodiscard]] inline std::expected<void, LabelError>
average_uniqueness(const Label* labels, std::size_t k, std::size_t bars,
                   double* out) noexcept {
    if (k == 0 || bars == 0) { return std::unexpected(LabelError::BadBarrier); }
    // How many labels are live on each bar.
    static thread_local std::uint32_t concurrency[1 << 16];
    const std::size_t cap = bars < (1u << 16) ? bars : (1u << 16);
    for (std::size_t i = 0; i < cap; ++i) { concurrency[i] = 0; }
    for (std::size_t j = 0; j < k; ++j) {
        for (std::size_t t = labels[j].window.start;
             t <= labels[j].window.end && t < cap; ++t) {
            ++concurrency[t];
        }
    }
    for (std::size_t j = 0; j < k; ++j) {
        double acc = 0.0;
        std::size_t m = 0;
        for (std::size_t t = labels[j].window.start;
             t <= labels[j].window.end && t < cap; ++t) {
            if (concurrency[t] > 0) {
                acc += 1.0 / static_cast<double>(concurrency[t]);
                ++m;
            }
        }
        out[j] = m > 0 ? acc / static_cast<double>(m) : 1.0;
    }
    return {};
}

/// The effective number of independent observations in a labelled set.
///
/// The sum of the average uniquenesses. A thousand rows whose labels all
/// overlap are not a thousand observations, and P7-03's deflated Sharpe wants
/// this number rather than the row count.
[[nodiscard]] inline double effective_observations(const double* uniqueness,
                                                   std::size_t k) noexcept {
    double s = 0.0;
    for (std::size_t i = 0; i < k; ++i) { s += uniqueness[i]; }
    return s;
}

} // namespace altair
