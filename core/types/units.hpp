#pragma once

#include <cstdint>
#include <compare>
#include <expected>
#include <type_traits>
#include <limits>

// Hot-path marker. Functions carrying this must never allocate.
#if defined(__GNUC__) || defined(__clang__)
#  define ALTAIR_HOT [[gnu::hot]]
#else
#  define ALTAIR_HOT
#endif

namespace altair {

// ─────────────────────────────────────────────────────────────────────────
// Errors
// ─────────────────────────────────────────────────────────────────────────
enum class ArithError : std::uint8_t {
    Overflow,
    DivideByZero,
    NotRepresentable   // e.g. a rupee value that is not a whole number of paise
};

enum class RoundMode : std::uint8_t { Nearest, Down, Up };

// ─────────────────────────────────────────────────────────────────────────
// Dimension tags — incomplete types, never instantiated
// ─────────────────────────────────────────────────────────────────────────
struct PriceTag;
struct QtyTag;
struct LotsTag;
struct LotSizeTag;
struct NotionalTag;

struct RateTag;
struct YearsTag;
struct VolTag;
struct BpsTag;

// ─────────────────────────────────────────────────────────────────────────
// Internal constexpr helpers (not part of the public interface)
// ─────────────────────────────────────────────────────────────────────────
namespace detail {

constexpr bool mul_overflows(std::int64_t a, std::int64_t b) noexcept {
    if (a == 0 || b == 0) {
        return false;
    }
    const auto mx = std::numeric_limits<std::int64_t>::max();
    const auto mn = std::numeric_limits<std::int64_t>::min();
    if (a > 0) {
        if (b > 0) {
            return a > mx / b;
        }
        return b < mn / a;
    }
    if (b > 0) {
        return a < mn / b;
    }
    return a < mx / b;
}

constexpr bool finite_ld(long double v) noexcept {
    return v == v &&
           v != std::numeric_limits<long double>::infinity() &&
           v != -std::numeric_limits<long double>::infinity();
}

/// True iff v is neither NaN nor infinite. constexpr-safe; no <cmath>.
constexpr bool is_finite_d(double v) noexcept {
    return v == v
        && v != std::numeric_limits<double>::infinity()
        && v != -std::numeric_limits<double>::infinity();
}

/// |v|. constexpr-safe.
constexpr double abs_d(double v) noexcept { return v < 0.0 ? -v : v; }

/// Round half away from zero.
/// PRECONDITION: v is finite and |v| < 2^63. The caller MUST range-check
/// before calling — static_cast<std::int64_t> outside that range is UB.
constexpr double round_half_away(double v) noexcept {
    const double t = static_cast<double>(static_cast<std::int64_t>(v)); // trunc toward zero
    const double frac = v - t;
    if (frac >=  0.5) { return t + 1.0; }
    if (frac <= -0.5) { return t - 1.0; }
    return t;
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────
// StrongInt — an exact integral quantity carrying a dimension
// ─────────────────────────────────────────────────────────────────────────
template <typename Tag, typename Rep>
class StrongInt {
public:
    using rep_type = Rep;
    using tag_type = Tag;

    constexpr StrongInt() noexcept = default;
    constexpr explicit StrongInt(Rep v) noexcept : v_(v) {}

    [[nodiscard]] constexpr Rep raw() const noexcept { return v_; }

    friend constexpr auto operator<=>(StrongInt, StrongInt) noexcept = default;
    friend constexpr bool operator==(StrongInt, StrongInt) noexcept = default;

    constexpr StrongInt& operator+=(StrongInt other) noexcept {
        v_ = static_cast<Rep>(v_ + other.v_);
        return *this;
    }
    constexpr StrongInt& operator-=(StrongInt other) noexcept {
        v_ = static_cast<Rep>(v_ - other.v_);
        return *this;
    }

    [[nodiscard]] constexpr StrongInt operator-() const noexcept {
        return StrongInt{static_cast<Rep>(-v_)};
    }

    friend constexpr StrongInt operator+(StrongInt a, StrongInt b) noexcept {
        return StrongInt{static_cast<Rep>(a.v_ + b.v_)};
    }
    friend constexpr StrongInt operator-(StrongInt a, StrongInt b) noexcept {
        return StrongInt{static_cast<Rep>(a.v_ - b.v_)};
    }

    // Scaling by a DIMENSIONLESS integer of the same representation.
    friend constexpr StrongInt operator*(StrongInt a, Rep k) noexcept {
        return StrongInt{static_cast<Rep>(a.v_ * k)};
    }
    friend constexpr StrongInt operator*(Rep k, StrongInt a) noexcept {
        return StrongInt{static_cast<Rep>(k * a.v_)};
    }

    [[nodiscard]] constexpr bool is_zero()     const noexcept { return v_ == Rep{0}; }
    [[nodiscard]] constexpr bool is_positive() const noexcept { return v_ > Rep{0}; }
    [[nodiscard]] constexpr bool is_negative() const noexcept { return v_ < Rep{0}; }

    [[nodiscard]] static constexpr StrongInt zero() noexcept { return StrongInt{Rep{0}}; }
    [[nodiscard]] static constexpr StrongInt min()  noexcept { return StrongInt{std::numeric_limits<Rep>::min()}; }
    [[nodiscard]] static constexpr StrongInt max()  noexcept { return StrongInt{std::numeric_limits<Rep>::max()}; }

private:
    Rep v_{};
};

// Dimensionless ratio of two like quantities.
template <typename Tag, typename Rep>
[[nodiscard]] constexpr std::expected<double, ArithError>
ratio(StrongInt<Tag, Rep> numerator, StrongInt<Tag, Rep> denominator) noexcept {
    if (denominator.raw() == Rep{0}) {
        return std::unexpected(ArithError::DivideByZero);
    }
    return static_cast<double>(numerator.raw()) / static_cast<double>(denominator.raw());
}

// ─────────────────────────────────────────────────────────────────────────
// StrongReal — a real-valued quantity carrying a dimension
// ─────────────────────────────────────────────────────────────────────────
template <typename Tag>
class StrongReal {
public:
    using rep_type = double;
    using tag_type = Tag;

    constexpr StrongReal() noexcept = default;
    constexpr explicit StrongReal(double v) noexcept : v_(v) {}

    [[nodiscard]] constexpr double raw() const noexcept { return v_; }

    friend constexpr auto operator<=>(StrongReal, StrongReal) noexcept = default;
    friend constexpr bool operator==(StrongReal, StrongReal) noexcept = default;

    constexpr StrongReal& operator+=(StrongReal other) noexcept {
        v_ += other.v_;
        return *this;
    }
    constexpr StrongReal& operator-=(StrongReal other) noexcept {
        v_ -= other.v_;
        return *this;
    }

    [[nodiscard]] constexpr StrongReal operator-() const noexcept {
        return StrongReal{-v_};
    }

    friend constexpr StrongReal operator+(StrongReal a, StrongReal b) noexcept {
        return StrongReal{a.v_ + b.v_};
    }
    friend constexpr StrongReal operator-(StrongReal a, StrongReal b) noexcept {
        return StrongReal{a.v_ - b.v_};
    }
    friend constexpr StrongReal operator*(StrongReal a, double k) noexcept {
        return StrongReal{a.v_ * k};
    }
    friend constexpr StrongReal operator*(double k, StrongReal a) noexcept {
        return StrongReal{k * a.v_};
    }

    [[nodiscard]] constexpr bool is_finite() const noexcept {
        return v_ == v_ &&
               v_ != std::numeric_limits<double>::infinity() &&
               v_ != -std::numeric_limits<double>::infinity();
    }

private:
    double v_{};
};

// ─────────────────────────────────────────────────────────────────────────
// The dimensions
// ─────────────────────────────────────────────────────────────────────────

/// Price of ONE unit, in integer paise. 1 rupee = 100 paise.
using Price    = StrongInt<PriceTag,    std::int64_t>;

/// A number of tradable UNITS (shares, or contracts x lot_size).
using Qty      = StrongInt<QtyTag,      std::int64_t>;

/// A number of CONTRACTS. Meaningless without a LotSize.
using Lots     = StrongInt<LotsTag,     std::int64_t>;

/// Units per contract. Auto-learned daily; never a literal outside tests.
using LotSize  = StrongInt<LotSizeTag,  std::int64_t>;

/// A money amount in integer paise. Price x Qty, P&L, cost, cash.
using Notional = StrongInt<NotionalTag, std::int64_t>;

/// Interest / dividend rate, per annum, CONTINUOUSLY COMPOUNDED.
using Rate     = StrongReal<RateTag>;

/// Time in years, ACT/365 Fixed.
using Years    = StrongReal<YearsTag>;

/// Annualised volatility (sigma), as a fraction. 0.15 == 15%.
using Vol      = StrongReal<VolTag>;

/// Basis points. 1.0 == 1 bp == 1e-4.
using Bps      = StrongReal<BpsTag>;

// ─────────────────────────────────────────────────────────────────────────
// Cross-dimension operations. These are the ONLY legal ways to combine
// different dimensions. All are checked and return std::expected.
// ─────────────────────────────────────────────────────────────────────────

/// Notional = Price x Qty.  Both in paise / units. Detects int64 overflow.
[[nodiscard]] ALTAIR_HOT constexpr std::expected<Notional, ArithError>
notional_of(Price p, Qty q) noexcept {
    if (detail::mul_overflows(p.raw(), q.raw())) {
        return std::unexpected(ArithError::Overflow);
    }
    return Notional{p.raw() * q.raw()};
}

/// Qty = Lots x LotSize.  The ONLY way to turn contracts into units.
[[nodiscard]] ALTAIR_HOT constexpr std::expected<Qty, ArithError>
qty_of(Lots l, LotSize ls) noexcept {
    if (ls.raw() < 0) {
        return std::unexpected(ArithError::NotRepresentable);
    }
    if (detail::mul_overflows(l.raw(), ls.raw())) {
        return std::unexpected(ArithError::Overflow);
    }
    return Qty{l.raw() * ls.raw()};
}

/// Lots = floor(Qty / LotSize).  Floor toward negative infinity, not toward zero.
[[nodiscard]] ALTAIR_HOT constexpr std::expected<Lots, ArithError>
lots_of(Qty q, LotSize ls) noexcept {
    if (ls.raw() == 0) {
        return std::unexpected(ArithError::DivideByZero);
    }
    if (ls.raw() < 0) {
        return std::unexpected(ArithError::NotRepresentable);
    }
    const auto qq = q.raw();
    const auto dd = ls.raw();
    auto quot = qq / dd;
    const auto rem = qq % dd;
    if (rem != 0 && rem < 0) {
        --quot;
    }
    return Lots{quot};
}

/// Price = Notional / Qty.  Truncates toward zero. DivideByZero if q == 0.
[[nodiscard]] ALTAIR_HOT constexpr std::expected<Price, ArithError>
price_of(Notional n, Qty q) noexcept {
    if (q.raw() == 0) {
        return std::unexpected(ArithError::DivideByZero);
    }
    if (n.raw() == std::numeric_limits<std::int64_t>::min() && q.raw() == -1) {
        return std::unexpected(ArithError::Overflow);
    }
    return Price{n.raw() / q.raw()};
}

/// Apply a basis-point rate to a money amount. Rounds half away from zero.
[[nodiscard]] ALTAIR_HOT constexpr std::expected<Notional, ArithError>
apply_bps(Notional n, Bps b) noexcept {
    const long double rate = static_cast<long double>(b.raw());
    if (!detail::finite_ld(rate)) {
        return std::unexpected(ArithError::Overflow);
    }

    const long double x = static_cast<long double>(n.raw()) * rate * 1e-4L;
    if (!detail::finite_ld(x)) {
        return std::unexpected(ArithError::Overflow);
    }

    const long double half = x < 0 ? -0.5L : 0.5L;
    const long double shifted = x + half;
    if (!detail::finite_ld(shifted)) {
        return std::unexpected(ArithError::Overflow);
    }

    const auto mn = static_cast<long double>(std::numeric_limits<std::int64_t>::min());
    const auto mx = static_cast<long double>(std::numeric_limits<std::int64_t>::max());
    if (shifted < mn || shifted > mx) {
        return std::unexpected(ArithError::Overflow);
    }

    return Notional{static_cast<std::int64_t>(shifted)};
}

// ─────────────────────────────────────────────────────────────────────────
// Tick rounding — snap a price to a legal exchange tick
// ─────────────────────────────────────────────────────────────────────────

/// Round `p` to a multiple of `tick`. Nearest uses half away from zero.
/// Down/Up are floor/ceil toward -inf/+inf respectively, INCLUDING for
/// negative prices (spreads can be negative).
/// DivideByZero if tick is zero; NotRepresentable if tick is negative;
/// Overflow if the rounded multiple is outside int64 (CX02-D1, finding
/// C21-001 -- the correction card is prompts/cx02/CORRECTIONS.md).
[[nodiscard]] ALTAIR_HOT constexpr std::expected<Price, ArithError>
round_to_tick(Price p, Price tick, RoundMode mode) noexcept {
    const auto t = tick.raw();
    if (t == 0) {
        return std::unexpected(ArithError::DivideByZero);
    }
    if (t < 0) {
        return std::unexpected(ArithError::NotRepresentable);
    }

    const auto v = p.raw();
    auto quot = v / t;
    const auto rem = v % t;

    if (mode == RoundMode::Down) {
        if (rem < 0) {
            --quot;
        }
    } else if (mode == RoundMode::Up) {
        if (rem > 0) {
            ++quot;
        }
    } else {
        const auto abs_rem = rem < 0 ? -rem : rem;
        if (abs_rem >= t - abs_rem) {
            if (rem >= 0) {
                ++quot;
            } else {
                --quot;
            }
        }
    }

    // CHECKED BEFORE THE MULTIPLY, exactly as item 4 requires of notional_of.
    // P0-01's item 9 listed only DivideByZero and NotRepresentable, so this
    // was left out and `quot * t` could wrap at the extremes: signed overflow
    // is undefined behaviour, not a large number (C21-001). Rounding
    // Price::max() UP to the next tick has no representable answer, and a
    // refusal is the answer.
    constexpr std::int64_t kI64Max = 0x7FFF'FFFF'FFFF'FFFF;
    constexpr std::int64_t kI64Min = -kI64Max - 1;
    if (quot > kI64Max / t || quot < kI64Min / t) {
        return std::unexpected(ArithError::Overflow);
    }
    return Price{quot * t};
}

/// True iff p is an exact multiple of tick.
[[nodiscard]] ALTAIR_HOT constexpr std::expected<bool, ArithError>
is_on_tick(Price p, Price tick) noexcept {
    const auto t = tick.raw();
    if (t == 0) {
        return std::unexpected(ArithError::DivideByZero);
    }
    if (t < 0) {
        return std::unexpected(ArithError::NotRepresentable);
    }
    return p.raw() % t == 0;
}

// ─────────────────────────────────────────────────────────────────────────
// Rupee conversion — for display, analytics, and test readability ONLY.
// Never use doubles in the ledger or the order book.
// ─────────────────────────────────────────────────────────────────────────

[[nodiscard]] constexpr double to_rupees(Price p)    noexcept {
    return static_cast<double>(p.raw()) / 100.0;
}
[[nodiscard]] constexpr double to_rupees(Notional n) noexcept {
    return static_cast<double>(n.raw()) / 100.0;
}

/// Convert rupees to exact paise. NotRepresentable if the value is not within
/// 1e-9 of a whole number of paise, or is not finite.
[[nodiscard]] constexpr std::expected<Price, ArithError>
price_from_rupees(double rupees) noexcept {
    if (!detail::is_finite_d(rupees)) {
        return std::unexpected(ArithError::NotRepresentable);
    }
    const double scaled = rupees * 100.0;
    if (!detail::is_finite_d(scaled)) {
        return std::unexpected(ArithError::NotRepresentable);
    }
    // Range-check BEFORE truncating. 2^63 as a double is exactly
    // 9223372036854775808.0; anything at or beyond it is out of int64 range.
    constexpr double kPow63 = 9223372036854775808.0;
    if (scaled >= kPow63 || scaled < -kPow63) {
        return std::unexpected(ArithError::NotRepresentable);
    }
    const double rounded = detail::round_half_away(scaled);
    if (detail::abs_d(scaled - rounded) > 1e-9) {
        return std::unexpected(ArithError::NotRepresentable);
    }
    return Price{static_cast<std::int64_t>(rounded)};
}

[[nodiscard]] constexpr std::expected<Notional, ArithError>
notional_from_rupees(double rupees) noexcept {
    if (!detail::is_finite_d(rupees)) {
        return std::unexpected(ArithError::NotRepresentable);
    }
    const double scaled = rupees * 100.0;
    if (!detail::is_finite_d(scaled)) {
        return std::unexpected(ArithError::NotRepresentable);
    }
    // Range-check BEFORE truncating. 2^63 as a double is exactly
    // 9223372036854775808.0; anything at or beyond it is out of int64 range.
    constexpr double kPow63 = 9223372036854775808.0;
    if (scaled >= kPow63 || scaled < -kPow63) {
        return std::unexpected(ArithError::NotRepresentable);
    }
    const double rounded = detail::round_half_away(scaled);
    if (detail::abs_d(scaled - rounded) > 1e-9) {
        return std::unexpected(ArithError::NotRepresentable);
    }
    return Notional{static_cast<std::int64_t>(rounded)};
}

// ─────────────────────────────────────────────────────────────────────────
// Literals — for tests and configuration only
// ─────────────────────────────────────────────────────────────────────────
namespace literals {

[[nodiscard]] constexpr Price    operator""_paise   (unsigned long long v) noexcept {
    return Price{static_cast<std::int64_t>(v)};
}
[[nodiscard]] constexpr Qty      operator""_qty     (unsigned long long v) noexcept {
    return Qty{static_cast<std::int64_t>(v)};
}
[[nodiscard]] constexpr Lots     operator""_lots    (unsigned long long v) noexcept {
    return Lots{static_cast<std::int64_t>(v)};
}
[[nodiscard]] constexpr LotSize  operator""_lotsize (unsigned long long v) noexcept {
    return LotSize{static_cast<std::int64_t>(v)};
}
[[nodiscard]] constexpr Notional operator""_np      (unsigned long long v) noexcept {
    return Notional{static_cast<std::int64_t>(v)};
}
[[nodiscard]] constexpr Bps      operator""_bps     (long double v)        noexcept {
    return Bps{static_cast<double>(v)};
}
[[nodiscard]] constexpr Vol      operator""_vol     (long double v)        noexcept {
    return Vol{static_cast<double>(v)};
}
[[nodiscard]] constexpr Years    operator""_years   (long double v)        noexcept {
    return Years{static_cast<double>(v)};
}

} // namespace literals
} // namespace altair
