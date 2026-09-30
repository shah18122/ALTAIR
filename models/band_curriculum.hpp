// models/band_curriculum.hpp -- range forecasts on the doubling curriculum.
//
// The question here is not "which way?" but "how far?": will the next close
// land inside a band around the last one? That is the question an option
// seller is paid to answer, and unlike direction it can be answered 80 % of
// the time honestly -- because the band's WIDTH carries the risk.
//
// ANY band hits 80 % if it is wide enough. So the target coverage is fixed
// (kBandCoverage) and the models compete on how NARROW a band they can hold
// it with:
//   - each model forecasts the scale of the next return (sigma-hat);
//   - the half-width is k * sigma-hat, with k the 80th percentile of the
//     model's own PAST OUT-OF-SAMPLE |r| / sigma-hat (split conformal over
//     finished stages; in-sample on the training window until 50 such
//     errors exist);
//   - the score is the interval (Winkler) score, which charges width plus
//     2/alpha times any miss -- a proper score, so neither a too-wide nor a
//     too-narrow band can game it -- and its skill against the constant-sigma
//     band (Brownian motion with the training window's volatility).
//
// Same schedule, same look-ahead guards as models/curriculum.hpp: a model
// sees the realised returns strictly before the row it forecasts, the
// training window's features and targets, nothing after.

#pragma once

#include <analytics/advanced_pricers.hpp>
#include <analytics/garch.hpp>
#include <backtest/montecarlo.hpp>
#include <models/curriculum.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace altair {

inline constexpr double kBandCoverage = 0.80;
/// Past out-of-sample errors a model needs before its own stop replaces the
/// in-sample calibration.
inline constexpr std::size_t kBandCalibrationMin = 50;
/// Variance models fit on at most this many of the window's latest returns
/// (the grid and coordinate searches are O(n) per trial); the filter then
/// runs over the whole history.
inline constexpr std::size_t kBandFitRows = 100000;

/// A band model forecasts the scale of the next log return.
class BandModel {
public:
    virtual ~BandModel() = default;
    [[nodiscard]] virtual std::string name() const = 0;
    [[nodiscard]] virtual std::string family() const = 0;
    [[nodiscard]] virtual std::string fit(const CurriculumDesign& d) = 0;
    /// Scale of row i's return, > 0; NaN to abstain.
    [[nodiscard]] virtual double scale(const CurriculumDesign& d, std::size_t i) const = 0;
    /// What the stage's fit chose or truncated, if anything.
    [[nodiscard]] virtual std::string tuned() const { return {}; }
};

namespace band_detail {

inline double nan() noexcept { return std::numeric_limits<double>::quiet_NaN(); }

/// The latest kBandFitRows returns of the training window.
/// RULE 11: visible truncation -- the models that use it say so in tuned().
inline std::span<const double> fit_window(const CurriculumDesign& d) {
    const auto h = d.history(d.train_rows());
    return h.size() > kBandFitRows ? h.subspan(h.size() - kBandFitRows) : h;
}

inline std::string window_note(const CurriculumDesign& d) {
    return d.train_rows() > kBandFitRows ? "fit on the latest " + std::to_string(kBandFitRows) + " returns" : std::string{};
}

inline double quantile(std::vector<double> v, double q) {
    if (v.empty()) { return nan(); }
    const auto k = static_cast<std::size_t>(q * static_cast<double>(v.size() - 1));
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
    return v[k];
}

} // namespace band_detail

/// Brownian motion / GBM (Atlas 3, backtest/montecarlo.hpp): the training
/// window's volatility, constant. The benchmark every other band must beat.
class BandConstant final : public BandModel {
public:
    std::string name() const override { return "Constant sigma (GBM)"; }
    std::string family() const override { return "baseline"; }
    std::string fit(const CurriculumDesign& d) override {
        sigma_ = d.sd();
        return sigma_ > 0.0 ? std::string{} : "returns have no variance";
    }
    double scale(const CurriculumDesign&, std::size_t) const override { return sigma_; }

private:
    double sigma_ = 0.0;
};

/// Bootstrapping (Atlas 9): the empirical quantile of |r| over the window.
class BandBootstrap final : public BandModel {
public:
    std::string name() const override { return "Bootstrap quantile"; }
    std::string family() const override { return "simulation"; }
    std::string fit(const CurriculumDesign& d) override {
        const auto h = d.history(d.train_rows());
        if (h.size() < 20) { return "fewer than 20 returns"; }
        std::vector<double> a(h.size());
        for (std::size_t k = 0; k < h.size(); ++k) { a[k] = std::fabs(h[k]); }
        q_ = band_detail::quantile(std::move(a), kBandCoverage);
        return q_ > 0.0 ? std::string{} : "returns have no spread";
    }
    double scale(const CurriculumDesign&, std::size_t) const override { return q_; }

private:
    double q_ = 0.0;
};

/// Jump diffusion (Atlas 9, backtest/montecarlo.hpp's Merton process): a
/// diffusion fitted to the window's ordinary returns plus a jump process
/// fitted to the ones beyond four sigma, simulated for the next-step quantile.
class BandJump final : public BandModel {
public:
    explicit BandJump(std::uint64_t seed) noexcept : seed_(seed) {}
    std::string name() const override { return "Jump diffusion (Merton)"; }
    std::string family() const override { return "simulation"; }
    std::string fit(const CurriculumDesign& d) override {
        const auto h = d.history(d.train_rows());
        if (h.size() < 100 || !(d.sd() > 0.0)) { return "fewer than 100 returns"; }
        const double cut = 4.0 * d.sd();
        double s = 0.0, ss = 0.0, js = 0.0, jss = 0.0;
        std::size_t n = 0, jumps = 0;
        for (const double r : h) {
            if (std::fabs(r) >= cut) { ++jumps; js += r; jss += r * r; }
            else { ++n; s += r; ss += r * r; }
        }
        JumpParams p;
        p.mu = n > 0 ? s / static_cast<double>(n) : 0.0;
        p.sigma = n > 1 ? std::sqrt(std::max(0.0, ss / static_cast<double>(n) - p.mu * p.mu)) : d.sd();
        p.lambda = static_cast<double>(jumps) / static_cast<double>(h.size());
        p.jump_mean = jumps > 0 ? js / static_cast<double>(jumps) : 0.0;
        p.jump_sd = jumps > 1 ? std::sqrt(std::max(0.0, jss / static_cast<double>(jumps) - p.jump_mean * p.jump_mean)) : 0.0;
        Rng rng(seed_);
        std::vector<double> sim(kDraws);
        if (!jump_returns(sim.data(), sim.size(), p, rng)) { return "simulation refused"; }
        for (double& v : sim) { v = std::fabs(v); }
        q_ = band_detail::quantile(std::move(sim), kBandCoverage);
        lambda_ = p.lambda;
        return q_ > 0.0 ? std::string{} : "simulated no spread";
    }
    double scale(const CurriculumDesign&, std::size_t) const override { return q_; }

private:
    static constexpr std::size_t kDraws = 20000;
    std::uint64_t seed_;
    double q_ = 0.0, lambda_ = 0.0;
};

/// Historical volatility (Atlas 3, analytics/ewma.hpp's section): the last 20
/// realised returns' RMS.
class BandHistorical final : public BandModel {
public:
    std::string name() const override { return "Historical vol (20)"; }
    std::string family() const override { return "volatility"; }
    std::string fit(const CurriculumDesign& d) override {
        return d.history(d.train_rows()).size() < kLook ? "fewer than 20 returns" : std::string{};
    }
    double scale(const CurriculumDesign& d, std::size_t i) const override {
        const auto h = d.history(i);
        if (h.size() < kLook) { return band_detail::nan(); }
        double ss = 0.0;
        for (std::size_t k = h.size() - kLook; k < h.size(); ++k) { ss += h[k] * h[k]; }
        return std::sqrt(ss / static_cast<double>(kLook));
    }

private:
    static constexpr std::size_t kLook = 20;
};

/// EWMA variance, RiskMetrics lambda = 0.94 (Atlas 3, analytics/ewma.hpp).
/// Carried forward incrementally as realised returns arrive.
class BandEwma final : public BandModel {
public:
    std::string name() const override { return "EWMA (0.94)"; }
    std::string family() const override { return "volatility"; }
    std::string fit(const CurriculumDesign& d) override {
        if (d.history(d.train_rows()).size() < 20 || !(d.sd() > 0.0)) { return "fewer than 20 returns"; }
        v0_ = d.sd() * d.sd();
        v_ = v0_;
        done_ = 0;
        return {};
    }
    double scale(const CurriculumDesign& d, std::size_t i) const override {
        const auto h = d.history(i);
        if (h.size() < done_) { return band_detail::nan(); }
        for (std::size_t t = done_; t < h.size(); ++t) { v_ = kLambda * v_ + (1.0 - kLambda) * h[t] * h[t]; }
        done_ = h.size();
        return std::sqrt(v_);
    }

private:
    static constexpr double kLambda = 0.94;
    double v0_ = 0.0;
    mutable double v_ = 0.0;
    mutable std::size_t done_ = 0;
};

/// GARCH(1,1) and GJR-GARCH (Atlas 3, analytics/garch.hpp), fitted on the
/// window, filtered forward over the realised returns.
class BandGarch final : public BandModel {
public:
    explicit BandGarch(bool asymmetric) noexcept : asym_(asymmetric) {}
    std::string name() const override { return asym_ ? "GJR-GARCH" : "GARCH(1,1)"; }
    std::string family() const override { return "volatility"; }
    std::string fit(const CurriculumDesign& d) override {
        const auto w = band_detail::fit_window(d);
        if (w.size() < 100) { return "fewer than 100 returns"; }
        auto f = fit_garch(w.data(), w.size(), asym_);
        if (!f) { return std::string{"GARCH fit refused: "} + garch_error_text(f.error()); }
        p_ = f->p;
        note_ = band_detail::window_note(d);
        if (!p_.stationary()) { return "not stationary"; }
        v_ = p_.unconditional_variance();
        done_ = 0;
        return {};
    }
    double scale(const CurriculumDesign& d, std::size_t i) const override {
        const auto h = d.history(i);
        if (h.size() < done_) { return band_detail::nan(); }
        for (std::size_t t = done_; t < h.size(); ++t) {
            const double e = h[t];
            v_ = p_.omega + p_.alpha * e * e + p_.beta * v_ + (e < 0.0 ? p_.gamma * e * e : 0.0);
        }
        done_ = h.size();
        return v_ > 0.0 ? std::sqrt(v_) : band_detail::nan();
    }

    std::string tuned() const override { return note_; }

private:
    bool asym_;
    GarchParams p_{};
    std::string note_;
    mutable double v_ = 0.0;
    mutable std::size_t done_ = 0;
};

/// EGARCH(1,1) (Atlas 3, analytics/advanced_pricers.hpp): the log variance,
/// with the leverage term, filtered forward.
class BandEgarch final : public BandModel {
public:
    std::string name() const override { return "EGARCH"; }
    std::string family() const override { return "volatility"; }
    std::string fit(const CurriculumDesign& d) override {
        const auto w = band_detail::fit_window(d);
        if (w.size() < 100) { return "fewer than 100 returns"; }
        auto f = fit_egarch(w);
        if (!f) { return "EGARCH fit refused"; }
        p_ = f->params;
        note_ = band_detail::window_note(d);
        logv_ = std::log(f->initial_variance);
        done_ = 0;
        return {};
    }
    double scale(const CurriculumDesign& d, std::size_t i) const override {
        const auto h = d.history(i);
        if (h.size() < done_) { return band_detail::nan(); }
        constexpr double kAbsNormal = 0.79788456080286535588;
        for (std::size_t t = done_; t < h.size(); ++t) {
            const double z = h[t] / std::exp(0.5 * logv_);
            logv_ = p_.omega + p_.beta * logv_ + p_.alpha * (std::fabs(z) - kAbsNormal) + p_.gamma * z;
            logv_ = std::max(-50.0, std::min(10.0, logv_));   // RULE 11: safe-side clamp -- exp() range, never reached by a fitted process
        }
        done_ = h.size();
        return std::exp(0.5 * logv_);
    }

    std::string tuned() const override { return note_; }

private:
    EgarchParams p_{};
    std::string note_;
    mutable double logv_ = 0.0;
    mutable std::size_t done_ = 0;
};

/// Heston stochastic volatility (Atlas 3): the variance mean-reverts, CIR/OU
/// style, and its one-step expectation is the forecast. Fitted as an
/// Ornstein-Uhlenbeck process on the EWMA variance of the window.
class BandHeston final : public BandModel {
public:
    std::string name() const override { return "Heston variance drift"; }
    std::string family() const override { return "volatility"; }
    std::string fit(const CurriculumDesign& d) override {
        const auto h = d.history(d.train_rows());
        if (h.size() < 100 || !(d.sd() > 0.0)) { return "fewer than 100 returns"; }
        std::vector<double> v(h.size());
        double x = d.sd() * d.sd();
        for (std::size_t t = 0; t < h.size(); ++t) { x = 0.94 * x + 0.06 * h[t] * h[t]; v[t] = x; }
        auto ou = fit_ou(v, 1.0);
        if (!ou) { return "variance not mean-reverting in this window"; }
        ou_ = *ou;
        x_ = d.sd() * d.sd();
        done_ = 0;
        return {};
    }
    double scale(const CurriculumDesign& d, std::size_t i) const override {
        const auto h = d.history(i);
        if (h.size() < done_) { return band_detail::nan(); }
        for (std::size_t t = done_; t < h.size(); ++t) { x_ = 0.94 * x_ + 0.06 * h[t] * h[t]; }
        done_ = h.size();
        const double next = ou_.expected_next(x_);
        return next > 0.0 ? std::sqrt(next) : band_detail::nan();
    }

private:
    OrnsteinUhlenbeck ou_{};
    mutable double x_ = 0.0;
    mutable std::size_t done_ = 0;
};

/// Seasonal volatility: the window's volatility profile by time of day (by
/// weekday on a daily track), times an EWMA of the deseasonalised returns.
/// The intraday open is several times as volatile as lunch; a band that does
/// not know it is too wide at noon and too narrow at 09:15.
class BandSeasonal final : public BandModel {
public:
    std::string name() const override { return "Seasonal vol (time of day)"; }
    std::string family() const override { return "volatility"; }
    std::string fit(const CurriculumDesign& d) override {
        const auto& tr = d.track();
        const std::size_t n = d.train_rows();
        if (tr.slot.size() != tr.rows() || n < 100) { return "no slot profile"; }
        std::size_t slots = 0;
        for (std::size_t j = 0; j < n; ++j) { slots = std::max<std::size_t>(slots, tr.slot[j] + 1u); }
        std::vector<double> ss(slots, 0.0), cnt(slots, 0.0);
        double all = 0.0;
        for (std::size_t j = 0; j < n; ++j) {
            const double r = d.target(j);
            ss[tr.slot[j]] += r * r;
            cnt[tr.slot[j]] += 1.0;
            all += r * r;
        }
        const double base = std::sqrt(all / static_cast<double>(n));
        if (!(base > 0.0)) { return "returns have no variance"; }
        f_.assign(slots, 1.0);
        for (std::size_t s = 0; s < slots; ++s) {
            if (cnt[s] >= 5.0 && ss[s] > 0.0) { f_[s] = std::sqrt(ss[s] / cnt[s]) / base; }
        }
        v_ = base * base;
        done_ = 0;
        return {};
    }
    double scale(const CurriculumDesign& d, std::size_t i) const override {
        const auto& tr = d.track();
        const auto h = d.history(i);
        if (h.size() < done_) { return band_detail::nan(); }
        for (std::size_t t = done_; t < h.size(); ++t) {
            const double f = tr.slot[t] < f_.size() ? f_[tr.slot[t]] : 1.0;
            const double u = h[t] / f;
            v_ = 0.94 * v_ + 0.06 * u * u;
        }
        done_ = h.size();
        const double f = tr.slot[i] < f_.size() ? f_[tr.slot[i]] : 1.0;
        return f * std::sqrt(v_);
    }

private:
    std::vector<double> f_;
    mutable double v_ = 0.0;
    mutable std::size_t done_ = 0;
};

/// Machine-learned volatility: a boosted tree (models/gbdt.hpp) or a random
/// forest (models/classical.hpp) regressing |r| on the features.
class BandLearned final : public BandModel {
public:
    BandLearned(bool forest, std::uint64_t seed) noexcept : forest_(forest), seed_(seed) {}
    std::string name() const override { return forest_ ? "Random forest on |r|" : "Gradient boosting on |r|"; }
    std::string family() const override { return "learned"; }
    std::string fit(const CurriculumDesign& d) override {
        const std::size_t n = d.train_rows();
        if (n < 64) { return "fewer than 64 rows"; }
        const auto x = d.block(0, n);
        std::vector<double> y(n);
        for (std::size_t j = 0; j < n; ++j) { y[j] = std::fabs(d.target(j)); }
        if (forest_) {
            ForestParams prm;
            prm.trees = 32;
            prm.max_depth = 6;
            prm.min_leaf = n / 200 > 5 ? n / 200 : 5;
            prm.seed = seed_;
            prm.regression = true;
            auto m = RandomForest::fit(x, n, d.p(), y, prm);
            if (!m) { return "forest fit refused"; }
            rf_ = std::move(*m);
        } else {
            Frame f;
            f.x = x;
            f.rows = n;
            f.p = d.p();
            GbdtParams prm;
            prm.seed = seed_;
            auto m = fit_gbdt(f, y, prm);
            if (!m) { return "GBDT fit refused"; }
            gb_ = std::move(*m);
        }
        return {};
    }
    double scale(const CurriculumDesign& d, std::size_t i) const override {
        const auto f = d.features(i);
        if (f.empty()) { return band_detail::nan(); }
        const double v = forest_ ? rf_.predict(f) : gb_.predict_row(f.data());
        return v > 0.0 ? v : band_detail::nan();
    }

private:
    bool forest_;
    std::uint64_t seed_;
    RandomForest rf_{};
    Gbdt gb_{};
};

[[nodiscard]] inline std::vector<std::unique_ptr<BandModel>> band_default_models(std::uint64_t seed = 0xB4ADu) {
    std::vector<std::unique_ptr<BandModel>> m;
    m.push_back(std::make_unique<BandConstant>());
    m.push_back(std::make_unique<BandBootstrap>());
    m.push_back(std::make_unique<BandJump>(seed));
    m.push_back(std::make_unique<BandHistorical>());
    m.push_back(std::make_unique<BandEwma>());
    m.push_back(std::make_unique<BandGarch>(false));
    m.push_back(std::make_unique<BandGarch>(true));
    m.push_back(std::make_unique<BandEgarch>());
    m.push_back(std::make_unique<BandHeston>());
    m.push_back(std::make_unique<BandSeasonal>());
    m.push_back(std::make_unique<BandLearned>(false, seed + 1));
    m.push_back(std::make_unique<BandLearned>(true, seed + 2));
    return m;
}

/// The ensembles added after the base band models:
///   Vol ensemble     geometric mean of the base models' scales that forecast
///   Best band so far the base model with the best interval score in finished
///                    stages (the Champion, for bands)
inline constexpr const char* kBandEnsembles[] = {"Vol ensemble", "Best band so far"};

struct BandRun {
    std::vector<std::string> models, families;
    std::size_t base_models = 0;
    std::vector<CurriculumStage> stages;
    std::size_t first_row = 0;
    std::vector<std::uint16_t> stage_of;
    /// Half-width of the band in log-return units, [model][row - first_row];
    /// NaN where the model abstained.
    std::vector<std::vector<float>> half;
    std::vector<std::vector<std::string>> notes;   ///< [model][stage]
    std::vector<std::vector<double>> k;            ///< [stage][model] the calibrated multiplier
    std::vector<double> fit_seconds;
    std::size_t lookahead_refusals = 0;
};

namespace band_detail {

/// Winkler / interval score of a symmetric band, in log-return units.
inline double interval_score(double half, double r, double alpha) noexcept {
    const double miss = std::fabs(r) - half;
    return 2.0 * half + (miss > 0.0 ? (2.0 / alpha) * miss : 0.0);
}

} // namespace band_detail

[[nodiscard]] inline std::expected<BandRun, CurriculumError>
band_run(const CurriculumTrack& tr, std::vector<std::unique_ptr<BandModel>>& models,
         const CurriculumOptions& opt = {}) {
    if (auto ok = curriculum_check_track(tr); !ok) { return std::unexpected(ok.error()); }
    BandRun run;
    run.stages = curriculum_stages(tr, opt.first_days, opt.step_cap_days);
    if (run.stages.empty()) { return std::unexpected(CurriculumError::TooFewDays); }
    const std::size_t base = models.size();
    run.base_models = base;
    for (const auto& m : models) { run.models.push_back(m->name()); run.families.push_back(m->family()); }
    for (const char* e : kBandEnsembles) { run.models.emplace_back(e); run.families.emplace_back("ensemble"); }
    const std::size_t total = run.models.size(), ens = base, best_so_far = base + 1;
    run.first_row = run.stages.front().test_begin;
    const std::size_t n = tr.rows() - run.first_row;
    run.stage_of.assign(n, 0);
    run.half.assign(total, std::vector<float>(n, std::numeric_limits<float>::quiet_NaN()));
    run.notes.assign(total, std::vector<std::string>(run.stages.size()));
    run.fit_seconds.assign(total, 0.0);

    // Past out-of-sample |r| / scale per model: the conformal calibration set.
    std::vector<std::vector<double>> past(total);
    std::vector<double> past_score(base, 0.0);
    std::vector<std::size_t> past_n(base, 0);
    const double alpha = 1.0 - kBandCoverage;
    std::vector<std::vector<double>> scale_now(total, std::vector<double>());

    for (const CurriculumStage& st : run.stages) {
        for (std::size_t i = st.test_begin; i < st.test_end; ++i) {
            run.stage_of[i - run.first_row] = static_cast<std::uint16_t>(st.index);
        }
        auto made = CurriculumDesign::make(tr, st);
        std::vector<double> ks(total, band_detail::nan());
        if (!made) {
            for (auto& v : run.notes) { v[st.index] = "abstained: scaler refused"; }
            run.k.push_back(ks);
            continue;
        }
        CurriculumDesign& d = *made;
        const std::size_t rows = st.test_end - st.test_begin;
        std::vector<std::vector<double>> sc(total, std::vector<double>(rows, band_detail::nan()));

        for (std::size_t m = 0; m < base; ++m) {
            const auto t0 = std::chrono::steady_clock::now();
            d.set_now(st.train_rows);
            const std::string why = models[m]->fit(d);
            if (!why.empty()) {
                run.notes[m][st.index] = "abstained: " + why;
                run.fit_seconds[m] += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                continue;
            }
            // Calibration: past out-of-sample errors if there are enough, else
            // the training window in-sample.
            std::vector<double> cal = past[m];
            if (cal.size() < kBandCalibrationMin) {
                cal.clear();
                for (std::size_t j = 0; j < st.train_rows; ++j) {
                    d.set_now(j);
                    const double s = models[m]->scale(d, j);
                    if (s > 0.0) { cal.push_back(std::fabs(tr.ret(j)) / s); }
                }
                // The in-sample pass moved the filters to the window's end; refit resets them.
                d.set_now(st.train_rows);
                (void)models[m]->fit(d);
                run.notes[m][st.index] = "calibrated in-sample";
            } else {
                run.notes[m][st.index] = "calibrated on " + std::to_string(cal.size()) + " past forecasts";
            }
            if (const std::string t = models[m]->tuned(); !t.empty()) { run.notes[m][st.index] += "; " + t; }
            ks[m] = band_detail::quantile(std::move(cal), kBandCoverage);
            for (std::size_t i = st.test_begin; i < st.test_end; ++i) {
                d.set_now(i);
                const double s = models[m]->scale(d, i);
                sc[m][i - st.test_begin] = s;
                if (s > 0.0 && std::isfinite(ks[m])) {
                    run.half[m][i - run.first_row] = static_cast<float>(ks[m] * s);
                }
            }
            run.fit_seconds[m] += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        }

        // Vol ensemble: geometric mean of the base scales, calibrated like a model.
        {
            std::vector<double> cal = past[ens];
            for (std::size_t r = 0; r < rows; ++r) {
                double lg = 0.0;
                std::size_t cnt = 0;
                for (std::size_t m = 0; m < base; ++m) {
                    if (run.families[m] == "baseline" || !(sc[m][r] > 0.0)) { continue; }
                    lg += std::log(sc[m][r]);
                    ++cnt;
                }
                sc[ens][r] = cnt > 0 ? std::exp(lg / static_cast<double>(cnt)) : band_detail::nan();
            }
            if (cal.size() >= kBandCalibrationMin) {
                ks[ens] = band_detail::quantile(std::move(cal), kBandCoverage);
                run.notes[ens][st.index] = "calibrated on " + std::to_string(past[ens].size()) + " past forecasts";
                for (std::size_t r = 0; r < rows; ++r) {
                    if (sc[ens][r] > 0.0) { run.half[ens][st.test_begin + r - run.first_row] = static_cast<float>(ks[ens] * sc[ens][r]); }
                }
            } else {
                run.notes[ens][st.index] = "abstained: calibrating on this stage";
            }
        }
        // Best band so far: the base model with the lowest mean interval score.
        {
            int best = -1;
            double bs = std::numeric_limits<double>::infinity();
            for (std::size_t m = 0; m < base; ++m) {
                if (past_n[m] < 50) { continue; }
                const double v = past_score[m] / static_cast<double>(past_n[m]);
                if (v < bs) { bs = v; best = static_cast<int>(m); }
            }
            if (best >= 0) {
                run.notes[best_so_far][st.index] = "follows " + run.models[static_cast<std::size_t>(best)];
                for (std::size_t r = 0; r < rows; ++r) {
                    run.half[best_so_far][st.test_begin + r - run.first_row] =
                        run.half[static_cast<std::size_t>(best)][st.test_begin + r - run.first_row];
                }
                ks[best_so_far] = ks[static_cast<std::size_t>(best)];
            } else {
                run.notes[best_so_far][st.index] = "abstained: no record yet";
            }
        }
        run.k.push_back(ks);

        // The stage is over: its errors join the calibration sets and the record.
        for (std::size_t r = 0; r < rows; ++r) {
            const std::size_t i = st.test_begin + r;
            const double ret = tr.ret(i);
            for (std::size_t m = 0; m <= ens; ++m) {
                if (sc[m][r] > 0.0 && std::isfinite(sc[m][r])) { past[m].push_back(std::fabs(ret) / sc[m][r]); }
            }
            for (std::size_t m = 0; m < base; ++m) {
                const float h = run.half[m][i - run.first_row];
                if (std::isfinite(h)) {
                    past_score[m] += band_detail::interval_score(static_cast<double>(h), ret, alpha);
                    ++past_n[m];
                }
            }
        }
        run.lookahead_refusals += d.refusals();
    }
    return run;
}

/// A band model's record over rows [from, to).
struct BandTally {
    std::size_t made = 0, abstained = 0, hits = 0;
    double width_bp = 0.0;        ///< summed full width, bp
    double score_bp = 0.0;        ///< summed interval score, bp
    std::size_t hi_made = 0, hi_hits = 0;   ///< the half of rows with the widest bands
    [[nodiscard]] double coverage() const noexcept {
        return made > 0 ? static_cast<double>(hits) / static_cast<double>(made) : std::numeric_limits<double>::quiet_NaN();
    }
    [[nodiscard]] double mean_width_bp() const noexcept {
        return made > 0 ? width_bp / static_cast<double>(made) : std::numeric_limits<double>::quiet_NaN();
    }
    [[nodiscard]] double mean_score_bp() const noexcept {
        return made > 0 ? score_bp / static_cast<double>(made) : std::numeric_limits<double>::quiet_NaN();
    }
};

[[nodiscard]] inline BandTally band_tally(const CurriculumTrack& tr, const BandRun& run, std::size_t model,
                                          std::size_t from_row, std::size_t to_row) {
    BandTally t;
    const double alpha = 1.0 - kBandCoverage;
    std::vector<float> widths;
    for (std::size_t i = from_row; i < to_row; ++i) {
        const float h = run.half[model][i - run.first_row];
        if (!std::isfinite(h)) { ++t.abstained; continue; }
        widths.push_back(h);
    }
    float median = 0.0F;
    if (!widths.empty()) {
        std::nth_element(widths.begin(), widths.begin() + static_cast<std::ptrdiff_t>(widths.size() / 2), widths.end());
        median = widths[widths.size() / 2];
    }
    for (std::size_t i = from_row; i < to_row; ++i) {
        const float hf = run.half[model][i - run.first_row];
        if (!std::isfinite(hf)) { continue; }
        const double h = static_cast<double>(hf), r = tr.ret(i);
        ++t.made;
        const bool hit = std::fabs(r) <= h;
        if (hit) { ++t.hits; }
        t.width_bp += 2.0 * h * 1e4;
        t.score_bp += band_detail::interval_score(h, r, alpha) * 1e4;
        if (hf >= median) { ++t.hi_made; if (hit) { ++t.hi_hits; } }
    }
    return t;
}

} // namespace altair
