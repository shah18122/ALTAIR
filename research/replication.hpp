// research/replication.hpp -- the replication harness and the promotion gate.
//
// P7-03 and P7-04.
//
// A SHARPE RATIO WITHOUT ITS TRIAL COUNT IS NOT A RESULT.
//
// This is the card. Test one strategy and a Sharpe of 1.5 is interesting. Test
// fifty and the BEST of fifty is expected to be around 1.5 under a null in
// which none of them work at all -- so the same number means "found something"
// in the first case and "found nothing" in the second, and nothing about the
// number itself distinguishes them.
//
// The Deflated Sharpe Ratio (Bailey and Lopez de Prado, 2014) is the
// correction. It asks: given N trials, a sample of this length, and returns
// with this skew and kurtosis, what is the probability the observed Sharpe
// exceeds what the best of N nulls would have produced? The trial count comes
// from P7-01's registry, which is why that registry keeps its failures.
//
// Measured, at an observed Sharpe of 1.50 over 1,000 observations:
//
//     trials   expected max under the null   deflated Sharpe
//          1              0.000                   1.0000
//          5              1.193                   1.0000
//         20              1.901                   0.0000
//         50              2.276                   0.0000
//        200              2.766                   0.0000
//
// The same number, the same data, and the conclusion inverts between five
// trials and twenty. The bar RISES with N because looking harder finds a
// bigger maximum without finding more edge -- the same mechanism P5-08
// measured for the scanner, one level up: there it was 200,000 strikes, here
// it is fifty papers.
//
// NON-NORMALITY MAKES IT WORSE, NOT BETTER.
//
// The correction also takes the skew and kurtosis of the returns, and for the
// negatively-skewed, fat-tailed series a short-premium or mean-reverting
// strategy produces, both push the significance DOWN.
//
// Measured at a Sharpe of 2.50 -- just above the 50-trial bar of 2.276, which
// is where the answer is actually decided:
//
//     normal returns      (skew  0.0, kurt 3.0)    0.9998
//     skewed, fat-tailed  (skew -1.2, kurt 9.0)    0.9591
//
// The same Sharpe, the same trial count, the same sample length, and a
// different answer because of the SHAPE of the returns. A strategy that makes
// a little most days and loses a lot occasionally has a flattering Sharpe and
// a poor deflated one, which is the entire point of computing it.
//
// THE THRESHOLD IS PRE-REGISTERED, AND THE HARNESS ENFORCES THAT.
//
// A threshold chosen after seeing the result is not a threshold. `preregister`
// must be called before `evaluate`, the harness refuses otherwise, and the
// recorded threshold is the one the verdict is measured against -- not one
// passed in alongside the data.
//
// THE PROMOTION GATE IS A CONJUNCTION, AND EVERY TERM CAN VETO.
//
// P7-04: replicated, AND deflated-significant, AND out-of-sample, AND net of
// full cost. Any one failing blocks, and the gate reports WHICH -- because
// "did not promote" is four different pieces of news and only one of them
// means the idea was wrong.

#pragma once

#include <analytics/greeks.hpp>          // detail::norm_cdf
#include <research/feature_card.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

/// Euler-Mascheroni, for the expected maximum of N normal draws.
inline constexpr double kEulerMascheroni = 0.577215664901532860606512;

enum class ReplicationError : std::uint8_t {
    /// No threshold was registered before the run.
    NotPreregistered,
    /// A threshold was registered twice, or after evaluation.
    AlreadyRegistered,
    /// Fewer observations than the statistic needs.
    TooFewSamples,
    /// The trial count was zero -- see the header on why that is not a
    /// harmless default.
    NoTrialCount,
    /// A parameter was outside its admissible range.
    BadParameter
};

/// Inverse standard normal CDF (probit), Acklam's rational approximation
/// refined by one Halley step.
///
/// Needed for the expected maximum of N draws. Accurate to about 1e-15 after
/// the refinement, which matters here: the deflated Sharpe evaluates it at
/// 1 - 1/N, and for N in the hundreds that is deep enough in the tail that a
/// crude approximation moves the verdict.
[[nodiscard]] inline double norm_inv(double p) noexcept {
    if (!(p > 0.0) || !(p < 1.0)) {
        return p <= 0.0 ? -1e308 : 1e308;
    }
    static const double a[6] = {-3.969683028665376e+01, 2.209460984245205e+02,
                                -2.759285104469687e+02, 1.383577518672690e+02,
                                -3.066479806614716e+01, 2.506628277459239e+00};
    static const double b[5] = {-5.447609879822406e+01, 1.615858368580409e+02,
                                -1.556989798598866e+02, 6.680131188771972e+01,
                                -1.328068155288572e+01};
    static const double c[6] = {-7.784894002430293e-03, -3.223964580411365e-01,
                                -2.400758277161838e+00, -2.549732539343734e+00,
                                 4.374664141464968e+00,  2.938163982698783e+00};
    static const double d[4] = { 7.784695709041462e-03,  3.224671290700398e-01,
                                 2.445134137142996e+00,  3.754408661907416e+00};
    const double plow = 0.02425, phigh = 1.0 - plow;
    double x;
    if (p < plow) {
        const double q = std::sqrt(-2.0 * std::log(p));
        x = (((((c[0]*q+c[1])*q+c[2])*q+c[3])*q+c[4])*q+c[5])
          / ((((d[0]*q+d[1])*q+d[2])*q+d[3])*q+1.0);
    } else if (p > phigh) {
        const double q = std::sqrt(-2.0 * std::log(1.0 - p));
        x = -(((((c[0]*q+c[1])*q+c[2])*q+c[3])*q+c[4])*q+c[5])
           / ((((d[0]*q+d[1])*q+d[2])*q+d[3])*q+1.0);
    } else {
        const double q = p - 0.5, r = q * q;
        x = (((((a[0]*r+a[1])*r+a[2])*r+a[3])*r+a[4])*r+a[5])*q
          / (((((b[0]*r+b[1])*r+b[2])*r+b[3])*r+b[4])*r+1.0);
    }
    // One Halley refinement against the true CDF.
    const double e = detail::norm_cdf(x) - p;
    const double u = e * 2.5066282746310002 * std::exp(0.5 * x * x);
    return x - u / (1.0 + 0.5 * x * u);
}

/// The Sharpe a null strategy would be expected to reach as the BEST of N
/// independent trials.
///
/// Bailey and Lopez de Prado's expected maximum:
///   sqrt(V) * [ (1-g)*Phi^-1(1 - 1/N) + g*Phi^-1(1 - 1/(N*e)) ]
/// with g the Euler-Mascheroni constant and V the variance of the Sharpe
/// estimates across trials.
///
/// This is the bar the observed Sharpe has to clear, and it RISES with N. That
/// is the whole mechanism: looking harder finds a bigger maximum without
/// finding more edge, exactly as P5-08 measured for the scanner.
[[nodiscard]] inline std::expected<double, ReplicationError>
expected_max_sharpe(std::size_t trials, double sharpe_variance) noexcept {
    if (trials == 0) { return std::unexpected(ReplicationError::NoTrialCount); }
    if (!(sharpe_variance >= 0.0)) {
        return std::unexpected(ReplicationError::BadParameter);
    }
    if (trials == 1) { return 0.0; }
    const double n = static_cast<double>(trials);
    const double g = kEulerMascheroni;
    const double t1 = norm_inv(1.0 - 1.0 / n);
    const double t2 = norm_inv(1.0 - 1.0 / (n * 2.718281828459045));
    return std::sqrt(sharpe_variance) * ((1.0 - g) * t1 + g * t2);
}

/// The Deflated Sharpe Ratio: P(true Sharpe > 0 | this observation, N trials).
///
/// `skew` and `kurtosis` are of the RETURN series, kurtosis in the raw (not
/// excess) convention, so a normal series has 3. Both push the answer down for
/// the negatively-skewed fat-tailed returns that short-premium and
/// mean-reverting strategies produce -- which is the point, not a nuisance.
[[nodiscard]] inline std::expected<double, ReplicationError>
deflated_sharpe(double observed_sharpe, std::size_t observations,
                std::size_t trials, double sharpe_variance, double skew,
                double kurtosis) noexcept {
    if (observations < 4) {
        return std::unexpected(ReplicationError::TooFewSamples);
    }
    const auto sr0 = expected_max_sharpe(trials, sharpe_variance);
    if (!sr0) { return std::unexpected(sr0.error()); }

    const double n = static_cast<double>(observations);
    // The standard error of a Sharpe estimate under non-normal returns.
    const double denom_sq = 1.0 - skew * observed_sharpe
                          + 0.25 * (kurtosis - 1.0) * observed_sharpe
                                 * observed_sharpe;
    if (!(denom_sq > 0.0)) {
        return std::unexpected(ReplicationError::BadParameter);
    }
    const double z = (observed_sharpe - *sr0) * std::sqrt(n - 1.0)
                   / std::sqrt(denom_sq);
    return detail::norm_cdf(z);
}

// ---------------------------------------------------------------------------
// P7-03: the harness
// ---------------------------------------------------------------------------

/// What must be decided BEFORE the data is looked at.
struct Preregistration {
    /// Minimum deflated Sharpe probability to call it replicated. No default:
    /// 0.95 is a convention, not a fact, and the right value depends on what
    /// the feature will be used for.
    double min_deflated = 0.0;
    /// Minimum observations. A replication on 200 bars is not a replication.
    std::size_t min_observations = 0;
    /// The trial count as of registration. Recorded here so that adding more
    /// papers AFTER seeing this one's result cannot retroactively change its
    /// verdict in either direction.
    std::size_t trials_at_registration = 0;

    [[nodiscard]] bool complete() const noexcept {
        return min_deflated > 0.0 && min_deflated < 1.0
            && min_observations > 0 && trials_at_registration > 0;
    }
};

/// What the run produced.
struct ReplicationResult {
    double observed_sharpe = 0.0;
    double deflated = 0.0;
    double expected_max_null = 0.0;
    std::size_t observations = 0;
    std::size_t trials = 0;
    /// What the naive comparison would have said: "Sharpe positive, done."
    bool naive_verdict = false;
    bool replicated = false;
};

/// One paper's replication attempt.
///
/// `preregister` then `evaluate`, in that order, enforced. A threshold chosen
/// after seeing the result is not a threshold, and the only way to make that
/// stick in code is to make the ordering a state machine rather than a
/// convention.
class ReplicationRun {
public:
    [[nodiscard]] std::expected<void, ReplicationError>
    preregister(const Preregistration& p) noexcept {
        if (registered_) {
            return std::unexpected(ReplicationError::AlreadyRegistered);
        }
        if (!p.complete()) {
            return std::unexpected(ReplicationError::BadParameter);
        }
        pre_ = p;
        registered_ = true;
        return {};
    }

    [[nodiscard]] std::expected<ReplicationResult, ReplicationError>
    evaluate(double observed_sharpe, std::size_t observations,
             double sharpe_variance, double skew, double kurtosis) noexcept {
        if (!registered_) {
            return std::unexpected(ReplicationError::NotPreregistered);
        }
        if (observations < pre_.min_observations) {
            return std::unexpected(ReplicationError::TooFewSamples);
        }
        const auto d = deflated_sharpe(observed_sharpe, observations,
                                       pre_.trials_at_registration,
                                       sharpe_variance, skew, kurtosis);
        if (!d) { return std::unexpected(d.error()); }
        const auto m = expected_max_sharpe(pre_.trials_at_registration,
                                           sharpe_variance);

        ReplicationResult r{};
        r.observed_sharpe = observed_sharpe;
        r.deflated = *d;
        r.expected_max_null = m ? *m : 0.0;
        r.observations = observations;
        r.trials = pre_.trials_at_registration;
        // What somebody looking at the Sharpe alone would have concluded.
        r.naive_verdict = observed_sharpe > 0.0;
        r.replicated = *d >= pre_.min_deflated;
        evaluated_ = true;
        return r;
    }

    [[nodiscard]] bool registered() const noexcept { return registered_; }
    [[nodiscard]] bool evaluated() const noexcept { return evaluated_; }

private:
    Preregistration pre_{};
    bool registered_ = false;
    bool evaluated_ = false;
};

// ---------------------------------------------------------------------------
// P7-04: the promotion gate
// ---------------------------------------------------------------------------

/// Why a paper did not get promoted. Ordinal 0 is Unknown.
enum class GateVerdict : std::uint8_t {
    Unknown = 0,
    Promote,
    /// The deflated Sharpe did not clear the pre-registered threshold.
    NotSignificant,
    /// It was only ever tested in sample.
    NoOutOfSample,
    /// The edge does not survive the cost of trading it (rule 5).
    CostExceedsEdge,
    /// The registry says this paper is not in a state that can be promoted.
    WrongStatus
};

[[nodiscard]] inline const char* verdict_name(GateVerdict v) noexcept {
    switch (v) {
        case GateVerdict::Unknown:         return "Unknown";
        case GateVerdict::Promote:         return "Promote";
        case GateVerdict::NotSignificant:  return "NotSignificant";
        case GateVerdict::NoOutOfSample:   return "NoOutOfSample";
        case GateVerdict::CostExceedsEdge: return "CostExceedsEdge";
        case GateVerdict::WrongStatus:     return "WrongStatus";
    }
    return "?";
}

/// Everything the gate needs. All four terms are required.
struct PromotionEvidence {
    ReplicationResult replication{};
    /// Was the result produced on data held out from every fitting decision?
    bool out_of_sample = false;
    /// Expected edge NET of the full round-trip cost, in paise, from P3-09.
    /// Signed: negative means the bill exceeds the edge.
    Notional net_edge_paise{};
    PaperStatus status = PaperStatus::Unknown;
};

/// The gate. A conjunction, and it reports which term vetoed.
///
/// "Did not promote" is four different pieces of news: the idea was wrong, the
/// test was too small, the test was in-sample, or the idea was right and
/// unprofitable. Only the first means stop.
[[nodiscard]] inline GateVerdict promotion_gate(
    const PromotionEvidence& e) noexcept {
    if (e.status != PaperStatus::Replicated) {
        return GateVerdict::WrongStatus;
    }
    if (!e.replication.replicated) { return GateVerdict::NotSignificant; }
    if (!e.out_of_sample)          { return GateVerdict::NoOutOfSample; }
    if (e.net_edge_paise.raw() <= 0) { return GateVerdict::CostExceedsEdge; }
    return GateVerdict::Promote;
}

} // namespace altair
