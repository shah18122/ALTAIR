// Exact, allocation-free sum of a bounded number of signed 64-bit values.
#pragma once

#include <cstdint>
#include <limits>

namespace altair {

/// Accumulates signed 64-bit terms without overflowing an intermediate.
/// Positive and negative magnitudes are kept separately in two limbs, so a
/// sum such as INT64_MAX + 1 + INT64_MIN is correctly recognised as zero.
/// The two-limb totals are sufficient for any number of terms addressable by
/// size_t; callers in the hot risk/accounting paths add at most five.
class ExactSignedSum {
public:
    constexpr void add(std::int64_t value) noexcept {
        if (value < 0) {
            negative_.add(magnitude(value));
        } else {
            positive_.add(static_cast<std::uint64_t>(value));
        }
    }

    /// Subtract a signed value without ever negating INT64_MIN.
    constexpr void subtract(std::int64_t value) noexcept {
        if (value < 0) {
            positive_.add(magnitude(value));
        } else {
            negative_.add(static_cast<std::uint64_t>(value));
        }
    }

    [[nodiscard]] constexpr bool is_zero() const noexcept {
        return positive_ == negative_;
    }

    /// True when the exact result is representable as int64_t.
    [[nodiscard]] constexpr bool try_value(std::int64_t& out) const noexcept {
        const int order = compare(positive_, negative_);
        if (order == 0) {
            out = 0;
            return true;
        }
        const Magnitude difference = order > 0
            ? subtract_magnitude(positive_, negative_)
            : subtract_magnitude(negative_, positive_);
        if (difference.high != 0) { return false; }

        constexpr std::uint64_t kNegativeLimit = std::uint64_t{1} << 63;
        if (order > 0) {
            if (difference.low > static_cast<std::uint64_t>(
                                     std::numeric_limits<std::int64_t>::max())) {
                return false;
            }
            out = static_cast<std::int64_t>(difference.low);
            return true;
        }
        if (difference.low > kNegativeLimit) { return false; }
        if (difference.low == kNegativeLimit) {
            out = std::numeric_limits<std::int64_t>::min();
        } else {
            out = -static_cast<std::int64_t>(difference.low);
        }
        return true;
    }

    /// Convert to int64_t, saturating only if the exact result is out of range.
    /// Use `try_value` when the caller must distinguish saturation from an
    /// exact endpoint value.
    [[nodiscard]] constexpr std::int64_t saturated_value() const noexcept {
        std::int64_t out = 0;
        if (try_value(out)) { return out; }
        return compare(positive_, negative_) > 0
            ? std::numeric_limits<std::int64_t>::max()
            : std::numeric_limits<std::int64_t>::min();
    }

private:
    struct Magnitude {
        std::uint64_t high = 0;
        std::uint64_t low = 0;

        constexpr void add(std::uint64_t value) noexcept {
            const std::uint64_t before = low;
            low += value;
            if (low < before) { ++high; }
        }

        friend constexpr bool operator==(Magnitude, Magnitude) noexcept = default;
    };

    [[nodiscard]] static constexpr std::uint64_t
    magnitude(std::int64_t value) noexcept {
        // value + 1 and its negation are representable even for INT64_MIN.
        return static_cast<std::uint64_t>(-(value + 1)) + 1u;
    }

    [[nodiscard]] static constexpr int compare(Magnitude a,
                                               Magnitude b) noexcept {
        if (a.high != b.high) { return a.high > b.high ? 1 : -1; }
        if (a.low != b.low) { return a.low > b.low ? 1 : -1; }
        return 0;
    }

    /// PRECONDITION: a >= b.
    [[nodiscard]] static constexpr Magnitude
    subtract_magnitude(Magnitude a, Magnitude b) noexcept {
        const bool borrow = a.low < b.low;
        return Magnitude{a.high - b.high - static_cast<std::uint64_t>(borrow),
                         a.low - b.low};
    }

    Magnitude positive_{};
    Magnitude negative_{};
};

} // namespace altair
