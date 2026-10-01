// app/depth_study_main.cpp -- altair_depth_study.
//
// Does the order book predict the next mid-price move, by more than the
// spread a trade must cross? (book/depth_study.hpp: order flow imbalance after
// Cont, Kukanov & Stoikov 2014, level-1 imbalance, microprice.)
//
// Input: depth recorded by
//   altair_fyers_ticker --symbols ... --depth --jsonl data\ticks\depth-<date>.jsonl --stamp --seconds 22800 --go
// (ops/record_depth.ps1 does exactly that for a session). Recordings are
// broker data: data/ticks/ is git-ignored.
//
// For each symbol and horizon: the contemporaneous fit (the paper's), then the
// predictive one fitted on the first half and scored on the second -- out-of-
// sample R^2, how often the sign is right, and how often the predicted move
// even exceeds half the spread. Research only: this cannot execute at HFT
// latency, and the verdict says so.
//
// Writes <out>/depth_study/summary.csv.

#include <book/depth_study.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;

void usage(const char* exe) {
    std::printf(
        "  Does recorded order-book depth predict the next mid-price move by more than the spread?\n\n"
        "    %s [--in PATH] [--out DIR] [--horizons 1,5,30] [--lookback 5] [--step-ms 1000]\n\n"
        "    --in PATH       a .jsonl file or a directory of them (default data/ticks)\n"
        "    --out DIR       default data/verified (writes depth_study/)\n"
        "    --horizons S    seconds ahead to predict (default 1,5,30)\n"
        "    --lookback S    seconds of order flow behind each prediction (default 5)\n"
        "    --step-ms N     grid step (default 1000)\n\n"
        "  Record first: ops\\record_depth.ps1 (altair_fyers_ticker --depth --jsonl ... --stamp).\n", exe);
}

std::string fixed(double v, int d) {
    char b[64];
    std::snprintf(b, sizeof b, "%.*f", d, v);
    return b;
}

} // namespace

int main(int argc, char** argv) {
    fs::path in = "data/ticks", out = "data/verified";
    std::string horizons_arg = "1,5,30";
    double lookback_s = 5, step_ms = 1000;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a{argv[i]};
        const bool has = i + 1 < argc;
        if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
        if (a == "--in" && has) { in = argv[++i]; continue; }
        if (a == "--out" && has) { out = argv[++i]; continue; }
        if (a == "--horizons" && has) { horizons_arg = argv[++i]; continue; }
        if (a == "--lookback" && has) { lookback_s = std::atof(argv[++i]); if (!(lookback_s > 0) || lookback_s > 3600) { usage(argv[0]); return 2; } continue; }
        if (a == "--step-ms" && has) { step_ms = std::atof(argv[++i]); if (!(step_ms >= 50) || step_ms > 60000) { usage(argv[0]); return 2; } continue; }
        usage(argv[0]);
        return 2;
    }
    std::vector<double> horizons;
    {
        std::stringstream ss(horizons_arg);
        for (std::string h; std::getline(ss, h, ',');) {
            const double v = std::atof(h.c_str());
            if (!(v > 0) || v > 3600) { usage(argv[0]); return 2; }
            horizons.push_back(v);
        }
    }

    std::vector<fs::path> files;
    std::error_code ec;
    if (fs::is_directory(in, ec)) {
        for (const auto& e : fs::directory_iterator(in, ec)) {
            if (e.is_regular_file() && e.path().extension() == ".jsonl") { files.push_back(e.path()); }
        }
        std::sort(files.begin(), files.end());
    } else if (fs::is_regular_file(in, ec)) {
        files.push_back(in);
    }
    if (files.empty()) {
        std::printf("  no .jsonl recordings under %s\n  record a session first: powershell -ExecutionPolicy Bypass -File ops\\record_depth.ps1 -Go\n",
                    in.string().c_str());
        return 1;
    }
    altair::DepthRecorderReader reader;
    for (const auto& f : files) {
        std::ifstream s(f, std::ios::binary);
        for (std::string line; std::getline(s, line);) { reader.line(line); }
    }
    const auto& rep = reader.report();
    std::printf("Depth study: %zu file(s), %zu lines, %zu depth updates, %zu unstamped, %zu other, %zu invalid\n", files.size(),
                rep.lines, rep.depth, rep.unstamped, rep.other, rep.invalid);
    if (rep.unstamped > 0 && rep.depth == 0) {
        std::printf("  the depth lines carry no receive time: record with --stamp\n");
        return 1;
    }

    const fs::path dir = out / "depth_study";
    fs::create_directories(dir, ec);
    std::ofstream f(dir / "summary.csv", std::ios::trunc);
    f << "symbol,horizon_s,lookback_s,events,samples,contemp_beta_bp_per_ofi,contemp_r2,oos_r2_ofi,oos_r2_all,hit_rate,"
         "mean_abs_move_bp,mean_abs_prediction_bp,mean_spread_bp,share_prediction_beyond_half_spread,verdict\n";
    std::printf("  %-24s %5s %8s %7s %8s %8s %6s %8s %8s %8s\n", "symbol", "H s", "samples", "R2 now", "R2 OFI", "R2 all", "hit",
                "|move|", "|pred|", "spread");
    for (const auto& [sym, ev] : reader.events()) {
        for (const double h : horizons) {
            altair::DepthStudyPolicy p;
            p.step_ms = static_cast<std::int64_t>(step_ms);
            p.lookback_ms = static_cast<std::int64_t>(lookback_s * 1000.0);
            p.horizon_ms = static_cast<std::int64_t>(h * 1000.0);
            const auto r = altair::depth_study(ev, p);
            if (!r) {
                std::printf("  %-24s %5.0f  too few events (%zu)\n", sym.c_str(), h, ev.size());
                continue;
            }
            const char* verdict = r->oos_r2_all <= 0.0 ? "no out-of-sample predictive power"
                                : r->share_pred_beyond_half_spread < 0.05 ? "predicts, but almost never by more than half the spread: not tradable by crossing it"
                                : r->hit_rate < 0.55 ? "predicts a little; direction barely better than a coin"
                                : "predicts beyond half the spread on some samples: a candidate for a passive (quote-side) test, not a market order";
            f << sym << ',' << h << ',' << lookback_s << ',' << ev.size() << ',' << r->samples << ',' << fixed(r->contemp_beta, 6) << ','
              << fixed(r->contemp_r2, 4) << ',' << fixed(r->oos_r2_ofi, 4) << ',' << fixed(r->oos_r2_all, 4) << ',' << fixed(r->hit_rate, 4)
              << ',' << fixed(r->mean_abs_move_bp, 3) << ',' << fixed(r->mean_abs_pred_bp, 3) << ',' << fixed(r->mean_spread_bp, 3) << ','
              << fixed(r->share_pred_beyond_half_spread, 4) << ",\"" << verdict << "\"\n";
            std::printf("  %-24s %5.0f %8zu %7.3f %8.3f %8.3f %6.3f %8.2f %8.2f %8.2f\n", sym.c_str(), h, r->samples, r->contemp_r2,
                        r->oos_r2_ofi, r->oos_r2_all, r->hit_rate, r->mean_abs_move_bp, r->mean_abs_pred_bp, r->mean_spread_bp);
        }
    }
    std::printf("  wrote %s\n", (dir / "summary.csv").string().c_str());
    return 0;
}
