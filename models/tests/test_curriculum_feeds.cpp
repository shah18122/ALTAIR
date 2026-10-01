// Tests for models/curriculum_feeds.hpp -- other models' forecasts as inputs.

#include <models/curriculum_feeds.hpp>

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

constexpr std::int64_t kDay = 86'400;

/// Daily rows: r_t = phi * r_{t-1} + noise; features: the last return and noise.
CurriculumTrack ar_track(std::size_t days, double phi, std::uint64_t seed) {
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
    std::vector<double> r(days + 1, 0.0);
    for (std::size_t k = 1; k <= days; ++k) { r[k] = phi * r[k - 1] + 0.01 * gauss(); }
    double price = 100.0;
    for (std::size_t i = 0; i < days; ++i) {
        tr.x.push_back(r[i]);
        tr.x.push_back(gauss());
        tr.t.push_back(static_cast<std::int64_t>(i) * kDay + 55'800);
        tr.t_out.push_back(static_cast<std::int64_t>(i + 1) * kDay + 55'800);
        tr.day.push_back(static_cast<std::int32_t>(i));
        tr.slot.push_back(static_cast<std::uint16_t>(i % 5));
        tr.anchor.push_back(price);
        price *= std::exp(r[i + 1]);
        tr.actual.push_back(price);
        tr.cost_bp.push_back(2.0);
    }
    tr.season = 5;
    return tr;
}

void test_as_of() {
    ModelFeed f;
    f.source = "src";
    f.names = {"a"};
    f.t = {10, 20, 30, kDay + 5};
    f.v = {1.0f, 2.0f, 3.0f, 4.0f};
    check(feed_detail::as_of(f, 25) == 1, "a row reads the latest value made at or before it");
    check(feed_detail::as_of(f, 20) == 1, "a value made at the decision itself is known at it");
    check(feed_detail::as_of(f, 5) == f.rows(), "nothing before the feed starts");
    check(feed_detail::as_of(f, kDay + 1) == f.rows(), "an intraday value is not carried into the next day");
    f.intraday = false;
    check(feed_detail::as_of(f, kDay + 1) == 2, "a daily value is");
    check(feed_detail::as_of(f, 9 * kDay) == f.rows(), "but not when it is more than a week old");
}

void test_attach() {
    const auto base = ar_track(20, 0.3, 3);
    ModelFeed req;
    req.source = "req";
    req.names = {"x"};
    req.intraday = false;
    req.required = true;
    for (std::size_t i = 5; i < 20; ++i) { req.t.push_back(base.t[i]); req.v.push_back(static_cast<float>(i)); }
    ModelFeed opt;
    opt.source = "opt";
    opt.names = {"y", "z"};
    opt.intraday = false;
    for (std::size_t i = 10; i < 20; ++i) {
        opt.t.push_back(base.t[i]);
        opt.v.push_back(1.0f);
        opt.v.push_back(-1.0f);
    }
    const ModelFeed* feeds[] = {&req, &opt};
    FeedAttachInfo info;
    const auto tr = curriculum_attach_feeds(base, feeds, "synthetic + feeds", base.t[7], info);
    check(tr.has_value(), "the attached track passes the curriculum's checks");
    if (!tr) { return; }
    check(tr->p == 5 && tr->feature_names[2] == "req: x" && tr->feature_names[4] == "opt: z",
          "the feeds' columns are appended and named by source");
    check(tr->rows() == 13 && info.before_start == 7 && tr->day.front() == 0 && tr->day.back() == 12,
          "rows before the start are dropped and days counted from zero again");
    check(tr->x[2] == 7.0 && tr->x[3] == 0.0 && info.gaps == 3,
          "an optional feed's gap reads 0 (no opinion) and is counted");
    check(tr->x[(12) * 5 + 2] == 19.0 && tr->x[12 * 5 + 3] == 1.0, "later rows read their own as-of values");
}

/// Pass 1, its forecasts as pass 2's inputs, pass 2 -- end to end.
struct TwoPass {
    CurriculumTrack second;
    CurriculumRun run;
};

std::expected<TwoPass, CurriculumError> two_pass(const CurriculumTrack& base) {
    auto m1 = curriculum_default_models();
    const auto r1 = curriculum_run(base, m1);
    if (!r1) { return std::unexpected(r1.error()); }
    auto b1 = band_default_models();
    const auto br = band_run(base, b1);
    if (!br) { return std::unexpected(br.error()); }
    ModelFeed models = feed_from_models(base, *r1, {}, false);
    models.required = true;
    ModelFeed fields = feed_from_fields(base, *r1, {{"Hidden Markov model", FeedField::LogSigma},
                                                    {"Kalman filter (drift)", FeedField::Mu}}, false);
    ModelFeed bands = feed_from_bands(base, *br, {"GJR-GARCH", "Vol ensemble"}, false);
    const ModelFeed* feeds[] = {&models, &fields, &bands};
    FeedAttachInfo info;
    auto second = curriculum_attach_feeds(base, feeds, base.name + " + models", base.t[r1->first_row], info);
    if (!second) { return std::unexpected(second.error()); }
    auto m2 = curriculum_default_models();
    auto r2 = curriculum_run(*second, m2);
    if (!r2) { return std::unexpected(r2.error()); }
    return TwoPass{std::move(*second), std::move(*r2)};
}

void test_two_pass_no_leak() {
    const auto base = ar_track(400, 0.5, 9);
    const auto a = two_pass(base);
    check(a.has_value() && a->run.lookahead_refusals == 0, "two passes run, and the second reads no future row");
    if (!a) { return; }
    check(a->second.p > 30, "the second pass sees every model's forecast");
    // Change one outcome in the middle: no second-pass call made before it
    // was known may move -- through the first pass's forecasts or otherwise.
    const std::size_t j = base.rows() / 2;
    auto moved = base;
    moved.actual[j] = moved.anchor[j] * (moved.actual[j] > moved.anchor[j] ? 0.97 : 1.03);
    const auto b = two_pass(moved);
    check(b.has_value(), "the perturbed track runs too");
    if (!b) { return; }
    bool same = a->run.first_row == b->run.first_row && a->second.rows() == b->second.rows();
    std::size_t compared = 0;
    for (std::size_t m = 0; same && m < a->run.models.size(); ++m) {
        for (std::size_t i = a->run.first_row; i < a->second.rows() && a->second.t[i] <= base.t[j]; ++i) {
            const CurriculumCall x = a->run.calls[m][i - a->run.first_row];
            const CurriculumCall y = b->run.calls[m][i - b->run.first_row];
            const bool eq = x.made == y.made && x.dir == y.dir
                && ((std::isnan(x.p_up) && std::isnan(y.p_up)) || x.p_up == y.p_up);
            if (!eq) { same = false; break; }
            ++compared;
        }
    }
    std::printf("        compared %zu second-pass calls made before the changed outcome\n", compared);
    check(same && compared > 1000, "an outcome cannot change any second-pass call made before it was known");
}

void test_feed_carries_information() {
    // Features are noise; a feed column made at each decision says the next
    // move's sign right 70 % of the time. The second pass must find it.
    auto base = ar_track(600, 0.0, 21);
    ModelFeed hint;
    hint.source = "hint";
    hint.names = {"sign"};
    hint.intraday = false;
    std::uint64_t s = 77;
    for (std::size_t i = 0; i < base.rows(); ++i) {
        s = curriculum_detail::mix(s);
        const bool truthful = (s >> 11) % 10 < 7;
        const double up = base.actual[i] > base.anchor[i] ? 1.0 : -1.0;
        hint.t.push_back(base.t[i]);
        hint.v.push_back(static_cast<float>(truthful ? up : -up));
    }
    const ModelFeed* feeds[] = {&hint};
    FeedAttachInfo info;
    const auto tr = curriculum_attach_feeds(base, feeds, "noise + hint", 0, info);
    std::vector<std::unique_ptr<CurriculumModel>> models;
    models.push_back(std::make_unique<CurriculumLogistic>());
    models.push_back(std::make_unique<CurriculumGbdt>(5));
    const auto run = curriculum_run(*tr, models);
    check(run.has_value(), "the hinted track runs");
    if (!run) { return; }
    const auto lg = curriculum_summary(*tr, *run, 0, run->models.size());
    const auto gb = curriculum_summary(*tr, *run, 1, run->models.size());
    std::printf("        logistic %.3f  gradient boosting %.3f (the hint is right 70 %%)\n", lg.accuracy, gb.accuracy);
    check(lg.accuracy > 0.65 && gb.accuracy > 0.6, "a model reads the feed it is given (boosting is held back by its 3-day first windows)");
}

} // namespace

int main() {
    std::printf("Curriculum feeds\n");
    test_as_of();
    test_attach();
    test_feed_carries_information();
    test_two_pass_no_leak();
    std::printf("Curriculum feeds: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
