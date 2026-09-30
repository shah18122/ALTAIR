// Tests for app/forecast_tracks.hpp -- dataset bars into curriculum tracks.

#include <app/forecast_tracks.hpp>

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

namespace ft = altair::forecast_tracks;
namespace da = altair::data_audit;

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::printf("FAIL: %s\n", what); }
    else { std::printf("  ok  : %s\n", what); }
}

da::AuditBar bar(std::int64_t t, double o, double h, double l, double c) {
    da::AuditBar b;
    b.t = t; b.o = o; b.h = h; b.l = l; b.c = c;
    return b;
}

/// `n` weekdays from `start`, daily bars, close drifting by `step` a day.
std::vector<da::AuditBar> weekdays(std::int64_t start, int n, double base, double step) {
    std::vector<da::AuditBar> v;
    for (std::int64_t d = start; static_cast<int>(v.size()) < n; ++d) {
        if (da::audit_weekday(d) > 4) continue;
        const double c = base + step * static_cast<double>(v.size()) + (v.size() % 3 == 0 ? 0.7 : -0.4);
        v.push_back(bar(d * 86'400, c - 0.2, c + 1.0, c - 1.0, c));
    }
    return v;
}

void test_clean() {
    ft::TrackInfo info;
    std::vector<da::AuditBar> v{bar(1, 10, 11, 9, 10.5), bar(2, 12, 11, 9, 10), bar(3, 10, 11, 9, 0),
                                bar(4, 10, 11, 9, 8.5)};
    ft::clean_bars(v, info);
    check(v.size() == 3 && info.bad_price == 1, "a zero close drops the bar");
    check(info.ohlc_repaired == 2 && v[1].h == 12.0 && v[2].l == 8.5 && v[2].c == 8.5,
          "open or close outside high-low widens the range, keeps the close, and is counted");
}

void test_expiries() {
    // August to October 2025 trading days, with Tuesday 30 September a holiday.
    std::vector<da::AuditBar> cal;
    const std::int64_t aug1 = da::audit_days_from_civil(2025, 8, 1);
    const std::int64_t sep30 = da::audit_days_from_civil(2025, 9, 30);
    for (std::int64_t d = aug1; d < da::audit_days_from_civil(2025, 11, 1); ++d) {
        if (da::audit_weekday(d) > 4 || d == sep30) continue;
        cal.push_back(bar(d * 86'400, 1, 1, 1, 1));
    }
    const auto e = ft::nifty_expiries(cal);
    check(e.size() == 3, "one expiry a month");
    check(e.contains(da::audit_days_from_civil(2025, 8, 28)), "August 2025: the last Thursday");
    check(e.contains(da::audit_days_from_civil(2025, 9, 29)), "September 2025: last Tuesday a holiday -> the Monday");
    check(e.contains(da::audit_days_from_civil(2025, 10, 28)), "October 2025: the last Tuesday");
}

void test_daily() {
    const std::int64_t start = da::audit_days_from_civil(2025, 1, 1);
    const auto own = weekdays(start, 60, 100.0, 0.3);
    auto vix = weekdays(start, 60, 15.0, 0.01);
    vix.erase(vix.begin() + 40);   // one day without VIX
    ft::TrackInfo info;
    const auto tr = ft::build_daily({"T daily", "T", &own, &vix, nullptr, true, 1.3}, info);
    check(altair::curriculum_check_track(tr).has_value(), "the daily track passes the curriculum's checks");
    // 60 bars: the last has no next day, 20 are warm-up, one lacks VIX, and
    // the day after the gap has VIX but only 19 VIX bars behind it... still >= 20.
    check(info.warmup == 20 && info.no_vix >= 1 && tr.rows() == 60 - 1 - 20 - info.no_vix,
          "warm-up, the missing VIX day and the last day are not decisions");
    check(std::fabs(tr.x[0] - std::log(own[20].c / own[19].c)) < 1e-12 && tr.anchor[0] == own[20].c
              && tr.actual[0] == own[21].c,
          "the first decision is day 20: its return is a feature, the next close the outcome");
    check(tr.t[0] == da::audit_day(own[20].t) * 86'400 + ft::kCloseSec && tr.t_out[0] == tr.t[1],
          "decided at 15:30, known at the next day's 15:30");
    check(tr.cost_bp.size() == tr.rows() && std::fabs(tr.cost_bp[0] - 3.3) < 1e-12, "2 bp STT + 1.3 bp before April 2026");
    check(std::fabs(ft::futures_cost_bp(da::audit_days_from_civil(2026, 4, 1), 1.3) - 6.3) < 1e-12,
          "5 bp STT from 2026-04-01");
}

void test_futures_roll() {
    const std::int64_t start = da::audit_days_from_civil(2025, 6, 2);
    const auto spot = weekdays(start, 90, 100.0, 0.1);
    auto fut = spot;
    const auto exp = ft::nifty_expiries(spot);
    // Basis 0.5 % that converges to zero on expiry and jumps back after it.
    for (auto& b : fut) {
        const std::int64_t d = da::audit_day(b.t);
        const double basis = exp.contains(d) ? 0.0 : 0.005;
        b.o *= 1 + basis; b.h *= 1 + basis; b.l *= 1 + basis; b.c *= 1 + basis;
    }
    const auto vix = weekdays(start, 90, 15.0, 0.0);
    ft::TrackInfo info;
    const auto tr = ft::build_daily({"F daily", "F", &fut, &vix, &spot, true, 1.3}, info);
    check(altair::curriculum_check_track(tr).has_value(), "the futures track passes the curriculum's checks");
    check(info.roll_excluded >= 3, "an outcome that crosses an expiry is not a decision");
    for (std::size_t i = 0; i < tr.rows(); ++i) {
        if (exp.contains(da::audit_day(tr.t[i]))) { check(false, "no decision on an expiry day"); break; }
    }
    check(std::fabs(info.basis_on_expiry_bp) < 1e-6 && info.basis_jump_after_bp > 40.0,
          "the calendar check: basis ~0 on expiry, jumps after");
    bool zero_after = true;
    const std::size_t basis_chg = tr.p - 1;
    for (std::size_t i = 0; i < tr.rows(); ++i) {
        const std::int64_t d = da::audit_day(tr.t[i]);
        std::int64_t prev = d - 1;
        while (da::audit_weekday(prev) > 4) --prev;
        if (exp.contains(prev) && tr.x[i * tr.p + basis_chg] != 0.0) zero_after = false;
    }
    check(zero_after, "the first day of a new contract carries no basis change");
}

void test_hourly() {
    const std::int64_t d0 = da::audit_days_from_civil(2025, 3, 3);   // a Monday
    std::vector<da::AuditBar> own, vix;
    for (int day = 0; day < 5; ++day) {
        const std::int64_t d = d0 + day;
        const int bars = day == 3 ? 4 : 7;   // Thursday is a short session
        for (int k = 0; k < bars; ++k) {
            const std::int64_t t = d * 86'400 + (555 + 60 * k) * 60;
            const double c = 100.0 + day + 0.1 * k + (k % 2 == 0 ? 0.05 : -0.03);
            own.push_back(bar(t, c - 0.02, c + 0.1, c - 0.1, c));
            if (!(day == 4 && k == 2)) vix.push_back(bar(t, 15.0, 15.2, 14.9, 15.0 + 0.01 * k + 0.1 * day));
        }
    }
    ft::TrackInfo info;
    const auto tr = ft::build_hourly({"T hourly", "T", &own, &vix, true, 1.3}, info);
    check(altair::curriculum_check_track(tr).has_value(), "the hourly track passes the curriculum's checks");
    check(info.short_days == 1 && info.warmup == 14, "a short session is excluded; two days are history only");
    // Wednesday: 6 decisions. Friday: 6, less the 11:15 VIX gap (its bar and the next need it).
    check(info.no_vix == 2 && tr.rows() == 10 && tr.days() == 2, "six decisions a full day; a VIX gap drops rows");
    check(tr.t[0] == (d0 + 2) * 86'400 + (555 + 60) * 60 && tr.t_out[5] == (d0 + 2) * 86'400 + ft::kCloseSec,
          "decided at the bar's close; the 14:15 bar's outcome is the 15:30 close");
    check(tr.anchor[0] == own[14].c && tr.actual[0] == own[15].c, "outcome: the next hourly close");
    check(std::fabs(tr.x[0] - std::log(own[14].c / own[13].c)) < 1e-12,
          "the first bar's return runs from yesterday's close");
}

} // namespace

int main() {
    std::printf("Forecast tracks\n");
    test_clean();
    test_expiries();
    test_daily();
    test_futures_roll();
    test_hourly();
    std::printf("Forecast tracks: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
