// features/vector.hpp -- a feature vector, and what "not ready" means.
//
// P5-02 (shared by every builder family).
//
// A FEATURE THAT IS NOT READY IS ABSENT, NOT ZERO.
//
// This is the whole reason the type exists. Zero is a perfectly ordinary value
// for almost everything a builder produces: a z-score of zero means at the
// mean, a velocity of zero means not moving, an imbalance of zero means
// balanced. So a vector that fills unready slots with 0.0 hands the model a
// confident, plausible, meaningful-looking number for a feature that has no
// value yet -- during warmup, after a gap, on an illiquid symbol that has not
// printed in ten minutes.
//
// A NaN is not the answer either. It propagates, it is not comparable, and
// half the code that meets one will silently turn it back into zero.
//
// So presence is a separate bitmask, and `value()` returns `std::expected`.
// A caller cannot read a feature without being handed the question of whether
// it is there.
//
// THE VECTOR CARRIES ITS OWN feature_version. Rule 10 wants every decision
// reproducible from {model_hash, feature_version, config_hash, spec_version,
// tick_seqno}, and the cheapest way to keep that honest is for the vector to
// carry the hash of the registry that shaped it -- so a model can refuse a
// vector built by a registry it was not trained against, rather than trusting
// that the wiring is right.

#pragma once

#include <features/registry.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

/// A slot the caller did not register. Builders skip it rather than writing
/// to some default index -- a builder that invented a slot would write over
/// whatever the registry actually put there.
///
/// Lives here, not in one family's header, because every family shares the
/// convention and two definitions of it would eventually disagree.
inline constexpr FeatureIndex kSkip = 0xFFFF;

enum class VectorError : std::uint8_t {
    /// The feature index is past the end of this vector.
    OutOfRange,
    /// The feature has no value at this timestamp -- warming up, or a gap.
    Absent,
    /// A builder tried to store a non-finite value.
    NotFinite,
    /// The vector was built by a different registry than the caller expects.
    VersionMismatch
};

/// One evaluation of the whole feature set at one instant.
class FeatureVector {
public:
    FeatureVector() = default;
    FeatureVector(std::uint64_t feature_version, std::size_t n,
                  Timestamp ts) noexcept
        : version_(feature_version), count_(n), ts_(ts) {}

    /// Store a value. Refuses non-finite: a NaN reaching a model is a NaN
    /// reaching a position size, and it is far better to be absent.
    [[nodiscard]] std::expected<void, VectorError>
    set(FeatureIndex i, double v) noexcept {
        if (i >= count_) { return std::unexpected(VectorError::OutOfRange); }
        if (!std::isfinite(v)) {
            return std::unexpected(VectorError::NotFinite);
        }
        value_[i] = v;
        present_[i / 64] |= (1ULL << (i % 64));
        return {};
    }

    /// Read a value. ABSENT IS AN ERROR, not a zero.
    [[nodiscard]] std::expected<double, VectorError>
    value(FeatureIndex i) const noexcept {
        if (i >= count_) { return std::unexpected(VectorError::OutOfRange); }
        if (!has(i)) { return std::unexpected(VectorError::Absent); }
        return value_[i];
    }

    [[nodiscard]] bool has(FeatureIndex i) const noexcept {
        if (i >= count_) { return false; }
        return (present_[i / 64] & (1ULL << (i % 64))) != 0ULL;
    }

    /// How many of the registered features actually have a value.
    [[nodiscard]] std::size_t present_count() const noexcept {
        std::size_t n = 0;
        for (std::size_t i = 0; i < count_; ++i) {
            if (has(static_cast<FeatureIndex>(i))) { ++n; }
        }
        return n;
    }

    /// True when every registered feature has a value.
    ///
    /// A model should refuse an incomplete vector rather than substituting
    /// anything for the gaps. Imputing a mean is a decision about the market
    /// disguised as a decision about missing data.
    [[nodiscard]] bool complete() const noexcept {
        return present_count() == count_;
    }

    [[nodiscard]] std::size_t size() const noexcept { return count_; }
    [[nodiscard]] Timestamp timestamp() const noexcept { return ts_; }
    [[nodiscard]] std::uint64_t feature_version() const noexcept {
        return version_;
    }

    /// Check this vector came from the registry a model was trained against.
    [[nodiscard]] std::expected<void, VectorError>
    require_version(std::uint64_t expected) const noexcept {
        if (version_ != expected) {
            return std::unexpected(VectorError::VersionMismatch);
        }
        return {};
    }

    void clear(Timestamp ts) noexcept {
        for (auto& w : present_) { w = 0ULL; }
        ts_ = ts;
    }

private:
    double value_[kMaxFeatures] = {};
    std::uint64_t present_[(kMaxFeatures + 63) / 64] = {};
    std::uint64_t version_ = 0;
    std::size_t count_ = 0;
    Timestamp ts_{};
};

} // namespace altair
