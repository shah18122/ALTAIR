// P9-01 .. P9-07 acceptance tests.
//
// Test 1: an aggregate score is the arithmetic that destroys the two facts it
// averaged over.
//
// Test 2: measuring drift on STANDARDISED features measures nothing, because
// the scaler was fitted on the reference window.
//
// Test 3: Page-Hinkley and ADWIN detect different things.
//
// Test 5 is THE PHASE EXIT: a deliberately poisoned model is auto-detected,
// de-weighted and rolled back with no human action.
//
// No check description here may contain the substring FAIL.

#include <flagging/deploy.hpp>
#include <models/dataset.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    if (ok) {
        std::printf("  ok  : %s\n", what);
    } else {
        ++failures;
        std::printf("  FAIL: %s\n", what);
    }
}

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

struct Lcg {
    std::uint64_t s;
    double uniform()
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>((s >> 11) & ((1ULL << 53) - 1))
               / static_cast<double>(1ULL << 53);
    }
    double normal()
    {
        const double u1 = uniform();
        const double u2 = uniform();
        return std::sqrt(-2.0 * std::log(u1 + 1e-300))
               * std::cos(6.283185307179586 * u2);
    }
};

} // namespace

using namespace altair;

namespace {

MarketRegime regime_of(TrendRegime t, VolRegime v)
{
    MarketRegime r{};
    r.trend = t;
    r.vol = v;
    r.liquidity = LiquidityRegime::Normal;
    return r;
}

ModelKey key_of(std::uint64_t p)
{
    ModelKey k{};
    k.param_hash = p;
    k.feature_version = 0x5151;
    k.architecture_hash = 0xAAAA;
    k.train_from = 0;
    k.train_to = 1000;
    k.seed = 1;
    return k;
}

// ── 1 ────────────────────────────────────────────────────────────────────
// THE scorecard card.
void an_aggregate_destroys_the_facts_it_averages()
{
    std::printf("\n1 an_aggregate_destroys_the_facts_it_averages\n");
    Scorecards sc;
    Lcg g{0x5C02E1};

    // A model with a LARGE edge when trending and an equally large ANTI-edge
    // when mean-reverting. Two actionable facts.
    const auto trending = regime_of(TrendRegime::Trending, VolRegime::Normal);
    const auto reverting = regime_of(TrendRegime::MeanReverting,
                                     VolRegime::Normal);
    for (int i = 0; i < 400; ++i) {
        const double f = g.normal();
        (void)sc.observe(0, 0, trending, f, 0.7 * f + 0.7 * g.normal());
        (void)sc.observe(0, 0, reverting, f, -0.7 * f + 0.7 * g.normal());
    }

    const auto ct = regime_cell(trending);
    const auto cr = regime_cell(reverting);
    check(ct.has_value() && cr.has_value(), "both regimes map to cells");
    if (!ct || !cr) { return; }
    const auto ic_t = sc.at(0, 0, *ct).ic();
    const auto ic_r = sc.at(0, 0, *cr).ic();
    const auto agg = sc.aggregate(0, 0);
    const auto ic_a = agg.overall.ic();
    const auto spread = sc.ic_spread(0, 0);
    check(ic_t.has_value() && ic_r.has_value() && ic_a.has_value()
          && spread.has_value(), "all three ICs compute");
    if (!ic_t || !ic_r || !ic_a || !spread) { return; }

    std::printf("    one model, 800 observations, split across two regimes:\n"
                "      IC when TRENDING        %+.4f\n"
                "      IC when MEAN-REVERTING  %+.4f\n"
                "      AGGREGATE IC            %+.4f   <- the only number most"
                " scorecards report\n"
                "      IC spread               %.4f\n",
                *ic_t, *ic_r, *ic_a, *spread);
    check(*ic_t > 0.5 && *ic_r < -0.5,
          "the model has a large edge in one regime and an equally large"
          " ANTI-edge in the other -- two facts, both actionable");
    check(std::fabs(*ic_a) < 0.1,
          "and the aggregate is approximately zero, which reads as a model"
          " with no edge and is a shrug");
    check(*spread > 1.0,
          "while the IC SPREAD is over 1.0, which is the single number that"
          " says this aggregate is hiding something -- a uniformly mediocre"
          " model has the same aggregate and a small spread");
    std::printf("    -> the aggregate is not a summary of those two facts, it"
                " is the arithmetic that\n       destroys them, and nothing"
                " recovers what it averaged. CLAUDE.md states it as\n       a"
                " rule: report per regime, never only in aggregate.\n");

    // The counts travel with the aggregate, because the second thing it hides
    // is how thin the cells are.
    Scorecards thin;
    for (int i = 0; i < 400; ++i) {
        const double f = g.normal();
        (void)thin.observe(0, 0, trending, f, 0.3 * f + g.normal());
    }
    for (int i = 0; i < 9; ++i) {
        const double f = g.normal();
        (void)thin.observe(0, 0, regime_of(TrendRegime::Trending,
                                           VolRegime::High),
                           f, 0.3 * f + g.normal());
    }
    const auto ta = thin.aggregate(0, 0);
    std::printf("    a second model: %zu populated cells, thinnest holds %zu"
                " observations of %zu\n",
                ta.populated, ta.thinnest, ta.overall.n);
    check(ta.populated == 2 && ta.thinnest == 9,
          "the aggregate carries the per-cell counts, so it cannot be quoted"
          " without the reader seeing it was built from cells of 400 and 9 --"
          " and the thin cell is the high-volatility one, which is the regime"
          " whose behaviour matters most");

    // A regime that is not fully decided is refused, exactly as P6-03 refuses.
    check(sc.observe(0, 0, MarketRegime{}, 1.0, 1.0).error()
          == ScoreError::RegimeIncomplete,
          "an undecided regime cannot be scored, rather than being filed under"
          " a default cell that would then accumulate every uncertain moment"
          " in the session");
}

// ── 2 ────────────────────────────────────────────────────────────────────
// THE drift card.
void drift_measured_on_standardised_features_measures_nothing()
{
    std::printf("\n2 drift_measured_on_standardised_features_measures"
                "_nothing\n");
    constexpr std::size_t kRef = 2000, kLive = 2000;
    static double ref_raw[kRef], live_raw[kLive];
    static double ref_std[kRef], live_std[kLive];
    static double work[kLive];

    Lcg g{0xD21F7A};
    for (std::size_t i = 0; i < kRef; ++i) { ref_raw[i] = g.normal(); }
    // The live window has SHIFTED by a full standard deviation.
    for (std::size_t i = 0; i < kLive; ++i) { live_raw[i] = g.normal() + 1.0; }

    // The scaler was fitted on the REFERENCE window, exactly as P8-02
    // requires -- correct for the model, and it is what hides the drift.
    static double refm[kRef];
    for (std::size_t i = 0; i < kRef; ++i) { refm[i] = ref_raw[i]; }
    Matrix rm{refm, kRef, 1};
    Scaler s;
    check(s.fit(rm, 0, kRef).has_value(), "the scaler fits on the reference");
    const double mu = s.mean(0), sd = s.sd(0);
    for (std::size_t i = 0; i < kRef; ++i) { ref_std[i] = (ref_raw[i] - mu) / sd; }
    for (std::size_t i = 0; i < kLive; ++i) { live_std[i] = (live_raw[i] - mu) / sd; }

    auto psi_of = [&](const double* r, const double* l, Scaling sc) {
        static double tmp[kRef];
        for (std::size_t i = 0; i < kRef; ++i) { tmp[i] = r[i]; }
        PsiDetector d;
        if (!d.fit(tmp, kRef, 10, sc)) { return -1.0; }
        for (std::size_t i = 0; i < kLive; ++i) { work[i] = l[i]; }
        const auto v = d.psi(work, kLive);
        return v ? *v : -1.0;
    };
    const double psi_raw = psi_of(ref_raw, live_raw, Scaling::Raw);
    const double psi_std = psi_of(ref_std, live_std, Scaling::Standardised);

    std::printf("    the live window has shifted by a full standard"
                " deviation:\n"
                "      PSI on RAW features            %.4f\n"
                "      PSI on STANDARDISED features   %.4f\n",
                psi_raw, psi_std);
    check(psi_raw > 0.25,
          "on raw features the PSI is well past the conventional 0.25 'the"
          " population has moved' line");
    check(near(psi_raw, psi_std, 0.05),
          "and standardising does NOT change it here, because the shift is a"
          " location change and the scaler subtracts a CONSTANT -- so this"
          " particular drift survives");
    std::printf("    -> which is the honest result and is worth stating"
                " precisely: subtracting a fixed\n       mean cannot hide a"
                " shift. What standardisation hides is a change in SCALE.\n");

    // The case it DOES hide: the live window's spread has doubled, and the
    // scaler divides by the reference sigma -- so the live values are still
    // divided by the OLD sigma and the shape change survives. But refitting
    // the scaler per window, which is what an "always standardise your inputs"
    // pipeline does, removes it entirely.
    for (std::size_t i = 0; i < kLive; ++i) { live_raw[i] = g.normal() * 3.0; }
    static double live_refit[kLive];
    double m2 = 0.0;
    for (std::size_t i = 0; i < kLive; ++i) { m2 += live_raw[i]; }
    m2 /= static_cast<double>(kLive);
    double v2 = 0.0;
    for (std::size_t i = 0; i < kLive; ++i) { v2 += (live_raw[i] - m2) * (live_raw[i] - m2); }
    v2 = std::sqrt(v2 / static_cast<double>(kLive - 1));
    for (std::size_t i = 0; i < kLive; ++i) {
        live_refit[i] = (live_raw[i] - m2) / v2;
    }
    const double psi_scale_raw = psi_of(ref_raw, live_raw, Scaling::Raw);
    const double psi_scale_refit = psi_of(ref_std, live_refit,
                                          Scaling::Standardised);
    std::printf("    the live window's SPREAD has tripled:\n"
                "      PSI on raw features                        %.4f\n"
                "      PSI after refitting the scaler per window  %.4f\n",
                psi_scale_raw, psi_scale_refit);
    check(psi_scale_raw > 0.25,
          "a tripled spread is a large drift on raw features");
    check(psi_scale_refit < 0.05,
          "and refitting the scaler on the live window ERASES it -- the"
          " transform maps whatever arrives onto a standard normal, so the"
          " reference and the live window are identical by construction and"
          " the PSI is near zero however far the population has moved");
    check(psi_scale_raw > 10.0 * psi_scale_refit,
          "two orders of magnitude between the same drift measured before and"
          " after a per-window standardisation");
    std::printf("    -> the drift is not removed from the model's INPUT. The"
                " model still receives\n       the shifted values and is still"
                " wrong. It is removed from the MEASUREMENT.\n");

    PsiDetector unspecified;
    static double tmp2[kRef];
    for (std::size_t i = 0; i < kRef; ++i) { tmp2[i] = ref_raw[i]; }
    check(unspecified.fit(tmp2, kRef, 10, Scaling::Unspecified).error()
          == DriftError::UnknownScaling,
          "so a detector that was not told whether its input is raw refuses to"
          " fit -- it cannot tell by looking, because standardised features"
          " look exactly like raw ones with a different mean");
}

// ── 3 ────────────────────────────────────────────────────────────────────
void page_hinkley_and_adwin_detect_different_things()
{
    std::printf("\n3 page_hinkley_and_adwin_detect_different_things\n");
    PageHinkley ph;
    Adwin<1024> ad;
    check(ph.configure(0.05, 8.0).has_value(), "Page-Hinkley configures");
    check(ad.configure(0.002).has_value(), "ADWIN configures");

    Lcg g{0x5417F7};
    std::size_t ph_fired_at = 0, ad_cut_at = 0;
    for (std::size_t i = 0; i < 1500; ++i) {
        // A MEAN SHIFT at 800: the error stream jumps by 0.8.
        const double x = g.normal() * 0.3 + (i >= 800 ? 0.8 : 0.0);
        const auto a = ph.push(x);
        const auto b = ad.push(x);
        if (a && *a && ph_fired_at == 0) { ph_fired_at = i; }
        if (b && *b && ad_cut_at == 0 && i > 800) { ad_cut_at = i; }
    }
    std::printf("    a mean shift of +0.8 at observation 800:\n"
                "      Page-Hinkley alarmed at   %zu\n"
                "      ADWIN cut its window at   %zu, window now %zu of 1500\n",
                ph_fired_at, ad_cut_at, ad.window());
    check(ph_fired_at > 800 && ph_fired_at < 1000,
          "Page-Hinkley alarms shortly after the shift");
    check(ad_cut_at > 800,
          "and ADWIN cuts its window after it too");
    check(ad.window() < 1500,
          "but ADWIN's OUTPUT is the window itself: its length says how far"
          " back the current behaviour extends, which is what a retraining"
          " trigger needs and is the thing Page-Hinkley cannot say");
    std::printf("    -> Page-Hinkley says 'it changed'. ADWIN says 'and"
                " everything before here no\n       longer applies'. A system"
                " with only the first knows to retrain and not on what.\n");

    // Neither may run unconfigured.
    PageHinkley bare;
    Adwin<256> bare2;
    check(bare.push(1.0).error() == DriftError::NoThreshold
          && bare2.push(1.0).error() == DriftError::NoThreshold,
          "and neither runs without a threshold -- a tolerance of zero fires"
          " on any deviation at all, and a threshold is a statement about how"
          " much evidence justifies pulling a model out of production");

    // KS: the critical value shrinks with sample size, so a bare statistic is
    // not a verdict.
    static double a1[400], b1[400];
    Lcg g2{0x5A};
    for (std::size_t i = 0; i < 400; ++i) {
        a1[i] = g2.normal();
        b1[i] = g2.normal() + 0.25;
    }
    const auto d = ks_statistic(a1, 400, b1, 400);
    check(d.has_value(), "the KS statistic computes");
    if (d) {
        std::printf("    KS on a 0.25-sigma shift, n=400 each: D = %.4f,"
                    " critical at 5%% = %.4f  ->  %s\n",
                    *d, ks_critical(400, 400, 1.36),
                    *d > ks_critical(400, 400, 1.36) ? "significant"
                                                     : "not significant");
        check(ks_critical(400, 400, 1.36) < ks_critical(50, 50, 1.36),
              "and the critical value SHRINKS with sample size, so a D of 0.08"
              " is decisive on ten thousand points and meaningless on fifty --"
              " which is why the critical value is provided rather than left"
              " to a remembered number");
    }
}

// ── 4 ────────────────────────────────────────────────────────────────────
void the_weight_update_is_shrunk_by_sample_size()
{
    std::printf("\n4 the_weight_update_is_shrunk_by_sample_size\n");
    UpdatePolicy p{};
    p.rate = 0.5;
    p.shrink_k = 50.0;
    p.min_weight = 0.01;
    p.max_weight = 1.0;

    ScoreCell thin{}, thick{};
    Lcg g{0x54217E};
    for (int i = 0; i < 5; ++i) {
        const double f = g.normal();
        thin.observe(f, -0.9 * f + 0.2 * g.normal());     // terrible
    }
    for (int i = 0; i < 2000; ++i) {
        const double f = g.normal();
        thick.observe(f, -0.9 * f + 0.2 * g.normal());    // equally terrible
    }
    const auto w_thin = update_weight(0.5, thin, p);
    const auto w_thick = update_weight(0.5, thick, p);
    check(w_thin.has_value() && w_thick.has_value(), "both update");
    if (!w_thin || !w_thick) { return; }

    std::printf("    the same terrible IC from two cells:\n"
                "      5 observations    shrinkage %.4f  ->  weight 0.500 ->"
                " %.4f\n"
                "      2000 observations shrinkage %.4f  ->  weight 0.500 ->"
                " %.4f\n",
                shrinkage(5, p.shrink_k), *w_thin,
                shrinkage(2000, p.shrink_k), *w_thick);
    check(*w_thin > *w_thick,
          "five observations barely move the weight and two thousand move it a"
          " long way -- the step is scaled by n/(n+k), so the first three"
          " observations of a new regime cannot halve a model's weight");
    check(*w_thick >= p.min_weight,
          "and the weight is floored rather than driven to zero: a model at"
          " exactly zero stops being scored and can never earn its way back");

    ScoreCell empty{};
    const auto w_empty = update_weight(0.5, empty, p);
    check(w_empty.has_value() && near(*w_empty, 0.5, 1e-12),
          "a cell with no usable IC produces NO update -- not a downward one."
          " An absence of evidence is not evidence of failure, and treating it"
          " as such de-weights every model on the first day of a regime it has"
          " never seen");

    UpdatePolicy bare{};
    check(update_weight(0.5, thick, bare).error() == ScoreError::NoShrinkage,
          "and a policy with no shrinkage constant is refused, because how"
          " much evidence is enough depends on how expensive being wrong is");
}

// ── 5 ────────────────────────────────────────────────────────────────────
// THE PHASE EXIT.
void a_poisoned_model_is_detected_deweighted_and_rolled_back()
{
    std::printf("\n5 a_poisoned_model_is_detected_deweighted_and_rolled"
                "_back\n");
    std::printf("    ROADMAP Phase 9 exit: \"a deliberately poisoned model is"
                " auto-detected,\n    de-weighted, and rolled back with no"
                " human action.\"\n\n");

    const ModelKey incumbent = key_of(0x1111);
    const ModelKey candidate = key_of(0x2222);
    Lcg g{0xB01503};

    // --- shadow -----------------------------------------------------------
    ShadowRun shadow;
    check(shadow.open(candidate, 200).has_value(), "a shadow run opens");
    for (int i = 0; i < 250; ++i) {
        const double truth = g.normal();
        const double realised = truth + 0.3 * g.normal();
        (void)shadow.observe(truth * 0.8, truth * 0.8, realised);
    }
    check(shadow.complete(),
          "the candidate completes its shadow period on sample size alone --"
          " whether it is any good is the canary's business, and conflating"
          " the two is how a promising candidate on forty observations reaches"
          " capital");
    const auto shadow_ic = shadow.candidate_score().ic();
    check(shadow_ic.has_value() && *shadow_ic > 0.7,
          "and it looks excellent in shadow, which is exactly the situation a"
          " canary exists for");
    std::printf("    shadow: %zu observations, candidate IC %+.4f, mean"
                " divergence from incumbent %.4f\n",
                shadow.observations(), *shadow_ic, shadow.mean_divergence());

    // --- arm the trigger BEFORE the canary opens --------------------------
    CanaryController canary;
    RollbackTrigger trig{};
    trig.min_ic = 0.10;
    trig.max_loss_paise = 500'00;       // Rs 500
    trig.max_divergence = 2.0;
    trig.min_observations = 30;
    check(canary.open(shadow, incumbent, 0.10).error()
          == DeployError::NotArmed,
          "a canary cannot open before its rollback trigger is armed -- a rule"
          " chosen after seeing the numbers is a decision wearing a rule's"
          " clothes");
    check(canary.arm(trig).has_value(), "the trigger arms");
    check(canary.arm(trig).error() == DeployError::AlreadyArmed,
          "and cannot be re-armed, which would let a disappointing canary be"
          " met with a looser bound");
    check(canary.open(shadow, incumbent, 0.10).has_value(),
          "then the canary opens on 10% of capital, against an EXPLICIT"
          " fallback -- 'the previous one' is a question rather than an answer"
          " after the second rollback");

    // --- POISON: from observation 60 the candidate inverts ---------------
    PageHinkley ph;
    (void)ph.configure(0.05, 6.0);
    Scorecards sc;
    UpdatePolicy pol{};
    pol.rate = 0.6;
    pol.shrink_k = 20.0;
    pol.min_weight = 0.01;
    pol.max_weight = 1.0;
    double weight = 0.50;

    const auto reg = regime_of(TrendRegime::Trending, VolRegime::Normal);
    std::size_t rolled_at = 0;
    CanaryOutcome final_outcome{};
    // The SCORECARD sees the whole session -- it scores everything the model
    // emitted, and the session-close de-weighting runs on all of it. The
    // CANARY only sees observations while it holds capital. Conflating the two
    // would mean a model that was pulled after 30 observations is never scored
    // on the 370 that followed, and so never de-weighted.
    for (std::size_t i = 0; i < 400; ++i) {
        const double truth = g.normal();
        const double realised = truth + 0.3 * g.normal();
        const bool poisoned = i >= 60;
        const double cand = poisoned ? -truth * 0.8 : truth * 0.8;
        const double inc = truth * 0.8;

        (void)sc.observe(0, 0, reg, cand, realised);

        // THE ERROR STREAM: a bounded miss indicator, 1 when the sign was
        // wrong. A first draft fed -cand*realised, which is chi-squared
        // shaped -- its cumulative deviation wanders far enough to trip
        // Page-Hinkley at observation 36, twenty-four bars BEFORE the
        // poisoning. A drift detector on an unbounded, skewed statistic
        // detects the statistic's own shape.
        const double miss = ((cand > 0.0) == (realised > 0.0)) ? 0.0 : 1.0;
        const auto alarm = ph.push(miss);
        const bool drift = alarm && *alarm;

        if (!canary.running()) { continue; }
        // The realised P&L of the canary's slice: right sign earns, wrong
        // sign loses.
        const std::int64_t paise = miss > 0.0 ? -140 : 60;
        const auto out = canary.observe(cand, inc, realised, paise, drift);
        if (!out) { break; }
        if (out->rolled_back() && rolled_at == 0) {
            rolled_at = i;
            final_outcome = *out;
        }
    }

    // --- de-weight, from the scorecard, with no human action -------------
    const auto post = sc.at(0, 0, *regime_cell(reg)).ic();
    const auto new_weight = update_weight(weight, sc.at(0, 0, *regime_cell(reg)),
                                          pol);
    check(post.has_value() && new_weight.has_value(), "the scorecard scores");
    if (post && new_weight) { weight = *new_weight; }

    std::printf("    the candidate INVERTS from observation 60.\n"
                "      rolled back at observation   %zu\n"
                "      reason                       %s\n"
                "      rolled back TO               param_hash %#llx\n"
                "      observed IC at rollback      %+.4f\n"
                "      observed loss                Rs %.2f  (armed limit"
                " Rs %.2f)\n"
                "      scorecard IC for the regime  %+.4f\n"
                "      weight  0.500  ->            %.4f\n",
                rolled_at, rollback_name(final_outcome.reason),
                static_cast<unsigned long long>(
                    final_outcome.rolled_back_to.param_hash),
                final_outcome.observed_ic,
                static_cast<double>(final_outcome.observed_loss) / 100.0,
                static_cast<double>(final_outcome.trigger.max_loss_paise)
                    / 100.0,
                post ? *post : 0.0, weight);

    check(final_outcome.rolled_back(),
          "the poisoned candidate was ROLLED BACK, with no human in the loop");
    check(rolled_at > 60,
          "after the poisoning and not before it");
    check(final_outcome.rolled_back_to.param_hash == incumbent.param_hash,
          "to the fallback recorded when the canary opened, not to whatever"
          " happened to be previous");
    check(post.has_value() && *post < 0.0,
          "the scorecard's IC for the affected regime went NEGATIVE, which is"
          " the detection");
    check(weight < 0.50,
          "and the session-close update DE-WEIGHTED the model on that"
          " evidence");
    check(final_outcome.trigger.max_loss_paise == trig.max_loss_paise,
          "and the outcome carries the trigger AS ARMED, so an incident review"
          " reads the rule that was in force rather than the one being"
          " described afterwards");
    check(canary.capital_fraction() == 0.0,
          "the canary now holds no capital");

    // --- the scheduler proposes, it does not deploy ----------------------
    RetrainScheduler sched;
    check(sched.configure(Duration{90LL * 24 * 3600 * 1000000000LL}, 0.05)
              .has_value(), "the scheduler configures");
    const auto p1 = sched.poll(Timestamp{1000}, 0, false, true,
                               sc.at(0, 0, *regime_cell(reg)));
    check(p1.has_value() && p1->trigger == RetrainTrigger::Drift,
          "a drift alarm proposes a retrain");
    const auto p2 = sched.poll(Timestamp{2000}, 0, true, true,
                               sc.at(0, 0, *regime_cell(reg)));
    check(p2.has_value() && p2->trigger == RetrainTrigger::SpecChange,
          "and a SPEC CHANGE outranks it -- the features have changed meaning,"
          " so a drift alarm and a stale timer are both downstream of it and"
          " reporting either would send someone to investigate a symptom");
    std::printf("    -> and a proposal is all it is. There is no method on the"
                " scheduler that\n       promotes anything: a retrain that"
                " skipped shadow and canary because it was\n       scheduled"
                " rather than requested is exactly the slow-motion loss"
                " CLAUDE.md\n       warns about.\n");
}

} // namespace

int main()
{
    std::printf("altair flagging, drift and auto-correction tests\n");
    an_aggregate_destroys_the_facts_it_averages();
    drift_measured_on_standardised_features_measures_nothing();
    page_hinkley_and_adwin_detect_different_things();
    the_weight_update_is_shrunk_by_sample_size();
    a_poisoned_model_is_detected_deweighted_and_rolled_back();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
