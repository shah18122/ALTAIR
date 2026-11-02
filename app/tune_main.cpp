// app/tune_main.cpp -- altair_tune: the event-driven strategies, tuned on the
// session tapes altair_live_engine recorded (app/live_tune.hpp).
//
//   altair_tune [--root DIR] [--strategy KEY|PREFIX|all] [--latency-ms LIST]
//               [--unverified-costs] [--allow-sim] [--dry-run]
//
// For each strategy (the arbitrage, OHL, option arbitrage, and every OBI
// trader: obi_<under>_<A-D>_<fut|opt>): every recorded day replayed through
// every point of its grid in ONE pass a tape, a walk-forward over the days,
// net of expenses, and the verdict. A setting is written to
// config/model_params/<key>.toml only when it passes the guard; the engine
// loads it at its next start. The per-day table goes to
// data/live/tune/<key>.csv and the stats sheet of every setting (hit rate,
// Sharpe, drawdown, cost share, markouts) to data/live/tune/<key>_stats.csv
// either way. --latency-ms 50,250,1000 re-runs everything at each latency:
// only the first is the verdict; the rest show how the result moves with
// speed (data/live/tune/latency.csv).

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
        "altair_tune -- tune the event-driven strategies and the OBI traders on recorded FYERS sessions\n\n"
        "    %s [--root DIR] [--strategy KEY|PREFIX|all] [--latency-ms LIST] [--unverified-costs] [--allow-sim] [--dry-run]\n\n"
        "    --root              the tree holding data/live/tapes, data/store/tapes and config/ (default: the source tree)\n"
        "    --strategy          arbitrage, ohl, option_arb, an OBI trader (obi_banknifty_A_opt), a prefix\n"
        "                        (obi, obi_banknifty, obi_nykaa_C), or all (default)\n"
        "    --latency-ms        decision-to-market latency, e.g. 250 or 50,250,1000 (default: each tape's own);\n"
        "                        the first is the verdict, the rest are sensitivity (data/live/tune/latency.csv)\n"
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
    std::vector<std::int64_t> latencies;   // ns; empty = each tape's own
    bool unverified = false, allow_sim = false, dry_run = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has = i + 1 < argc;
        if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
        if (a == "--root" && has) { root = argv[++i]; continue; }
        if (a == "--strategy" && has) { which = argv[++i]; continue; }
        if (a == "--latency-ms" && has) {
            std::string list = argv[++i];
            for (std::size_t at = 0; at <= list.size();) {
                const auto comma = list.find(',', at);
                const std::string v = list.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
                char* end = nullptr;
                const double ms = std::strtod(v.c_str(), &end);
                if (v.empty() || end == v.c_str() || !(ms >= 0.0)) { std::printf("bad --latency-ms value '%s'\n", v.c_str()); return 2; }
                latencies.push_back(static_cast<std::int64_t>(ms * 1e6));
                if (comma == std::string::npos) break;
                at = comma + 1;
            }
            continue;
        }
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

    // The tapes, oldest first: the engine's and the recorder's.
    const auto tapes = tn::find_tapes(root);
    std::printf("%zu tape(s) in data/live/tapes and data/store/tapes under %s\n", tapes.size(), root.string().c_str());
    if (tapes.empty()) {
        std::printf("nothing to tune on yet: altair_live_engine and altair_feed_recorder record every session by themselves\n");
        return 0;
    }

    // The grids asked for: a key, a prefix ("obi", "obi_banknifty"), or all.
    std::vector<tn::Grid> grids = tn::all_grids();
    for (auto& g : tn::obi_grids(tn::obi_underlyings(root))) grids.push_back(std::move(g));
    std::vector<const tn::Grid*> chosen;
    std::size_t engines = 0;
    for (const auto& g : grids)
        if (which == "all" || g.key == which || g.key.rfind(which + "_", 0) == 0 || (which == "obi" && g.key.rfind("obi_", 0) == 0)) {
            chosen.push_back(&g);
            engines += g.points.size();
        }
    if (chosen.empty()) { std::printf("no strategy matches '%s'\n", which.c_str()); return 2; }
    std::printf("%zu strateg%s, %zu settings: one engine each, every tape read once\n", chosen.size(), chosen.size() == 1 ? "y" : "ies", engines);
    if (latencies.empty()) latencies.push_back(-1);

    std::error_code ec;
    fs::create_directories(root / "data/live/tune", ec);
    std::ofstream lat_csv;
    if (latencies.size() > 1) {
        lat_csv.open(root / "data/live/tune/latency.csv", std::ios::trunc);
        lat_csv << "latency_ms,key,setting,params,oos_net,defaults_oos_net,total_net_defaults\n";
    }
    int written = 0;
    for (std::size_t li = 0; li < latencies.size(); ++li) {
        const std::int64_t lat = latencies[li];
        const bool verdict = li == 0;
        if (lat >= 0) std::printf("\n#### latency %.0f ms%s\n", static_cast<double>(lat) / 1e6, verdict ? " (the verdict)" : " (sensitivity only)");
        // One pass a tape for every chosen grid; tapes of the same day (an
        // engine restarted) add up.
        std::vector<std::map<std::string, tn::DayRun>> by_day(chosen.size());
        tn::FeedLag lag;
        for (const auto& t : tapes) {
            std::vector<tn::DayRun> runs;
            std::string err;
            const auto t0 = std::chrono::steady_clock::now();
            if (!tn::run_tape_group(t, chosen, cost, runs, err, lat, verdict ? &lag : nullptr)) {
                std::printf("  skipped: %s\n", err.c_str());
                continue;
            }
            const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (runs.front().simulated && !allow_sim) { std::printf("  %s: SIM, left out (%.1f s)\n", t.filename().string().c_str(), secs); continue; }
            std::printf("  %s: %s replayed (%.1f s)\n", t.filename().string().c_str(), runs.front().day.c_str(), secs);
            for (std::size_t gi = 0; gi < chosen.size(); ++gi) {
                auto& d = runs[gi];
                auto it = by_day[gi].find(d.day);
                if (it == by_day[gi].end()) { by_day[gi].emplace(d.day, std::move(d)); continue; }
                for (std::size_t p = 0; p < chosen[gi]->points.size(); ++p) {
                    it->second.net[p] += d.net[p];
                    it->second.gross[p] += d.gross[p];
                    it->second.trips[p] += d.trips[p];
                    auto& a = it->second.rec[p];
                    const auto& b = d.rec[p];
                    a.trade_net.insert(a.trade_net.end(), b.trade_net.begin(), b.trade_net.end());
                    a.hold_s.insert(a.hold_s.end(), b.hold_s.begin(), b.hold_s.end());
                    a.gross += b.gross;
                    a.expenses += b.expenses;
                    for (std::size_t k = 0; k < 3; ++k) { a.markout_sum[k] += b.markout_sum[k]; a.markout_n[k] += b.markout_n[k]; }
                }
                it->second.open_at_end = std::max(it->second.open_at_end, d.open_at_end);
            }
        }
        if (verdict && lag.hist.count() > 0)
            std::printf("  feed lag (receive minus exchange time, trade prints; FYERS stamps to the second): %s%s\n",
                        lag.hist.text(1e6, "ms").c_str(),
                        lag.negative > 0 ? (", " + std::to_string(lag.negative) + " stamped after arrival (clock skew)").c_str() : "");

        for (std::size_t gi = 0; gi < chosen.size(); ++gi) {
            const tn::Grid& g = *chosen[gi];
            std::vector<tn::DayRun> days;
            for (auto& [day, d] : by_day[gi]) {
                bool priced = true;
                for (const double n : d.net) priced = priced && std::isfinite(n);
                if (!priced) { std::printf("  %s %s: a fill could not be priced; the day is left out\n", g.key.c_str(), day.c_str()); continue; }
                days.push_back(std::move(d));
            }
            const auto w = tn::walk_forward(days, g.points.size(), g.defaults);
            if (!verdict) {
                std::printf("  %s: out of sample %.2f (defaults %.2f)\n", g.key.c_str(), w.oos_total, w.defaults_total);
                double total_def = 0.0;
                for (const auto& d : days) total_def += d.net[g.defaults];
                char b[96];
                std::snprintf(b, sizeof b, ",%.2f,%.2f,%.2f\n", w.oos_total, w.defaults_total, total_def);
                lat_csv << static_cast<double>(lat) / 1e6 << "," << g.key << "," << w.final_choice << ",\"" << tn::params_text(g.points[w.final_choice]) << "\"" << b;
                continue;
            }
            std::printf("\n== %s (%s): %zu settings, defaults %s\n", g.key.c_str(), g.model.c_str(), g.points.size(),
                        tn::params_text(g.points[g.defaults]).c_str());
            // The table: every day, every setting.
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
            // The stats sheet: every setting over every day.
            std::vector<lv::StatsSheet> sheets(g.points.size());
            {
                std::ofstream f(root / "data/live/tune" / (g.key + "_stats.csv"), std::ios::trunc);
                f << "setting,params,is_default," << lv::kStatsHeader << "\n";
                for (std::size_t p = 0; p < g.points.size(); ++p) {
                    lv::TradeRecord r;
                    for (const auto& d : days) {
                        const auto& x = d.rec[p];
                        r.trade_net.insert(r.trade_net.end(), x.trade_net.begin(), x.trade_net.end());
                        r.hold_s.insert(r.hold_s.end(), x.hold_s.begin(), x.hold_s.end());
                        r.daily_net.push_back(d.net[p]);
                        r.gross += x.gross;
                        r.expenses += x.expenses;
                        for (std::size_t k = 0; k < 3; ++k) { r.markout_sum[k] += x.markout_sum[k]; r.markout_n[k] += x.markout_n[k]; }
                    }
                    sheets[p] = lv::stats_sheet(r);
                    f << p << ",\"" << tn::params_text(g.points[p]) << "\"," << (p == g.defaults ? 1 : 0) << "," << lv::stats_row(sheets[p]) << "\n";
                }
            }
            for (std::size_t i = 0; i < w.test_days.size(); ++i)
                std::printf("  %s: walk-forward chose %s -> net %.2f (defaults %.2f)\n", w.test_days[i].c_str(),
                            tn::params_text(g.points[w.chosen[i]]).c_str(), w.oos[i], w.defaults_oos[i]);
            std::printf("  defaults: %s\n", lv::stats_text(sheets[g.defaults]).c_str());
            if (w.final_choice != g.defaults) std::printf("  best:     %s\n", lv::stats_text(sheets[w.final_choice]).c_str());
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
    }
    std::printf("\n%d setting file(s) written\n", written);
    return 0;
}
