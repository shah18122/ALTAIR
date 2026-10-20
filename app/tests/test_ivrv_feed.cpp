// Tests for app/ivrv_feed.hpp -- implied and realised volatility as inputs.

#include <app/ivrv_feed.hpp>

#include <analytics/greeks.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {

namespace iv = altair::ivrv;
namespace da = altair::data_audit;

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::printf("FAIL: %s\n", what); }
    else { std::printf("  ok  : %s\n", what); }
}

/// `days` weekdays of 75 five-minute bars, each bar's return from a fixed
/// pseudo-random sequence of size `scale`.
iv::Bars five_minute(int days, double scale, std::int64_t first_day = 20000) {
    iv::Bars v;
    double px = 100.0;
    std::uint64_t s = 99;
    int made = 0;
    for (std::int64_t d = first_day; made < days; ++d) {
        if (da::audit_weekday(d) > 4) { continue; }
        ++made;
        for (int k = 0; k < 75; ++k) {
            s = s * 6364136223846793005ULL + 1442695040888963407ULL;
            const double u = static_cast<double>(s >> 11) * (1.0 / 9007199254740992.0) - 0.5;
            da::AuditBar b;
            b.t = d * iv::kDaySec + 9 * 3600 + 15 * 60 + k * 300;
            b.o = px;
            px *= std::exp(scale * u);
            b.c = px;
            b.h = std::max(b.o, b.c);
            b.l = std::min(b.o, b.c);
            v.push_back(b);
        }
    }
    return v;
}

/// Feed rows stamped at or before `cut`, as (stamp, values).
std::vector<std::pair<std::int64_t, std::vector<float>>> rows_by(const altair::ModelFeed& f, std::int64_t cut) {
    std::vector<std::pair<std::int64_t, std::vector<float>>> out;
    for (std::size_t r = 0; r < f.rows(); ++r) {
        if (f.t[r] > cut) { break; }
        out.push_back({f.t[r], std::vector<float>(f.v.begin() + static_cast<std::ptrdiff_t>(r * f.cols()),
                                                  f.v.begin() + static_cast<std::ptrdiff_t>((r + 1) * f.cols()))});
    }
    return out;
}

void test_realised() {
    const auto bars = five_minute(60, 0.002);
    const auto s = iv::session_rv(bars);
    check(s.day.size() == 60, "one realised variance per session");
    // Session 1 by hand: its squared five-minute returns plus the gap from session 0.
    double want = 0.0;
    double last = bars[74].c;
    for (int k = 75; k < 150; ++k) {
        const double g = std::log(bars[static_cast<std::size_t>(k)].o / last);
        const double r = std::log(bars[static_cast<std::size_t>(k)].c / bars[static_cast<std::size_t>(k)].o);
        want += (k == 75 ? g * g : 0.0) + r * r;
        last = bars[static_cast<std::size_t>(k)].c;
    }
    check(std::fabs(s.rv[1] - want) < 1e-15, "a session's realised variance is its squared returns plus the overnight gap");

    const auto f = iv::rv_daily_feed("NIFTY", s, nullptr);
    bool stamped = f.rows() > 0;
    for (std::size_t r = 0; r < f.rows(); ++r) {
        stamped = stamped && f.t[r] % iv::kDaySec == iv::kCloseSec && (r == 0 || f.t[r] > f.t[r - 1]);
    }
    check(stamped, "daily realised inputs are stamped at the 15:30 close, in order");
    const auto g = iv::rv_intraday_feed("NIFTY", bars, s);
    bool ends = g.rows() > 0;
    for (std::size_t r = 0; r < g.rows(); ++r) { ends = ends && (r == 0 || g.t[r] > g.t[r - 1]); }
    check(ends && g.t.front() % 300 == 0, "intraday realised inputs are stamped at each bar's end, in order");
}

void test_no_lookahead() {
    const auto bars = five_minute(80, 0.002);
    const auto full_s = iv::session_rv(bars);
    const auto full_d = iv::rv_daily_feed("NIFTY", full_s, nullptr);
    const auto full_i = iv::rv_intraday_feed("NIFTY", bars, full_s);
    bool same = true;
    std::size_t compared = 0;
    // Cuts mid-session and at a close.
    for (const std::size_t at : {std::size_t{4000}, std::size_t{4874}, std::size_t{5555}}) {
        const std::int64_t cut = iv::bar_end(bars[at]);
        iv::Bars known(bars.begin(), bars.begin() + static_cast<std::ptrdiff_t>(at) + 1);
        iv::Bars other = bars;
        for (std::size_t k = at + 1; k < other.size(); ++k) {
            const double m = std::exp(0.03 * std::sin(1.3 * static_cast<double>(k)));
            other[k].o *= m; other[k].h *= m; other[k].l *= m; other[k].c *= m;
        }
        for (const auto* b : {&known, &other}) {
            const auto s = iv::session_rv(*b);
            const auto d = iv::rv_daily_feed("NIFTY", s, nullptr);
            const auto i = iv::rv_intraday_feed("NIFTY", *b, s);
            same = same && rows_by(d, cut) == rows_by(full_d, cut) && rows_by(i, cut) == rows_by(full_i, cut);
            compared += rows_by(d, cut).size() + rows_by(i, cut).size();
        }
    }
    std::printf("        %zu realised-vol rows compared at three cuts\n", compared);
    check(same && compared > 0, "realised inputs up to a cut are the same from the past alone and from another future");
}

void test_bhavcopy_iv() {
    // One day, two monthly expiries priced by Black-76 at 14 % and 18 %.
    const std::int64_t day = 20100;
    altair::bhavcopy::Day d;
    const double F = 20000.0;
    for (const auto& [dte, vol] : {std::pair{20, 0.14}, std::pair{48, 0.18}}) {
        const std::int64_t e = day + dte;
        d.fut[e] = F;
        for (const double K : {19900.0, 20000.0, 20100.0}) {
            altair::bhavcopy::Leg leg;
            for (const bool call : {true, false}) {
                const auto g = altair::black76(call ? altair::OptionRight::Call : altair::OptionRight::Put,
                                               altair::Price{std::llround(F * 100.0)}, altair::Price{std::llround(K * 100.0)},
                                               altair::Years{static_cast<double>(dte) / 365.0}, altair::Vol{vol}, 0.065);
                (call ? leg.call : leg.put) = g ? g->price / 100.0 : 0.0;
            }
            d.opt[e][K] = leg;
        }
    }
    const double near = iv::atm_iv(d, day, day + 20, 0.065);
    check(std::fabs(near - 0.14) < 2e-4, "at-the-money IV recovers the vol the options were priced at");
    std::map<std::int64_t, altair::bhavcopy::Day> days{{day, d}};
    const auto s = iv::iv_from_bhavcopy(days);
    // Total variance interpolated to 30 days between 20 and 48.
    const double w = (30.0 - 20.0) / 28.0;
    const double want = std::sqrt(((1 - w) * 0.14 * 0.14 * 20 + w * 0.18 * 0.18 * 48) / 30.0);
    check(s.size() == 1 && std::fabs(s[0].iv - want) < 2e-4, "the 30-day IV interpolates total variance between expiries");
    check(s.size() == 1 && s[0].t == day * iv::kDaySec + iv::kBhavKnownSec, "bhavcopy IV is stamped at 18:00, when NSE publishes it");
    // As of that day's 15:30 decision it is not there yet; the next day's is the first to read it.
    altair::ModelFeed f;
    f.t = {s[0].t};
    f.names = {"x"};
    f.v = {1.0F};
    f.intraday = false;
    check(altair::feed_detail::as_of(f, day * iv::kDaySec + iv::kCloseSec) == f.rows()
              && altair::feed_detail::as_of(f, (day + 1) * iv::kDaySec + iv::kCloseSec) == 0,
          "a 15:30 decision reads the previous evening's bhavcopy IV, never its own day's");
}

void test_recorded_iv() {
    const auto dir = std::filesystem::temp_directory_path() / "altair_ivrv_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "NIFTY");
    {
        std::ofstream o(dir / "NIFTY" / "2026-10-19.csv");
        o << "time,iv\n2026-10-19T09:20:00+05:30,0.131\n2026-10-19T09:21:00+05:30,0.129\nbad\n2026-10-19T09:22:00+05:30,-1\n";
    }
    const auto s = iv::load_recorded_iv(dir, "NIFTY");
    check(s.size() == 2 && std::fabs(s[0].iv - 0.131) < 1e-12 && iv::sessions_covered(s) == 1,
          "recorded FYERS IV is read minute by minute; a bad line and a non-positive IV are skipped");
    check(iv::load_recorded_iv(dir, "BANKNIFTY").empty(), "no recording, no series");
    std::filesystem::remove_all(dir);
}

} // namespace

int main() {
    std::printf("IV / RV inputs\n");
    test_realised();
    test_no_lookahead();
    test_bhavcopy_iv();
    test_recorded_iv();
    std::printf("IV / RV inputs: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
