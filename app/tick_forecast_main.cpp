// app/tick_forecast_main.cpp -- altair_tick_forecast: forecasts trade by
// trade on raw tick files (no bars), scored on RMSE / MAE against the plain
// baselines (models/tick_forecast.hpp).
//
// Input: a folder of tick CSVs, searched recursively, one contract a file,
// rows "YYYYMMDD,HH:MM:SS,price[,qty[,oi]]" (Smit's Google Drive files, source
// "drive-ticks", NOT FYERS: docs/DATA_POLICY.md). Futures files are named
// <NIFTY|BANKNIFTY><YY><MON>FUT.csv; each day uses the contract with the most
// traded quantity that day (the near month until the roll), so every stream
// is one continuous, time-ordered front month. NIFTY.csv files under a folder
// whose path says "spot" are the NIFTY spot index ticks (per day, the file
// with the most ticks).
//
//     altair_tick_forecast --ticks DIR [--out DIR] [--only TEXT]

#include <models/tick_forecast.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace tf = altair::tick_forecast;

void usage(const char* exe) {
    std::printf(
        "  Forecast trade by trade on raw tick files; RMSE / MAE against the baselines.\n\n"
        "    %s --ticks DIR [--out DIR] [--only TEXT]\n\n"
        "    --ticks DIR   folder of tick CSVs (Drive files; NOT FYERS)\n"
        "    --out DIR     default data/verified (writes tick_forecast/summary.csv)\n"
        "    --only TEXT   streams whose name contains TEXT (NIFTY FUT, BANKNIFTY FUT, NIFTY spot)\n", exe);
}

struct Stream {
    std::string name;
    std::vector<fs::path> files;
    bool by_qty = true;   ///< pick the day's file by traded quantity (futures) or by tick count (spot)
};

/// Feeds `e` with one continuous, time-ordered stream: per day, the ticks of
/// the file that dominates that day.
void run_stream(const Stream& s, tf::TkEngine& e) {
    // Pass 1: per file, per day, the quantity (or tick count).
    std::map<int, std::pair<double, std::size_t>> best;   // day -> (weight, file index)
    for (std::size_t i = 0; i < s.files.size(); ++i) {
        std::map<int, double> w;
        tf::tk_read_file(s.files[i], [&](const tf::TkTick& t) { w[t.day] += s.by_qty ? t.qty : 1.0; });
        for (const auto& [d, x] : w) {
            auto it = best.find(d);
            if (it == best.end() || x > it->second.first) best[d] = {x, i};
        }
    }
    // Pass 2: the files in the order of their first chosen day; each feeds its chosen days.
    std::map<std::size_t, int> first_day;
    for (const auto& [d, v] : best) if (!first_day.count(v.second)) first_day[v.second] = d;
    std::vector<std::pair<int, std::size_t>> order;
    for (const auto& [i, d] : first_day) order.push_back({d, i});
    std::sort(order.begin(), order.end());
    int last_day = 0;
    for (const auto& [d0, i] : order) {
        std::vector<tf::TkTick> ticks;
        tf::tk_read_file(s.files[i], [&](const tf::TkTick& t) {
            const auto it = best.find(t.day);
            if (it != best.end() && it->second.second == i && t.day > last_day) ticks.push_back(t);
        });
        // Time order inside the file is the file's own (trades within a second keep their order).
        std::stable_sort(ticks.begin(), ticks.end(), [](const tf::TkTick& a, const tf::TkTick& b) {
            return a.day != b.day ? a.day < b.day : a.sec < b.sec;
        });
        for (const auto& t : ticks) e.add(t);
        if (!ticks.empty()) last_day = ticks.back().day;
        std::printf("    %s: %zu ticks fed\n", s.files[i].filename().string().c_str(), ticks.size());
        std::fflush(stdout);
    }
    e.finish();
}

double bonferroni_z(std::size_t m) {
    const double p = 0.05 / static_cast<double>(std::max<std::size_t>(1, m));
    double lo = 0.0, hi = 10.0;
    for (int k = 0; k < 80; ++k) {
        const double mid = 0.5 * (lo + hi);
        (std::erfc(mid / std::sqrt(2.0)) > p ? lo : hi) = mid;
    }
    return hi;
}

} // namespace

int main(int argc, char** argv) {
    fs::path ticks, out = "data/verified";
    std::string only;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a{argv[i]};
        const bool has = i + 1 < argc;
        if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
        if (a == "--ticks" && has) { ticks = argv[++i]; continue; }
        if (a == "--out" && has) { out = argv[++i]; continue; }
        if (a == "--only" && has) { only = argv[++i]; continue; }
        usage(argv[0]);
        return 2;
    }
    if (ticks.empty() || !fs::is_directory(ticks)) { usage(argv[0]); return 2; }
    std::printf("altair_tick_forecast: %s (source drive-ticks, NOT FYERS)\n", ticks.string().c_str());

    Stream nifty{"NIFTY FUT", {}, true}, bank{"BANKNIFTY FUT", {}, true}, spot{"NIFTY spot", {}, false};
    const std::regex fut(R"(^(BANKNIFTY|NIFTY)\d\d[A-Z]{3}FUT\.csv$)", std::regex::icase);
    for (const auto& de : fs::recursive_directory_iterator(ticks)) {
        if (!de.is_regular_file()) continue;
        const std::string n = de.path().filename().string();
        std::smatch m;
        if (std::regex_match(n, m, fut)) {
            std::string u = m[1].str();
            for (auto& ch : u) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            (u == "NIFTY" ? nifty : bank).files.push_back(de.path());
            continue;
        }
        std::string up = n, path = de.path().parent_path().string();
        for (auto& ch : up) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        for (auto& ch : path) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (up == "NIFTY.CSV" && path.find("spot") != std::string::npos) spot.files.push_back(de.path());
    }

    struct Done { std::string name; tf::TkEngine e; };
    std::vector<Done> done;
    for (Stream* s : {&nifty, &bank, &spot}) {
        if (s->files.empty() || (!only.empty() && s->name.find(only) == std::string::npos)) continue;
        std::sort(s->files.begin(), s->files.end());
        std::printf("  %s: %zu files\n", s->name.c_str(), s->files.size());
        done.push_back({s->name, {}});
        run_stream(*s, done.back().e);
    }
    if (done.empty()) { std::printf("altair_tick_forecast: no tick files under %s\n", ticks.string().c_str()); return 1; }

    // Every model-vs-baseline test of the run, for one Bonferroni bar.
    const std::size_t tests = done.size() * (2 + 3 + 2 * 3 + 3);
    const double bar = bonferroni_z(tests);
    std::printf("\nSignificant: |DM t| or |z| >= %.2f (Bonferroni over %zu tests) and better in every third.\n", bar, tests);

    const fs::path dir = out / "tick_forecast";
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::ofstream csv(dir / "summary.csv");
    csv << "stream,task,model,n,rmse,mae,skill_vs_base,dm_vs_base,third1,third2,third3,in_sample_rmse,oos_rmse_same_rows,verdict\n";

    const auto table = [&](const std::string& stream, const std::string& task, const char* unit, const tf::TkScorer& s, std::size_t base,
                           std::size_t ridge, double in_rmse, double oos_rmse) {
        std::printf("\n  %s, %s (%zu forecasts; %s; overfit check, last rows: ridge in-sample RMSE %.4g vs out-of-sample %.4g)\n",
                    stream.c_str(), task.c_str(), s.n(), unit, in_rmse, oos_rmse);
        std::printf("    %-14s %10s %10s %9s %8s  %s\n", "model", "RMSE", "MAE", "skill", "DM t", "verdict");
        std::size_t best = 0;
        for (std::size_t m = 0; m < s.names().size(); ++m) if (s.rmse(m) < s.rmse(best)) best = m;
        for (std::size_t m = 0; m < s.names().size(); ++m) {
            const auto th = s.thirds(m, base);
            const double sk = s.skill(m, base), dm = s.dm(m, base);
            std::string verdict = m == base ? "baseline" : (dm <= -bar && th[0] > 0 && th[1] > 0 && th[2] > 0) ? "beats baseline (significant)" : "no proven edge";
            std::printf("    %-14s %10.4g %10.4g %+8.2f%% %+8.1f  %s%s\n", s.names()[m].c_str(), s.rmse(m), s.mae(m), 100 * sk, dm,
                        verdict.c_str(), m == best ? "  <- lowest RMSE" : "");
            csv << '"' << stream << "\",\"" << task << "\",\"" << s.names()[m] << "\"," << s.n() << ',' << s.rmse(m) << ',' << s.mae(m)
                << ',' << sk << ',' << dm << ',' << th[0] << ',' << th[1] << ',' << th[2] << ',' << (m == ridge ? in_rmse : std::nan("")) << ','
                << (m == ridge ? oos_rmse : std::nan("")) << ",\"" << verdict << "\"\n";
        }
    };
    const auto hits = [&](const std::string& stream, const char* name, const tf::TkHits& h) {
        const std::string verdict = h.z() >= bar ? "better than a coin (significant)" : "no proven edge";
        std::printf("    %-24s %6.2f%% of %zu calls, z %+.1f  %s\n", name, 100 * h.rate(), h.n, h.z(), verdict.c_str());
        csv << '"' << stream << "\",\"direction\",\"" << name << "\"," << h.n << ",,," << h.rate() << ',' << h.z() << ",,,,,,\"" << verdict << "\"\n";
    };

    for (const auto& d : done) {
        const auto& e = d.e;
        std::printf("\n%s: %zu ticks, %zu sessions\n", d.name.c_str(), e.ticks(), e.days());
        double i1, o1, i2, o2;
        e.overfit_ret(i1, o1);
        table(d.name, "next-trade return", "bp", e.ret(), tf::TkEngine::kRetRw, tf::TkEngine::kRetAr, i1, o1);
        std::printf("\n  %s, direction of the next price change\n", d.name.c_str());
        hits(d.name, "Persistence (same again)", e.persistence());
        hits(d.name, "Reversal (bounce)", e.reversal());
        hits(d.name, "Ridge sign", e.sign_ridge());
        hits(d.name, "AR ridge, next trade", e.ar_direction());
        e.overfit_gap(i2, o2);
        table(d.name, "seconds to next trade", "s", e.gap(), tf::TkEngine::kGapMean, tf::TkEngine::kGapRidge, i2, o2);
        for (std::size_t h = 0; h < tf::TkEngine::kRvH.size(); ++h) {
            double i3, o3;
            e.overfit_rv(h, i3, o3);
            table(d.name, "realised vol next " + std::to_string(tf::TkEngine::kRvH[h]) + " s", "bp, from every tick", e.rv(h),
                  tf::TkEngine::kRvLast, tf::TkEngine::kRvHar, i3, o3);
        }
    }
    std::printf("\nwrote %s\n", (dir / "summary.csv").string().c_str());
    return 0;
}
