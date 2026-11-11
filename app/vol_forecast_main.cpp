// app/vol_forecast_main.cpp -- altair_vol_forecast: how big the next move is,
// on every timeframe, on FYERS history, judged on RMSE and MAE.
//
// For NIFTY and BANKNIFTY (spot and continuous futures) on 1m, 5m, 15m, 60m
// and 1d, every model in models/vol_forecast.hpp is walked forward over the
// whole history (FYERS 1m from 2018/2019, daily from 2005), and scored
// against the random walk and the 22-session average. A model is called
// better only when its Diebold-Mariano t clears the Bonferroni bar for every
// test in the run AND it wins in each third of the scored period.
//
// History: the newest FYERS snapshot, else data/store/fyers (never dataset/
// unless named; docs/DATA_POLICY.md). Writes <out>/vol_forecast/summary.csv.

#include <app/fyers_source.hpp>
#include <app/forecast_tracks.hpp>
#include <models/vol_forecast.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace ft = altair::forecast_tracks;
namespace da = altair::data_audit;
namespace vf = altair::vol_forecast;

void usage(const char* exe) {
    std::printf(
        "  Forecast the size of the next move on every timeframe; RMSE / MAE against a random walk.\n\n"
        "    %s [--dataset DIR] [--out DIR] [--only TEXT] [--burn N]\n\n"
        "    --dataset DIR   history (default: the newest FYERS snapshot, else data/store/fyers)\n"
        "    --out DIR       default data/verified\n"
        "    --only TEXT     tracks whose name contains TEXT (e.g. NIFTY, 5m)\n"
        "    --burn N        sessions before scoring starts (default 120)\n", exe);
}

/// The |t| a two-sided test at 5 % needs after `m` tests (Bonferroni).
double bonferroni_t(std::size_t m) {
    const double p = 0.05 / static_cast<double>(std::max<std::size_t>(1, m));
    double lo = 0.0, hi = 10.0;
    for (int k = 0; k < 80; ++k) {
        const double mid = 0.5 * (lo + hi);
        (std::erfc(mid / std::sqrt(2.0)) > p ? lo : hi) = mid;
    }
    return hi;
}

struct Track { std::string name, dir; int tf; };

} // namespace

int main(int argc, char** argv) {
    fs::path root, out = "data/verified";
    std::string only;
    vf::Options base;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a{argv[i]};
        const bool has = i + 1 < argc;
        if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
        if (a == "--dataset" && has) { root = argv[++i]; continue; }
        if (a == "--out" && has) { out = argv[++i]; continue; }
        if (a == "--only" && has) { only = argv[++i]; continue; }
        if (a == "--burn" && has) { base.burn_sessions = static_cast<std::size_t>(std::max(5L, std::atol(argv[++i]))); continue; }
        usage(argv[0]);
        return 2;
    }
    root = altair::app::pick_history(root, "altair_vol_forecast");
    if (root.empty()) return 2;

    // INDIA VIX daily close, known at 15:30 IST of its day.
    std::vector<vf::Known> vix;
    {
        ft::TrackInfo info;
        auto b = ft::load_bars(root / "spot/indiavix/1d", da::kDailyTf, info);
        ft::clean_bars(b, info);
        for (const auto& x : b) vix.push_back({da::audit_day(x.t) * vf::kVfDaySec + 15 * 3600 + 30 * 60, x.c});
    }

    std::vector<Track> tracks;
    for (const auto& [inst, seg, dir] : {std::tuple<std::string, std::string, std::string>{"NIFTY", "spot", "spot/nifty"},
                                         {"BANKNIFTY", "spot", "spot/banknifty"},
                                         {"NIFTY FUT", "fut", "fut/nifty"},
                                         {"BANKNIFTY FUT", "fut", "fut/banknifty"}}) {
        (void)seg;
        for (const auto& [tf, tfn] : {std::pair<int, std::string>{1, "1m"}, {5, "5m"}, {15, "15m"}, {60, "60m"}, {da::kDailyTf, "1d"}}) {
            const std::string name = inst + " " + tfn;
            if (!only.empty() && name.find(only) == std::string::npos) continue;
            tracks.push_back({name, dir + "/" + tfn, tf});
        }
    }

    struct Result { std::string track; std::size_t bars, sessions; double in_rmse, oos_rmse; std::vector<vf::VolScore> s; };
    std::vector<Result> results;
    for (const auto& t : tracks) {
        ft::TrackInfo info;
        auto raw = ft::load_bars(root / t.dir, t.tf, info);
        ft::clean_bars(raw, info);
        if (raw.size() < 100) { std::printf("  %-18s no bars under %s\n", t.name.c_str(), (root / t.dir).string().c_str()); continue; }
        std::vector<vf::Bar> bars;
        bars.reserve(raw.size());
        for (const auto& b : raw) bars.push_back({b.t, b.c});
        vf::Options o = base;
        o.intraday = t.tf != da::kDailyTf;
        const auto r = vf::run(bars, vix, o);
        if (r.rows.empty()) { std::printf("  %-18s %zu bars, %zu sessions: fewer than --burn sessions, not scored\n", t.name.c_str(), r.bars, r.sessions); continue; }
        Result res{t.name, r.bars, r.sessions, r.har_in_sample_rmse, r.har_oos_same_weights, {}};
        for (std::size_t m = 0; m < vf::kModels; ++m) res.s.push_back(vf::score(r.rows, m));
        std::printf("  %-18s %zu bars, %zu sessions, %zu scored\n", t.name.c_str(), r.bars, r.sessions, r.rows.size());
        results.push_back(std::move(res));
    }
    if (results.empty()) { std::printf("altair_vol_forecast: nothing to score\n"); return 1; }

    const std::size_t tests = results.size() * (vf::kModels - 1) * 2;
    const double bar = bonferroni_t(tests);
    std::printf("\nNext-bar move size |r| in bp. Skill = 1 - RMSE / RMSE(baseline). Significant: DM |t| >= %.2f "
                "(Bonferroni over %zu tests) and better in every third.\n", bar, tests);

    const fs::path dir = out / "vol_forecast";
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::ofstream csv(dir / "summary.csv");
    csv << "track,model,n,rmse_bp,mae_bp,skill_vs_rw,skill_vs_mean22,dm_vs_rw,dm_vs_mean22,skill_rw_third1,skill_rw_third2,"
           "skill_rw_third3,har_in_sample_rmse,har_oos_rmse_same_rows,verdict\n";
    for (const auto& r : results) {
        std::printf("\n%s (%zu bars, %zu sessions; overfit check, recent rows: HAR in-sample RMSE %.2f vs out-of-sample %.2f bp)\n",
                    r.track.c_str(), r.bars, r.sessions, r.in_rmse, r.oos_rmse);
        std::printf("  %-14s %9s %8s %8s %10s %10s %8s %8s\n", "model", "n", "RMSE", "MAE", "vs RW", "vs Mean22", "DM RW", "DM M22");
        std::size_t best = vf::kRw;
        for (std::size_t m = 0; m < vf::kModels; ++m) if (r.s[m].rmse < r.s[best].rmse) best = m;
        for (std::size_t m = 0; m < vf::kModels; ++m) {
            const auto& s = r.s[m];
            const bool all_thirds = s.skill_rw_thirds[0] > 0 && s.skill_rw_thirds[1] > 0 && s.skill_rw_thirds[2] > 0;
            std::string verdict = "baseline";
            if (m != vf::kRw && m != vf::kMean) {
                const bool beats_rw = s.dm_rw <= -bar && all_thirds;
                const bool beats_mean = s.dm_mean <= -bar;
                verdict = beats_rw && beats_mean ? "beats both (significant)"
                        : beats_rw              ? "beats RW only"
                                                : "no proven edge";
            }
            std::printf("  %-14s %9zu %8.2f %8.2f %+9.1f%% %+9.1f%% %+8.1f %+8.1f  %s%s\n", vf::model_names()[m].c_str(), s.n, s.rmse,
                        s.mae, 100 * s.skill_rw, 100 * s.skill_mean, s.dm_rw, s.dm_mean, verdict.c_str(), m == best ? "  <- lowest RMSE" : "");
            csv << '"' << r.track << "\",\"" << vf::model_names()[m] << "\"," << s.n << ',' << s.rmse << ',' << s.mae << ',' << s.skill_rw
                << ',' << s.skill_mean << ',' << s.dm_rw << ',' << s.dm_mean << ',' << s.skill_rw_thirds[0] << ','
                << s.skill_rw_thirds[1] << ',' << s.skill_rw_thirds[2] << ',' << r.in_rmse << ',' << r.oos_rmse << ",\"" << verdict << "\"\n";
        }
    }
    std::printf("\nwrote %s\n", (dir / "summary.csv").string().c_str());
    return 0;
}
