// Tests for app/forecast_leak_check.hpp -- the leak check passes the real
// builders and models, and catches every planted leak, including one the
// curriculum's own look-ahead counter cannot see.

#include <app/forecast_leak_check.hpp>

#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <vector>

namespace {

namespace da = altair::data_audit;
namespace ft = altair::forecast_tracks;
namespace lk = altair::forecast_leak;
using altair::CurriculumTrack;

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::printf("FAIL: %s\n", what); }
    else { std::printf("  ok  : %s\n", what); }
}

struct Rng {
    std::uint64_t s;
    double gauss() {
        double u = 0.0;
        for (int k = 0; k < 12; ++k) {
            s = s * 6364136223846793005ull + 1442695040888963407ull;
            u += static_cast<double>(s >> 11) * (1.0 / 9007199254740992.0);
        }
        return u - 6.0;
    }
};

da::AuditBar bar(std::int64_t t, double o, double c, double wiggle) {
    da::AuditBar b;
    b.t = t;
    b.o = o;
    b.c = c;
    b.h = std::max(o, c) * (1.0 + wiggle);
    b.l = std::min(o, c) * (1.0 - wiggle);
    return b;
}

/// `days` weekdays of daily bars, a random walk from `level`.
lk::Bars daily(std::int64_t start, int days, double level, double vol, std::uint64_t seed) {
    Rng r{seed};
    lk::Bars v;
    double c = level;
    std::int64_t d = start;
    for (int k = 0; k < days; ++d) {
        if (da::audit_weekday(d) > 4) { continue; }
        const double o = c * std::exp(0.2 * vol * r.gauss());
        c = o * std::exp(vol * r.gauss());
        v.push_back(bar(d * 86'400, o, c, 0.3 * vol));
        ++k;
    }
    return v;
}

/// Intraday bars of `tf` minutes from 09:15, `days` weekdays; day `short_day`
/// stops after `short_bars` bars (a feed that broke, a halted market).
lk::Bars intraday(std::int64_t start, int days, int tf, double level, double vol, std::uint64_t seed, int short_day,
                  int short_bars) {
    Rng r{seed};
    lk::Bars v;
    double c = level;
    const int per_day = (375 + tf - 1) / tf;
    std::int64_t d = start;
    for (int k = 0; k < days; ++d) {
        if (da::audit_weekday(d) > 4) { continue; }
        const int n = k == short_day ? short_bars : per_day;
        for (int b = 0; b < n; ++b) {
            const double o = c;
            c = o * std::exp(vol * r.gauss());
            v.push_back(bar(d * 86'400 + (555 + tf * b) * 60, o, c, 0.3 * vol));
        }
        ++k;
    }
    return v;
}

const std::int64_t kStart = da::audit_days_from_civil(2025, 1, 6);   // a Monday

/// Every cut, both checks clean, and something compared.
void expect_clean(const lk::Build& build, const std::vector<std::int64_t>& cuts, const char* what) {
    const CurriculumTrack full = build(lk::identity());
    bool clean = full.rows() > 0;
    std::size_t compared = 0;
    for (const std::int64_t cut : cuts) {
        const auto c = lk::check_builder(build, full, cut);
        clean = clean && c.truncated.clean() && c.perturbed.clean();
        compared += c.truncated.compared + c.perturbed.compared;
        if (!c.truncated.clean()) { std::printf("        truncated: %s\n", c.truncated.first.c_str()); }
        if (!c.perturbed.clean()) { std::printf("        perturbed: %s\n", c.perturbed.first.c_str()); }
    }
    check(clean && compared > 0, what);
}

void test_builders_clean() {
    std::printf("[1] every builder: the same rows from the past whatever the future\n");
    const auto own = daily(kStart, 120, 22000.0, 0.01, 1), vix = daily(kStart, 120, 14.0, 0.04, 2),
               pair = daily(kStart, 120, 48000.0, 0.012, 3);
    ft::VixForecast fc;
    for (const auto& b : vix) { fc[da::audit_day(b.t)] = 0.5 + 0.3 * std::sin(static_cast<double>(b.t % 97)); }
    const auto day_cut = [&](int k) { return da::audit_day(own[static_cast<std::size_t>(k)].t) * ft::kDaySec + ft::kCloseSec; };
    expect_clean(
        [&](const lk::Transform& t) {
            const auto o = t.bars(own, da::kDailyTf), v = t.bars(vix, da::kDailyTf), p = t.bars(pair, da::kDailyTf);
            const auto f = t.vix_forecast(fc);
            ft::TrackInfo info;
            return ft::build_daily({"D", "D", &o, &v, nullptr, true, 1.3, &f, &p, "P"}, info);
        },
        {day_cut(40), day_cut(80), day_cut(119)}, "daily, with the VIX forecast and the pair: clean at three cuts");

    const auto h = intraday(kStart, 30, 60, 22000.0, 0.003, 4, 12, 4), hv = intraday(kStart, 30, 60, 14.0, 0.01, 5, 12, 4);
    const std::int64_t d12 = da::audit_day(h[12 * 7].t);   // the short day's first bar is at 12 * 7
    expect_clean(
        [&](const lk::Transform& t) {
            const auto o = t.bars(h, 60), v = t.bars(hv, 60);
            const auto f = t.vix_forecast(fc);
            ft::TrackInfo info;
            return ft::build_hourly({"H", "H", &o, &v, true, 1.3, &f}, info);
        },
        {d12 * ft::kDaySec + (555 + 60) * 60, d12 * ft::kDaySec + 11 * 3600 + 40 * 60, (d12 + 7) * ft::kDaySec + 13 * 3600 + 15 * 60},
        "hourly, a short day among them: clean at mid-session cuts");

    const auto m5 = intraday(kStart, 12, 5, 22000.0, 0.001, 6, 8, 40), v5 = intraday(kStart, 12, 5, 14.0, 0.004, 7, 8, 40),
               p5 = intraday(kStart, 12, 5, 48000.0, 0.0012, 8, 8, 40);
    const std::int64_t d8 = da::audit_day(m5[8 * 75].t);
    expect_clean(
        [&](const lk::Transform& t) {
            const auto o = t.bars(m5, 5), v = t.bars(v5, 5), p = t.bars(p5, 5);
            ft::TrackInfo info;
            return ft::build_intraday({"M", "M", 5, &o, &v, true, 1.3, &p, "P"}, info);
        },
        {d8 * ft::kDaySec + 10 * 3600, d8 * ft::kDaySec + 10 * 3600 + 2 * 60, (d8 + 3) * ft::kDaySec + 14 * 3600 + 35 * 60},
        "5-minute, a day cut off at 12:35: clean, including inside a bar");

    const auto s5 = intraday(kStart, 20, 5, 22000.0, 0.001, 9, 14, 40), sv = intraday(kStart, 20, 5, 14.0, 0.004, 10, 14, 40);
    std::vector<std::int64_t> cuts;
    for (const int k : {9, 14, 18}) {
        const std::int64_t d = da::audit_day(s5[static_cast<std::size_t>(k) * 75].t);
        cuts.push_back(d * ft::kDaySec + 10 * 3600 + 15 * 60);   // at the 10:15 decision
        cuts.push_back(d * ft::kDaySec + 13 * 3600);             // between it and its outcome
    }
    expect_clean(
        [&](const lk::Transform& t) {
            const auto o = t.bars(s5, 5), v = t.bars(sv, 5);
            ft::TrackInfo info;
            return ft::build_session({"S", "S", 615, &o, &v, 1.3, nullptr, ""}, info);
        },
        cuts, "10:15 to the close: clean at the decision and between it and its outcome");
}

void test_builders_caught() {
    std::printf("[2] planted leaks in a builder are caught\n");
    const auto own = daily(kStart, 120, 22000.0, 0.01, 11), vix = daily(kStart, 120, 14.0, 0.04, 12);
    const auto base = [&](const lk::Transform& t) {
        const auto o = t.bars(own, da::kDailyTf), v = t.bars(vix, da::kDailyTf);
        ft::TrackInfo info;
        return ft::build_daily({"D", "D", &o, &v, nullptr, true, 1.3, nullptr}, info);
    };
    const std::int64_t cut = da::audit_day(own[70].t) * ft::kDaySec + ft::kCloseSec;

    // Door 1: a feature that is the row's own outcome.
    const lk::Build outcome_feature = [&](const lk::Transform& t) {
        CurriculumTrack tr = base(t);
        for (std::size_t i = 0; i < tr.rows(); ++i) { tr.x[i * tr.p] = std::log(tr.actual[i] / tr.anchor[i]); }
        return tr;
    };
    auto c = lk::check_builder(outcome_feature, outcome_feature(lk::identity()), cut);
    check(!c.perturbed.clean(), "a feature built from the next close: caught (another future moves it)");

    // Door 2: a feature standardised over the whole sample.
    const lk::Build whole_sample = [&](const lk::Transform& t) {
        CurriculumTrack tr = base(t);
        double s = 0.0;
        for (std::size_t i = 0; i < tr.rows(); ++i) { s += tr.x[i * tr.p + 4]; }
        const double mean = s / static_cast<double>(tr.rows());
        for (std::size_t i = 0; i < tr.rows(); ++i) { tr.x[i * tr.p + 4] -= mean; }
        return tr;
    };
    c = lk::check_builder(whole_sample, whole_sample(lk::identity()), cut);
    check(!c.truncated.clean() && !c.perturbed.clean(), "a whole-sample mean: caught both ways");

    // Door 4: days chosen by how they turned out (the old full-grid rule).
    const auto h = intraday(kStart, 20, 60, 22000.0, 0.003, 13, -1, 0), hv = intraday(kStart, 20, 60, 14.0, 0.01, 14, -1, 0);
    const lk::Build full_days_only = [&](const lk::Transform& t) {
        const auto o = t.bars(h, 60), v = t.bars(hv, 60);
        ft::TrackInfo info;
        CurriculumTrack tr = ft::build_hourly({"H", "H", &o, &v, true, 1.3, nullptr}, info);
        // Keep a day's rows only if all seven of its bars are in the data.
        std::map<std::int64_t, int> bars_on;
        for (const auto& b : o) { ++bars_on[da::audit_day(b.t)]; }
        CurriculumTrack kept = tr;
        kept.x.clear(); kept.t.clear(); kept.t_out.clear(); kept.day.clear(); kept.anchor.clear();
        kept.actual.clear(); kept.cost_bp.clear(); kept.slot.clear(); kept.pair.clear();
        for (std::size_t i = 0; i < tr.rows(); ++i) {
            if (bars_on[da::audit_day(tr.t[i])] < 7) { continue; }
            kept.x.insert(kept.x.end(), tr.x.begin() + static_cast<std::ptrdiff_t>(i * tr.p),
                          tr.x.begin() + static_cast<std::ptrdiff_t>((i + 1) * tr.p));
            kept.t.push_back(tr.t[i]); kept.t_out.push_back(tr.t_out[i]); kept.day.push_back(tr.day[i]);
            kept.anchor.push_back(tr.anchor[i]); kept.actual.push_back(tr.actual[i]); kept.cost_bp.push_back(tr.cost_bp[i]);
            kept.slot.push_back(tr.slot[i]);
        }
        return kept;
    };
    const std::int64_t mid = da::audit_day(h[10 * 7].t) * ft::kDaySec + 12 * 3600 + 15 * 60;
    c = lk::check_builder(full_days_only, full_days_only(lk::identity()), mid);
    check(!c.truncated.clean(), "keeping only days that turned out complete: caught at a mid-session cut");
}

/// A model that holds the track itself and calls each row by its own outcome:
/// the guarded reads never see it, so the look-ahead counter reads zero.
class Peek final : public altair::CurriculumModel {
public:
    explicit Peek(const CurriculumTrack* tr) : tr_(tr) {}
    std::string name() const override { return "Peek"; }
    std::string family() const override { return "test"; }
    std::string fit(const altair::CurriculumDesign&) override { return {}; }
    altair::CurriculumCall predict(const altair::CurriculumDesign&, std::size_t i) const override {
        return altair::curriculum_detail::from_return(std::log(tr_->actual[i] / tr_->anchor[i]), 0.01);
    }

private:
    const CurriculumTrack* tr_;
};

/// A daily track of i.i.d. returns: nothing in it can be forecast.
CurriculumTrack random_walk(std::size_t days, std::uint64_t seed) {
    CurriculumTrack tr;
    tr.name = "random walk";
    tr.instrument = "RW";
    tr.horizon = "next day";
    tr.p = 3;
    tr.feature_names = {"r1", "r2", "r5"};
    tr.seq_cols = {0, 1};
    Rng r{seed};
    std::vector<double> ret(days + 6);
    for (double& x : ret) { x = 0.01 * r.gauss(); }
    double price = 100.0;
    for (std::size_t i = 0; i < days; ++i) {
        const std::size_t k = i + 5;
        tr.x.push_back(ret[k]);
        tr.x.push_back(ret[k - 1]);
        tr.x.push_back(ret[k] + ret[k - 1] + ret[k - 2] + ret[k - 3] + ret[k - 4]);
        tr.t.push_back(static_cast<std::int64_t>(i) * 86'400 + 55'800);
        tr.t_out.push_back(static_cast<std::int64_t>(i + 1) * 86'400 + 55'800);
        tr.day.push_back(static_cast<std::int32_t>(i));
        tr.anchor.push_back(price);
        price *= std::exp(ret[k + 1]);
        tr.actual.push_back(price);
        tr.cost_bp.push_back(2.0);
    }
    return tr;
}

void test_models() {
    std::printf("[3] every model and ensemble: the same calls whatever comes next\n");
    const CurriculumTrack tr = random_walk(420, 21);
    auto models = altair::curriculum_default_models();
    const altair::CurriculumOptions opt;
    const auto run = altair::curriculum_run(tr, models, opt);
    check(run.has_value(), "the full model set runs");
    if (!run) { return; }
    auto bm = altair::band_default_models();
    const auto bands = altair::band_run(tr, bm, opt);
    check(bands.has_value(), "the band models run");
    const auto cuts = lk::cut_rows(*run);
    check(cuts.size() == 2, "cuts in the third- and second-to-last stages");
    bool clean = true;
    std::size_t calls = 0, bands_n = 0, refusals = 0;
    for (const std::size_t cut : cuts) {
        const auto c = lk::check_models(tr, *run, bands ? &*bands : nullptr, cut, opt);
        clean = clean && c.ran && c.calls.clean() && c.bands.clean();
        calls += c.calls.compared;
        bands_n += c.bands.compared;
        refusals += c.refusals;
        if (!c.calls.clean()) { std::printf("        calls: %s\n", c.calls.first.c_str()); }
        if (!c.bands.clean()) { std::printf("        bands: %s\n", c.bands.first.c_str()); }
    }
    std::printf("        %zu calls and %zu bands compared\n", calls, bands_n);
    check(clean && calls > 0 && bands_n > 0 && refusals == 0,
          "every call and band up to each cut is bit for bit the same with the cut row's outcome changed");

    // Placebo: on a random walk no model beats a coin once the tests are counted.
    const std::size_t tests = run->models.size();
    bool none = true;
    for (std::size_t m = 0; m < run->models.size(); ++m) {
        const auto s = altair::curriculum_summary(tr, *run, m, tests);
        if (s.all.scored() >= 100 && s.p_adjusted < 0.01 && s.accuracy > 0.5) {
            none = false;
            std::printf("        %s: %.1f %% on a random walk\n", run->models[m].c_str(), 100.0 * s.accuracy);
        }
    }
    check(none, "placebo: on a random walk no model beats a coin (Bonferroni over every model)");

    std::printf("[4] a model the look-ahead counter cannot see is caught\n");
    const lk::MakeModels peek = [](const CurriculumTrack& t) {
        std::vector<std::unique_ptr<altair::CurriculumModel>> v;
        v.push_back(std::make_unique<Peek>(&t));
        return v;
    };
    auto pm = peek(tr);
    const auto prun = altair::curriculum_run(tr, pm, opt);
    check(prun && prun->lookahead_refusals == 0, "the peeking model reads no guarded future: the counter says 0");
    if (!prun) { return; }
    const auto pc = lk::check_models(tr, *prun, nullptr, lk::cut_rows(*prun).front(), opt, peek);
    check(pc.ran && !pc.calls.clean(), "the cut check catches it: its call moves when the row's own outcome does");
}

} // namespace

int main() {
    test_builders_clean();
    test_builders_caught();
    test_models();
    std::printf("\nforecast leak check: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
