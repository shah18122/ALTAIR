// features/registry.hpp -- what the models are allowed to see, and under
// which version.
//
// P5-01. The first card of the modelling side.
//
// RULE 10 SAYS EVERY LIVE DECISION IS REPRODUCIBLE FROM
// {model_hash, feature_version, config_hash, spec_version, tick_seqno}.
// This file is where `feature_version` comes from. If the hash it produces is
// wrong, that tuple is a lie and every replay built on it is a lie too.
//
// THE HASH COVERS THE ORDER, NOT JUST THE SET.
//
// A model's input is a POSITIONAL VECTOR. Feature 7 is whatever the registry
// put at index 7, and the weights the model learned are attached to that
// index, not to a name. So two registries holding the same features in a
// different order are NOT interchangeable, and a hash over an unordered set
// would say they are.
//
// That is the failure this card exists to prevent, and it is silent: the
// tensor is the right shape, every value is in range, nothing errors, and the
// model reads realised volatility where it was trained to read spread. It
// scores confidently and it is wrong. `tests/test_registry.cpp` builds exactly
// that pair and shows the hashes differing.
//
// HORIZON BANDS ARE ENFORCED, NOT ADVISORY.
//
// CLAUDE.md's Nyquist argument: order-book imbalance decays in 10-200 ms. It
// is a real signal for a 30-second forecast and noise for a 10-day one. A
// registry that lets a swing model request it is a registry that lets someone
// fit a swing model to microstructure -- which will backtest beautifully,
// because the noise is in-sample too.
//
// So every feature declares the band range it is valid over, and asking for it
// outside that range is an ERROR. Not a warning; a warning in a feature
// pipeline is a line in a log nobody reads.
//
// A SEALED REGISTRY IS IMMUTABLE. Once sealed the hash is fixed and nothing
// further can be registered. A feature added mid-session would change the
// vector under a model that is already serving, and the tuple recorded against
// the last decision would no longer describe the registry that produced it.

#pragma once

#include <core/time/timestamp.hpp>
#include <core/types/units.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string_view>

namespace altair {

/// Forecast horizon bands.
///
/// Ordinal 0 is Unset so a zeroed spec cannot read as a valid band -- the
/// defect class this repo keeps meeting. The bands are ordered, so a range
/// check is two comparisons.
enum class HorizonBand : std::uint8_t {
    Unset = 0,
    /// Sub-second. Book imbalance, microprice, queue dynamics.
    Micro,
    /// One second to one minute. Flow, short kinematics.
    Fast,
    /// One minute to one session. Most of the quant features.
    Intraday,
    /// Overnight and longer. Fundamentals, term structure, regime.
    Swing
};

[[nodiscard]] constexpr const char* band_name(HorizonBand b) noexcept {
    switch (b) {
    case HorizonBand::Unset:    return "unset";
    case HorizonBand::Micro:    return "micro";
    case HorizonBand::Fast:     return "fast";
    case HorizonBand::Intraday: return "intraday";
    case HorizonBand::Swing:    return "swing";
    }
    return "unknown";
}

/// Which band a forecast horizon falls in.
///
/// The boundaries are stated once, here, so a builder and a model cannot
/// disagree about which band a 90-second forecast is in.
[[nodiscard]] inline HorizonBand band_of(Duration horizon) noexcept {
    const std::int64_t ns = horizon.raw();
    if (ns <= 0) { return HorizonBand::Unset; }
    if (ns < 1'000'000'000LL) { return HorizonBand::Micro; }              // <1s
    if (ns < 60LL * 1'000'000'000LL) { return HorizonBand::Fast; }        // <1m
    if (ns < 6LL * 3600LL * 1'000'000'000LL) { return HorizonBand::Intraday; }
    return HorizonBand::Swing;
}

enum class RegistryError : std::uint8_t {
    /// A name that is empty, or longer than the registry stores.
    BadName,
    /// A version of zero. Versions start at 1, so a zeroed spec is invalid
    /// rather than "version zero of something".
    BadVersion,
    /// min_band is Unset, or above max_band.
    BadBand,
    /// A lookback of zero or negative. Every feature reads SOMETHING.
    BadLookback,
    /// This (name, version) is already registered.
    Duplicate,
    /// The registry is full.
    Full,
    /// The registry is sealed and cannot be changed.
    Sealed,
    /// The registry is not sealed yet, so its hash is not final.
    NotSealed,
    /// No such feature.
    NotFound,
    /// The feature exists but is not valid at the requested horizon.
    OutOfBand
};

/// What one feature declares about itself.
struct FeatureSpec {
    /// Stable identifier. Changing the NAME makes a different feature;
    /// changing the COMPUTATION must bump the version instead.
    std::string_view name;
    /// Starts at 1. Bumped whenever the computation changes in any way that
    /// could move a value -- including a bug fix, because a model trained on
    /// the buggy version was fitted to the bug.
    std::uint16_t version = 0;
    /// Inclusive band range this feature is meaningful over.
    HorizonBand min_band = HorizonBand::Unset;
    HorizonBand max_band = HorizonBand::Unset;
    /// How far back the builder reads. Carried so a replay can prove no
    /// feature was evaluated before it had enough history -- a feature
    /// computed on a short window is not the same feature (rule 7).
    Duration lookback{0};
};

/// Position in the model's input vector. This IS the index the weights are
/// attached to, which is why the registry's order is part of its identity.
using FeatureIndex = std::uint16_t;

inline constexpr std::size_t kMaxFeatures = 512;
inline constexpr std::size_t kMaxFeatureNameLen = 48;

namespace detail {

/// FNV-1a, 64-bit. Not cryptographic and does not need to be: this identifies
/// a configuration, it does not defend against one. What it must be is STABLE
/// -- across builds, across platforms, and across runs -- because a recorded
/// feature_version has to still mean something next quarter.
inline constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
inline constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

[[nodiscard]] constexpr std::uint64_t
fnv1a(std::uint64_t h, std::string_view s) noexcept {
    for (unsigned char c : s) {
        h ^= static_cast<std::uint64_t>(c);
        h *= kFnvPrime;
    }
    return h;
}

[[nodiscard]] constexpr std::uint64_t
fnv1a_u64(std::uint64_t h, std::uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i) {
        h ^= (v >> (i * 8)) & 0xFFULL;
        h *= kFnvPrime;
    }
    return h;
}

} // namespace detail

/// The set of features a model is entitled to see, in order.
class FeatureRegistry {
public:
    /// Add a feature. Returns the index it will occupy in the input vector.
    ///
    /// The index is assigned in REGISTRATION ORDER and never changes, because
    /// it is what the model's weights are attached to.
    [[nodiscard]] std::expected<FeatureIndex, RegistryError>
    add(const FeatureSpec& s) noexcept {
        if (sealed_) { return std::unexpected(RegistryError::Sealed); }
        if (s.name.empty() || s.name.size() > kMaxFeatureNameLen) {
            return std::unexpected(RegistryError::BadName);
        }
        if (s.version == 0) { return std::unexpected(RegistryError::BadVersion); }
        if (s.min_band == HorizonBand::Unset
            || s.max_band == HorizonBand::Unset
            || static_cast<std::uint8_t>(s.min_band)
               > static_cast<std::uint8_t>(s.max_band)) {
            return std::unexpected(RegistryError::BadBand);
        }
        if (s.lookback.raw() <= 0) {
            return std::unexpected(RegistryError::BadLookback);
        }
        if (count_ >= kMaxFeatures) { return std::unexpected(RegistryError::Full); }
        for (std::size_t i = 0; i < count_; ++i) {
            if (name_len_[i] == s.name.size()
                && same(i, s.name) && version_[i] == s.version) {
                return std::unexpected(RegistryError::Duplicate);
            }
        }
        const FeatureIndex idx = static_cast<FeatureIndex>(count_);
        for (std::size_t c = 0; c < s.name.size(); ++c) {
            name_[count_][c] = s.name[c];
        }
        name_len_[count_] = static_cast<std::uint8_t>(s.name.size());
        version_[count_] = s.version;
        min_[count_] = s.min_band;
        max_[count_] = s.max_band;
        lookback_[count_] = s.lookback;
        ++count_;
        return idx;
    }

    /// Freeze the registry and compute its hash.
    ///
    /// Sealing an EMPTY registry is refused: a model served no features is a
    /// model reading an empty vector, and a hash over nothing would still look
    /// like a valid feature_version in the audit tuple.
    [[nodiscard]] std::expected<std::uint64_t, RegistryError> seal() noexcept {
        if (sealed_) { return hash_; }
        if (count_ == 0) { return std::unexpected(RegistryError::NotFound); }
        std::uint64_t h = detail::kFnvOffset;
        for (std::size_t i = 0; i < count_; ++i) {
            // The INDEX is hashed in, which is what makes this a hash of the
            // sequence rather than of the set. Two registries with the same
            // features in a different order must not collide.
            h = detail::fnv1a_u64(h, static_cast<std::uint64_t>(i));
            h = detail::fnv1a(h, std::string_view(name_[i], name_len_[i]));
            h = detail::fnv1a_u64(h, version_[i]);
            h = detail::fnv1a_u64(h, static_cast<std::uint64_t>(min_[i]));
            h = detail::fnv1a_u64(h, static_cast<std::uint64_t>(max_[i]));
            // The lookback is part of the identity too: the same formula over
            // a different window is a different feature, and a model fitted to
            // one will not read the other correctly.
            h = detail::fnv1a_u64(h,
                static_cast<std::uint64_t>(lookback_[i].raw()));
        }
        hash_ = h;
        sealed_ = true;
        return hash_;
    }

    [[nodiscard]] bool sealed() const noexcept { return sealed_; }
    [[nodiscard]] std::size_t size() const noexcept { return count_; }

    /// The `feature_version` for rule 10's tuple. Only available once sealed,
    /// because an unsealed registry can still change and a recorded hash that
    /// no longer describes it is worse than none.
    [[nodiscard]] std::expected<std::uint64_t, RegistryError>
    feature_version() const noexcept {
        if (!sealed_) { return std::unexpected(RegistryError::NotSealed); }
        return hash_;
    }

    /// Find a feature by name and version.
    [[nodiscard]] std::expected<FeatureIndex, RegistryError>
    find(std::string_view name, std::uint16_t version) const noexcept {
        for (std::size_t i = 0; i < count_; ++i) {
            if (name_len_[i] == name.size() && same(i, name)
                && version_[i] == version) {
                return static_cast<FeatureIndex>(i);
            }
        }
        return std::unexpected(RegistryError::NotFound);
    }

    /// Is this feature meaningful at this forecast horizon?
    [[nodiscard]] std::expected<void, RegistryError>
    check_band(FeatureIndex idx, Duration horizon) const noexcept {
        if (idx >= count_) { return std::unexpected(RegistryError::NotFound); }
        const HorizonBand b = band_of(horizon);
        if (b == HorizonBand::Unset) {
            return std::unexpected(RegistryError::BadBand);
        }
        const auto v = static_cast<std::uint8_t>(b);
        if (v < static_cast<std::uint8_t>(min_[idx])
            || v > static_cast<std::uint8_t>(max_[idx])) {
            return std::unexpected(RegistryError::OutOfBand);
        }
        return {};
    }

    /// The features a model forecasting at `horizon` is entitled to, written
    /// into `out` in REGISTRY ORDER. Returns how many.
    ///
    /// Order is preserved on purpose: a model's weights are positional, and a
    /// filtered view that renumbered would hand it the right values in the
    /// wrong slots.
    [[nodiscard]] std::expected<std::size_t, RegistryError>
    features_for(Duration horizon, FeatureIndex* out,
                 std::size_t cap) const noexcept {
        if (band_of(horizon) == HorizonBand::Unset) {
            return std::unexpected(RegistryError::BadBand);
        }
        std::size_t n = 0;
        for (std::size_t i = 0; i < count_; ++i) {
            if (check_band(static_cast<FeatureIndex>(i), horizon)) {
                if (n >= cap) { return std::unexpected(RegistryError::Full); }
                out[n++] = static_cast<FeatureIndex>(i);
            }
        }
        return n;
    }

    [[nodiscard]] std::string_view name_of(FeatureIndex i) const noexcept {
        if (i >= count_) { return {}; }
        return std::string_view(name_[i], name_len_[i]);
    }
    [[nodiscard]] std::uint16_t version_of(FeatureIndex i) const noexcept {
        return i < count_ ? version_[i] : 0;
    }
    [[nodiscard]] Duration lookback_of(FeatureIndex i) const noexcept {
        return i < count_ ? lookback_[i] : Duration{0};
    }

    /// The longest lookback in the registry.
    ///
    /// A session cannot emit any feature vector until this much history has
    /// accumulated. Exposed so a replay can refuse to score before then rather
    /// than quietly emitting vectors computed on short windows -- which is a
    /// look-ahead-adjacent bug: the values are not wrong in the direction of
    /// the future, but they are not the feature the model was trained on.
    [[nodiscard]] Duration warmup() const noexcept {
        std::int64_t m = 0;
        for (std::size_t i = 0; i < count_; ++i) {
            if (lookback_[i].raw() > m) { m = lookback_[i].raw(); }
        }
        return Duration{m};
    }

private:
    [[nodiscard]] bool same(std::size_t i, std::string_view s) const noexcept {
        for (std::size_t c = 0; c < s.size(); ++c) {
            if (name_[i][c] != s[c]) { return false; }
        }
        return true;
    }

    char name_[kMaxFeatures][kMaxFeatureNameLen] = {};
    std::uint8_t name_len_[kMaxFeatures] = {};
    std::uint16_t version_[kMaxFeatures] = {};
    HorizonBand min_[kMaxFeatures] = {};
    HorizonBand max_[kMaxFeatures] = {};
    Duration lookback_[kMaxFeatures] = {};
    std::size_t count_ = 0;
    std::uint64_t hash_ = 0;
    bool sealed_ = false;
};

} // namespace altair
