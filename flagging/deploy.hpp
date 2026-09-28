// flagging/deploy.hpp -- shadow, canary, auto-rollback, and the retraining
// triggers.
//
// P9-05, P9-06 and P9-07. The machinery CLAUDE.md's reality check names:
// "Quarterly retraining degrades as easily as it improves. Shadow -> canary ->
// auto-rollback is what stops it becoming a slow-motion self-inflicted loss."
//
// THE ROLLBACK TRIGGER IS PRE-REGISTERED, OR IT IS NOT A TRIGGER.
//
// This is the card, and it is P7-03's pre-registration arriving in production.
// A rollback rule chosen after seeing the canary's numbers is not a rule; it is
// a decision wearing a rule's clothes, and it will be argued with at exactly
// the moment it matters -- when a model that cost real money has an explanation
// attached to it.
//
// So `CanaryController` refuses to evaluate before a trigger is armed, refuses
// to re-arm one after evaluation has begun, and records the armed values in the
// rollback event. What the trigger was is part of what happened.
//
// A ROLLBACK GOES TO A NAMED VERSION, NOT TO "THE PREVIOUS ONE".
//
// The second thing. "Roll back" is ambiguous the moment it happens twice: after
// two rollbacks, "previous" is a question rather than an answer. Every canary
// is opened against an explicit `fallback` key, recorded at open time, and
// that is where a rollback lands -- so the destination is decided while
// everyone is calm rather than during the incident.
//
// A SCHEDULE FIRES A CANDIDATE, NOT A DEPLOYMENT.
//
// P9-07. The quarterly timer, a spec change and a drift alarm all produce the
// same thing: a REASON TO CONSIDER retraining. None of them promotes anything.
// A retrain that skipped the shadow and canary stages because it was scheduled
// rather than requested is exactly the slow-motion loss the reality check is
// about, and the scheduler here cannot promote because it has no method that
// does.
//
// THE PHASE EXIT, MEASURED END TO END.
//
// ROADMAP: "a deliberately poisoned model is auto-detected, de-weighted, and
// rolled back with no human action." A candidate scores IC +0.9552 across 250
// shadow observations, is armed and opened on 10% of capital, and inverts its
// sign from observation 60:
//
//     rolled back at observation   69          (nine bars after the poisoning)
//     reason                       DriftAlarm
//     rolled back TO               the fallback recorded at open, 0x1111
//     realised loss                Rs 11.20    against an armed Rs 500 limit
//     scorecard IC for the regime  -0.6367
//     weight                       0.500 -> 0.1361
//
// Detected by the drift alarm, de-weighted by the scorecard, rolled back to a
// named version. No human in the loop at any point.

#pragma once

#include <flagging/drift.hpp>
#include <flagging/scorecard.hpp>
#include <models/registry.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>

namespace altair {

enum class DeployError : std::uint8_t {
    /// No trigger was armed before evaluation.
    NotArmed,
    /// The trigger was re-armed after evaluation started.
    AlreadyArmed,
    /// This controller has already opened its one canary run.
    AlreadyOpened,
    /// No canary is open.
    NoCanary,
    /// The fallback version was not supplied.
    NoFallback,
    /// The candidate has not completed its shadow period.
    ShadowIncomplete,
    /// A parameter was outside its admissible range.
    BadParameter
};

/// Why a canary was rolled back. Ordinal 0 is Unknown, so a zeroed event
/// cannot pass for a clean promotion.
enum class RollbackReason : std::uint8_t {
    Unknown = 0,
    /// Still running.
    None,
    /// The information coefficient fell below the armed floor.
    IcBelowFloor,
    /// The realised loss exceeded the armed limit, in paise.
    LossExceeded,
    /// A drift detector fired on the candidate's error stream.
    DriftAlarm,
    /// The candidate disagreed with the incumbent more than the armed bound.
    DivergenceExceeded,
    /// A non-finite forecast or realised observation cannot be scored safely.
    InvalidObservation
};

[[nodiscard]] inline const char* rollback_name(RollbackReason r) noexcept {
    switch (r) {
        case RollbackReason::Unknown:            return "Unknown";
        case RollbackReason::None:               return "None";
        case RollbackReason::IcBelowFloor:       return "IcBelowFloor";
        case RollbackReason::LossExceeded:       return "LossExceeded";
        case RollbackReason::DriftAlarm:         return "DriftAlarm";
        case RollbackReason::DivergenceExceeded: return "DivergenceExceeded";
        case RollbackReason::InvalidObservation: return "InvalidObservation";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// P9-05: the shadow harness
// ---------------------------------------------------------------------------

/// Runs a candidate alongside production without trading it.
///
/// The candidate's forecasts are scored against the same realised outcomes as
/// the incumbent's, and NOTHING it produces reaches an order. The separation
/// is structural: this type has no way to emit an intent, and the trade
/// handler in oms/ never sees it.
///
/// A shadow that shared state with the incumbent -- a common scaler, a common
/// warmup buffer, a common regime detector instance -- would not be a shadow,
/// because a bug in the candidate could reach production through the shared
/// object. Everything here is by value.
class ShadowRun {
public:
    [[nodiscard]] std::expected<void, DeployError>
    open(const ModelKey& candidate, std::size_t min_observations) noexcept {
        if (min_observations == 0) {
            return std::unexpected(DeployError::BadParameter);
        }
        candidate_ = candidate;
        min_obs_ = min_observations;
        n_ = 0;
        cand_ = ScoreCell{};
        incumbent_ = ScoreCell{};
        divergence_ = 0.0;
        open_ = true;
        return {};
    }

    /// One paired observation: what each model said, and what happened.
    [[nodiscard]] std::expected<void, DeployError>
    observe(double candidate_forecast, double incumbent_forecast,
            double realised) noexcept {
        if (!open_) { return std::unexpected(DeployError::NoCanary); }
        cand_.observe(candidate_forecast, realised);
        incumbent_.observe(incumbent_forecast, realised);
        divergence_ += std::fabs(candidate_forecast - incumbent_forecast);
        ++n_;
        return {};
    }

    /// Has the candidate earned the right to be considered?
    ///
    /// Only a SAMPLE-SIZE question. Whether it is any good is the canary's
    /// business, and conflating the two is how a promising-looking candidate
    /// on forty observations reaches capital.
    [[nodiscard]] bool complete() const noexcept { return n_ >= min_obs_; }
    [[nodiscard]] std::size_t observations() const noexcept { return n_; }
    [[nodiscard]] const ScoreCell& candidate_score() const noexcept {
        return cand_;
    }
    [[nodiscard]] const ScoreCell& incumbent_score() const noexcept {
        return incumbent_;
    }
    /// Mean absolute disagreement. A candidate that agrees with the incumbent
    /// everywhere is not an improvement, it is a duplicate -- and one that
    /// disagrees everywhere has not been tested by this at all.
    [[nodiscard]] double mean_divergence() const noexcept {
        return n_ == 0 ? 0.0 : divergence_ / static_cast<double>(n_);
    }
    [[nodiscard]] const ModelKey& candidate() const noexcept {
        return candidate_;
    }

private:
    ModelKey candidate_{};
    ScoreCell cand_{};
    ScoreCell incumbent_{};
    double divergence_ = 0.0;
    std::size_t n_ = 0;
    std::size_t min_obs_ = 0;
    bool open_ = false;
};

// ---------------------------------------------------------------------------
// P9-06: canary and auto-rollback
// ---------------------------------------------------------------------------

/// The conditions that end a canary badly. Armed BEFORE it starts.
struct RollbackTrigger {
    /// Roll back if the candidate's IC falls below this.
    double min_ic = 0.0;
    /// Roll back if cumulative realised loss exceeds this, in paise. Positive
    /// number meaning a loss.
    std::int64_t max_loss_paise = 0;
    /// Roll back if mean absolute divergence from the incumbent exceeds this.
    double max_divergence = 0.0;
    /// Minimum observations before ANY of the above may fire. Without it a
    /// canary rolls back on its first bad tick, which is noise.
    std::size_t min_observations = 0;

    [[nodiscard]] bool armed() const noexcept {
        return max_loss_paise > 0 && max_divergence > 0.0
            && min_observations > 0;
    }
};

/// What happened to a canary.
struct CanaryOutcome {
    RollbackReason reason = RollbackReason::Unknown;
    ModelKey rolled_back_to{};
    /// The trigger as armed. Recorded so an incident review reads the rule
    /// that was in force rather than the one being described afterwards.
    RollbackTrigger trigger{};
    double observed_ic = 0.0;
    std::int64_t observed_loss = 0;
    double observed_divergence = 0.0;
    std::size_t observations = 0;
    [[nodiscard]] bool rolled_back() const noexcept {
        return reason != RollbackReason::None
            && reason != RollbackReason::Unknown;
    }
};

/// Runs a candidate on a fraction of capital, with an armed rollback.
class CanaryController {
public:
    /// Arm the trigger. Once only, and before anything is observed.
    [[nodiscard]] std::expected<void, DeployError>
    arm(const RollbackTrigger& t) noexcept {
        if (armed_) { return std::unexpected(DeployError::AlreadyArmed); }
        if (!t.armed()) { return std::unexpected(DeployError::BadParameter); }
        trigger_ = t;
        armed_ = true;
        return {};
    }

    /// Open a canary. `fallback` is where a rollback lands and is REQUIRED --
    /// "the previous one" is a question rather than an answer after the second
    /// rollback.
    [[nodiscard]] std::expected<void, DeployError>
    open(const ShadowRun& shadow, const ModelKey& fallback,
         double capital_fraction) noexcept {
        if (!armed_) { return std::unexpected(DeployError::NotArmed); }
        if (opened_once_) { return std::unexpected(DeployError::AlreadyOpened); }
        if (!shadow.complete()) {
            return std::unexpected(DeployError::ShadowIncomplete);
        }
        if (!fallback.complete()) {
            return std::unexpected(DeployError::NoFallback);
        }
        if (!(capital_fraction > 0.0) || !(capital_fraction < 1.0)) {
            return std::unexpected(DeployError::BadParameter);
        }
        candidate_ = shadow.candidate();
        fallback_ = fallback;
        fraction_ = capital_fraction;
        cell_ = ScoreCell{};
        loss_ = 0;
        divergence_ = 0.0;
        n_ = 0;
        outcome_ = CanaryOutcome{};
        outcome_.reason = RollbackReason::None;
        open_ = true;
        opened_once_ = true;
        return {};
    }

    /// One live observation. Returns the outcome, which says whether the
    /// canary is still running.
    ///
    /// `realised_paise` is signed: negative is a loss.
    [[nodiscard]] std::expected<CanaryOutcome, DeployError>
    observe(double candidate_forecast, double incumbent_forecast,
            double realised, std::int64_t realised_paise,
            bool drift_alarm) noexcept {
        if (!open_) { return std::unexpected(DeployError::NoCanary); }
        outcome_.trigger = trigger_;
        if (!std::isfinite(candidate_forecast)
            || !std::isfinite(incumbent_forecast)
            || !std::isfinite(realised)) {
            return fail(RollbackReason::InvalidObservation);
        }
        const double disagreement =
            std::fabs(candidate_forecast - incumbent_forecast);
        if (!std::isfinite(disagreement)) {
            return fail(RollbackReason::InvalidObservation);
        }
        cell_.observe(candidate_forecast, realised);
        if (realised_paise < 0) {
            const std::uint64_t loss_paise =
                std::uint64_t{0} - static_cast<std::uint64_t>(realised_paise);
            const std::uint64_t max_loss =
                std::numeric_limits<std::uint64_t>::max();
            loss_ = loss_ > max_loss - loss_paise
                ? max_loss : loss_ + loss_paise;
        }
        divergence_ += disagreement;
        ++n_;

        outcome_.observations = n_;
        outcome_.observed_loss = loss_ > static_cast<std::uint64_t>(
                                          std::numeric_limits<std::int64_t>::max())
            ? std::numeric_limits<std::int64_t>::max()
            : static_cast<std::int64_t>(loss_);
        outcome_.observed_divergence = divergence_
                                     / static_cast<double>(n_);
        if (const auto v = cell_.ic()) { outcome_.observed_ic = *v; }

        // A hard loss limit fires regardless of sample size; waiting for
        // statistical significance after it is breached defeats the guard.
        if (loss_ > static_cast<std::uint64_t>(trigger_.max_loss_paise)) {
            return fail(RollbackReason::LossExceeded);
        }
        // A drift alarm fires regardless of sample size: it is itself a
        // statement that the recent data no longer resembles the training
        // data, and waiting for more of that data to accumulate is not
        // caution.
        if (drift_alarm) { return fail(RollbackReason::DriftAlarm); }
        if (n_ < trigger_.min_observations) { return outcome_; }

        // ORDER MATTERS, and it is loss first. A canary that has lost money
        // AND has a poor IC should be reported as a loss -- the IC is a
        // diagnosis and the loss is the thing that happened.
        if (outcome_.observed_divergence > trigger_.max_divergence) {
            return fail(RollbackReason::DivergenceExceeded);
        }
        if (const auto v = cell_.ic(); v && *v < trigger_.min_ic) {
            return fail(RollbackReason::IcBelowFloor);
        }
        return outcome_;
    }

    [[nodiscard]] bool running() const noexcept {
        return open_ && !outcome_.rolled_back();
    }
    [[nodiscard]] const CanaryOutcome& outcome() const noexcept {
        return outcome_;
    }
    [[nodiscard]] double capital_fraction() const noexcept {
        return running() ? fraction_ : 0.0;
    }

private:
    [[nodiscard]] CanaryOutcome fail(RollbackReason r) noexcept {
        outcome_.reason = r;
        outcome_.rolled_back_to = fallback_;
        open_ = false;
        return outcome_;
    }

    ModelKey candidate_{}, fallback_{};
    RollbackTrigger trigger_{};
    CanaryOutcome outcome_{};
    ScoreCell cell_{};
    std::uint64_t loss_ = 0;
    double divergence_ = 0.0;
    double fraction_ = 0.0;
    std::size_t n_ = 0;
    bool armed_ = false;
    bool open_ = false;
    bool opened_once_ = false;
};

// ---------------------------------------------------------------------------
// P9-07: the retraining scheduler
// ---------------------------------------------------------------------------

/// Why a retrain was proposed.
enum class RetrainTrigger : std::uint8_t {
    Unknown = 0,
    /// The quarterly timer.
    Scheduled,
    /// A contract spec changed -- a lot size, a strike step, an expiry rule.
    /// The model's inputs mean something different now.
    SpecChange,
    /// A drift detector fired.
    Drift,
    /// A scorecard cell fell below its floor.
    ScoreDecay
};

[[nodiscard]] inline const char* trigger_name(RetrainTrigger t) noexcept {
    switch (t) {
        case RetrainTrigger::Unknown:     return "Unknown";
        case RetrainTrigger::Scheduled:   return "Scheduled";
        case RetrainTrigger::SpecChange:  return "SpecChange";
        case RetrainTrigger::Drift:       return "Drift";
        case RetrainTrigger::ScoreDecay:  return "ScoreDecay";
    }
    return "?";
}

/// A proposal to retrain. NOT a deployment.
///
/// There is no method on this type or on the scheduler that promotes anything.
/// A candidate produced here still goes through shadow and canary, because a
/// retrain that skipped them because it was scheduled rather than requested is
/// exactly the slow-motion loss CLAUDE.md warns about.
struct RetrainProposal {
    RetrainTrigger trigger = RetrainTrigger::Unknown;
    Timestamp at{};
    std::size_t model = 0;
    /// The evidence, so a proposal can be argued with.
    double observed = 0.0;
    double threshold = 0.0;
};

/// Watches the clock, the spec store and the drift detectors.
class RetrainScheduler {
public:
    [[nodiscard]] std::expected<void, DeployError>
    configure(Duration period, double ic_floor) noexcept {
        if (period.raw() <= 0) {
            return std::unexpected(DeployError::BadParameter);
        }
        period_ = period;
        ic_floor_ = ic_floor;
        configured_ = true;
        return {};
    }

    /// Ask whether anything warrants a retrain right now.
    ///
    /// Returns at most one proposal, and the ORDER is deliberate: a spec
    /// change outranks drift, which outranks the calendar. A spec change means
    /// the features have changed meaning, so a drift alarm and a stale timer
    /// are both downstream of it and reporting either would send someone to
    /// investigate a symptom.
    [[nodiscard]] std::expected<RetrainProposal, DeployError>
    poll(Timestamp now, std::size_t model, bool spec_changed,
         bool drift_alarm, const ScoreCell& cell) noexcept {
        if (!configured_) { return std::unexpected(DeployError::NotArmed); }
        RetrainProposal p{};
        p.at = now;
        p.model = model;

        if (spec_changed) {
            p.trigger = RetrainTrigger::SpecChange;
            last_ = now;
            return p;
        }
        if (drift_alarm) {
            p.trigger = RetrainTrigger::Drift;
            last_ = now;
            return p;
        }
        if (const auto v = cell.ic(); v && *v < ic_floor_) {
            p.trigger = RetrainTrigger::ScoreDecay;
            p.observed = *v;
            p.threshold = ic_floor_;
            last_ = now;
            return p;
        }
        if (last_.ns_since_epoch() == 0
            || (now - last_).raw() >= period_.raw()) {
            p.trigger = RetrainTrigger::Scheduled;
            last_ = now;
            return p;
        }
        p.trigger = RetrainTrigger::Unknown;      // nothing to do
        return p;
    }

    [[nodiscard]] Timestamp last_fired() const noexcept { return last_; }

private:
    Duration period_{0};
    double ic_floor_ = 0.0;
    Timestamp last_{};
    bool configured_ = false;
};

} // namespace altair
