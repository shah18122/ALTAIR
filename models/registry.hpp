// models/registry.hpp -- the immutable, versioned, hash-addressed model
// registry.
//
// P8-09, and the card CLAUDE.md hard rule 10 rests on: "every live decision is
// reproducible from {model_hash, feature_version, config_hash, spec_version,
// tick_seqno}."
//
// A MODEL HASH THAT DOES NOT COVER THE FEATURE VERSION IS NOT A MODEL HASH.
//
// This is the card. The obvious hash is over the weights. It is not enough,
// and the gap is silent.
//
// A model is a function from a FEATURE VECTOR to a forecast, and P5-01
// established that a feature vector is defined by the ORDER of the registry
// that built it -- reorder two features and every weight is now multiplying
// the wrong column. Weights identical, registry different, answers wrong, and
// a weights-only hash reports a match.
//
// So `ModelKey` is the hash of the weights AND the feature_version AND the
// architecture AND the training window AND the seed. Two models agree only
// when all five agree, and `serve` refuses a feature vector whose version does
// not match the key it was trained against.
//
// IMMUTABLE MEANS A REGISTERED VERSION IS NEVER EDITED.
//
// Retraining produces a NEW version with a new key. The old one stays, because
// a decision made last Tuesday has to remain explicable after this Tuesday's
// retrain -- and because P9's shadow/canary/auto-rollback needs somewhere to
// roll back to. Re-registering an identical key is a no-op and reports so;
// re-registering the same NAME with a different key is a new version, not an
// overwrite.

#pragma once

#include <features/vector.hpp>
#include <models/training.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

inline constexpr std::size_t kMaxModels = 128;
inline constexpr std::size_t kModelNameLen = 40;

/// Named apart from features/registry.hpp's `RegistryError`. Two error
/// enums with one name in one namespace is a collision the compiler
/// catches; two with confusingly similar meanings is one it does not.
enum class ModelRegistryError : std::uint8_t {
    /// The registry is full.
    Full,
    /// No model with that key or name.
    NotFound,
    /// A key field was missing -- see the header on why a weights-only hash is
    /// not enough.
    IncompleteKey,
    /// The vector was built by a different feature registry than the model was
    /// trained against.
    FeatureVersionMismatch,
    /// The model is not in a state that can serve.
    NotServable,
    /// An attempt to modify a registered version.
    Immutable
};

/// What a model is being asked to do right now.
enum class ModelStage : std::uint8_t {
    Unknown = 0,
    /// Registered, never evaluated live.
    Registered,
    /// Predicting alongside production, its answers recorded and not traded.
    Shadow,
    /// Trading a fraction.
    Canary,
    /// Trading.
    Live,
    /// Withdrawn. Stays in the registry.
    Retired
};

/// The five things that together identify a model.
///
/// All five are hashed. A weights-only hash matches two models that multiply
/// different columns by identical numbers.
struct ModelKey {
    std::uint64_t param_hash = 0;
    /// P5-01's registry hash. Covers feature ORDER, not just the set.
    std::uint64_t feature_version = 0;
    /// Architecture identity: layer sizes, activation choices, anything that
    /// changes the function without changing the parameter count.
    std::uint64_t architecture_hash = 0;
    /// First and last bar of the training window. Two models with identical
    /// weights trained on different windows are different evidence.
    std::size_t train_from = 0;
    std::size_t train_to = 0;
    std::uint64_t seed = 0;

    [[nodiscard]] bool complete() const noexcept {
        return param_hash != 0 && feature_version != 0
            && architecture_hash != 0 && train_to > train_from;
    }

    /// The single value that goes in a decision record.
    [[nodiscard]] std::uint64_t digest() const noexcept {
        std::uint64_t h = 14695981039346656037ull;
        const std::uint64_t parts[6] = {
            param_hash, feature_version, architecture_hash,
            static_cast<std::uint64_t>(train_from),
            static_cast<std::uint64_t>(train_to), seed};
        for (const std::uint64_t p : parts) {
            for (int i = 0; i < 8; ++i) {
                h ^= (p >> (i * 8)) & 0xFFull;
                h *= 1099511628211ull;
            }
        }
        return h;
    }
    [[nodiscard]] bool operator==(const ModelKey& o) const noexcept {
        return digest() == o.digest();
    }
};

/// One registered version. Never edited after registration except for `stage`,
/// which is the model's relationship to production rather than its identity.
struct ModelRecord {
    char name[kModelNameLen] = {};
    ModelKey key{};
    TrainingResult training{};
    /// The test loss, from `Trainer::consume_test`. Recorded once.
    double test_loss = 0.0;
    bool test_recorded = false;
    ModelStage stage = ModelStage::Unknown;
    /// Version number within this name, from 1.
    std::uint32_t version = 0;
};

/// Immutable, hash-addressed, and it never forgets.
class ModelRegistry {
public:
    /// Register a trained model. Returns its version number.
    ///
    /// An identical key under the same name is a NO-OP returning the existing
    /// version -- retraining that lands on the same place is not a new model.
    /// A different key is a new version; the old record is untouched.
    [[nodiscard]] std::expected<std::uint32_t, ModelRegistryError>
    register_model(const char* name, const ModelKey& key,
                   const TrainingResult& tr) noexcept {
        if (!key.complete()) {
            return std::unexpected(ModelRegistryError::IncompleteKey);
        }
        if (name == nullptr || name[0] == '\0') {
            return std::unexpected(ModelRegistryError::IncompleteKey);
        }
        std::uint32_t highest = 0;
        for (std::size_t i = 0; i < n_; ++i) {
            if (!same_name(recs_[i].name, name)) { continue; }
            if (recs_[i].key == key) { return recs_[i].version; }
            if (recs_[i].version > highest) { highest = recs_[i].version; }
        }
        if (n_ >= kMaxModels) { return std::unexpected(ModelRegistryError::Full); }

        ModelRecord r{};
        std::size_t i = 0;
        for (; i + 1 < kModelNameLen && name[i] != '\0'; ++i) {
            r.name[i] = name[i];
        }
        r.name[i] = '\0';
        r.key = key;
        r.training = tr;
        r.stage = ModelStage::Registered;
        r.version = highest + 1;
        recs_[n_++] = r;
        return r.version;
    }

    /// Record the test loss. ONCE per model, matching P8-03's one-shot test.
    [[nodiscard]] std::expected<void, ModelRegistryError>
    record_test(const ModelKey& key, double loss) noexcept {
        ModelRecord* r = find(key);
        if (r == nullptr) { return std::unexpected(ModelRegistryError::NotFound); }
        if (r->test_recorded) {
            return std::unexpected(ModelRegistryError::Immutable);
        }
        r->test_loss = loss;
        r->test_recorded = true;
        return {};
    }

    /// Move a model through the shadow -> canary -> live pipeline.
    ///
    /// The transitions are enumerated. A model cannot go from Registered
    /// straight to Live, which is the whole of CLAUDE.md's "shadow, canary,
    /// auto-rollback is what stops retraining becoming a slow-motion
    /// self-inflicted loss".
    [[nodiscard]] std::expected<void, ModelRegistryError>
    promote(const ModelKey& key, ModelStage to) noexcept {
        ModelRecord* r = find(key);
        if (r == nullptr) { return std::unexpected(ModelRegistryError::NotFound); }
        const ModelStage from = r->stage;
        const bool ok =
            (from == ModelStage::Registered && to == ModelStage::Shadow)
         || (from == ModelStage::Shadow     && (to == ModelStage::Canary
                                             || to == ModelStage::Retired))
         || (from == ModelStage::Canary     && (to == ModelStage::Live
                                             || to == ModelStage::Retired))
         || (from == ModelStage::Live       && to == ModelStage::Retired);
        if (!ok) { return std::unexpected(ModelRegistryError::NotServable); }
        r->stage = to;
        return {};
    }

    /// Check a feature vector against the model it is about to be fed to.
    ///
    /// The reason ModelKey carries feature_version at all: identical weights
    /// against a reordered registry multiply the wrong columns, and there is
    /// no runtime symptom.
    [[nodiscard]] std::expected<void, ModelRegistryError>
    check_vector(const ModelKey& key, const FeatureVector& v) const noexcept {
        if (v.feature_version() != key.feature_version) {
            return std::unexpected(ModelRegistryError::FeatureVersionMismatch);
        }
        return {};
    }

    [[nodiscard]] const ModelRecord* get(const ModelKey& key) const noexcept {
        for (std::size_t i = 0; i < n_; ++i) {
            if (recs_[i].key == key) { return &recs_[i]; }
        }
        return nullptr;
    }
    /// The version of `name` currently Live, if any.
    [[nodiscard]] const ModelRecord* live(const char* name) const noexcept {
        for (std::size_t i = 0; i < n_; ++i) {
            if (same_name(recs_[i].name, name)
                && recs_[i].stage == ModelStage::Live) {
                return &recs_[i];
            }
        }
        return nullptr;
    }
    [[nodiscard]] std::size_t size() const noexcept { return n_; }
    [[nodiscard]] const ModelRecord* at(std::size_t i) const noexcept {
        return i < n_ ? &recs_[i] : nullptr;
    }
    [[nodiscard]] std::size_t versions_of(const char* name) const noexcept {
        std::size_t k = 0;
        for (std::size_t i = 0; i < n_; ++i) {
            if (same_name(recs_[i].name, name)) { ++k; }
        }
        return k;
    }

private:
    [[nodiscard]] static bool same_name(const char* a, const char* b) noexcept {
        for (std::size_t i = 0; i < kModelNameLen; ++i) {
            if (a[i] != b[i]) { return false; }
            if (a[i] == '\0') { return true; }
        }
        return true;
    }
    [[nodiscard]] ModelRecord* find(const ModelKey& key) noexcept {
        for (std::size_t i = 0; i < n_; ++i) {
            if (recs_[i].key == key) { return &recs_[i]; }
        }
        return nullptr;
    }

    ModelRecord recs_[kMaxModels] = {};
    std::size_t n_ = 0;
};

} // namespace altair
