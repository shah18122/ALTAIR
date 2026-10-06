// app/tune_main.cpp -- altair_tune: the event-driven strategies, tuned on the
// session tapes altair_live_engine recorded (app/live_tune.hpp).
//
//   altair_tune [--root DIR] [--strategy arbitrage|ohl|option_arb|all]
//               [--unverified-costs] [--allow-sim] [--dry-run]
//
// For each strategy: every recorded day replayed through every point of its
// grid, a walk-forward over the days, net of expenses, and the verdict. A
// setting is written to config/model_params/<key>.toml only when it passes the
// guard; the engine loads it at its next start. The per-day table goes to
// data/live/tune/<key>.csv either way.

#include <app/demo_costs.hpp>
#include <app/live_tune.hpp>
#include <risk/charges_toml.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace tn = altair::tune;
namespace lv = altair::live;

void usage(const char* exe) {
    std::printf(
        "altair_tune -- tune the arbitrage, OHL and option arbitrage on recorded FYERS sessions\n\n"
        "    %s [--root DIR] [--strategy arbitrage|ohl|option_arb|all] [--unverified-costs] [--allow-sim] [--dry-run]\n\n"
        "    --root              the tree holding data/live/tapes and config/ (default: the source tree)\n"
        "    --strategy          one strategy, or all (default)\n"
        "    --unverified-costs  price expenses from config/charges.toml although it is UNVERIFIED\n"
        "                        (without expenses nothing is tuned: the verdict is net of them)\n"
        "    --allow-sim         use SIM tapes too (for trying the tuner out: not evidence)\n"
        "    --dry-run           report, but write no config/model_params file\n\n"
        "  A setting replaces the defaults only after %zu recorded days, when out of sample it beat them\n"
        "  and made money net of expenses, and its daily net survives the Romano-Wolf adjustment for the\n"
        "  whole grid (p < %.2f). The engine reads config/model_params/<key>.toml at its next start.\n",
        exe, tn::kMinDays, tn::kMaxP);
}

std::string today_text() {
    const auto now = std::chrono::system_clock::now();
    const std::int64_t s = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    return lv::day_text(lv::ist_today(s));
}

} // namespace

int main(int argc, char** argv) {
    fs::path root = ALTAIR_SOURCE_DIR;
    std::string which = "all";
    bool unverified = false, allow_sim = false, dry_run = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has = i + 1 < argc;
        if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
        if (a == "--root" && has) { root = argv[++i]; continue; }
        if (a == "--strategy" && has) { which = argv[++i]; continue; }
        if (a == "--unverified-costs") { unverified = true; continue; }
        if (a == "--allow-sim") { allow_sim = true; continue; }
        if (a == "--dry-run") { dry_run = true; continue; }
        std::printf("unknown argument %s\n", a.c_str());
        usage(argv[0]);
        return 2;
    }

    // Expenses, as the engine prices them.
    std::vector<altair::ChargeSchedule> schedules;
    const fs::path charges = root / "config/charges.toml";
    if (const auto rep = altair::load_charges_file(charges.string().c_str(), schedules); !rep) {
        std::printf("config/charges.toml did not load (%s): nothing can be judged net of expenses\n",
                    altair::charges_error_text(rep.error()));
        return 2;
    } else if (!rep->verified) {
        if (!unverified) {
            std::printf("config/charges.toml is UNVERIFIED: verify the rates and set last_verified, or pass --unverified-costs\n");
            return 2;
        }
        for (auto& s : schedules) s.verified = true;
        std::printf("expenses from an UNVERIFIED config/charges.toml (--unverified-costs): every net is UNVERIFIED\n");
    }
    const lv::LiveCostFn cost = [&schedules](const lv::LiveInstrument& in, bool buy, double qty, double px, std::int64_t ns) {
        const altair::Segment seg = in.kind == lv::LiveKind::Future ? altair::Segment::Fut
                                  : (in.kind == lv::LiveKind::Equity ? altair::Segment::Cash : altair::Segment::Opt);
        const auto c = altair::demo_costs::fill(seg, buy ? altair::Side::Buy : altair::Side::Sell, qty, px,
                                                ns / 1'000'000'000LL + 19800, schedules,
                                                in.fyers.rfind("BSE:", 0) == 0 ? altair::Exchange::BSE : altair::Exchange::NSE);
        return c.priced ? c.total : std::numeric_limits<double>::quiet_NaN();
    };

    // The tapes, oldest first (their names start with the session's date).
    std::vector<fs::path> tapes;
    std::error_code ec;
    for (const auto& de : fs::directory_iterator(root / "data/live/tapes", ec))
        if (de.path().extension() == ".tape") tapes.push_back(de.path());
    std::sort(tapes.begin(), tapes.end());
    std::printf("%zu tape(s) in %s\n", tapes.size(), (root / "data/live/tapes").string().c_str());
    if (tapes.empty()) {
        std::printf("nothing to tune on yet: altair_live_engine records every session there by itself\n");
        return 0;
    }

    int written = 0;
    for (const auto& g : tn::all_grids()) {
        if (which != "all" && which != g.key) continue;
        std::printf("\n== %s (%s): %zu settings, defaults %s\n", g.key.c_str(), g.model.c_str(), g.points.size(),
                    tn::params_text(g.points[g.defaults]).c_str());
        // One run per tape; tapes of the same day (an engine restarted) add up.
        std::map<std::string, tn::DayRun> by_day;
        for (const auto& t : tapes) {
            tn::DayRun d;
            std::string err;
            const auto t0 = std::chrono::steady_clock::now();
            if (!tn::run_tape(t, g, cost, d, err)) { std::printf("  skipped: %s\n", err.c_str()); continue; }
            const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (d.simulated && !allow_sim) { std::printf("  %s: SIM, left out (%.1f s)\n", t.filename().string().c_str(), secs); continue; }
            std::printf("  %s: %s, defaults net %.2f over %d trip(s) (%.1f s)%s\n", t.filename().string().c_str(), d.day.c_str(),
                        d.net[g.defaults], d.trips[g.defaults], secs,
                        d.open_at_end > 0 ? ", some positions still open at the end (not counted)" : "");
            auto it = by_day.find(d.day);
            if (it == by_day.end()) { by_day.emplace(d.day, std::move(d)); continue; }
            for (std::size_t p = 0; p < g.points.size(); ++p) {
                it->second.net[p] += d.net[p];
                it->second.gross[p] += d.gross[p];
                it->second.trips[p] += d.trips[p];
            }
        }
        std::vector<tn::DayRun> days;
        for (auto& [day, d] : by_day) {
            bool priced = true;
            for (const double n : d.net) priced = priced && std::isfinite(n);
            if (!priced) { std::printf("  %s: a fill could not be priced; the day is left out\n", day.c_str()); continue; }
            days.push_back(std::move(d));
        }
        // The table: every day, every setting.
        fs::create_directories(root / "data/live/tune", ec);
        {
            std::ofstream f(root / "data/live/tune" / (g.key + ".csv"), std::ios::trunc);
            f << "day,setting,params,trips,gross,net,is_default\n";
            for (const auto& d : days)
                for (std::size_t p = 0; p < g.points.size(); ++p) {
                    char b[160];
                    std::snprintf(b, sizeof b, ",%d,%.2f,%.2f,%d\n", d.trips[p], d.gross[p], d.net[p], p == g.defaults ? 1 : 0);
                    f << d.day << "," << p << ",\"" << tn::params_text(g.points[p]) << "\"" << b;
                }
        }
        const auto w = tn::walk_forward(days, g.points.size(), g.defaults);
        for (std::size_t i = 0; i < w.test_days.size(); ++i)
            std::printf("  %s: walk-forward chose %s -> net %.2f (defaults %.2f)\n", w.test_days[i].c_str(),
                        tn::params_text(g.points[w.chosen[i]]).c_str(), w.oos[i], w.defaults_oos[i]);
        std::printf("  out of sample: %.2f (defaults %.2f) over %zu test day(s); best overall %s, Romano-Wolf p %.3f\n",
                    w.oos_total, w.defaults_total, w.test_days.size(), tn::params_text(g.points[w.final_choice]).c_str(), w.p_rw);
        std::printf("  verdict: %s\n", w.why.c_str());
        if (w.accept && !dry_run) {
            if (tn::write_params(root, g, g.points[w.final_choice], w, days.size(), today_text())) {
                std::printf("  written: config/model_params/%s.toml (the engine reads it at its next start)\n", g.key.c_str());
                ++written;
            } else {
                std::printf("  could not write config/model_params/%s.toml\n", g.key.c_str());
            }
        } else {
            std::printf("  the defaults stay%s\n", w.accept ? " (--dry-run)" : "");
        }
    }
    std::printf("\n%d setting file(s) written\n", written);
    return 0;
}
