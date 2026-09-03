// desktop/chart/candles.hpp -- ticks into candles, and coordinates into
// float32 without destroying them.
//
// P11Q-04. The C++ half of what P11-10 measured in the web client; the
// findings are language-independent and every one of them applies here.
//
// A NANOSECOND TIMESTAMP IN A float32 IS NOT A TIMESTAMP.
//
// A GPU vertex attribute is float32: twenty-four mantissa bits. Engine time is
// ~1.79e18 ns, where consecutive float32 values are about 1.4e11 ns apart --
// 137 SECONDS. Every tick in a two-minute window lands on one x coordinate.
//
// The chart does not error. It draws a staircase, and the staircase reads as
// low-frequency structure in the market.
//
// This is not avoided by drawing with QPainter instead of OpenGL. QPainter
// takes `qreal` (a double) and would hide it, and then the first card that
// moves the chart onto a GPU reintroduces it silently. So the transform below
// goes through an EXPLICIT float32 stage -- `Vertex` holds floats, exactly as
// a VBO would -- and `Domain` subtracts the origin in int64 BEFORE the
// conversion. Doing it the other way round loses the precision before the
// subtraction can save it, which is the version that looks correct in a diff.
//
// PRICE IS FINE; NOTIONAL IS NOT.
//
// Measured in P11-10 and re-stated because it is the counter-intuitive half:
// NIFTY at 24,000 is 2,400,000 paise, where float32 resolves to 0.25 paise
// against a 5-paise tick -- twenty times the headroom needed. An equity curve
// at Rs 50 crore (5e10 paise) resolves to 4,096 paise, Rs 41, and draws Rs 10
// moves as a staircase. So the axis is rebased PER SERIES, and the reason is
// not "prices are big".
//
// A BUCKET BOUNDARY BELONGS TO EXACTLY ONE CANDLE.
//
// Half-open [start, end). Closed at both ends counts a boundary tick twice,
// and exchange timestamps are quantised so a batch of orders lands exactly on
// the boundary. P11-10 measured +2.0% on the volume a confirmation rule keys
// on. `check_conservation` totals the candles against the tape -- the engine's
// own per-tick invariant, applied to the chart.
//
// A CANDLE STILL FORMING IS NOT A CANDLE.
//
// Rule 7 at the chart. The last bucket's close is the latest trade, not a
// close, and an overlay computed over it repaints its own last point.
// `complete` says which is which, and `now_ns` comes off the TAPE.
//
// AND A GAP IS NOT A ZERO. A minute with no trades produces no candle. A zero
// candle spikes to the axis; one carrying the previous close draws a flat line
// saying trading happened. Neither is true.

#pragma once

#include <core/time/timestamp.hpp>
#include <core/types/units.hpp>
#include <feed/replay.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

namespace altair::ui {

struct Candle {
    std::int64_t start_ns = 0;
    std::int64_t end_ns = 0;      ///< exclusive
    std::int64_t open = 0;        ///< paise
    std::int64_t high = 0;
    std::int64_t low = 0;
    std::int64_t close = 0;
    std::int64_t volume = 0;
    std::uint32_t ticks = 0;
    /// False for the bucket the tape is still inside.
    bool complete = false;
    /// False when the SOURCE did not report a volume, as distinct from
    /// reporting zero. 1,408 daily NIFTY rows carry 0 because the index
    /// published no turnover in the early years, and drawing that as a
    /// zero-height bar says nothing traded on a day the market was open.
    /// True for anything built from ticks, where a quantity always exists.
    bool volume_known = true;
};

/// Bucket ticks for ONE instrument into candles.
///
/// `now_ns` is the engine's current tick time -- off the tape, never a wall
/// clock (rule 7). Passing a wall clock is how a finished replay renders its
/// last candle as forever-forming, or a live one as already settled.
[[nodiscard]] inline std::vector<Candle>
build_candles(const ReplayTick* ticks, std::size_t n, std::uint32_t token,
              std::int64_t bucket_ns, std::int64_t now_ns) {
    std::vector<Candle> out;
    if (ticks == nullptr || n == 0 || bucket_ns <= 0) {
        return out;
    }

    bool open_bucket = false;
    Candle c{};

    const auto flush = [&] {
        if (open_bucket && c.ticks > 0) {
            c.complete = c.end_ns <= now_ns;
            out.push_back(c);
        }
        open_bucket = false;
    };

    for (std::size_t i = 0; i < n; ++i) {
        const ReplayTick& t = ticks[i];
        if (t.token != token) {
            continue;
        }
        const std::int64_t ts = t.ts.ns_since_epoch();
        // Floor division toward negative infinity, so a pre-epoch timestamp
        // buckets the same way a post-epoch one does rather than folding
        // toward zero.
        std::int64_t q = ts / bucket_ns;
        if (ts % bucket_ns != 0 && ts < 0) {
            --q;
        }
        const std::int64_t start = q * bucket_ns;

        if (!open_bucket || start != c.start_ns) {
            flush();
            c = Candle{};
            c.start_ns = start;
            c.end_ns = start + bucket_ns;
            c.open = t.last.raw();
            c.high = t.last.raw();
            c.low = t.last.raw();
            open_bucket = true;
        }
        const std::int64_t px = t.last.raw();
        if (px > c.high) c.high = px;
        if (px < c.low) c.low = px;
        c.close = px;
        c.volume += t.qty.raw();
        ++c.ticks;
    }
    flush();
    return out;
}

struct Conservation {
    std::int64_t tape_volume = 0;
    std::int64_t chart_volume = 0;
    std::uint64_t tape_ticks = 0;
    std::uint64_t chart_ticks = 0;
    bool agrees = false;
};

/// Total the candles against the tape, for one instrument.
///
/// Meant to be RUN, not admired: a chart whose volume disagrees with the tape
/// has a bucketing bug, and it is far cheaper to assert than to notice.
[[nodiscard]] inline Conservation
check_conservation(const ReplayTick* ticks, std::size_t n, std::uint32_t token,
                   const std::vector<Candle>& built) {
    Conservation k{};
    for (std::size_t i = 0; i < n; ++i) {
        if (ticks[i].token != token) {
            continue;
        }
        k.tape_volume += ticks[i].qty.raw();
        ++k.tape_ticks;
    }
    for (const Candle& c : built) {
        k.chart_volume += c.volume;
        k.chart_ticks += c.ticks;
    }
    k.agrees = k.tape_volume == k.chart_volume && k.tape_ticks == k.chart_ticks;
    return k;
}

// ---------------------------------------------------------------------------
// The float32 boundary
// ---------------------------------------------------------------------------

/// Spacing between representable float32 values at magnitude `v`.
/// IEEE-754 binary32: 24 mantissa bits including the implicit one.
[[nodiscard]] inline double float32_resolution(double v) noexcept {
    const double a = std::fabs(v);
    if (!(a > 0.0)) {
        return 0.0;
    }
    const int e = static_cast<int>(std::floor(std::log2(a)));
    return std::pow(2.0, e - 23);
}

/// The visible time range. `origin_ns` is int64 because the whole point is
/// that it does not fit anywhere smaller; the SPAN fits everywhere.
struct Domain {
    std::int64_t origin_ns = 0;
    std::int64_t span_ns = 1;
};

/// Price range for ONE series, in paise. Rebased per series, not per chart.
struct PriceAxis {
    std::int64_t origin_paise = 0;
    std::int64_t span_paise = 1;
};

[[nodiscard]] inline PriceAxis
price_axis(const std::vector<Candle>& built, double pad_fraction = 0.06) {
    PriceAxis a{};
    if (built.empty()) {
        return a;
    }
    std::int64_t lo = built.front().low;
    std::int64_t hi = built.front().high;
    for (const Candle& c : built) {
        if (c.low < lo) lo = c.low;
        if (c.high > hi) hi = c.high;
    }
    const std::int64_t raw = hi - lo;
    // A flat series still needs a non-zero span, or the transform divides by
    // zero and the chart becomes a single line at NaN.
    const std::int64_t span = raw > 0 ? raw : 1;
    const auto pad = static_cast<std::int64_t>(
        static_cast<double>(span) * pad_fraction);

    // A PRICE AXIS MUST NOT GO BELOW ZERO.
    //
    // Padding symmetrically is right for a P&L series, which is signed. It is
    // wrong for a price: NIFTY's 35-year daily range is 279 to 27,939, so six
    // percent of the span is 1,659 paise and the padded origin lands at
    // MINUS 1,286.63. The axis then labels a price that cannot exist, and the
    // first version of this function did exactly that -- visible on screen the
    // moment the real series was loaded.
    //
    // The pad below is clamped so the origin stops at zero. The pad above is
    // untouched; there is no upper bound on a price.
    const std::int64_t lo_padded = lo - pad;
    a.origin_paise = lo_padded < 0 ? 0 : lo_padded;
    a.span_paise = (lo + span + pad) - a.origin_paise;
    if (a.span_paise <= 0) {
        a.span_paise = 1;
    }
    return a;
}

/// What crosses to the renderer -- and, when this chart moves onto a GPU, what
/// goes into the VBO. FLOATS, deliberately: making the boundary explicit is
/// what stops the rebasing being quietly dropped later.
struct Vertex {
    float x = 0.0f;   ///< normalised 0..1 across the domain
    float y = 0.0f;   ///< normalised 0..1 across the price axis
};

/// Normalise ONE instant to the domain, rebasing in int64 first.
[[nodiscard]] inline float to_x(std::int64_t ts_ns, const Domain& d) noexcept {
    const std::int64_t offset = ts_ns - d.origin_ns;   // int64, exact
    return static_cast<float>(static_cast<double>(offset)
                              / static_cast<double>(d.span_ns));
}

[[nodiscard]] inline float to_y(std::int64_t paise,
                                const PriceAxis& a) noexcept {
    const std::int64_t offset = paise - a.origin_paise;
    return static_cast<float>(static_cast<double>(offset)
                              / static_cast<double>(a.span_paise));
}

/// The naive path: straight to float32 with NO rebasing. Present only so the
/// test can measure what it destroys; nothing in the render path calls it.
[[nodiscard]] inline float naive_x(std::int64_t ts_ns) noexcept {
    return static_cast<float>(ts_ns);
}

// ---------------------------------------------------------------------------
// Crosshair
// ---------------------------------------------------------------------------

struct Crosshair {
    std::size_t index = 0;
    bool valid = false;
    /// True when the pointer is past the last candle, so the snap is to the
    /// nearest rather than to something under the cursor.
    bool extrapolated = false;
};

/// Snap to a candle and report THAT candle's values.
///
/// Not the interpolated value at the pixel: a readout that interpolates
/// between two candles shows a price at which nothing traded, to the paisa, in
/// the box the user is reading precisely because they want the real number.
[[nodiscard]] inline Crosshair
snap_crosshair(const std::vector<Candle>& built, std::int64_t at_ns) noexcept {
    Crosshair h{};
    if (built.empty()) {
        return h;
    }
    std::int64_t best = -1;
    for (std::size_t i = 0; i < built.size(); ++i) {
        const std::int64_t mid =
            built[i].start_ns + (built[i].end_ns - built[i].start_ns) / 2;
        const std::int64_t d = at_ns > mid ? at_ns - mid : mid - at_ns;
        if (best < 0 || d < best) {
            best = d;
            h.index = i;
        }
    }
    h.valid = true;
    h.extrapolated = at_ns >= built.back().end_ns;
    return h;
}

} // namespace altair::ui
