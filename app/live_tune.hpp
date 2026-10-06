// app/live_tune.hpp -- tuning the event-driven strategies on recorded FYERS
// ticks (altair_tune).
//
// WHY TAPES. FYERS history is one-minute bars; a strategy that lives at the
// touch (the arbitrage, OHL's first second, option locks) can only be judged
// on the ticks and quotes it would have seen. altair_live_engine records every
// session to data/live/tapes/ (on by default); this replays them.
//
// ONE PASS A TAPE. Every point of a strategy's grid gets an engine of its own,
// holding that strategy alone, and every frame of the tape is handed to all of
// them: a multi-GB tape is read once, not once per point.
//
// WALK-FORWARD, NET OF EXPENSES. Days in order; for each test day the point
// chosen is the one with the best net over the days BEFORE it (the defaults on
// a tie), and its net that day is the out-of-sample result. A setting is only
// written (config/model_params/<key>.toml, which the engine loads at start)
// when, over at least kMinDays recorded FYERS days, it beat the defaults out
// of sample, made money out of sample, and its daily net survives the
// Romano-Wolf adjustment for having tried the whole grid (live/report.hpp).
// Otherwise the defaults stay and the report says why. SIM tapes are left out
// unless asked for: nothing learned from simulated prices is evidence.
//
// NOT TUNED HERE: passive (join-the-queue) against aggressive execution. The
// paper book fills at the touch; a queue-position fill model would be needed to
// judge resting orders honestly, and that is not yet built into it.

#pragma once

#include <live/arbitrage.hpp>
#include <live/engine.hpp>
#include <live/feed_consumer.hpp>
#include <live/option_arb.hpp>
#include <live/report.hpp>
#include <live/tape.hpp>
#include <live/threshold.hpp>
#include <live/universe.hpp>
#include <server/protocol.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace altair::tune {

namespace fs = std::filesystem;
using Params = std::map<std::string, double>;

/// Fewest recorded FYERS days before a setting may replace the defaults.
inline constexpr std::size_t kMinDays = 5;
/// Romano-Wolf p the chosen setting's daily net must be under.
inline constexpr double kMaxP = 0.10;

struct Grid {
    std::string key;          ///< arbitrage | ohl | option_arb (the LIVE arm's names)
    std::string model;        ///< the engine's model name
    std::vector<Params> points;
    std::size_t defaults = 0; ///< the index of today's settings
    std::function<std::unique_ptr<live::LiveModel>(const Params&)> make;
};

[[nodiscard]] inline std::string params_text(const Params& p) {
    std::string t;
    for (const auto& [k, v] : p) {
        char b[64];
        std::snprintf(b, sizeof b, "%s%s=%g", t.empty() ? "" : " ", k.c_str(), v);
        t += b;
    }
    return t;
}

namespace detail {
/// Every combination of the axes, in order; `def` names today's value on each.
inline Grid cross(std::string key, std::string model, const std::vector<std::pair<std::string, std::vector<double>>>& axes,
                  const Params& def, std::function<std::unique_ptr<live::LiveModel>(const Params&)> make) {
    Grid g;
    g.key = std::move(key);
    g.model = std::move(model);
    g.make = std::move(make);
    g.points.emplace_back();
    for (const auto& [name, values] : axes) {
        std::vector<Params> next;
        for (const auto& p : g.points)
            for (const double v : values) {
                Params q = p;
                q[name] = v;
                next.push_back(std::move(q));
            }
        g.points = std::move(next);
    }
    for (std::size_t i = 0; i < g.points.size(); ++i)
        if (g.points[i] == def) g.defaults = i;
    return g;
}
} // namespace detail

/// The arbitrage: the margin over costs to enter, and how long a pair may wait to meet.
[[nodiscard]] inline Grid arbitrage_grid() {
    const live::LiveCrossArbRule d;
    return detail::cross("arbitrage", "Cross-exchange arbitrage",
                         {{"min_profit_bps", {0.5, 1.0, 2.0, 3.0, 5.0}}, {"max_hold_min", {5.0, 15.0, 30.0}}},
                         {{"min_profit_bps", d.min_profit_bps}, {"max_hold_min", static_cast<double>(d.max_hold_ns) / 60e9}},
                         [](const Params& p) {
                             live::LiveCrossArbRule r;
                             r.min_profit_bps = p.at("min_profit_bps");
                             r.max_hold_ns = static_cast<std::int64_t>(p.at("max_hold_min") * 60e9);
                             return std::make_unique<live::LiveCrossArbModel>(r);
                         });
}

/// OHL: the stop beyond the open, when the trail starts, how far it follows.
[[nodiscard]] inline Grid ohl_grid() {
    const live::LiveOhlRule d;
    return detail::cross("ohl", "OHL",
                         {{"stop_pct", {0.3, 0.5, 0.75, 1.0}}, {"trail_start_pct", {1.0, 1.5, 2.0}}, {"trail_pct", {0.15, 0.25, 0.5}}},
                         {{"stop_pct", d.stop_pct}, {"trail_start_pct", d.trail_start_pct}, {"trail_pct", d.trail_pct}},
                         [](const Params& p) {
                             live::LiveOhlRule r;
                             r.stop_pct = p.at("stop_pct");
                             r.trail_start_pct = p.at("trail_start_pct");
                             r.trail_pct = p.at("trail_pct");
                             return std::make_unique<live::LiveOhlModel>(r);
                         });
}

/// Option arbitrage: the margin over costs to enter a lock, and when to unwind it.
[[nodiscard]] inline Grid option_arb_grid() {
    const live::LiveOptionArbRule d;
    return detail::cross("option_arb", "Option arbitrage", {{"margin_bp", {0.5, 1.0, 2.0, 3.0, 5.0}}, {"exit_bp", {0.25, 0.5, 1.0}}},
                         {{"margin_bp", d.margin_bp}, {"exit_bp", d.exit_bp}}, [](const Params& p) {
                             live::LiveOptionArbRule r;
                             r.margin_bp = p.at("margin_bp");
                             r.exit_bp = p.at("exit_bp");
                             return std::make_unique<live::LiveOptionArbModel>(r);
                         });
}

[[nodiscard]] inline std::vector<Grid> all_grids() { return {arbitrage_grid(), ohl_grid(), option_arb_grid()}; }

// ---- config/model_params/<key>.toml ------------------------------------------------

/// `key = number` lines; # comments. What the engine loads at start.
[[nodiscard]] inline Params read_params(const fs::path& path) {
    Params p;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        if (const auto h = line.find('#'); h != std::string::npos) line.resize(h);
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        const auto trim = [](std::string& s) {
            s.erase(0, s.find_first_not_of(" \t\r"));
            s.erase(s.find_last_not_of(" \t\r") + 1);
        };
        trim(k);
        trim(v);
        char* end = nullptr;
        const double x = std::strtod(v.c_str(), &end);
        if (!k.empty() && end != v.c_str() && std::isfinite(x)) p[k] = x;
    }
    return p;
}

/// The grid's defaults overlaid with what config/model_params/<key>.toml
/// holds (only the grid's own keys; anything else is ignored).
[[nodiscard]] inline Params tuned_or_default(const Grid& g, const fs::path& root) {
    Params p = g.points[g.defaults];
    for (const auto& [k, v] : read_params(root / "config/model_params" / (g.key + ".toml")))
        if (p.count(k) != 0) p[k] = v;
    return p;
}

// ---- one tape ------------------------------------------------------------------------

struct DayRun {
    std::string day, tape;
    bool simulated = false;
    std::vector<double> net, gross;   ///< per grid point; net NaN when a fill was unpriced
    std::vector<int> trips;
    std::size_t open_at_end = 0;      ///< positions still held when the tape ended (not counted)
};

/// Replay `tape` through one engine per grid point. False (with `err`) when
/// the tape cannot be read.
[[nodiscard]] inline bool run_tape(const fs::path& tape, const Grid& g, const live::LiveCostFn& cost, DayRun& out,
                                   std::string& err) {
    live::TapeReader in(tape.string());
    live::TapeRecord rec;
    live::TapeSections start;
    if (!in.ok() || !in.next(rec) || rec.kind != live::TapeKind::Start || !live::tape_unpack(rec.text(), start)) {
        err = tape.string() + " is not a session tape";
        return false;
    }
    const std::string* uni = live::tape_section(start, "universe.csv");
    const std::string* args = live::tape_section(start, "args");
    if (uni == nullptr) { err = tape.string() + " has no universe"; return false; }
    std::istringstream us(*uni);
    const auto universe = live::read_universe_stream(us);
    live::LiveExecPolicy policy;
    if (args != nullptr) {
        std::istringstream as(*args);
        for (std::string line; std::getline(as, line);) {
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            if (k == "date") out.day = v;
            else if (k == "latency_ns") policy.latency_ns = std::atoll(v.c_str());
            else if (k == "max_quote_age_ns") policy.max_quote_age_ns = std::atoll(v.c_str());
            else if (k == "entry_timeout_ns") policy.entry_timeout_ns = std::atoll(v.c_str());
        }
    }
    out.tape = tape.string();
    std::vector<std::unique_ptr<live::LiveEngine>> engines;
    std::vector<std::unique_ptr<live::LiveFeedConsumer>> consumers;
    for (const auto& p : g.points) {
        engines.push_back(std::make_unique<live::LiveEngine>(universe, cost, policy));
        engines.back()->add_model(g.make(p));
        consumers.push_back(std::make_unique<live::LiveFeedConsumer>(*engines.back()));
    }
    std::vector<std::uint8_t> buf;
    while (in.next(rec)) {
        if (rec.kind == live::TapeKind::Start) break;   // a second session: stop at the first
        switch (rec.kind) {
        case live::TapeKind::Data: {
            buf.insert(buf.end(), rec.bytes.begin(), rec.bytes.end());
            std::size_t at = 0;
            while (buf.size() - at >= kFrameHeaderBytes) {
                const auto h = decode_header(buf.data() + at, buf.size() - at);
                if (!h) { buf.clear(); at = 0; break; }
                const std::size_t need = kFrameHeaderBytes + h->payload_len;
                if (buf.size() - at < need) break;
                for (auto& c : consumers) (void)c->on_frame(*h, buf.data() + at + kFrameHeaderBytes);
                at += need;
            }
            if (at > 0) buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(at));
            break;
        }
        case live::TapeKind::Connected:
            buf.clear();
            for (auto& c : consumers) c->on_reconnect();
            break;
        case live::TapeKind::Disconnected: buf.clear(); break;
        case live::TapeKind::Stale: for (auto& e : engines) e->set_stale(rec.text() == "1"); break;
        case live::TapeKind::Kill: for (auto& e : engines) e->set_kill(rec.text() == "1"); break;
        case live::TapeKind::Halt: for (auto& e : engines) e->set_halt(rec.text()); break;
        case live::TapeKind::Watchdog: for (auto& e : engines) (void)e->watchdog(rec.wall_ns); break;
        case live::TapeKind::Start: break;
        }
    }
    for (const auto& e : engines) {
        double net = 0.0, gross = 0.0;
        int trips = 0;
        for (const auto& t : e->book().trades()) {
            if (t.model != g.model) continue;
            net += t.net;   // NaN once any fill was unpriced
            gross += t.gross;
            ++trips;
        }
        out.net.push_back(net);
        out.gross.push_back(gross);
        out.trips.push_back(trips);
        out.simulated = out.simulated || e->simulated();
        std::size_t open = 0;
        for (const auto& p : e->book().positions()) open += p.model == g.model ? 1u : 0u;
        out.open_at_end = std::max(out.open_at_end, open);
    }
    return true;
}

// ---- walk-forward --------------------------------------------------------------------

struct WalkForward {
    std::vector<std::string> test_days;
    std::vector<std::size_t> chosen;          ///< per test day
    std::vector<double> oos, defaults_oos;    ///< net per test day: walk-forward, and the defaults
    double oos_total = 0.0, defaults_total = 0.0;
    std::size_t final_choice = 0;             ///< best over every day: what would be written
    double p_rw = std::numeric_limits<double>::quiet_NaN();
    bool accept = false;
    std::string why;
};

/// `days` in date order, every one priced (net finite for every point).
[[nodiscard]] inline WalkForward walk_forward(const std::vector<DayRun>& days, std::size_t points, std::size_t defaults,
                                              std::size_t min_train = 2) {
    WalkForward w;
    w.final_choice = defaults;
    if (points == 0) { w.why = "no grid"; return w; }
    const auto best_over = [&](std::size_t upto) {
        std::size_t best = defaults;
        double best_sum = -std::numeric_limits<double>::infinity();
        for (std::size_t p = 0; p < points; ++p) {
            double sum = 0.0;
            for (std::size_t j = 0; j < upto; ++j) sum += days[j].net[p];
            if (sum > best_sum + 1e-9 || (std::fabs(sum - best_sum) <= 1e-9 && p == defaults)) { best_sum = sum; best = p; }
        }
        return best;
    };
    for (std::size_t i = min_train; i < days.size(); ++i) {
        const std::size_t c = best_over(i);
        w.test_days.push_back(days[i].day);
        w.chosen.push_back(c);
        w.oos.push_back(days[i].net[c]);
        w.defaults_oos.push_back(days[i].net[defaults]);
        w.oos_total += days[i].net[c];
        w.defaults_total += days[i].net[defaults];
    }
    w.final_choice = best_over(days.size());
    // The guard: the whole grid tried at once, Romano-Wolf on the daily nets.
    std::vector<std::vector<double>> series(points);
    std::vector<live::report::DailyStats> stats(points);
    for (std::size_t p = 0; p < points; ++p) {
        for (const auto& d : days) series[p].push_back(d.net[p]);
        stats[p] = live::report::describe(series[p]);
    }
    if (days.size() >= kMinDays) live::report::bootstrap(series, stats);
    w.p_rw = stats[w.final_choice].p_rw;
    if (days.size() < kMinDays)
        w.why = std::to_string(days.size()) + " recorded FYERS day(s); at least " + std::to_string(kMinDays) + " are needed";
    else if (w.final_choice == defaults) w.why = "the defaults are already the best setting";
    else if (!(w.oos_total > w.defaults_total)) w.why = "out of sample it did not beat the defaults";
    else if (!(w.oos_total > 0.0)) w.why = "out of sample it did not make money after expenses";
    else if (!(w.p_rw < kMaxP)) w.why = "its daily net does not survive the Romano-Wolf adjustment for the whole grid";
    else w.accept = true;
    if (w.accept) w.why = "beat the defaults out of sample, net of expenses, and survived the adjustment";
    return w;
}

/// config/model_params/<key>.toml, in one step.
inline bool write_params(const fs::path& root, const Grid& g, const Params& p, const WalkForward& w, std::size_t days,
                         const std::string& today) {
    const fs::path dir = root / "config/model_params";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path path = dir / (g.key + ".toml"), tmp = dir / (g.key + ".toml.tmp");
    {
        std::ofstream f(tmp, std::ios::trunc);
        char b[256];
        f << "# " << g.model << ": written by altair_tune on " << today << "\n";
        std::snprintf(b, sizeof b, "# walk-forward over %zu recorded FYERS day(s): out of sample net Rs %.2f (defaults Rs %.2f); "
                                   "Romano-Wolf p %.3f\n", days, w.oos_total, w.defaults_total, w.p_rw);
        f << b << "# Delete this file to go back to the defaults.\n";
        for (const auto& [k, v] : p) f << k << " = " << v << "\n";
        if (!f) return false;
    }
    fs::rename(tmp, path, ec);
    return !ec;
}

} // namespace altair::tune
