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
    std::vector<da::AuditBar> part(cal.begin(), cal.end() - 5);   // ends Friday 24 October
    check(!ft::nifty_expiries(part).contains(da::audit_day(part.back().t)),
          "data that ends before a month's expiry does not invent one");
}

void test_daily() {
    const std::int64_t start = da::audit_days_from_civil(2025, 1, 1);
    const auto own = weekdays(start, 60, 100.0, 0.3);
    auto vix = weekdays(start, 60, 15.0, 0.01);
    vix.erase(vix.begin() + 40);   // one day without VIX
    ft::TrackInfo info;
    const auto tr = ft::build_daily({"T daily", "T", &own, &vix, nullptr, true, 1.3, nullptr}, info);
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
    const auto tr = ft::build_daily({"F daily", "F", &fut, &vix, &spot, true, 1.3, nullptr}, info);
    check(altair::curriculum_check_track(tr).has_value(), "the futures track passes the curriculum's checks");
    // June's expiry falls in the 20-day warm-up; July, August and September are rolls.
    check(info.roll_excluded == 3 && info.expiries == 4,
          "an outcome that crosses an expiry is not a decision; expiries counted in the contract's range");
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
    const auto tr = ft::build_hourly({"T hourly", "T", &own, &vix, true, 1.3, nullptr}, info);
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

void test_after_one_bar_session() {
    // Full day, a one-bar Muhurat session, then two full days.
    const std::int64_t d0 = da::audit_days_from_civil(2025, 10, 20);
    std::vector<da::AuditBar> own, vix;
    const int bars_per_day[] = {7, 1, 7, 7};
    for (int day = 0; day < 4; ++day) {
        const std::int64_t d = d0 + day;
        for (int k = 0; k < bars_per_day[day]; ++k) {
            const std::int64_t t = d * 86'400 + (555 + 60 * k) * 60;
            const double c = 200.0 + day + 0.1 * k;
            own.push_back(bar(t, c, c + 0.2, c - 0.2, c));
            vix.push_back(bar(t, 12.0, 12.1, 11.9, 12.0 + 0.01 * k));
        }
    }
    ft::TrackInfo info;
    const auto tr = ft::build_hourly({"M hourly", "M", &own, &vix, true, 1.3, nullptr}, info);
    check(tr.rows() == 12 && info.short_days == 1 && info.warmup == 7,
          "the day after a one-bar session is still forecast");
    check(std::fabs(tr.x[1] - std::log(own[7].c / own[6].c)) < 1e-12,
          "its previous-bar return is the one-bar session against the bar before it");
}

void test_vix_forecast_feature() {
    const std::int64_t start = da::audit_days_from_civil(2025, 1, 1);
    const auto own = weekdays(start, 40, 100.0, 0.3);
    const auto vix = weekdays(start, 40, 15.0, 0.01);
    ft::VixForecast fc;
    for (std::size_t k = 22; k < own.size(); ++k) { fc[da::audit_day(own[k].t)] = 0.01 * static_cast<double>(k); }
    ft::TrackInfo info;
    const auto tr = ft::build_daily({"T daily", "T", &own, &vix, nullptr, true, 1.3, &fc}, info);
    check(altair::curriculum_check_track(tr).has_value() && tr.feature_names.back() == "VIX forecast P(up)",
          "the VIX forecast is one more daily feature");
    check(info.no_vix_forecast == 2 && std::fabs(tr.x[tr.p - 1] - 0.22) < 1e-12,
          "a daily row reads the forecast made at its own close; rows before it exist are dropped");

    // Hourly: the forecast made at the previous close.
    const std::int64_t d0 = da::audit_days_from_civil(2025, 3, 3);
    std::vector<da::AuditBar> h, hv;
    for (int day = 0; day < 4; ++day) {
        for (int k = 0; k < 7; ++k) {
            const std::int64_t t = (d0 + day) * 86'400 + (555 + 60 * k) * 60;
            h.push_back(bar(t, 100, 100.2, 99.8, 100.0 + 0.1 * k + day));
            hv.push_back(bar(t, 15, 15.2, 14.8, 15.0 + 0.01 * k));
        }
    }
    ft::VixForecast hfc{{d0 + 1, 0.7}, {d0 + 2, 0.3}};
    ft::TrackInfo hinfo;
    const auto ht = ft::build_hourly({"T hourly", "T", &h, &hv, true, 1.3, &hfc}, hinfo);
    check(altair::curriculum_check_track(ht).has_value() && ht.rows() == 12,
          "hourly rows on days 2 and 3 carry a forecast");
    check(std::fabs(ht.x[ht.p - 1] - 0.7) < 1e-12 && std::fabs(ht.x[6 * ht.p + ht.p - 1] - 0.3) < 1e-12,
          "an hourly row reads the forecast made at the previous day's close, never its own day's");
}

void test_intraday() {
    // Five days of 5-minute bars: day 3 is short; everything else a full grid.
    const std::int64_t d0 = da::audit_days_from_civil(2025, 3, 3);
    std::vector<da::AuditBar> own, vix, pair;
    for (int day = 0; day < 5; ++day) {
        const int bars = day == 3 ? 40 : 75;
        for (int k = 0; k < bars; ++k) {
            const std::int64_t t = (d0 + day) * 86'400 + (555 + 5 * k) * 60;
            const double c = 100.0 + day + 0.01 * k + (k % 2 == 0 ? 0.03 : -0.02);
            own.push_back(bar(t, c - 0.01, c + 0.05, c - 0.05, c));
            vix.push_back(bar(t, 15.0, 15.1, 14.9, 15.0 + 0.001 * k));
            pair.push_back(bar(t, 2 * c, 2 * c, 2 * c, 2 * c));
        }
    }
    ft::TrackInfo info;
    const auto tr = ft::build_intraday({"T 5m", "T", 5, &own, &vix, true, 1.3, &pair, "P"}, info);
    check(altair::curriculum_check_track(tr).has_value(), "the 5-minute track passes the curriculum's checks");
    check(info.short_days == 1 && info.warmup == 150 && tr.rows() == 2 * 74 && tr.days() == 2,
          "74 decisions a full day (75 bars, none across the night); a short day and two history days skipped");
    check(tr.season == 74 && tr.slot[0] == 0 && tr.slot[73] == 73 && tr.slot[74] == 0,
          "each row knows its bar of the day");
    check(tr.t[0] == (d0 + 2) * 86'400 + (555 + 5) * 60 && tr.t_out[73] == (d0 + 2) * 86'400 + ft::kCloseSec,
          "decided at the bar's close; the 15:20 bar's outcome is the 15:30 close");
    check(tr.pair.size() == tr.rows() && std::fabs(tr.pair[0] - 2.0 * tr.anchor[0]) < 1e-12,
          "the pair's close at the same stamp rides along");
    check(std::fabs(tr.x[2] - std::log(own[150].c / own[149].c)) < 1e-12,
          "ret last hour at the first bar runs from yesterday's close");
}

void test_session() {
    // Nine days of 5-minute bars; day 7 is short.
    const std::int64_t d0 = da::audit_days_from_civil(2025, 3, 3);
    std::vector<da::AuditBar> own, vix;
    for (int day = 0; day < 9; ++day) {
        const int bars = day == 7 ? 40 : 75;
        for (int k = 0; k < bars; ++k) {
            const std::int64_t t = (d0 + day) * 86'400 + (555 + 5 * k) * 60;
            const double c = 100.0 + day + 0.01 * k + (k % 2 == 0 ? 0.03 : -0.02);
            own.push_back(bar(t, c - 0.01, c + 0.05, c - 0.05, c));
            vix.push_back(bar(t, 15.0, 15.1, 14.9, 15.0 + 0.001 * k));
        }
    }
    ft::TrackInfo info;
    const auto tr = ft::build_session({"T 10:15", "T", 615, &own, &vix, 1.3, nullptr, ""}, info);
    check(altair::curriculum_check_track(tr).has_value(), "the horizon track passes the curriculum's checks");
    check(tr.rows() == 2 && info.short_days == 1 && info.warmup == 6,
          "one decision a full day, after six days of history; the short day is skipped");
    const std::size_t day6 = 6 * 75;
    check(tr.t[0] == (d0 + 6) * 86'400 + 615 * 60 && tr.t_out[0] == (d0 + 6) * 86'400 + ft::kCloseSec,
          "decided at 10:15; the outcome is the 15:30 close, before the next day's decision");
    check(tr.anchor[0] == own[day6 + 11].c && tr.actual[0] == own[day6 + 74].c,
          "from the close of the bar that ends at 10:15 to the session's last close");
    check(std::fabs(tr.x[0] - std::log(own[day6 + 11].c / own[day6].o)) < 1e-12
              && std::fabs(tr.x[1] - std::log(own[day6].o / own[day6 - 1].c)) < 1e-12,
          "return since the open and the overnight gap, both known at 10:15");
    check(tr.cost_bp.size() == tr.rows() && tr.horizon == "10:15 to the close", "costed like a futures trade");

    // Live: today, unfinished, up to 10:30 -- 16 bars from 09:15.
    std::vector<da::AuditBar> own2 = own, vix2 = vix;
    for (int k = 0; k < 16; ++k) {
        const std::int64_t t = (d0 + 9) * 86'400 + (555 + 5 * k) * 60;
        const double c = 110.0 + 0.01 * k;
        own2.push_back(bar(t, c - 0.01, c + 0.05, c - 0.05, c));
        vix2.push_back(bar(t, 15.0, 15.1, 14.9, 15.0));
    }
    ft::TrackInfo i2;
    const auto off = ft::build_session({"T 10:15", "T", 615, &own2, &vix2, 1.3, nullptr, ""}, i2);
    check(off.rows() == 2 && !i2.partial_last, "without the live flag an unfinished day gets no row");
    ft::SessionInputs live{"T 10:15", "T", 615, &own2, &vix2, 1.3, nullptr, ""};
    live.partial_last_day = true;
    ft::TrackInfo i3;
    const auto on = ft::build_session(live, i3);
    check(on.rows() == 3 && i3.partial_last && altair::curriculum_check_track(on).has_value(),
          "with it, today's 10:15 row is built and the track still passes the checks");
    check(on.anchor[2] == own2[own.size() + 11].c && on.actual[2] == on.anchor[2]
              && on.t[2] == (d0 + 9) * 86'400 + 615 * 60,
          "today's row: decided at 10:15, its outcome a placeholder until the close");
    check(std::fabs(on.x[2 * on.p] - std::log(own2[own.size() + 11].c / own2[own.size()].o)) < 1e-12,
          "and its features are the same arithmetic as a finished day's");
    std::vector<da::AuditBar> early = own, vearly = vix;
    for (int k = 0; k < 8; ++k) {   // only to 09:55: the decision bar has not closed
        const std::int64_t t = (d0 + 9) * 86'400 + (555 + 5 * k) * 60;
        early.push_back(bar(t, 109.99, 110.05, 109.95, 110.0));
        vearly.push_back(bar(t, 15.0, 15.1, 14.9, 15.0));
    }
    ft::SessionInputs before{"T 10:15", "T", 615, &early, &vearly, 1.3, nullptr, ""};
    before.partial_last_day = true;
    ft::TrackInfo i4;
    check(ft::build_session(before, i4).rows() == 2 && !i4.partial_last, "before 10:15 today has no row");
}

} // namespace

int main() {
    std::printf("Forecast tracks\n");
    test_clean();
    test_expiries();
    test_daily();
    test_futures_roll();
    test_hourly();
    test_after_one_bar_session();
    test_vix_forecast_feature();
    test_intraday();
    test_session();
    std::printf("Forecast tracks: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
