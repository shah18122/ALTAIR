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
// A double buffer alone is not enough: after two swaps, a slow reader may
// still be in the slot that has become inactive again. Each slot therefore
// has an atomic reader count and an exclusive writer bit. Readers pin the
// selected slot for the full inference; a publisher makes one bounded CAS to
// reserve the inactive slot and defers with SlotBusy if it is still pinned.
// A packed atomic publication descriptor carries both slot index and
// generation, so neither data nor reproducibility metadata can tear.
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
#include <limits>

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
    NotServable,
    /// The inactive slot is still pinned by one or more inferences.
    SlotBusy,
    /// A reader could not pin a stable published slot within the fixed retry
    /// budget. The caller may retry on a later tick.
    Retry,
    /// Reader-retention counter reached its representable limit.
    ReaderLimit,
    /// The 32-bit forecast generation space is exhausted.
    GenerationExhausted
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
/// `Predict::operator()` is called concurrently by inference readers after
/// the same immutable slot is published. Predictors must therefore make
/// concurrent const calls thread-safe and reentrant; mutable shared scratch
/// state belongs in reader-owned storage, not in the predictor object.
class ModelServer {
public:

    /// Load into the INACTIVE slot. Never touches what readers are using.
    ///
    /// The serving path is multi-reader safe; stage/warm/publish/discard have
    /// a SINGLE-WRITER precondition and must be serialized by the loader.
    /// stage performs one bounded slot-reservation CAS. If a slow inference
    /// still pins the inactive slot, it returns SlotBusy rather than waiting
    /// on the market-data thread or overwriting the model under that reader.
    [[nodiscard]] std::expected<void, ServingError>
    stage(const Predict& p, const ModelKey& key, ModelStage st) noexcept {
        if (st != ModelStage::Shadow && st != ModelStage::Canary
            && st != ModelStage::Live) {
            return std::unexpected(ServingError::NotServable);
        }
        const std::size_t idx = 1 - active_slot();
        if (staged_ && staged_slot_ != idx) {
            // This can only happen if the documented single-writer contract
            // was violated (or a publisher changed state behind this one).
            return std::unexpected(ServingError::SlotBusy);
        }
        if (!staged_) {
            std::uint32_t expected = 0;
            if (!reader_state_[idx].compare_exchange_strong(
                    expected, kWriterBit, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return std::unexpected(ServingError::SlotBusy);
            }
            staged_ = true;
            staged_slot_ = idx;
        }
        slot_[idx].predict = p;
        slot_[idx].key = key;
        slot_[idx].stage = st;
        // Restaging the reserved slot replaces the predictor and key, so a
        // previous warm pass must never authorize this new model for publish.
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
        // Zero iterations execute no predictor call, so they cannot establish
        // that this model's runtime/workspace has been warmed. Keep the
        // publish gate closed in that case.
        if (!staged_ || iterations == 0) { return; }
        const std::size_t idx = staged_slot_;
        double dummy[Features] = {};
        double sink = 0.0;
        for (std::size_t i = 0; i < iterations; ++i) {
            sink += slot_[idx].predict(dummy, Features);
        }
        warm_sink_ += sink;         // defeat the optimiser
        slot_[idx].warmed = true;
    }

    /// Publish the staged slot. The packed publication word contains both
    /// slot index and generation, so those two pieces of metadata can never
    /// be observed from different publications.
    [[nodiscard]] std::expected<std::uint32_t, ServingError>
    publish() noexcept {
        if (!staged_) {
            return std::unexpected(ServingError::NotWarmed);
        }
        const std::size_t idx = staged_slot_;
        if (!slot_[idx].warmed) {
            return std::unexpected(ServingError::NotWarmed);
        }
        const std::uint32_t old_generation = static_cast<std::uint32_t>(
            publication_.load(std::memory_order_seq_cst) >> 1);
        if (old_generation == std::numeric_limits<std::uint32_t>::max()) {
            discard();
            return std::unexpected(ServingError::GenerationExhausted);
        }
        const std::uint32_t next_generation = old_generation + 1;
        slot_[idx].generation = next_generation;
        // Publish index and generation together while the slot remains
        // writer-pinned. Readers that see this word retry until the following
        // release unlock; acquiring reader CASes then see all slot contents.
        publication_.store(pack_publication(idx, next_generation),
                           std::memory_order_seq_cst);
        reader_state_[idx].store(0, std::memory_order_release);
        staged_ = false;
        return next_generation;
    }

    /// Abandon a staged model and release its reserved slot. Useful when a
    /// loader decides not to publish after warming or validation.
    void discard() noexcept {
        if (!staged_) { return; }
        reader_state_[staged_slot_].store(0, std::memory_order_release);
        staged_ = false;
    }

    /// Serve one vector. Allocation-free and noexcept.
    [[nodiscard]] ALTAIR_HOT std::expected<Forecast, ServingError>
    infer(const FeatureVector& v, const FeatureIndex* idx) const noexcept {
        std::size_t s = 0;
        ServingError pin_error = ServingError::Retry;
        if (!pin_active(s, pin_error)) {
            return std::unexpected(pin_error);
        }
        ReaderPin pin{this, s};
        const Slot& m = slot_[s];
        if (m.generation == 0) {
            return std::unexpected(ServingError::NoModel);
        }

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
        return static_cast<std::uint32_t>(
            publication_.load(std::memory_order_seq_cst) >> 1);
    }
    [[nodiscard]] std::size_t active_slot() const noexcept {
        return static_cast<std::size_t>(
            publication_.load(std::memory_order_seq_cst) & 1U);
    }

private:
    static constexpr std::uint32_t kWriterBit = 0x80000000U;
    static constexpr std::uint32_t kReaderMask = ~kWriterBit;
    static constexpr unsigned kPinAttempts = 8;

    [[nodiscard]] static constexpr std::uint64_t pack_publication(
        std::size_t index, std::uint32_t generation) noexcept {
        return (static_cast<std::uint64_t>(generation) << 1)
               | static_cast<std::uint64_t>(index & 1U);
    }

    /// One bounded pin attempt per observed publication. The count and the
    /// exclusive writer bit share one atomic word: either a reader owns a
    /// retention reference before stage reserves a slot, or stage owns the
    /// slot before the reader can touch its contents.
    [[nodiscard]] bool pin_active(
        std::size_t& index, ServingError& error) const noexcept {
        for (unsigned attempt = 0; attempt < kPinAttempts; ++attempt) {
            const std::uint64_t publication =
                publication_.load(std::memory_order_seq_cst);
            const std::size_t candidate =
                static_cast<std::size_t>(publication & 1U);
            std::uint32_t state =
                reader_state_[candidate].load(std::memory_order_acquire);
            if ((state & kWriterBit) != 0) { continue; }
            if ((state & kReaderMask) == kReaderMask) {
                error = ServingError::ReaderLimit;
                return false;
            }
            if (!reader_state_[candidate].compare_exchange_weak(
                    state, state + 1, std::memory_order_acquire,
                    std::memory_order_relaxed)) {
                continue;
            }

            // Reject an ABA (the same buffer index reused by a later model)
            // before reading any slot field. The full descriptor includes the
            // generation, not merely the index.
            if (publication_.load(std::memory_order_seq_cst) != publication) {
                reader_state_[candidate].fetch_sub(1,
                                                   std::memory_order_release);
                continue;
            }
            index = candidate;
            return true;
        }
        error = ServingError::Retry;
        return false;
    }

    struct ReaderPin {
        const ModelServer* server;
        std::size_t index;
        ~ReaderPin() {
            server->reader_state_[index].fetch_sub(1,
                                                  std::memory_order_release);
        }
    };

    struct Slot {
        Predict predict{};
        ModelKey key{};
        ModelStage stage = ModelStage::Unknown;
        std::uint32_t generation = 0;
        bool warmed = false;
    };
    Slot slot_[2] = {};
    // Low bit = active index, remaining 32 bits = generation. A single
    // descriptor makes generation/index observations coherent.
    std::atomic<std::uint64_t> publication_{0};
    // The high bit is the exclusive stage/warm/publish reservation; low bits
    // count inferences currently reading the slot.
    mutable std::atomic<std::uint32_t> reader_state_[2] = {};
    // stage/warm/publish/discard are serialized by the single-writer caller.
    bool staged_ = false;
    std::size_t staged_slot_ = 0;
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
