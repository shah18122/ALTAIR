// app/magnitude_main.cpp -- altair_magnitude.
//
// Direction is half a forecast (models/magnitude.hpp). Over the forecast
// curriculum's per-forecast logs, for every model on the daily and horizon
// tracks:
//   * magnitude-aware scoring: accuracy against magnitude-weighted accuracy,
//     |move| on right calls against wrong ones, gross edge in bp;
//   * the value gate: take a call only when (2q - 1) x E|r| beats the cost,
//     with q the model's own probability calibrated walk-forward and E|r| from
//     a HAR forecast of that day's variance (5-minute realised variance for
//     the same window: close-to-close for daily tracks, the decision to the
//     close for 09:20 / 10:15 tracks). Nothing in either is known late: the
//     calibration learns an outcome only after it has happened, and HAR is
//     fitted on days before the call.
//
// Run altair_forecast_curriculum first (it writes forecast_log/). Writes
// <out>/magnitude/summary.csv; prints each track's best gated models and the
// t a result needs to survive the number of tests.

#include <analytics/har_rv.hpp>
#include <app/forecast_tracks.hpp>
#include <models/magnitude.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace ft = altair::forecast_tracks;
namespace da = altair::data_audit;

void usage(const char* exe) {
    std::printf(
        "  Direction is half a forecast: score the magnitude, and gate calls on what they earn.\n\n"
        "    %s [--logs DIR] [--dataset DIR] [--out DIR] [--cost-bp C] [--with-feeds]\n\n"
        "    --logs DIR      the curriculum's forecast_log/ (default data/verified/forecast_log)\n"
        "    --cost-bp C     round-trip cost a call must beat (default 6.3: today's STT and charges)\n"
        "    --with-feeds    also the second-pass tracks (+ vol / cross / models / all)\n", exe);
}

std::string fixed(double v, int d) {
    if (!std::isfinite(v)) { return {}; }
    char b[64];
    std::snprintf(b, sizeof b, "%.*f", d, v);
    return b;
}

/// Realised variance per day for one window of the session: daily
/// (close to close: the overnight gap and every bar), or from `start_minute`
/// (the close of the bar ending then) to the last bar.
std::map<std::int64_t, double> window_rv(const std::vector<da::AuditBar>& bars, std::int64_t start_minute) {
    std::map<std::int64_t, double> out;
    double prev_close = 0.0;
    std::size_t k = 0;
    while (k < bars.size()) {
        const std::int64_t day = ft::bar_day(bars[k]);
        std::size_t e = k;
        while (e < bars.size() && ft::bar_day(bars[e]) == day) { ++e; }
        if (e - k >= 60) {
            std::vector<double> closes;
            double base = 0.0;
            if (start_minute < 0) {
                for (std::size_t j = k; j < e; ++j) { closes.push_back(bars[j].c); }
                const double rv = altair::session_rv(closes, bars[k].o, prev_close);
                if (rv > 0.0) { out[day] = rv; }
            } else {
                for (std::size_t j = k; j < e; ++j) {
                    const auto m = da::audit_minute_of_day(bars[j].t);
                    if (m + 5 == start_minute) { base = bars[j].c; }   // the bar that closes at the decision
                    else if (m >= start_minute) { closes.push_back(bars[j].c); }
                }
                if (base > 0.0 && !closes.empty()) {
                    const double rv = altair::session_rv(closes, base, 0.0);
                    if (rv > 0.0) { out[day] = rv; }
                }
            }
        }
        prev_close = bars[e - 1].c;
        k = e;
    }
    return out;
}

struct Row {
    std::int64_t day = 0;
    int dir = 0;
    double p_up = std::nan("");
    double r = 0.0;
};

} // namespace

int main(int argc, char** argv) {
    fs::path logs = "data/verified/forecast_log", root = "dataset", out = "data/verified";
    double cost = 6.3;
    bool with_feeds = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a{argv[i]};
        const bool has = i + 1 < argc;
        if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
        if (a == "--logs" && has) { logs = argv[++i]; continue; }
        if (a == "--dataset" && has) { root = argv[++i]; continue; }
        if (a == "--out" && has) { out = argv[++i]; continue; }
        if (a == "--cost-bp" && has) { cost = std::atof(argv[++i]); if (!(cost >= 0) || cost > 200) { usage(argv[0]); return 2; } continue; }
        if (a == "--with-feeds") { with_feeds = true; continue; }
        usage(argv[0]);
        return 2;
    }
    struct Track { std::string file, instrument; std::int64_t start_minute; };   // -1: daily close to close
    std::vector<Track> tracks;
    for (const std::string base : {"nifty_daily", "banknifty_daily", "nifty_fut_daily", "nifty_0920_close", "banknifty_0920_close",
                                   "nifty_1015_close", "banknifty_1015_close"}) {
        const std::string inst = base.rfind("banknifty", 0) == 0 ? "banknifty" : "nifty";
        const std::int64_t start = base.find("0920") != std::string::npos ? 560 : base.find("1015") != std::string::npos ? 615 : -1;
        tracks.push_back({base, inst, start});
        if (with_feeds) {
            for (const char* f : {"_vol", "_cross", "_models", "_all"}) { tracks.push_back({base + f, inst, start}); }
        }
    }

    std::map<std::string, std::vector<da::AuditBar>> bars;
    const auto load5 = [&](const std::string& inst) -> const std::vector<da::AuditBar>& {
        auto it = bars.find(inst);
        if (it != bars.end()) { return it->second; }
        ft::TrackInfo info;
        auto v = ft::load_bars(root / "spot" / inst / "5m", 5, info);
        ft::clean_bars(v, info);
        return bars.emplace(inst, std::move(v)).first->second;
    };

    const fs::path dir = out / "magnitude";
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::ofstream f(dir / "summary.csv", std::ios::trunc);
    f << "track,model,calls,accuracy,weighted_accuracy,abs_move_right_bp,abs_move_wrong_bp,payoff_ratio,gross_bp,net_bp_all,"
         "gated_calls,gated_share,gated_accuracy,gated_net_bp,gated_t\n";
    std::size_t tests = 0;
    struct Best { std::string track, model; double net, t; std::size_t n; double acc; };
    std::vector<Best> best;
    std::printf("Magnitude: cost %.1f bp a round trip\n", cost);

    for (const Track& tr : tracks) {
        const fs::path file = logs / (tr.file + ".csv");
        std::ifstream in(file);
        if (!in) { continue; }
        const auto& b5 = load5(tr.instrument);
        if (b5.empty()) { std::printf("  %s: no 5-minute data for %s\n", tr.file.c_str(), tr.instrument.c_str()); continue; }
        const auto rvmap = window_rv(b5, tr.start_minute);
        std::vector<std::int64_t> rv_day;
        std::vector<double> rv;
        for (const auto& [d, v] : rvmap) { rv_day.push_back(d); rv.push_back(v); }
        // E|r| in bp for a call made on `day`: daily tracks forecast tomorrow
        // from today's close; horizon tracks forecast today from yesterday's.
        std::map<std::int64_t, double> eabs_cache;
        const auto eabs = [&](std::int64_t day) {
            if (const auto c = eabs_cache.find(day); c != eabs_cache.end()) { return c->second; }
            double v = std::nan("");
            auto it = std::upper_bound(rv_day.begin(), rv_day.end(), day);   // first day after `day`
            std::ptrdiff_t t = (it - rv_day.begin()) - 1;                    // last day <= day
            if (tr.start_minute >= 0 && t >= 0 && rv_day[static_cast<std::size_t>(t)] == day) { --t; }   // today is not known at 09:20
            if (t >= 0) {
                const auto fc = altair::har_forecast(rv, static_cast<std::size_t>(t), 1, 1000);
                if (fc) { v = 1e4 * altair::expected_abs_move(std::sqrt(fc->variance)); }
            }
            eabs_cache[day] = v;
            return v;
        };

        std::map<std::string, std::vector<Row>> by_model;
        std::string line;
        std::getline(in, line);
        while (std::getline(in, line)) {
            // time,stage,learned_days,"model",last_price,next_price,forecast_price,p_up,call,moved,result,net_bp
            const auto q1 = line.find('"'), q2 = line.find('"', q1 + 1);
            if (q1 == std::string::npos || q2 == std::string::npos) { continue; }
            std::vector<std::string> c;
            std::stringstream ss(line.substr(q2 + 2));
            for (std::string x; std::getline(ss, x, ',');) { c.push_back(x); }
            if (c.size() < 5) { continue; }
            const double last = std::atof(c[0].c_str()), next = std::atof(c[1].c_str());
            if (!(last > 0.0) || !(next > 0.0)) { continue; }
            Row r;
            const int y = std::atoi(line.substr(0, 4).c_str()), mo = std::atoi(line.substr(5, 2).c_str()), d = std::atoi(line.substr(8, 2).c_str());
            r.day = da::audit_days_from_civil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d));
            r.dir = c[4] == "UP" ? 1 : (c[4] == "DOWN" ? -1 : 0);
            if (!c[3].empty()) { r.p_up = std::atof(c[3].c_str()); }
            r.r = std::log(next / last);
            by_model[line.substr(q1 + 1, q2 - q1 - 1)].push_back(r);
        }
        std::printf("  %-28s %zu models\n", tr.file.c_str(), by_model.size());
        for (const auto& [model, rows] : by_model) {
            std::vector<altair::MagCall> all;
            altair::WalkForwardCalibrator cal;
            std::size_t gn = 0, gright = 0, gdecided = 0;
            double gsum = 0, gss = 0, net_all = 0;
            std::size_t calls = 0;
            for (const Row& r : rows) {
                if (r.dir == 0) { continue; }
                ++calls;
                all.push_back({r.dir, r.r});
                net_all += static_cast<double>(r.dir) * r.r * 1e4 - cost;
                const double q = std::isfinite(r.p_up) ? (r.dir > 0 ? r.p_up : 1.0 - r.p_up) : std::nan("");
                const double qc = cal.calibrated(q);
                const double e = eabs(r.day);
                if (std::isfinite(e) && altair::call_value_bp(qc, e, cost) > 0.0) {
                    const double net = static_cast<double>(r.dir) * r.r * 1e4 - cost;
                    ++gn;
                    gsum += net;
                    gss += net * net;
                    if (r.r != 0.0) { ++gdecided; gright += (r.r > 0.0) == (r.dir > 0) ? 1 : 0; }
                }
                if (r.r != 0.0) { cal.add(q, (r.r > 0.0) == (r.dir > 0)); }   // known only after the outcome
            }
            const auto s = altair::magnitude_stats(all);
            const double gmean = gn > 0 ? gsum / static_cast<double>(gn) : std::nan("");
            const double gsd = gn > 1 ? std::sqrt(std::max(0.0, gss / static_cast<double>(gn) - gmean * gmean)) : std::nan("");
            const double gt = gn > 1 && gsd > 0 ? gmean / (gsd / std::sqrt(static_cast<double>(gn))) : std::nan("");
            const double gacc = gdecided > 0 ? static_cast<double>(gright) / static_cast<double>(gdecided) : std::nan("");
            if (gn >= 30) {
                ++tests;
                best.push_back({tr.file, model, gmean, gt, gn, gacc});
            }
            f << tr.file << ",\"" << model << "\"," << calls << ',' << fixed(s.accuracy, 4) << ',' << fixed(s.weighted_accuracy, 4) << ','
              << fixed(s.mean_abs_right_bp, 2) << ',' << fixed(s.mean_abs_wrong_bp, 2) << ',' << fixed(s.payoff_ratio, 3) << ','
              << fixed(s.gross_bp, 2) << ',' << fixed(calls > 0 ? net_all / static_cast<double>(calls) : std::nan(""), 2) << ',' << gn
              << ',' << fixed(calls > 0 ? static_cast<double>(gn) / static_cast<double>(calls) : std::nan(""), 4) << ','
              << fixed(gacc, 4) << ',' << fixed(gmean, 2) << ',' << fixed(gt, 2) << '\n';
        }
    }
    if (best.empty()) {
        std::printf("  no forecast logs under %s -- run altair_forecast_curriculum first\n", logs.string().c_str());
        return 1;
    }
    // The t a gated result needs to survive Bonferroni over every test (two-sided 5 %).
    const double z = [&] {
        const double alpha = 0.05 / static_cast<double>(tests);
        double lo = 0, hi = 10;   // invert the normal tail by bisection
        for (int k = 0; k < 100; ++k) {
            const double mid = 0.5 * (lo + hi);
            (0.5 * std::erfc(mid / std::sqrt(2.0)) * 2.0 > alpha ? lo : hi) = mid;
        }
        return hi;
    }();
    std::sort(best.begin(), best.end(), [](const Best& a, const Best& b) { return a.t > b.t; });
    std::printf("\n  %zu gated model-track tests; a result needs t > %.2f to survive them all\n", tests, z);
    std::printf("  %-28s %-30s %7s %7s %10s %7s\n", "track", "model", "gated", "acc %", "net bp/tr", "t");
    for (std::size_t k = 0; k < std::min<std::size_t>(15, best.size()); ++k) {
        const auto& b = best[k];
        std::printf("  %-28s %-30s %7zu %7.1f %10.2f %7.2f%s\n", b.track.c_str(), b.model.substr(0, 30).c_str(), b.n, 100 * b.acc, b.net,
                    b.t, b.t > z ? "  SURVIVES" : "");
    }
    std::printf("  wrote %s\n", (dir / "summary.csv").string().c_str());
    return 0;
}
