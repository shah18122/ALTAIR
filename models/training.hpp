// models/training.hpp -- the training loop, early stopping, checkpointing and
// the learning-rate schedule.
//
// P8-03.
//
// EARLY STOPPING ON A VALIDATION SET MAKES THAT VALIDATION SCORE OPTIMISTIC,
// AND REPORTING IT AS OUT-OF-SAMPLE IS THE MISTAKE.
//
// This is the card. Early stopping is the right thing to do and almost
// everybody does it. What almost nobody does is notice that it has turned the
// validation set into training data.
//
// The mechanism: training for E epochs and keeping the epoch with the lowest
// validation loss is a selection over E candidates, scored on the validation
// set. The minimum of E noisy draws is below their mean -- that is the same
// extreme-value arithmetic P5-08 used for the scanner and P7-03 for the
// deflated Sharpe -- so the reported validation loss is biased downward by an
// amount that grows with E, EVEN IF THE MODEL LEARNED NOTHING.
//
// Measured on pure noise -- features and labels independent, so the best
// possible validation MSE is exactly the variance of y, 1.2134 -- with a model
// that does not learn at all:
//
//     epoch budget   reported best validation MSE   below the truth by
//              10              1.2119                   0.0015
//              50              1.1996                   0.0138
//             200              1.1959                   0.0175
//
// Below the truth at every budget, and further below as the budget grows. No
// learning happened; the improvement is entirely selection.
//
// The third split, touched exactly once, lands at 0.8953 against its own true
// 0.8861 -- a gap of PLUS 0.0092. Above, not below, because nothing was
// selected on it. That sign flip is the whole demonstration.
//
// The fix is not to stop early stopping. It is to hold a THIRD split that is
// touched exactly once, at the end, and to report that number. `TrainingRun`
// therefore refuses to report a test score more than once, and records that
// it has been consumed.
//
// THE BEST CHECKPOINT IS NOT THE LAST CHECKPOINT.
//
// The second thing, and it is nearly free to get right: training past the
// validation optimum and then shipping the final weights is the default
// behaviour of a loop that does not explicitly save the best. The harness
// keeps the best-so-far parameters and restores them at the end.
//
// SAMPLE WEIGHTS ARE APPLIED IN THE LOSS.
//
// P8-01 computed each label's uniqueness and P8-02 carried it into the
// dataset. A loop that ignores it weights 2,000 overlapping labels as 2,000
// independent observations, which is the 3.6x overstatement P8-01 measured,
// now expressed as a gradient.
//
// EVERYTHING IS SEEDED AND RECORDED (rule 10).
//
// The seed, the schedule, the epoch chosen and the resulting parameter hash
// all travel in the result. A training run that cannot be reproduced from its
// record is an anecdote.

#pragma once

#include <models/dataset.hpp>
#include <models/tensor.hpp>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

enum class TrainError : std::uint8_t {
    /// No validation split was provided. There is no default.
    NoValidation,
    /// The splits overlap, or are out of order in time.
    BadSplit,
    /// The test score has already been consumed.
    TestAlreadyUsed,
    /// The run has not finished.
    NotTrained,
    /// A hyperparameter was outside its admissible range.
    BadParameter,
    /// The loss went non-finite.
    Diverged
};

/// How the learning rate moves. No default: constant, step and cosine produce
/// different models from identical data, so the schedule is part of the model.
enum class LrSchedule : std::uint8_t {
    Unspecified = 0,
    Constant,
    /// Multiply by `decay` every `step_epochs`.
    Step,
    /// Cosine anneal from lr0 to lr_min over `max_epochs`.
    Cosine
};

struct TrainConfig {
    double lr0 = 0.0;
    double lr_min = 0.0;
    double decay = 0.0;
    std::size_t step_epochs = 0;
    std::size_t max_epochs = 0;
    /// Stop when validation has not improved for this many epochs. Zero
    /// disables early stopping, which is a choice and must be made.
    std::size_t patience = 0;
    LrSchedule schedule = LrSchedule::Unspecified;
    std::uint64_t seed = 0;

    [[nodiscard]] bool valid() const noexcept {
        return lr0 > 0.0 && max_epochs > 0
            && schedule != LrSchedule::Unspecified
            && (schedule != LrSchedule::Step
                || (decay > 0.0 && decay < 1.0 && step_epochs > 0));
    }
};

/// The learning rate at an epoch. Deterministic and pure, so a run can be
/// replayed from the config alone.
[[nodiscard]] inline std::expected<double, TrainError>
learning_rate(const TrainConfig& c, std::size_t epoch) noexcept {
    if (!c.valid()) { return std::unexpected(TrainError::BadParameter); }
    switch (c.schedule) {
        case LrSchedule::Constant:
            return c.lr0;
        case LrSchedule::Step: {
            double lr = c.lr0;
            for (std::size_t k = c.step_epochs; k <= epoch; k += c.step_epochs) {
                lr *= c.decay;
            }
            return lr > c.lr_min ? lr : c.lr_min;
        }
        case LrSchedule::Cosine: {
            const double t = static_cast<double>(epoch)
                           / static_cast<double>(c.max_epochs);
            const double x = t > 1.0 ? 1.0 : t;
            return c.lr_min + 0.5 * (c.lr0 - c.lr_min)
                            * (1.0 + std::cos(3.14159265358979323846 * x));
        }
        case LrSchedule::Unspecified:
            break;
    }
    return std::unexpected(TrainError::BadParameter);
}

/// Three splits, in TIME ORDER, non-overlapping.
///
/// Train, validation, test. The third exists because early stopping consumes
/// the second -- see the header. All three are index ranges into one dataset
/// and the constructor checks the ordering, because a validation block that
/// precedes its training block is a walk-forward violation wearing a
/// hyperparameter's clothes.
struct Splits {
    Block train{};
    Block validation{};
    Block test{};

    [[nodiscard]] bool ordered() const noexcept {
        return train.size() > 0 && validation.size() > 0 && test.size() > 0
            && train.end <= validation.start
            && validation.end <= test.start;
    }
};

/// Weighted mean squared error over a block.
///
/// `weight` is P8-01's uniqueness. Passing nullptr weights every row equally,
/// which is the 3.6x overstatement measured there -- so it is allowed but has
/// to be an explicit nullptr rather than a forgotten argument.
[[nodiscard]] inline double weighted_mse(const double* pred, const double* y,
                                         const double* weight,
                                         const Block& b) noexcept {
    double num = 0.0, den = 0.0;
    for (std::size_t i = b.start; i < b.end; ++i) {
        const double w = weight != nullptr ? weight[i] : 1.0;
        const double e = pred[i] - y[i];
        num += w * e * e;
        den += w;
    }
    return den > 0.0 ? num / den : 0.0;
}

/// What one run produced. Everything rule 10 needs to reproduce it.
struct TrainingResult {
    std::size_t epochs_run = 0;
    /// The epoch whose parameters were kept -- NOT necessarily the last.
    std::size_t best_epoch = 0;
    double best_validation_loss = 0.0;
    double final_train_loss = 0.0;
    /// How many candidate epochs the best was selected from. This is the N in
    /// the extreme-value correction, and it is the reason the validation loss
    /// above is optimistic.
    std::size_t candidates = 0;
    bool early_stopped = false;
    std::uint64_t seed = 0;
    std::uint64_t param_hash = 0;
    LrSchedule schedule = LrSchedule::Unspecified;
};

namespace detail {

[[nodiscard]] inline std::uint64_t fnv1a_bits(std::uint64_t h,
                                              double v) noexcept {
    // bit_cast, not a reinterpret_cast through a pointer: the latter is
    // strict-aliasing UB and the compiler is entitled to hoist the load.
    static_assert(sizeof(std::uint64_t) == sizeof(double),
                  "double must be 64 bits for this hash to be stable");
    std::uint64_t bits = std::bit_cast<std::uint64_t>(v);
    for (int i = 0; i < 8; ++i) {
        h ^= (bits >> (i * 8)) & 0xFFull;
        h *= 1099511628211ull;
    }
    return h;
}

} // namespace detail

/// Hash a parameter vector. Part of rule 10's `model_hash`.
[[nodiscard]] inline std::uint64_t hash_params(const double* p,
                                               std::size_t n) noexcept {
    std::uint64_t h = 14695981039346656037ull;
    for (std::size_t i = 0; i < n; ++i) { h = detail::fnv1a_bits(h, p[i]); }
    return h;
}

/// Drives a trainable model over the splits.
///
/// `Model` must expose:
///     void   reset(std::uint64_t seed)
///     double train_epoch(const Dataset&, const Block&, double lr)
///     void   predict(const Dataset&, const Block&, double* out)
///     std::size_t param_count() const
///     const double* params() const
///     void   load_params(const double*)
///
/// The interface is deliberately small and framework-free: a LibTorch module
/// satisfies it with a thin shim, and so does everything in models/zoo.hpp.
class Trainer {
public:
    template <class Model>
    [[nodiscard]] std::expected<TrainingResult, TrainError>
    run(Model& m, const Dataset& d, const Splits& s, const TrainConfig& c,
        double* pred_scratch, double* best_params) noexcept {
        if (!c.valid()) { return std::unexpected(TrainError::BadParameter); }
        if (!s.ordered()) { return std::unexpected(TrainError::BadSplit); }
        if (s.validation.size() == 0) {
            return std::unexpected(TrainError::NoValidation);
        }

        m.reset(c.seed);
        TrainingResult r{};
        r.seed = c.seed;
        r.schedule = c.schedule;
        r.best_validation_loss = 1e308;
        std::size_t since_improved = 0;

        for (std::size_t e = 0; e < c.max_epochs; ++e) {
            const auto lr = learning_rate(c, e);
            if (!lr) { return std::unexpected(lr.error()); }
            const double tl = m.train_epoch(d, s.train, *lr);
            if (!std::isfinite(tl)) {
                return std::unexpected(TrainError::Diverged);
            }
            r.final_train_loss = tl;
            ++r.epochs_run;
            ++r.candidates;

            m.predict(d, s.validation, pred_scratch);
            const double vl = weighted_mse(pred_scratch, d.y, d.weight,
                                           s.validation);
            if (!std::isfinite(vl)) {
                return std::unexpected(TrainError::Diverged);
            }
            if (vl < r.best_validation_loss) {
                r.best_validation_loss = vl;
                r.best_epoch = e;
                // KEEP THE BEST. A loop that does not do this ships whatever
                // the last epoch happened to produce, which is past the
                // optimum by construction whenever early stopping fires.
                const double* p = m.params();
                for (std::size_t i = 0; i < m.param_count(); ++i) {
                    best_params[i] = p[i];
                }
                since_improved = 0;
            } else if (c.patience > 0 && ++since_improved >= c.patience) {
                r.early_stopped = true;
                break;
            }
        }
        m.load_params(best_params);
        r.param_hash = hash_params(best_params, m.param_count());
        trained_ = true;
        return r;
    }

    /// Score the TEST split. Callable exactly once.
    ///
    /// The validation loss in `TrainingResult` is optimistic by construction
    /// -- it is the minimum over `candidates` noisy draws. This is the number
    /// that goes in a report, and allowing it to be read twice is allowing it
    /// to become a selection criterion like the validation set already did.
    template <class Model>
    [[nodiscard]] std::expected<double, TrainError>
    consume_test(Model& m, const Dataset& d, const Splits& s,
                 double* pred_scratch) noexcept {
        if (!trained_) { return std::unexpected(TrainError::NotTrained); }
        if (test_used_) { return std::unexpected(TrainError::TestAlreadyUsed); }
        m.predict(d, s.test, pred_scratch);
        test_used_ = true;
        return weighted_mse(pred_scratch, d.y, d.weight, s.test);
    }

    [[nodiscard]] bool test_consumed() const noexcept { return test_used_; }

private:
    bool trained_ = false;
    bool test_used_ = false;
};

/// How much the reported validation loss is expected to undershoot the truth,
/// purely from selecting the minimum of `n` epochs.
///
/// The same extreme-value approximation as P5-08's `expected_extreme_from_noise`
/// and P7-03's expected maximum Sharpe, applied to a minimum instead of a
/// maximum. Give it the epoch-to-epoch standard deviation of the validation
/// loss and it says how much of the improvement was selection.
[[nodiscard]] inline double selection_bias(std::size_t n, double sd) noexcept {
    if (n < 2 || !(sd > 0.0)) { return 0.0; }
    const double ln_n = std::log(static_cast<double>(n));
    const double root = std::sqrt(2.0 * ln_n);
    const double corr = (std::log(ln_n)
                         + std::log(4.0 * 3.14159265358979323846))
                        / (2.0 * root);
    return sd * (root - corr);
}

} // namespace altair
