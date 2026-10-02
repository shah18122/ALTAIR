// app/live_direction.hpp: the 10:15 direction models, live.
//   * fitted BEFORE the session, they make exactly the calls a fit at 10:15
//     would have made -- nothing is fitted inside the market-data path;
//   * a history that changed under the fit is refused, not traded;
//   * the gate is the conditional one: each model's walk-forward right and
//     wrong calls give its gain and loss, and the value carries an error.
//
// No check description here may contain the substring "F" "AIL" joined.

#include <app/live_direction.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) { ++failures; }
}

namespace da = altair::data_audit;
namespace ld = altair::live_direction;

da::AuditBar bar(std::int64_t t, double c, double prev) {
    da::AuditBar b;
    b.t = t;
    b.o = prev;
    b.c = c;
    b.h = std::max(prev, c) * 1.0004;
    b.l = std::min(prev, c) * 0.9996;
    return b;
}

/// `days` full sessions of 5-minute bars (75 a day), then `partial` bars of
/// the next one. A random walk with some persistence, and a VIX beside it.
void make_bars(int days, int partial, std::vector<da::AuditBar>& own, std::vector<da::AuditBar>& vix,
               std::vector<da::AuditBar>& own_today, std::vector<da::AuditBar>& vix_today) {
    std::mt19937_64 g(11);
    std::normal_distribution<double> z(0.0, 1.0);
    const std::int64_t d0 = da::audit_days_from_civil(2023, 1, 2);
    double px = 18000.0, v = 14.0, last = 0.0;
    for (int d = 0; d <= days; ++d) {
        const int n = d < days ? 75 : partial;
        const double gap = 0.002 * z(g);
        px *= std::exp(gap);
        for (int k = 0; k < n; ++k) {
            const std::int64_t t = (d0 + d) * 86'400 + (555 + 5 * k) * 60;
            const double step = 0.0006 * z(g) + 0.15 * last;
            last = step;
            const double prev = px;
            px *= std::exp(step);
            const double pv = v;
            v = std::max(8.0, v * std::exp(0.004 * z(g)));
            (d < days ? own : own_today).push_back(bar(t, px, prev));
            (d < days ? vix : vix_today).push_back(bar(t, v, pv));
        }
    }
}

bool same(double a, double b) { return (std::isnan(a) && std::isnan(b)) || std::memcmp(&a, &b, sizeof a) == 0; }

}  // namespace

int main() {
    using namespace altair;
    std::printf("live direction\n");
    std::vector<da::AuditBar> own, vix, own_today, vix_today;
    make_bars(320, 16, own, vix, own_today, vix_today);   // today: 09:15 to 10:30

    ld::DirectionShared s;
    s.nifty5 = own;
    s.vix5 = vix;
    ld::calibrate(s, 60);
    check(s.ok && s.fitted.size() == s.names.size() && s.fitted_rows > 200,
          "the walk-forward ran and the models were fitted before the session, on every finished day");
    if (!s.ok) {
        std::printf("    why: %s\n", s.why.c_str());
        return 1;
    }

    ld::decide_from_bars(s, own_today, vix_today);
    check(s.today_note.empty(), "today's 10:15 row is built from today's bars");
    std::printf("    the 10:15 decision took %.2f ms (the fit, done before the session, took %.2f s)\n", s.decide_ms, s.fit_seconds);

    // The reference: what a fit AT 10:15 -- the old way -- would have called.
    std::vector<da::AuditBar> nb = own, vb = vix;
    nb.insert(nb.end(), own_today.begin(), own_today.end());
    vb.insert(vb.end(), vix_today.begin(), vix_today.end());
    forecast_tracks::SessionInputs in{"NIFTY 10:15", "NIFTY", ld::kDecideMinute, &nb, &vb, s.other_cost_bp, nullptr, ""};
    in.partial_last_day = true;
    in.exit_minute = live::kLiveSquareOffMinute;
    forecast_tracks::TrackInfo info;
    const auto tr = forecast_tracks::build_session(in, info);
    const std::size_t i = tr.rows() - 1;
    CurriculumStage st;
    st.train_rows = i;
    st.test_begin = i;
    st.test_end = i + 1;
    const auto d = CurriculumDesign::make(tr, st);
    bool identical = d.has_value();
    std::size_t made = 0;
    auto fresh = ld::direction_models();
    for (std::size_t m = 0; m < fresh.size() && identical; ++m) {
        if (!fresh[m]->fit(*d).empty()) { identical = identical && !s.today_call[m].made; continue; }
        CurriculumCall c = fresh[m]->predict(*d, i);
        curriculum_detail::finish(c, *d);
        const CurriculumCall& p = s.today_call[m];
        identical = identical && c.made == p.made && c.dir == p.dir && same(c.p_up, p.p_up) && same(c.mu, p.mu)
                 && same(c.sigma, p.sigma);
        made += c.made ? 1u : 0u;
    }
    check(identical && made >= 3, "every pre-fitted model calls exactly what a fit at 10:15 would have: bit for bit");

    // The gate's inputs are today's.
    const auto pay = s.payoff[0].estimate();
    check(s.today_sigma_bp > 0.0 && std::isfinite(pay.gain) && std::isfinite(pay.loss)
              && pay.n_right + pay.n_wrong == static_cast<double>(s.scored[0]),
          "today's sigma is forecast, and each model's gain and loss come from all its scored calls");
    const GateValue gv = gate_value(s.cal[0].calibrated(ld::stated_q(s.today_call[0])),
                                    s.cal[0].calibrated_se(ld::stated_q(s.today_call[0])), pay, s.today_sigma_bp,
                                    s.today_cost_bp, s.gate_z);
    check(gv.se_bp > 0.0 && gv.lower_bp < gv.value_bp, "and the value it gates on carries its standard error");

    // A history that changed under the fit is refused.
    ld::DirectionShared moved = std::move(s);
    moved.nifty5.erase(moved.nifty5.begin(), moved.nifty5.begin() + 75);   // one session fewer
    moved.vix5.erase(moved.vix5.begin(), moved.vix5.begin() + 75);
    ld::decide_from_bars(moved, own_today, vix_today);
    bool none = true;
    for (const auto& c : moved.today_call) none = none && !c.made;
    check(!moved.today_note.empty() && none, "a history that no longer matches the fit is refused, not traded");

    std::printf("%s\n", failures == 0 ? "all live direction checks passed" : "live direction checks did not pass");
    return failures == 0 ? 0 : 1;
}
