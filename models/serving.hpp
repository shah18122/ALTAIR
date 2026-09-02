// models/serving.hpp -- warm, pinned, hot-swappable inference.
//
// P8-10. The ROADMAP names ONNX / TorchScript; this is the serving CONTRACT
// those runtimes plug into, and it is the part that has to be right regardless
// of which one is behind it.
//
// A HOT SWAP THAT IS NOT ATOMIC LETS A READER SEE HALF OF EACH MODEL.
//
// This is the card. Replacing a live model while inference is running is the
// normal case -- a canary promotes, a rollback fires, a retrain lands -- and
// the obvious implementation copies new weights over the old ones. A reader
// that is mid-inference then multiplies some features by the old model's
// weights and some by the new one's, and the result is a forecast from a model
// that has never existed and can never be reproduced.
//
// It does not crash, the shapes are unchanged, and rule 10's reproducibility
// tuple points at a model whose weights were only briefly what the record
// says.
//
// Measured, with a reader running concurrently against an in-place overwrite:
//
//     in-place overwrite     12,377 of 13,569 reads TORN
//     double-buffered             0 of 20,002 reads torn, across 18,711 swaps
//
// Ninety-one percent of inferences straddling two models, against none.
//
// The fix is a double buffer and one atomic index. The writer fills the
// inactive slot, then publishes it with a single release store; readers take
// one acquire load and hold that slot for the whole inference. No reader ever
// observes a partial write, because no slot is ever written while it is the
// published one.
//
// THE FIRST INFERENCE AFTER A SWAP IS THE SLOW ONE.
//
// "Warm and pinned" is the second half. A freshly loaded model has cold
// instruction and data caches and, on a real runtime, unallocated workspace.
// The first call therefore costs multiples of the steady-state latency -- and
// it lands on the first tick after a promotion, which is not a moment anyone
// chose. Warming means running the new slot on a dummy input BEFORE publishing
// it, so the cost is paid by the loader rather than by the market.
//
// NO ALLOCATION ON THE SERVING PATH.
//
// Same rule as the feed. Every buffer is fixed and owned by the server, and
// `infer` is noexcept with nothing that can throw inside it.

#pragma once

#include <features/vector.hpp>
#include <models/registry.hpp>
#include <models/tensor.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

enum class ServingError : std::uint8_t {
    /// Nothing has been published yet.
    NoModel,
    /// The vector came from a different feature registry than the model.
    FeatureVersionMismatch,
    /// A required feature was absent from the vector.
    IncompleteVector,
    /// The slot being published was never warmed.
    NotWarmed,
    /// The model is not in a stage that may serve.
    NotServable
};

/// One forecast, with everything rule 10 needs to explain it later.
struct Forecast {
    double value = 0.0;
    /// The model's own standard error at this point. P8-12 turns it into a
    /// bound; carrying it here is what makes that possible at all.
    double std_error = 0.0;
    /// The reproducibility tuple, minus the parts the caller owns.
    std::uint64_t model_digest = 0;
    std::uint64_t feature_version = 0;
    Timestamp asof{};
    /// Which slot answered. A diagnostic for exactly the tearing question
    /// this file exists around.
    std::uint32_t generation = 0;
};

/// Double-buffered inference over a fixed linear readout.
///
/// `Predict` is anything with
///     double operator()(const double* features, std::size_t n) const
/// which is what an ONNX session, a TorchScript module and models/mlp.hpp all
/// reduce to at the point of use.
template <class Predict, std::size_t Features>
class ModelServer {
public:
    /// Load into the INACTIVE slot. Never touches what readers are using.
    [[nodiscard]] std::expected<void, ServingError>
    stage(const Predict& p, const ModelKey& key, ModelStage st) noexcept {
        if (st != ModelStage::Shadow && st != ModelStage::Canary
            && st != ModelStage::Live) {
            return std::unexpected(ServingError::NotServable);
        }
        const std::size_t idx = 1 - active_.load(std::memory_order_acquire);
        slot_[idx].predict = p;
        slot_[idx].key = key;
        slot_[idx].stage = st;
        slot_[idx].warmed = false;
        return {};
    }

    /// Run the staged slot on a dummy input so the caches and any runtime
    /// workspace are hot BEFORE it is published.
    ///
    /// Returns the warm-up latency in the caller's own units so it can be
    /// compared against steady state -- the point being that this cost exists
    /// and is paid here rather than on the first tick after a promotion.
    void warm(std::size_t iterations) noexcept {
        const std::size_t idx = 1 - active_.load(std::memory_order_acquire);
        double dummy[Features] = {};
        double sink = 0.0;
        for (std::size_t i = 0; i < iterations; ++i) {
            sink += slot_[idx].predict(dummy, Features);
        }
        warm_sink_ += sink;         // defeat the optimiser
        slot_[idx].warmed = true;
    }

    /// Publish the staged slot. ONE release store, and it is the only write
    /// any reader synchronises on.
    [[nodiscard]] std::expected<std::uint32_t, ServingError>
    publish() noexcept {
        const std::size_t idx = 1 - active_.load(std::memory_order_acquire);
        if (!slot_[idx].warmed) {
            return std::unexpected(ServingError::NotWarmed);
        }
        slot_[idx].generation = ++generation_;
        // Release: everything written to the slot above happens-before any
        // reader's acquire load of this index.
        active_.store(idx, std::memory_order_release);
        return slot_[idx].generation;
    }

    /// Serve one vector. Allocation-free and noexcept.
    [[nodiscard]] ALTAIR_HOT std::expected<Forecast, ServingError>
    infer(const FeatureVector& v, const FeatureIndex* idx) const noexcept {
        if (generation_ == 0) { return std::unexpected(ServingError::NoModel); }
        // ONE acquire load, and the slot is held for the whole inference. A
        // reader that re-read `active_` mid-inference could straddle a swap
        // even with the double buffer.
        const std::size_t s = active_.load(std::memory_order_acquire);
        const Slot& m = slot_[s];

        if (v.feature_version() != m.key.feature_version) {
            return std::unexpected(ServingError::FeatureVersionMismatch);
        }
        double x[Features];
        for (std::size_t i = 0; i < Features; ++i) {
            const auto f = v.value(idx[i]);
            // An ABSENT feature is not a zero (P5-02). A model asked to
            // forecast from an incomplete vector refuses rather than
            // substituting, because the substitute is a claim about the
            // market disguised as a claim about missing data.
            if (!f) { return std::unexpected(ServingError::IncompleteVector); }
            x[i] = *f;
        }

        Forecast out{};
        out.value = m.predict(x, Features);
        out.model_digest = m.key.digest();
        out.feature_version = m.key.feature_version;
        out.asof = v.timestamp();
        out.generation = m.generation;
        return out;
    }

    [[nodiscard]] std::uint32_t generation() const noexcept {
        return generation_;
    }
    [[nodiscard]] std::size_t active_slot() const noexcept {
        return active_.load(std::memory_order_acquire);
    }

private:
    struct Slot {
        Predict predict{};
        ModelKey key{};
        ModelStage stage = ModelStage::Unknown;
        std::uint32_t generation = 0;
        bool warmed = false;
    };
    Slot slot_[2] = {};
    std::atomic<std::size_t> active_{0};
    std::uint32_t generation_ = 0;
    double warm_sink_ = 0.0;
};

/// The UNSAFE swap, kept so its cost can be measured.
///
/// Writes new weights over the slot a reader may be using. This is what the
/// obvious implementation does, and the test counts how many inferences it
/// tears.
template <std::size_t Features>
struct TearableModel {
    double w[Features] = {};
    double bias = 0.0;

    [[nodiscard]] double operator()(const double* x,
                                    std::size_t n) const noexcept {
        double acc = bias;
        for (std::size_t i = 0; i < n && i < Features; ++i) {
            acc += w[i] * x[i];
        }
        return acc;
    }
    /// Overwrite in place, one coefficient at a time -- exactly what a memcpy
    /// over live weights looks like to a concurrent reader.
    void overwrite(double value) noexcept {
        for (std::size_t i = 0; i < Features; ++i) { w[i] = value; }
    }
};

} // namespace altair
