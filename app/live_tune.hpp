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
// THE OBI TRADER TOO (live/obi_trader.hpp): every trade type on every
// underlying in both modes is its own grid ("obi_banknifty_A_fut" ...), each
// point an engine with its own OBI hub. Its option sales may rest at the ask
// (`passive` 1) or hit the bid (0): the paper book judges a resting sale with
// the queue model of book/queue_fill.hpp, so passive against aggressive IS
// tuned here, for the option sales (Smit, 2026-11-02: futures stay at the
// touch). The engine reads config/model_params/obi_*.toml at its next start.
//
// THE STATS SHEET (live/trade_stats.hpp) for every setting -- hit rate,
// expectancy, Sharpe, drawdown, cost share and the 1 s / 10 s / 60 s markouts
// of every entry -- goes to data/live/tune/<key>_stats.csv beside the per-day
// table. A latency other than the tape's (--latency-ms) re-runs everything at
// it: how much of a result survives a slower (or a co-located) path.
//
// NOT TUNED HERE: the straddle sellers. They decide once a day on forecasts
// from files that are not on the tape; a handful of recorded days says nothing
// about a once-a-day trade. They are judged on the recorded chains instead.

#pragma once

#include <live/arbitrage.hpp>
#include <live/engine.hpp>
#include <live/feed_consumer.hpp>
#include <live/latency.hpp>
#include <live/obi_quoter.hpp>
#include <live/obi_trader.hpp>
#include <live/obi_underlyings.hpp>
#include <live/option_arb.hpp>
#include <live/report.hpp>
#include <live/tape.hpp>
#include <live/threshold.hpp>
#include <live/trade_stats.hpp>
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
    /// When set, a point's engine gets these models instead of make()'s one
    /// (the OBI trader needs its hub in front of it); `model` names the one judged.
    std::function<std::vector<std::unique_ptr<live::LiveModel>>(const Params&)> make_all;
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

/// The arbitrage: the margin over the two fills' expenses, and how long before
/// the same stock is entered again.
[[nodiscard]] inline Grid arbitrage_grid() {
    const live::LiveCrossArbRule d;
    return detail::cross("arbitrage", "Cross-exchange arbitrage",
                         {{"min_profit_bps", {0.5, 1.0, 2.0, 3.0, 5.0}}, {"cooldown_s", {1.0, 5.0, 15.0}}},
                         {{"min_profit_bps", d.min_profit_bps}, {"cooldown_s", static_cast<double>(d.cooldown_ns) / 1e9}},
                         [](const Params& p) {
                             live::LiveCrossArbRule r;
                             r.min_profit_bps = p.at("min_profit_bps");
                             r.cooldown_ns = static_cast<std::int64_t>(p.at("cooldown_s") * 1e9);
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

// ---- the OBI trader ----------------------------------------------------------------

/// "obi_banknifty_A_fut": one trade type, one underlying, one mode.
[[nodiscard]] inline std::string obi_key(const std::string& under, live::ObiType t, live::ObiMode m) {
    std::string u = under;
    for (char& c : u) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return "obi_" + u + "_" + live::obi_type_letter(t) + (m == live::ObiMode::Futures ? "_fut" : "_opt");
}

/// The rules of one OBI trader with `p` applied (keys a grid does not carry
/// keep the rule's value). `target_scale` scales the underlying's target and
/// stop together (config/obi_underlyings.csv stays the base).
inline void apply_obi_params(live::ObiTraderRules& r, const Params& p) {
    const auto get = [&](const char* k, double& out) {
        if (const auto it = p.find(k); it != p.end() && std::isfinite(it->second)) out = it->second;
    };
    get("obi_entry", r.obi_entry);
    double v = static_cast<double>(r.min_persist_ns) / 1e6;
    get("persist_ms", v);
    r.min_persist_ns = static_cast<std::int64_t>(v * 1e6);
    double scale = 1.0;
    get("target_scale", scale);
    r.target_pts *= scale;
    r.stop_pts *= scale;
    v = static_cast<double>(r.max_hold_ns) / 60e9;
    get("hold_min", v);
    r.max_hold_ns = static_cast<std::int64_t>(v * 60e9);
    get("option_target_pct", r.option_target_pct);
    get("option_stop_pct", r.option_stop_pct);
    v = r.passive_options ? 1.0 : 0.0;
    get("passive", v);
    r.passive_options = v >= 0.5;
}

/// Today's rules for one OBI trader, before any tuning.
[[nodiscard]] inline live::ObiTraderRules obi_base_rules(const live::ObiUnderlying& u, live::ObiType t, live::ObiMode m) {
    live::ObiTraderRules r;
    r.under = u.symbol;
    r.type = t;
    r.mode = m;
    r.target_pts = u.target_pts;
    r.stop_pts = u.stop_pts;
    r.safety_pts = u.safety_pts;
    return r;
}

/// The grids of every OBI trader on `unders`. A and B carry the imbalance
/// threshold and how long it must hold; C and D (the engine's own flags) the
/// size of the target and the time stop. Option sales add resting (passive 1)
/// against hitting the bid (0).
[[nodiscard]] inline std::vector<Grid> obi_grids(const std::vector<live::ObiUnderlying>& unders) {
    std::vector<Grid> out;
    for (const auto& u : unders)
        for (const live::ObiType t : live::kObiTypes)
            for (const live::ObiMode m : {live::ObiMode::Futures, live::ObiMode::SellOptions}) {
                const live::ObiTraderRules d = obi_base_rules(u, t, m);
                const bool ab = t == live::ObiType::A || t == live::ObiType::B;
                const bool opt = m == live::ObiMode::SellOptions;
                std::vector<std::pair<std::string, std::vector<double>>> axes;
                Params def;
                if (ab) {
                    axes.push_back({"obi_entry", {0.20, 0.30, 0.40}});
                    axes.push_back({"persist_ms", {250.0, 500.0, 1000.0}});
                    def["obi_entry"] = d.obi_entry;
                    def["persist_ms"] = static_cast<double>(d.min_persist_ns) / 1e6;
                } else {
                    axes.push_back({"hold_min", {1.0, 5.0, 15.0}});
                    def["hold_min"] = static_cast<double>(d.max_hold_ns) / 60e9;
                }
                if (opt) {
                    axes.push_back({"option_target_pct", {0.10, 0.15, 0.25}});
                    axes.push_back({"passive", {0.0, 1.0}});
                    def["option_target_pct"] = d.option_target_pct;
                    def["passive"] = d.passive_options ? 1.0 : 0.0;
                } else {
                    axes.push_back({"target_scale", {0.6, 1.0, 1.5}});
                    def["target_scale"] = 1.0;
                }
                const std::string model = live::LiveObiTraderModel(d, nullptr).name();
                Grid g = detail::cross(obi_key(u.symbol, t, m), model, axes, def, nullptr);
                g.make_all = [d](const Params& p) {
                    live::ObiTraderRules r = d;
                    apply_obi_params(r, p);
                    live::ObiHub::Config hc;
                    hc.unders = {r.under};
                    auto hub = std::make_unique<live::ObiHub>(hc);
                    const live::ObiHub* h = hub.get();
                    std::vector<std::unique_ptr<live::LiveModel>> v;
                    v.push_back(std::move(hub));
                    v.push_back(std::make_unique<live::LiveObiTraderModel>(r, h));
                    return v;
                };
                out.push_back(std::move(g));
            }
    return out;
}

// ---- the OBI option quoter ---------------------------------------------------------

/// "obiq_banknifty": the quoter's settings key.
[[nodiscard]] inline std::string quoter_key(const std::string& under) {
    std::string k = "obiq_";
    for (const char c : under) k.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return k;
}

/// `p`'s keys onto the quoter's rules (the rest stay).
inline void apply_quoter_params(live::ObiQuoterRules& r, const Params& p) {
    for (const auto& [k, v] : p) {
        if (k == "pull_obi") r.pull_obi = v;
        else if (k == "target_pct") r.target_pct = v;
        else if (k == "strikes_otm") r.strikes_otm = static_cast<int>(std::lround(v));
    }
}

/// One grid an underlying: where to pull a leg x the target x how far out.
[[nodiscard]] inline std::vector<Grid> quoter_grids(const std::vector<live::ObiUnderlying>& unders) {
    std::vector<Grid> out;
    for (const auto& u : unders) {
        live::ObiQuoterRules d;
        d.under = u.symbol;
        const std::vector<std::pair<std::string, std::vector<double>>> axes{
            {"pull_obi", {0.20, 0.30, 0.50}}, {"target_pct", {0.05, 0.10, 0.15}}, {"strikes_otm", {1.0, 2.0, 3.0}}};
        const Params def{{"pull_obi", d.pull_obi}, {"target_pct", d.target_pct}, {"strikes_otm", static_cast<double>(d.strikes_otm)}};
        Grid g = detail::cross(quoter_key(u.symbol), live::LiveObiQuoterModel(d, nullptr).name(), axes, def, nullptr);
        g.make_all = [d](const Params& p) {
            live::ObiQuoterRules r = d;
            apply_quoter_params(r, p);
            live::ObiHub::Config hc;
            hc.unders = {r.under};
            auto hub = std::make_unique<live::ObiHub>(hc);
            const live::ObiHub* h = hub.get();
            std::vector<std::unique_ptr<live::LiveModel>> v;
            v.push_back(std::move(hub));
            v.push_back(std::make_unique<live::LiveObiQuoterModel>(r, h));
            return v;
        };
        out.push_back(std::move(g));
    }
    return out;
}

/// BANKNIFTY and NIFTY (live/obi_underlyings.hpp) and the stocks of config/obi_underlyings.csv.
[[nodiscard]] inline std::vector<live::ObiUnderlying> obi_underlyings(const fs::path& root) {
    return live::obi_trade_underlyings((root / "config/obi_underlyings.csv").string());
}

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
    std::vector<live::TradeRecord> rec;   ///< per grid point: the trades and markouts (daily_net left empty)
};

/// Feed lag on a tape: the price service's receive time minus the exchange's
/// time on each trade print (FYERS stamps prints to the second, so this is
/// coarse: an upper bound good to about a second).
struct FeedLag {
    live::LatencyHistogram hist;
    std::size_t negative = 0;   ///< prints stamped after they arrived (clock skew)
};

namespace detail {
/// An entry fill waiting for its markouts.
struct Pending {
    std::uint32_t token = 0;
    int side = 0;
    double price = 0.0;
    std::int64_t qty = 0, ns = 0;
    std::size_t point = 0;
    int done = 0;   ///< horizons taken (kMarkoutNs order)
};
} // namespace detail

/// Replay `tape` once through every point of every grid in `grids` (one
/// engine a point); `out[i]` gets grid i's day. `latency_ns` >= 0 replaces the
/// tape's own. `lag` (optional) collects the feed lag. False (with `err`) when
/// the tape cannot be read.
[[nodiscard]] inline bool run_tape_group(const fs::path& tape, const std::vector<const Grid*>& grids, const live::LiveCostFn& cost,
                                         std::vector<DayRun>& out, std::string& err, std::int64_t latency_ns = -1,
                                         FeedLag* lag = nullptr) {
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
    std::string day;
    if (args != nullptr) {
        std::istringstream as(*args);
        for (std::string line; std::getline(as, line);) {
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            if (k == "date") day = v;
            else if (k == "latency_ns") policy.latency_ns = std::atoll(v.c_str());
            else if (k == "max_quote_age_ns") policy.max_quote_age_ns = std::atoll(v.c_str());
            else if (k == "entry_timeout_ns") policy.entry_timeout_ns = std::atoll(v.c_str());
        }
    }
    if (latency_ns >= 0) policy.latency_ns = latency_ns;
    out.assign(grids.size(), DayRun{});
    struct Slot {
        std::size_t grid = 0, point = 0;
        std::unique_ptr<live::LiveEngine> engine;
        std::unique_ptr<live::LiveFeedConsumer> consumer;
        std::size_t fills_seen = 0;
        std::vector<detail::Pending> pending;
    };
    std::vector<Slot> slots;
    for (std::size_t gi = 0; gi < grids.size(); ++gi) {
        const Grid& g = *grids[gi];
        out[gi].day = day;
        out[gi].tape = tape.string();
        out[gi].rec.assign(g.points.size(), live::TradeRecord{});
        for (std::size_t pi = 0; pi < g.points.size(); ++pi) {
            Slot sl;
            sl.grid = gi;
            sl.point = pi;
            sl.engine = std::make_unique<live::LiveEngine>(universe, cost, policy);
            if (g.make_all) for (auto& m : g.make_all(g.points[pi])) sl.engine->add_model(std::move(m));
            else sl.engine->add_model(g.make(g.points[pi]));
            sl.consumer = std::make_unique<live::LiveFeedConsumer>(*sl.engine);
            slots.push_back(std::move(sl));
        }
    }
    // The markouts: every entry fill of the judged model, the mid 1 s, 10 s
    // and 60 s later on the feed's clock, in the trade's favour.
    const auto markouts = [&](Slot& sl, std::int64_t now) {
        const auto& fills = sl.engine->book().fills();
        const std::string& model = grids[sl.grid]->model;
        for (; sl.fills_seen < fills.size(); ++sl.fills_seen) {
            const auto& f = fills[sl.fills_seen];
            if (f.role == live::LiveFillRole::Open && f.model == model)
                sl.pending.push_back(detail::Pending{f.token, f.side, f.price, f.qty, f.ns, sl.point, 0});
        }
        if (sl.pending.empty()) return;
        auto& r = out[sl.grid].rec[sl.point];
        for (auto& pd : sl.pending)
            while (pd.done < 3 && now >= pd.ns + live::kMarkoutNs[static_cast<std::size_t>(pd.done)]) {
                const live::LiveTop t = sl.engine->top(pd.token);
                if (t.bid > 0 && t.ask > 0) {
                    const double mid = static_cast<double>(t.bid + t.ask) / 200.0;
                    r.markout_sum[static_cast<std::size_t>(pd.done)] += pd.side * (mid - pd.price) * static_cast<double>(pd.qty);
                    ++r.markout_n[static_cast<std::size_t>(pd.done)];
                }
                ++pd.done;
            }
        std::erase_if(sl.pending, [](const detail::Pending& pd) { return pd.done >= 3; });
    };
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
                const std::uint8_t* body = buf.data() + at + kFrameHeaderBytes;
                if (lag != nullptr && h->topic == kTopicTrades && h->kind == FrameKind::Delta)
                    if (const auto d = decode_price(body, h->payload_len); d && d->payload.exchange_ts_ns > 0) {
                        const std::int64_t l = h->engine_time_ns - d->payload.exchange_ts_ns;
                        if (l >= 0) lag->hist.record(l); else ++lag->negative;
                    }
                for (auto& sl : slots) {
                    (void)sl.consumer->on_frame(*h, body);
                    markouts(sl, std::max(h->engine_time_ns, sl.engine->clock_ns()));
                }
                at += need;
            }
            if (at > 0) buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(at));
            break;
        }
        case live::TapeKind::Connected:
            buf.clear();
            for (auto& sl : slots) sl.consumer->on_reconnect();
            break;
        case live::TapeKind::Disconnected: buf.clear(); break;
        case live::TapeKind::Stale: for (auto& sl : slots) sl.engine->set_stale(rec.text() == "1"); break;
        case live::TapeKind::Kill: for (auto& sl : slots) sl.engine->set_kill(rec.text() == "1"); break;
        case live::TapeKind::Halt: for (auto& sl : slots) sl.engine->set_halt(rec.text()); break;
        case live::TapeKind::Watchdog: for (auto& sl : slots) (void)sl.engine->watchdog(rec.wall_ns); break;
        case live::TapeKind::Start: break;
        }
    }
    for (auto& d : out) {
        const std::size_t n = d.rec.size();
        d.net.assign(n, 0.0);
        d.gross.assign(n, 0.0);
        d.trips.assign(n, 0);
    }
    for (const auto& sl : slots) {
        const Grid& g = *grids[sl.grid];
        DayRun& d = out[sl.grid];
        live::TradeRecord& r = d.rec[sl.point];
        double net = 0.0, gross = 0.0;
        int trips = 0;
        for (const auto& t : sl.engine->book().trades()) {
            if (t.model != g.model) continue;
            net += t.net;   // NaN once any fill was unpriced
            gross += t.gross;
            ++trips;
            r.trade_net.push_back(t.net);
            r.hold_s.push_back(static_cast<double>(t.exit_ns - t.entry_ns) / 1e9);
            r.gross += t.gross;
            r.expenses += t.expenses;
        }
        d.net[sl.point] = net;
        d.gross[sl.point] = gross;
        d.trips[sl.point] = trips;
        d.simulated = d.simulated || sl.engine->simulated();
        std::size_t open = 0;
        for (const auto& p : sl.engine->book().positions()) open += p.model == g.model ? 1u : 0u;
        d.open_at_end = std::max(d.open_at_end, open);
    }
    return true;
}

/// Replay `tape` through one engine per grid point. False (with `err`) when
/// the tape cannot be read.
[[nodiscard]] inline bool run_tape(const fs::path& tape, const Grid& g, const live::LiveCostFn& cost, DayRun& out,
                                   std::string& err) {
    std::vector<DayRun> v;
    if (!run_tape_group(tape, {&g}, cost, v, err)) return false;
    out = std::move(v.front());
    return true;
}

/// Every session tape under `root`, oldest first: the engine's
/// (data/live/tapes/*.tape) and the recorder's
/// (data/store/tapes/<YYYY-MM-DD>/*.tape). Both start with the session's date.
[[nodiscard]] inline std::vector<fs::path> find_tapes(const fs::path& root) {
    std::vector<fs::path> tapes;
    std::error_code ec;
    for (const auto& de : fs::directory_iterator(root / "data/live/tapes", ec))
        if (de.path().extension() == ".tape") tapes.push_back(de.path());
    for (const auto& de : fs::recursive_directory_iterator(root / "data/store/tapes", ec))
        if (de.is_regular_file(ec) && de.path().extension() == ".tape") tapes.push_back(de.path());
    std::sort(tapes.begin(), tapes.end(), [](const fs::path& a, const fs::path& b) {
        return a.filename().string() < b.filename().string();
    });
    return tapes;
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
