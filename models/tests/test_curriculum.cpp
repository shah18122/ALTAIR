// Tests for models/curriculum.hpp -- the doubling-window forecast curriculum.

#include <models/curriculum.hpp>

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

using namespace altair;

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::printf("FAIL: %s\n", what); }
    else { std::printf("  ok  : %s\n", what); }
}

/// A daily track whose returns follow r_t = phi * r_{t-1} + noise.
/// Features: the last realised return and a constant-free noise column.
CurriculumTrack ar_track(std::size_t days, double phi, double drift = 0.0, std::uint64_t seed = 7) {
    CurriculumTrack tr;
    tr.name = "synthetic";
    tr.instrument = "SYN";
    tr.horizon = "next day";
    tr.p = 2;
    tr.feature_names = {"r1", "noise"};
    tr.seq_cols = {0, 1};
    std::uint64_t s = seed;
    const auto gauss = [&s]() {
        double u = 0.0;
        for (int k = 0; k < 12; ++k) {
            s = curriculum_detail::mix(s);
            u += static_cast<double>(s >> 11) * (1.0 / 9007199254740992.0);
        }
        return u - 6.0;
    };
    // days + 1 returns: r[0] is the history before the first decision.
    std::vector<double> r(days + 1);
    r[0] = 0.0;
    for (std::size_t k = 1; k <= days; ++k) { r[k] = drift + phi * r[k - 1] + 0.01 * gauss(); }
    double price = 100.0;
    for (std::size_t i = 0; i < days; ++i) {
        tr.x.push_back(r[i]);            // today's return: known at today's close
        tr.x.push_back(gauss());
        tr.t.push_back(static_cast<std::int64_t>(i) * 86'400 + 55'800);
        tr.t_out.push_back(static_cast<std::int64_t>(i + 1) * 86'400 + 55'800);
        tr.day.push_back(static_cast<std::int32_t>(i));
        tr.anchor.push_back(price);
        price *= std::exp(r[i + 1]);     // tomorrow's move is the outcome
        tr.actual.push_back(price);
        tr.cost_bp.push_back(2.0);
    }
    return tr;
}

void test_schedule() {
    auto tr = ar_track(100, 0.0);
    // Two rows a day: the schedule counts DAYS, not rows.
    CurriculumTrack two = tr;
    two.x.clear(); two.t.clear(); two.t_out.clear(); two.day.clear(); two.anchor.clear(); two.actual.clear();
    two.cost_bp.clear();
    for (std::size_t i = 0; i < 100; ++i) {
        for (int h = 0; h < 2; ++h) {
            two.x.push_back(0.001 * static_cast<double>(i % 7));
            two.x.push_back(static_cast<double>(h));
            two.t.push_back(static_cast<std::int64_t>(i) * 86'400 + 36'000 + h * 3600);
            two.t_out.push_back(two.t.back() + 3600);
            two.day.push_back(static_cast<std::int32_t>(i));
            two.anchor.push_back(100.0);
            two.actual.push_back(100.0 + (h == 0 ? 1.0 : -1.0));
            two.cost_bp.push_back(2.0);
        }
    }
    const auto st = curriculum_stages(two, 3);
    check(st.size() == 6, "3,6,12,24,48,96 days: six stages over 100 days");
    check(st[0].train_days == 3 && st[0].test_days == 3 && st[0].train_rows == 6 && st[0].test_end == 12,
          "stage 0 learns 3 days (6 rows) and forecasts the next 3");
    check(st[4].train_days == 48 && st[4].test_days == 48, "stage 4 learns 48 and forecasts 48");
    check(st[5].train_days == 96 && st[5].test_days == 4 && st[5].test_end == two.rows(),
          "the last block is what is left");
    const auto capped = curriculum_stages(two, 3, 20);
    // 3, 6, 12, 24, then +20: 44, 64, 84, and the last 16 days.
    check(capped.size() == 7 && capped[3].train_days == 24 && capped[3].test_days == 20
              && capped[4].train_days == 44 && capped.back().test_days == 16
              && capped.back().test_end == two.rows(),
          "a step cap turns doubling into fixed blocks once reached");
    check(curriculum_stages(ar_track(3, 0.0), 3).empty(), "three days cannot learn three and forecast one");
}

void test_track_checks() {
    auto tr = ar_track(50, 0.0);
    check(curriculum_check_track(tr).has_value(), "a well-formed track passes");
    auto late = tr;
    late.t_out[10] = late.t[11] + 1;
    check(curriculum_check_track(late).error() == CurriculumError::LookAhead,
          "an outcome known after the next decision is refused as look-ahead");
    auto bad = tr;
    bad.anchor[3] = 0.0;
    check(curriculum_check_track(bad).error() == CurriculumError::BadTrack, "a zero price is refused");
    auto wide = tr;
    wide.p = 17;
    check(!curriculum_check_track(wide).has_value(), "a ragged or over-wide track is refused");
    auto nocost = tr;
    nocost.cost_bp.pop_back();
    check(curriculum_check_track(nocost).error() == CurriculumError::BadTrack, "a tradable track needs a cost per row");
}

/// Tries to read the future every way the design allows.
class Cheat final : public CurriculumModel {
public:
    std::string name() const override { return "Cheat"; }
    std::string family() const override { return "test"; }
    std::string fit(const CurriculumDesign& d) override {
        (void)d.target(d.train_rows());            // first test target
        (void)d.features(d.train_rows() + 1);      // a row after now
        (void)d.history(d.train_rows() + 2);
        (void)d.block(0, d.train_rows() + 1);
        return {};
    }
    CurriculumCall predict(const CurriculumDesign& d, std::size_t i) const override {
        const double peek = d.level(i + 1);        // tomorrow's price
        return curriculum_detail::from_return(std::isfinite(peek) ? peek - d.level(i) : 0.0, 0.01);
    }
};

void test_guards() {
    auto tr = ar_track(64, 0.0);
    std::vector<std::unique_ptr<CurriculumModel>> cheat;
    cheat.push_back(std::make_unique<Cheat>());
    const auto run = curriculum_run(tr, cheat);
    check(run && run->lookahead_refusals > 0, "every future read is refused and counted");
    // The refused peek returns NaN, so the cheat can never be right by peeking.
    const auto s = curriculum_summary(tr, *run, 0, 1);
    check(s.all.scored() > 0 && s.accuracy < 0.8, "the cheat gains nothing");
}

void test_learning_and_no_leak() {
    // Strong persistence: yesterday's sign predicts today's.
    auto tr = ar_track(400, 0.7);
    auto models = curriculum_default_models();
    const auto run = curriculum_run(tr, models);
    check(run.has_value(), "the full model set runs on a synthetic track");
    if (!run) { return; }
    check(run->lookahead_refusals == 0, "no model reads the future");
    const auto idx = [&](const char* name) {
        for (std::size_t m = 0; m < run->models.size(); ++m) { if (run->models[m] == name) return m; }
        return run->models.size();
    };
    const std::size_t tests = run->models.size();
    const auto logit = curriculum_summary(tr, *run, idx("Logistic regression"), tests);
    const auto mom = curriculum_summary(tr, *run, idx("Momentum"), tests);
    const auto rev = curriculum_summary(tr, *run, idx("Mean reversion"), tests);
    const auto coin = curriculum_summary(tr, *run, idx("Coin flip"), tests);
    const auto ar = curriculum_summary(tr, *run, idx("AR(2)"), tests);
    std::printf("        logistic %.3f  momentum %.3f  reversion %.3f  coin %.3f  AR(2) %.3f\n",
                logit.accuracy, mom.accuracy, rev.accuracy, coin.accuracy, ar.accuracy);
    check(mom.accuracy > 0.65 && rev.accuracy < 0.35, "momentum finds the persistence, reversion the opposite");
    check(logit.accuracy > 0.65 && logit.p_adjusted < 0.05, "logistic learns it, significantly");
    check(ar.accuracy > 0.65 && ar.have_price && ar.price.skill > 0.0, "AR(2) learns it and beats the random walk");
    check(coin.accuracy > 0.4 && coin.accuracy < 0.6 && coin.p_adjusted > 0.05, "the coin is a coin");

    const std::size_t champ = idx("Champion"), hedge = idx("Hedge");
    const auto c0 = curriculum_tally(tr, *run, champ, run->stages[0].test_begin, run->stages[0].test_end);
    check(c0.forecasts == 0 && run->champion[0] == -1, "stage 0 has no track record: Champion abstains");
    const int last = run->champion.back();
    check(last >= 0 && run->families[static_cast<std::size_t>(last)] != "baseline"
              ? true : (last >= 0 && run->models[static_cast<std::size_t>(last)] == "Momentum"),
          "the champion is a model that found the pattern");
    double wsum = 0.0;
    for (const double w : run->hedge_weight.back()) { wsum += w; }
    check(std::fabs(wsum - 1.0) < 1e-9, "Hedge weights sum to one");
    const auto h = curriculum_summary(tr, *run, hedge, tests);
    check(h.accuracy > 0.65, "Hedge follows the models that are right");

    // The rest of the Atlas: the learners that can see the persistence must find it.
    for (const char* name : {"Ridge regression", "VAR(1)", "Seasonal AR (SARIMA)", "Momentum (tuned lookback)"}) {
        const auto s = curriculum_summary(tr, *run, idx(name), tests);
        std::printf("        %-28s %.3f on %zu\n", name, s.accuracy, s.all.scored());
        check(s.all.scored() > 100 && s.accuracy > 0.62, name);
    }
    for (const char* name : {"Decision tree", "CNN (random kernels)", "Autoencoder + logistic", "Kalman filter (drift)",
                             "Hidden Markov model", "DQN (reinforcement)"}) {
        const auto s = curriculum_summary(tr, *run, idx(name), tests);
        std::printf("        %-28s %.3f on %zu\n", name, s.accuracy, s.all.scored());
        check(s.all.scored() > 100, name);
    }
    {
        const auto z = curriculum_summary(tr, *run, idx("Mean reversion (z-score band)"), tests);
        check(z.all.forecasts < (tr.rows() - run->first_row) / 2, "the z-score band abstains inside its band");
        const auto pairs = curriculum_summary(tr, *run, idx("Pairs (cointegration)"), tests);
        check(pairs.all.forecasts == 0 && run->notes[idx("Pairs (cointegration)")].back().find("no paired") != std::string::npos,
              "the pairs model abstains, and says why, on a track with no pair");
    }

    const auto stack = curriculum_summary(tr, *run, idx("Stack"), tests);
    const auto conf = curriculum_summary(tr, *run, idx("Stack (confident third)"), tests);
    const auto cons = curriculum_summary(tr, *run, idx("Consensus 75%"), tests);
    std::printf("        stack %.3f (%zu)  confident %.3f (%zu)  consensus %.3f (%zu)\n", stack.accuracy,
                stack.all.scored(), conf.accuracy, conf.all.scored(), cons.accuracy, cons.all.scored());
    check(stack.accuracy > 0.65, "Stack learns whom to trust from the models' past calls");
    check(run->notes[idx("Stack")][0].find("fewer than") != std::string::npos
              && stack.all.forecasts < tr.rows() - run->first_row,
          "Stack abstains until it has 60 past calls to learn from");
    const double conf_share = static_cast<double>(conf.all.forecasts) / static_cast<double>(stack.all.forecasts);
    check(conf_share > 0.15 && conf_share < 0.6 && conf.accuracy >= stack.accuracy - 0.02,
          "the confident third calls on roughly a third of the Stack's rows, no less accurately");
    check(cons.all.forecasts < tr.rows() - run->first_row && cons.accuracy > 0.65,
          "Consensus calls only when the models agree, and is right when they do");

    // Changing one outcome must not change any call made before it was known.
    auto moved = tr;
    const std::size_t j = run->stages[5].test_begin + 3;
    moved.actual[j] = moved.anchor[j] * (moved.actual[j] > moved.anchor[j] ? 0.97 : 1.03);
    auto models2 = curriculum_default_models();
    const auto run2 = curriculum_run(moved, models2);
    bool same = run2.has_value();
    for (std::size_t m = 0; same && m < run->models.size(); ++m) {
        for (std::size_t i = run->first_row; i <= j; ++i) {
            const CurriculumCall a = run->calls[m][i - run->first_row];
            const CurriculumCall b = run2->calls[m][i - run2->first_row];
            if (a.made != b.made || a.dir != b.dir || (a.made && a.mu != b.mu)) { same = false; break; }
        }
    }
    check(same, "an outcome cannot change any call made before it was known");
}

/// Two prices tied by a mean-reverting spread: this leg reverts toward the pair.
void test_pairs() {
    CurriculumTrack tr = ar_track(600, 0.0, 0.0, 17);
    std::uint64_t st = 99;
    const auto gauss = [&st]() {
        double u = 0.0;
        for (int k = 0; k < 12; ++k) {
            st = curriculum_detail::mix(st);
            u += static_cast<double>(st >> 11) * (1.0 / 9007199254740992.0);
        }
        return u - 6.0;
    };
    double pair = 100.0, spread = 0.0, price = 0.0;
    for (std::size_t i = 0; i < tr.rows(); ++i) {
        pair *= std::exp(0.01 * gauss());
        spread = 0.5 * spread + 0.02 * gauss();          // strongly mean-reverting
        price = std::log(pair) + spread;
        tr.pair.push_back(pair);
        tr.anchor[i] = std::exp(price);
        // Tomorrow: the pair drifts, the spread halves -- this leg moves back toward the pair.
        tr.actual[i] = std::exp(price - 0.5 * spread + 0.003 * gauss());
    }
    std::vector<std::unique_ptr<CurriculumModel>> m;
    m.push_back(std::make_unique<CurriculumPairs>());
    const auto run = curriculum_run(tr, m);
    check(run.has_value() && run->lookahead_refusals == 0, "the pairs model runs with no look-ahead");
    if (!run) { return; }
    const auto s = curriculum_summary(tr, *run, 0, 1);
    std::printf("        pairs: %.3f on %zu calls\n", s.accuracy, s.all.scored());
    check(s.all.scored() > 20 && s.accuracy > 0.7, "beyond two sigma the leg reverts toward its pair");
}

void test_scoring() {
    auto tr = ar_track(40, 0.0, 0.0, 11);
    CurriculumRun run;
    run.first_row = 0;
    run.models = {"m"};
    run.calls.assign(1, std::vector<CurriculumStoredCall>(tr.rows()));
    std::size_t expect_right = 0, expect_wrong = 0;
    for (std::size_t i = 0; i < tr.rows(); ++i) {
        CurriculumCall c = curriculum_detail::from_probability(i % 3 == 0 ? 0.3 : 0.7, 0.001);
        run.calls[0][i] = c;
        const double r = tr.ret(i);
        if (r == 0.0) { continue; }
        ((c.dir > 0) == (r > 0.0) ? expect_right : expect_wrong) += 1;
    }
    const auto tie_up = curriculum_detail::from_probability(0.5, 0.002);
    const auto tie_down = curriculum_detail::from_probability(0.5, -0.002);
    const auto tie_none = curriculum_detail::from_probability(0.5);
    check(tie_up.dir == 1 && tie_down.dir == -1 && tie_none.made && tie_none.dir == 0,
          "an exact 0.5 is broken by the expected return; with none it has no direction");
    const auto t = curriculum_tally(tr, run, 0, 0, tr.rows());
    check(t.right == expect_right && t.wrong == expect_wrong && t.brier_n == t.scored(),
          "right and wrong are counted against the realised direction");
    // 10 bp expected move clears a 2 bp cost: every call trades.
    check(t.trades == tr.rows(), "a call whose expected move clears the cost trades");
    {
        double sum = 0.0, sq = 0.0;
        for (std::size_t i = 0; i < tr.rows(); ++i) {
            const double net = curriculum_trade_bp(tr, run.calls[0][i], i);
            sum += net; sq += net * net;
        }
        const double n = static_cast<double>(tr.rows()), mean = sum / n;
        const double want = mean / std::sqrt((sq - n * mean * mean) / (n - 1.0) / n);
        check(std::fabs(t.net_t() - want) < 1e-9, "net t-stat is the mean net bp over its standard error");
    }
    CurriculumTrack vix = tr;
    vix.tradable = false;
    vix.cost_bp.clear();
    check(curriculum_tally(vix, run, 0, 0, tr.rows()).trades == 0, "an untradable index never trades");
    CurriculumRun none = run;
    for (auto& c : none.calls[0]) { c = curriculum_detail::from_direction(0); }
    const auto tn = curriculum_tally(tr, none, 0, 0, tr.rows());
    check(tn.right == 0 && tn.wrong == tn.scored() && tn.no_direction == tn.scored(),
          "a call with no direction is wrong, and counted as such");

    // Wilson and the binomial test on a known record: 60 of 100.
    CurriculumRun known;
    known.first_row = 0;
    auto flat = ar_track(100, 0.0, 0.0, 3);
    known.calls.assign(1, std::vector<CurriculumStoredCall>(flat.rows()));
    std::size_t made_right = 0;
    for (std::size_t i = 0; i < flat.rows(); ++i) {
        const int out = flat.ret(i) > 0.0 ? 1 : -1;
        const int dir = made_right < 60 ? out : -out;
        if (dir == out) { ++made_right; }
        known.calls[0][i] = curriculum_detail::from_direction(dir);
        known.calls[0][i].mu = 0.0F;
        known.calls[0][i].sigma = 0.01F;
    }
    const auto s = curriculum_summary(flat, known, 0, 10);
    check(std::fabs(s.accuracy - 0.60) < 1e-12 && std::fabs(s.z_vs_half - 2.0) < 1e-9,
          "60 of 100 is two standard errors above a coin");
    check(std::fabs(s.p_vs_half - 0.0455) < 1e-3 && std::fabs(s.p_adjusted - 0.455) < 1e-2,
          "two-sided p and its Bonferroni correction over ten tests");
    check(std::fabs(s.lo95 - 0.5020) < 1e-3 && std::fabs(s.hi95 - 0.6906) < 1e-3, "Wilson 95 % interval");
    // Up-rate of these bars sets the best constant call; the edge over it is
    // corrected for the ten tests too.
    const double zc = (0.60 - s.best_constant) / 0.05;
    check(std::fabs(s.z_vs_constant - zc) < 1e-9
              && std::fabs(s.p_constant_adjusted - std::min(1.0, 5.0 * std::erfc(zc / std::sqrt(2.0)))) < 1e-9,
          "the edge over the best constant call is Bonferroni-corrected as well");
}

} // namespace

int main() {
    std::printf("Forecast curriculum\n");
    test_schedule();
    test_track_checks();
    test_guards();
    test_scoring();
    test_pairs();
    test_learning_and_no_leak();
    std::printf("Forecast curriculum: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
