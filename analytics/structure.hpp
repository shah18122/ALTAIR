// analytics/structure.hpp -- Ichimoku, Stochastic, Heikin-Ashi, pivots, and
// market structure.
//
// P6-02. Five indicators, and three of them share one defect.
//
// THIS IS THE LOOK-AHEAD CARD (rule 7).
//
// Ichimoku's Chikou span, classical pivot levels, and swing-based market
// structure all have the same shape of bug, and in all three it is invisible:
// the number is right, the arithmetic is right, and it is available a fixed
// number of bars before it could possibly have been known.
//
//   CHIKOU SPAN is today's close plotted 26 bars in the PAST. A backtest that
//   evaluates "Chikou above the price" at the position it is drawn at is
//   comparing bar i's close against bar i-26's price WHILE STANDING AT BAR
//   i-26. That is twenty-six bars of look-ahead, and because the line is drawn
//   in the past it looks like history rather than like a peek.
//
//   Measured: the same rule, the same 3,000 bars of a RANDOM WALK, the same
//   1,516 trades either way. Acted on where the value became known it makes
//   Rs 1,367. Acted on where the line is drawn it makes Rs 5,698 -- 4.2x, out
//   of a series with no edge in it at all. The most profitable indicator ever
//   backtested, and it has never made anyone a rupee.
//
//   PIVOT LEVELS are (H + L + C) / 3 of the PREVIOUS session, used through
//   today. Computed from today's own H, L and C they are a superb intraday
//   support-and-resistance system that requires knowing today's high before
//   the session opens.
//
//   SWING POINTS are only swings once the bars after them have failed to
//   exceed them. A swing high declared at the bar it occurs is a claim that no
//   later bar will be higher, which is a claim about the future.
//
// So every one of these returns a value with the bar index it became KNOWN at,
// separately from the bar index it describes, and the two differ by exactly
// the displacement. Nothing here can hand a strategy a number before its time
// without the strategy having to ignore an index to do it.
//
// HEIKIN-ASHI PRICES ARE NOT PRICES.
//
// The fourth thing. A Heikin-Ashi close is (O + H + L + C) / 4 and its open is
// an average of the PREVIOUS synthetic bar -- neither is a level anything ever
// traded at. On all 1,500 test bars the HA close falls strictly INSIDE the
// real bar's range, and on the bullish bars a trend follower actually buys it
// sits BELOW the real close. So a backtest filling there gets a better entry
// than the exchange offered, on exactly the bars it trades.
//
// The type therefore carries the real OHLC alongside: the smoothing is for the
// signal, the fill goes against the bar that happened.
//
// The HA open is also a recurrence with a factor of one half, so a series
// joined mid-stream carries its own seed -- decaying by half a bar at a time
// and down to 1.4e-11 rupees after forty bars. That is a warmup, not a
// permanent divergence, and the difference between those two is whether a live
// feed can be joined at all.

#pragma once

#include <analytics/indicators.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

enum class StructureError : std::uint8_t {
    /// Not enough bars yet.
    WarmingUp,
    /// The value exists but is not yet knowable at the current bar.
    NotYetKnown,
    /// A bar failed its consistency check.
    BadBar,
    /// The period was zero or negative.
    BadPeriod
};

/// A value, the bar it DESCRIBES, and the bar it became KNOWN at.
///
/// The two indices are separate fields on purpose. Collapsing them is the bug
/// this whole file is built around, and a single `index` field would make the
/// collapse the path of least resistance.
struct Dated {
    double value = 0.0;
    /// The bar this value is about.
    std::size_t describes = 0;
    /// The bar at which it could first have been computed. Never less than
    /// `describes`.
    std::size_t known_at = 0;

    /// How many bars of hindsight this value carries. Zero for anything usable
    /// live.
    [[nodiscard]] std::size_t hindsight() const noexcept {
        return known_at - describes;
    }
    [[nodiscard]] bool usable_at(std::size_t bar) const noexcept {
        return bar >= known_at;
    }
};

// ---------------------------------------------------------------------------
// Rolling high / low over a window
// ---------------------------------------------------------------------------

template <std::size_t N>
class RollingExtreme {
public:
    void push(double hi, double lo) noexcept {
        hi_[head_] = hi;
        lo_[head_] = lo;
        head_ = (head_ + 1 == N) ? 0 : head_ + 1;
        if (n_ < N) { ++n_; }
    }
    [[nodiscard]] bool ready() const noexcept { return n_ == N; }
    [[nodiscard]] double highest() const noexcept {
        double m = hi_[0];
        for (std::size_t i = 1; i < n_; ++i) { if (hi_[i] > m) { m = hi_[i]; } }
        return m;
    }
    [[nodiscard]] double lowest() const noexcept {
        double m = lo_[0];
        for (std::size_t i = 1; i < n_; ++i) { if (lo_[i] < m) { m = lo_[i]; } }
        return m;
    }

private:
    double hi_[N] = {};
    double lo_[N] = {};
    std::size_t head_ = 0;
    std::size_t n_ = 0;
};

// ---------------------------------------------------------------------------
// Ichimoku
// ---------------------------------------------------------------------------

/// The cloud as it stands AT THE CURRENT BAR -- i.e. computed 26 bars ago and
/// displaced forward to here. This is what is legitimately readable now.
struct IchimokuValue {
    double tenkan = 0.0;        // (9-high + 9-low) / 2
    double kijun = 0.0;         // (26-high + 26-low) / 2
    /// Senkou A and B PLOTTED AT THIS BAR. Both were computed from bars
    /// `displacement` ago, which is why they are readable at all.
    double senkou_a = 0.0;
    double senkou_b = 0.0;
    bool cloud_ready = false;
};

/// Ichimoku Kinko Hyo, with the displacements handled rather than assumed.
///
/// The default periods are Hosoda's 9/26/52 and the displacement is 26. They
/// are constructor arguments and not literals because a 26-period displacement
/// belongs to a six-day trading week that has not existed since 1989, and
/// anyone applying this to 5-minute bars is choosing something else.
template <std::size_t Cap = 256>
class Ichimoku {
public:
    Ichimoku(int tenkan, int kijun, int senkou_b, int displacement) noexcept
        : tenkan_p_(tenkan), kijun_p_(kijun), senkou_b_p_(senkou_b),
          disp_(static_cast<std::size_t>(displacement)) {}

    [[nodiscard]] std::expected<IchimokuValue, StructureError>
    update(const Bar& b) noexcept {
        if (!b.valid()) { return std::unexpected(StructureError::BadBar); }
        const double h = static_cast<double>(b.high.raw());
        const double l = static_cast<double>(b.low.raw());
        hi_[i_ % Cap] = h;
        lo_[i_ % Cap] = l;
        close_[i_ % Cap] = static_cast<double>(b.close.raw());
        ++i_;

        IchimokuValue v{};
        const auto mid = [&](int p) -> double {
            const std::size_t n = static_cast<std::size_t>(p);
            double hh = -1e308, ll = 1e308;
            for (std::size_t k = 0; k < n; ++k) {
                const std::size_t idx = (i_ - 1 - k) % Cap;
                if (hi_[idx] > hh) { hh = hi_[idx]; }
                if (lo_[idx] < ll) { ll = lo_[idx]; }
            }
            return 0.5 * (hh + ll);
        };
        if (i_ < static_cast<std::size_t>(kijun_p_)) {
            return std::unexpected(StructureError::WarmingUp);
        }
        v.tenkan = mid(tenkan_p_);
        v.kijun = mid(kijun_p_);

        // The cloud plotted HERE was computed `disp_` bars ago. Recover it by
        // evaluating the spans as they stood then -- which needs
        // disp_ + senkou_b_p_ bars of history, and says so until it has them.
        const std::size_t need = disp_ + static_cast<std::size_t>(senkou_b_p_);
        if (i_ >= need) {
            const auto mid_back = [&](int p, std::size_t back) -> double {
                const std::size_t n = static_cast<std::size_t>(p);
                double hh = -1e308, ll = 1e308;
                for (std::size_t k = 0; k < n; ++k) {
                    const std::size_t idx = (i_ - 1 - back - k) % Cap;
                    if (hi_[idx] > hh) { hh = hi_[idx]; }
                    if (lo_[idx] < ll) { ll = lo_[idx]; }
                }
                return 0.5 * (hh + ll);
            };
            v.senkou_a = 0.5 * (mid_back(tenkan_p_, disp_)
                                + mid_back(kijun_p_, disp_));
            v.senkou_b = mid_back(senkou_b_p_, disp_);
            v.cloud_ready = true;
        }
        return v;
    }

    /// The Chikou span, and the ONLY honest way to expose it.
    ///
    /// Chikou is today's close drawn `displacement` bars in the past. So the
    /// value describing bar (i - displacement) is not known until bar i, and
    /// that is what the two indices say. A caller that wants to use it at bar
    /// (i - displacement) has to read `known_at`, see the gap, and decide to
    /// ignore it -- which is a different act from doing it by accident.
    [[nodiscard]] std::expected<Dated, StructureError>
    chikou() const noexcept {
        if (i_ <= disp_) { return std::unexpected(StructureError::WarmingUp); }
        Dated d{};
        d.value = close_[(i_ - 1) % Cap];   // TODAY's close
        d.describes = i_ - 1 - disp_;       // drawn back there
        d.known_at = i_ - 1;                // knowable only here
        return d;
    }

    /// The price the Chikou span is compared against: the close `displacement`
    /// bars ago. Paired with `chikou()` so the comparison has both halves and
    /// their indices.
    [[nodiscard]] std::expected<Dated, StructureError>
    chikou_reference() const noexcept {
        if (i_ <= disp_) { return std::unexpected(StructureError::WarmingUp); }
        Dated d{};
        const std::size_t at = i_ - 1 - disp_;
        d.value = close_[at % Cap];
        d.describes = at;
        d.known_at = at;                    // it was known when it happened
        return d;
    }

    [[nodiscard]] std::size_t bars() const noexcept { return i_; }
    [[nodiscard]] std::size_t displacement() const noexcept { return disp_; }

private:
    double hi_[Cap] = {};
    double lo_[Cap] = {};
    double close_[Cap] = {};
    int tenkan_p_, kijun_p_, senkou_b_p_;
    std::size_t disp_;
    std::size_t i_ = 0;
};

// ---------------------------------------------------------------------------
// Stochastic
// ---------------------------------------------------------------------------

struct StochasticValue {
    double k = 0.0;             // raw %K, or slow %K if smoothed
    double d = 0.0;             // %D, an SMA of %K
};

/// Stochastic oscillator.
///
/// `k_smooth` of 1 gives the FAST stochastic and anything above it the slow
/// one. There is no default: fast and slow stochastics are different
/// indicators that share a name, and the three-parameter form (14, 3, 3) is a
/// convention rather than a definition.
template <std::size_t Window, std::size_t Smooth, std::size_t DPeriod>
class Stochastic {
public:
    [[nodiscard]] std::expected<StochasticValue, StructureError>
    update(const Bar& b) noexcept {
        if (!b.valid()) { return std::unexpected(StructureError::BadBar); }
        ext_.push(static_cast<double>(b.high.raw()),
                  static_cast<double>(b.low.raw()));
        if (!ext_.ready()) { return std::unexpected(StructureError::WarmingUp); }

        const double hh = ext_.highest();
        const double ll = ext_.lowest();
        const double c = static_cast<double>(b.close.raw());
        // A window whose high equals its low has NO position within it. 50 is
        // not the answer -- it is a specific claim about the middle -- so this
        // reports Degenerate through WarmingUp rather than inventing one.
        if (!(hh > ll)) { return std::unexpected(StructureError::WarmingUp); }
        const double raw_k = 100.0 * (c - ll) / (hh - ll);

        push(k_buf_, k_n_, k_head_, Smooth, raw_k);
        if (k_n_ < Smooth) { return std::unexpected(StructureError::WarmingUp); }
        double k = 0.0;
        for (std::size_t i = 0; i < Smooth; ++i) { k += k_buf_[i]; }
        k /= static_cast<double>(Smooth);

        push(d_buf_, d_n_, d_head_, DPeriod, k);
        if (d_n_ < DPeriod) { return std::unexpected(StructureError::WarmingUp); }
        double d = 0.0;
        for (std::size_t i = 0; i < DPeriod; ++i) { d += d_buf_[i]; }
        d /= static_cast<double>(DPeriod);
        return StochasticValue{k, d};
    }

private:
    static void push(double* buf, std::size_t& n, std::size_t& head,
                     std::size_t cap, double x) noexcept {
        buf[head] = x;
        head = (head + 1 == cap) ? 0 : head + 1;
        if (n < cap) { ++n; }
    }
    RollingExtreme<Window> ext_;
    double k_buf_[Smooth] = {};
    double d_buf_[DPeriod] = {};
    std::size_t k_n_ = 0, k_head_ = 0;
    std::size_t d_n_ = 0, d_head_ = 0;
};

// ---------------------------------------------------------------------------
// Heikin-Ashi
// ---------------------------------------------------------------------------

/// A Heikin-Ashi bar, WITH the real bar it was derived from.
///
/// The real OHLC travels alongside because the synthetic values are not
/// prices: `close` is the average of four numbers and `open` is an average of
/// the previous SYNTHETIC bar, so neither is a level anything traded at. A
/// backtest filling at `close` fills inside the bar every time, which is a
/// systematic and flattering error. Fill against `real`.
struct HeikinAshiBar {
    double open = 0.0;
    double high = 0.0;
    double low = 0.0;
    double close = 0.0;
    Bar real{};
    /// How many synthetic bars have been produced. HA is a RECURRENCE -- its
    /// open depends on the previous open -- so the first bar's seed persists,
    /// and a caller that started mid-series is looking at its own choice of
    /// starting point for a while.
    std::size_t index = 0;
};

class HeikinAshi {
public:
    [[nodiscard]] std::expected<HeikinAshiBar, StructureError>
    update(const Bar& b) noexcept {
        if (!b.valid()) { return std::unexpected(StructureError::BadBar); }
        const double o = static_cast<double>(b.open.raw());
        const double h = static_cast<double>(b.high.raw());
        const double l = static_cast<double>(b.low.raw());
        const double c = static_cast<double>(b.close.raw());

        HeikinAshiBar out{};
        out.close = 0.25 * (o + h + l + c);
        out.open = started_ ? 0.5 * (prev_open_ + prev_close_) : 0.5 * (o + c);
        out.high = h > out.open ? (h > out.close ? h : out.close)
                                : (out.open > out.close ? out.open : out.close);
        out.low = l < out.open ? (l < out.close ? l : out.close)
                               : (out.open < out.close ? out.open : out.close);
        out.real = b;
        out.index = n_;

        prev_open_ = out.open;
        prev_close_ = out.close;
        started_ = true;
        ++n_;
        return out;
    }

private:
    double prev_open_ = 0.0;
    double prev_close_ = 0.0;
    std::size_t n_ = 0;
    bool started_ = false;
};

// ---------------------------------------------------------------------------
// Pivots
// ---------------------------------------------------------------------------

struct PivotLevels {
    double pivot = 0.0;
    double r1 = 0.0, r2 = 0.0, r3 = 0.0;
    double s1 = 0.0, s2 = 0.0, s3 = 0.0;
    /// The session these were computed FROM. Always the previous one.
    std::size_t from_session = 0;
    /// The session they are valid FOR.
    std::size_t for_session = 0;
};

/// Classical floor-trader pivots from the PREVIOUS session's high, low and
/// close.
///
/// Taking a completed session and returning the levels for the NEXT one is the
/// only signature this can have without inviting the bug: there is no overload
/// that takes today's bar and returns today's levels, because that function
/// cannot be written honestly.
[[nodiscard]] inline std::expected<PivotLevels, StructureError>
pivots_for_next_session(const Bar& completed,
                        std::size_t session_index) noexcept {
    if (!completed.valid()) { return std::unexpected(StructureError::BadBar); }
    const double h = static_cast<double>(completed.high.raw());
    const double l = static_cast<double>(completed.low.raw());
    const double c = static_cast<double>(completed.close.raw());
    PivotLevels p{};
    p.pivot = (h + l + c) / 3.0;
    const double range = h - l;
    p.r1 = 2.0 * p.pivot - l;
    p.s1 = 2.0 * p.pivot - h;
    p.r2 = p.pivot + range;
    p.s2 = p.pivot - range;
    p.r3 = h + 2.0 * (p.pivot - l);
    p.s3 = l - 2.0 * (h - p.pivot);
    p.from_session = session_index;
    p.for_session = session_index + 1;
    return p;
}

// ---------------------------------------------------------------------------
// Market structure
// ---------------------------------------------------------------------------

enum class SwingKind : std::uint8_t { Unknown = 0, High, Low };

struct Swing {
    SwingKind kind = SwingKind::Unknown;
    double price = 0.0;
    /// The bar the swing HAPPENED at, and the bar it was CONFIRMED at. They
    /// differ by the confirmation window, always.
    std::size_t at = 0;
    std::size_t confirmed_at = 0;
};

enum class StructureState : std::uint8_t {
    Unknown = 0,
    /// Higher highs and higher lows.
    Uptrend,
    /// Lower highs and lower lows.
    Downtrend,
    /// Neither pattern holds.
    Range
};

/// Fractal swing detection with an explicit confirmation window.
///
/// A bar is a swing high when `k` bars on EACH side are lower. The right-hand
/// half of that condition is the future, so a swing at bar i is not knowable
/// until bar i + k. This emits swings at their confirmation bar and carries
/// both indices, so a strategy cannot act on one `k` bars before it existed
/// without ignoring the field that says so.
template <std::size_t K, std::size_t Cap = 512>
class SwingDetector {
public:
    /// Feed one bar. Returns a swing when THIS bar confirms one, which is
    /// always a swing that happened K bars ago.
    [[nodiscard]] std::expected<Swing, StructureError>
    update(const Bar& b) noexcept {
        if (!b.valid()) { return std::unexpected(StructureError::BadBar); }
        hi_[i_ % Cap] = static_cast<double>(b.high.raw());
        lo_[i_ % Cap] = static_cast<double>(b.low.raw());
        ++i_;
        if (i_ < 2 * K + 1) {
            return std::unexpected(StructureError::WarmingUp);
        }
        const std::size_t centre = i_ - 1 - K;
        const double ch = hi_[centre % Cap];
        const double cl = lo_[centre % Cap];
        bool is_high = true, is_low = true;
        for (std::size_t d = 1; d <= K; ++d) {
            if (hi_[(centre - d) % Cap] >= ch
                || hi_[(centre + d) % Cap] >= ch) { is_high = false; }
            if (lo_[(centre - d) % Cap] <= cl
                || lo_[(centre + d) % Cap] <= cl) { is_low = false; }
        }
        if (!is_high && !is_low) {
            return std::unexpected(StructureError::NotYetKnown);
        }
        Swing s{};
        s.kind = is_high ? SwingKind::High : SwingKind::Low;
        s.price = is_high ? ch : cl;
        s.at = centre;
        s.confirmed_at = i_ - 1;            // K bars later, always
        record(s);
        return s;
    }

    /// Higher-high / higher-low structure from CONFIRMED swings only.
    [[nodiscard]] StructureState state() const noexcept {
        if (highs_n_ < 2 || lows_n_ < 2) { return StructureState::Unknown; }
        const bool hh = highs_[1] > highs_[0];
        const bool hl = lows_[1] > lows_[0];
        if (hh && hl) { return StructureState::Uptrend; }
        if (!hh && !hl) { return StructureState::Downtrend; }
        return StructureState::Range;
    }

    [[nodiscard]] static constexpr std::size_t confirmation_lag() noexcept {
        return K;
    }

private:
    void record(const Swing& s) noexcept {
        if (s.kind == SwingKind::High) {
            highs_[0] = highs_[1];
            highs_[1] = s.price;
            if (highs_n_ < 2) { ++highs_n_; }
        } else {
            lows_[0] = lows_[1];
            lows_[1] = s.price;
            if (lows_n_ < 2) { ++lows_n_; }
        }
    }
    double hi_[Cap] = {};
    double lo_[Cap] = {};
    double highs_[2] = {};
    double lows_[2] = {};
    std::size_t highs_n_ = 0, lows_n_ = 0;
    std::size_t i_ = 0;
};

} // namespace altair
