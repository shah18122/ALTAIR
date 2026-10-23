// live/desk_trader.hpp -- the Desk trader: reads every model, makes one call,
// demo-trades it.
//
// ONE PLACE THAT LOOKS AT EVERYTHING. At 10:30 each session (after the
// Direction models' 10:15 entries and the OHL rule's 09:15 one), it reads:
//   * every other live model in the engine: its state, its signal, and what it
//     holds in the paper book;
//   * altair_trader's call for this session -- the last row of its NIFTY
//     daily arm (data/verified/altair_trader/days.csv), the expected move
//     against the round trip's cost, made at the previous close;
//   * the variance-premium seller's latest reading
//     (data/verified/vol_premium/signals.csv): implied vol against the HAR
//     forecast.
// Every source is listed on its Live Models card with what it said and
// whether it counted.
//
// THE RULE, WITH NOTHING TUNED. A source VOTES on NIFTY's direction only when
// it has already committed by its own rule:
//   * a live model votes the side of the NIFTY futures it holds, when they
//     all point one way (the Direction models once their magnitude gate
//     opened, the OHL rule's NIFTY leg); options, other underlyings, and the
//     relative-value models (pairs, stat-arb, arbitrage: a NIFTY leg there is
//     a hedge) are noted, not counted;
//   * altair_trader votes when its call is for this session (made at most 4
//     calendar days before, so a weekend or a holiday) and its expected move
//     beats its cost -- its own entry rule.
//   * the variance premium is a volatility view, not a direction: shown only.
// One vote each. The majority, if there is one, is traded: one lot of the
// near NIFTY future, at the touch, through the same paper book and risk
// checks as every model, held to the 15:20 square-off. A tie or no votes:
// no trade, and the card says why.
//
// PAPER ONLY. The real-order bridge (app/live_real_orders.hpp) sends only the
// strategies switched on in the LIVE arm; this one has no switch there.
//
// HONESTY. On NSE closes 2015-2026 no forecasting model beats "no change"
// after costs (ops/research-ideas.md); this combines their calls, it does not
// give them an edge. It is for watching every model in one place.

#pragma once

#include <live/engine.hpp>
#include <live/universe.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace altair::live {

/// altair_trader's latest NIFTY daily call, as read from its files.
struct DeskTraderCall {
    bool ok = false;
    std::string why;                 ///< when not ok
    std::int64_t day = 0;            ///< the close it was made at
    std::string model;               ///< the candidate that made it
    double expected_bp = 0.0, cost_bp = 0.0;
    std::size_t trips = 0;           ///< its NIFTY daily trips in the backtest
    double net = 0.0;                ///< their net, rupees
};

/// The variance-premium seller's latest NIFTY reading.
struct DeskVolReading {
    bool ok = false;
    std::string why;
    std::int64_t day = 0;
    double implied = 0.0, har = 0.0, spread = 0.0;   ///< vol points
};

namespace desk_detail {

[[nodiscard]] inline std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false;
    for (const char c : line) {
        if (c == '"') { quoted = !quoted; continue; }
        if (c == ',' && !quoted) { out.push_back(cur); cur.clear(); continue; }
        if (c != '\r') cur += c;
    }
    out.push_back(cur);
    return out;
}

[[nodiscard]] inline int column(const std::vector<std::string>& head, const std::string& name) {
    for (std::size_t i = 0; i < head.size(); ++i) if (head[i] == name) return static_cast<int>(i);
    return -1;
}

[[nodiscard]] inline bool number(const std::string& s, double& v) {
    char* end = nullptr;
    v = std::strtod(s.c_str(), &end);
    return end != s.c_str() && std::isfinite(v);
}

} // namespace desk_detail

/// Reads days.csv (the call) and trades.csv (its record) from altair_trader's output.
[[nodiscard]] inline DeskTraderCall desk_read_trader(const std::filesystem::path& dir, const std::string& index = "NIFTY") {
    using namespace desk_detail;
    DeskTraderCall c;
    std::ifstream days(dir / "days.csv");
    if (!days) { c.why = "no " + (dir / "days.csv").string() + ": run altair_forecast_curriculum, then altair_trader"; return c; }
    std::string line;
    std::getline(days, line);
    const auto head = split(line);
    const int c_date = column(head, "date"), c_index = column(head, "index"), c_model = column(head, "model"),
              c_exp = column(head, "expected_bp"), c_cost = column(head, "cost_bp");
    if (c_date < 0 || c_index < 0 || c_model < 0 || c_exp < 0 || c_cost < 0) { c.why = "days.csv lacks its columns"; return c; }
    std::vector<std::string> last;
    while (std::getline(days, line)) {
        auto f = split(line);
        if (static_cast<int>(f.size()) > c_cost && f[static_cast<std::size_t>(c_index)] == index) last = std::move(f);
    }
    if (last.empty()) { c.why = "days.csv has no " + index + " daily row"; return c; }
    c.day = parse_day(last[static_cast<std::size_t>(c_date)]);
    c.model = last[static_cast<std::size_t>(c_model)];
    if (c.day == 0 || !number(last[static_cast<std::size_t>(c_exp)], c.expected_bp) || !number(last[static_cast<std::size_t>(c_cost)], c.cost_bp)) {
        c.why = "days.csv's last " + index + " row does not parse";
        return c;
    }
    c.ok = true;
    std::ifstream trades(dir / "trades.csv");
    if (trades && std::getline(trades, line)) {
        const auto th = split(line);
        const int t_index = column(th, "index"), t_arm = column(th, "arm"), t_net = column(th, "net");
        while (t_index >= 0 && t_arm >= 0 && t_net >= 0 && std::getline(trades, line)) {
            const auto f = split(line);
            double v = 0.0;
            if (static_cast<int>(f.size()) <= std::max({t_index, t_arm, t_net})) continue;
            if (f[static_cast<std::size_t>(t_index)] != index || f[static_cast<std::size_t>(t_arm)] != "daily") continue;
            if (number(f[static_cast<std::size_t>(t_net)], v)) { ++c.trips; c.net += v; }
        }
    }
    return c;
}

/// Reads the last `inst` row of altair_vol_premium's signals.csv.
[[nodiscard]] inline DeskVolReading desk_read_vol(const std::filesystem::path& signals_csv, const std::string& inst = "NIFTY") {
    using namespace desk_detail;
    DeskVolReading r;
    std::ifstream in(signals_csv);
    if (!in) { r.why = "no " + signals_csv.string() + ": run altair_vol_premium --source bhavcopy"; return r; }
    std::string line;
    std::getline(in, line);
    const auto head = split(line);
    const int c_i = column(head, "instrument"), c_d = column(head, "date"), c_iv = column(head, "implied_vol"),
              c_h = column(head, "har_vol"), c_s = column(head, "spread");
    if (c_i < 0 || c_d < 0 || c_iv < 0 || c_h < 0 || c_s < 0) { r.why = "signals.csv lacks its columns"; return r; }
    std::vector<std::string> last;
    while (std::getline(in, line)) {
        auto f = split(line);
        if (static_cast<int>(f.size()) > c_s && f[static_cast<std::size_t>(c_i)] == inst) last = std::move(f);
    }
    if (last.empty()) { r.why = "signals.csv has no " + inst + " row"; return r; }
    r.day = parse_day(last[static_cast<std::size_t>(c_d)]);
    r.ok = r.day != 0 && number(last[static_cast<std::size_t>(c_iv)], r.implied) && number(last[static_cast<std::size_t>(c_h)], r.har)
        && number(last[static_cast<std::size_t>(c_s)], r.spread);
    if (!r.ok) r.why = "signals.csv's last " + inst + " row does not parse";
    return r;
}

/// One source, as the Desk trader read it at its decision.
struct DeskSource {
    std::string name, said;
    int vote = 0;          ///< +1 up, -1 down, 0 not counted
    std::string why;       ///< why it counted, or why not
};

class DeskTraderModel final : public LiveModel {
public:
    DeskTraderModel(DeskTraderCall call, DeskVolReading vol) : call_(std::move(call)), vol_(std::move(vol)) {}

    static constexpr int kDecideMinute = 10 * 60 + 30;
    /// An outside call counts for a session at most this many calendar days after it was made.
    static constexpr std::int64_t kFreshDays = 4;

    [[nodiscard]] std::string name() const override { return "Desk trader"; }
    [[nodiscard]] std::string family() const override { return "desk (every model)"; }
    void on_new_day(LiveEngine&) override { decided_ = false; note_.clear(); signal_.clear(); votes_.clear(); sources_.clear(); }

    void on_minute(LiveEngine& e, int m) override {
        if (decided_ || m < kDecideMinute || m > kLastEntryMinute) return;
        decided_ = true;
        if (!e.book().flat(name())) { note_ = "Holding the position resumed from earlier today to the square-off."; e.note_decision(name(), note_); return; }
        LiveDecisionScope log(e, name(), [this] {
            std::string t = note_;
            for (const auto& s : sources_) if (s.vote != 0) t += " | " + s.name + (s.vote > 0 ? " UP" : " DOWN");
            return t;
        });
        const std::string at = live_fmt::hhmm(m);
        sources_ = read_sources(e);
        int up = 0, down = 0;
        for (const auto& s : sources_) { if (s.vote > 0) ++up; else if (s.vote < 0) ++down; }
        votes_ = std::to_string(up) + " up, " + std::to_string(down) + " down, of " + std::to_string(sources_.size()) + " sources";
        if (up == down) {
            note_ = at + ": " + votes_ + (up == 0 ? " -- no source committed to a direction; no trade." : " -- a tie; no trade.");
            return;
        }
        const int dir = up > down ? 1 : -1;
        signal_ = std::string(dir > 0 ? "UP" : "DOWN") + " " + at + " to 15:20";
        const LiveInstrument* fut = e.near_future("NIFTY");
        if (fut == nullptr || e.stale()) { note_ = at + ": majority " + signal_ + ", but no NIFTY future or a stale feed."; return; }
        std::string why;
        const std::string reason = "Desk: " + votes_;
        if (e.book().open(name(), *fut, dir, 1, e.clock_ns(), reason, false, &why)) {
            note_ = at + ": " + votes_ + "; " + (dir > 0 ? "bought" : "sold") + " one lot of " + fut->symbol + ".";
        } else {
            note_ = at + ": majority " + signal_ + ", but the entry was refused: " + why;
        }
    }

    [[nodiscard]] LiveModelView view(const LiveEngine& e) const override {
        LiveModelView v;
        v.name = name();
        v.family = family();
        v.state = !e.book().flat(name()) ? "in position" : (decided_ ? "done today" : "watching");
        v.signal = signal_;
        v.reason = !note_.empty() ? note_
                                  : "At 10:30: reads every model; a source votes only once it has committed by its own rule; "
                                    "the majority trades one NIFTY future lot to 15:20 on paper. A tie or no votes: no trade.";
        const auto shown = sources_.empty() ? read_sources(e) : sources_;
        for (const auto& s : shown)
            v.fields.push_back({s.name, s.said + (s.vote > 0 ? "  → votes UP" : s.vote < 0 ? "  → votes DOWN" : "  → not counted") + (s.why.empty() ? "" : ": " + s.why)});
        if (!votes_.empty()) v.fields.push_back({"votes", votes_});
        return v;
    }

    [[nodiscard]] const std::vector<DeskSource>& sources() const noexcept { return sources_; }

private:
    static constexpr int kLastEntryMinute = 15 * 60 + 10;

    /// What every source says now.
    [[nodiscard]] std::vector<DeskSource> read_sources(const LiveEngine& e) const {
        std::vector<DeskSource> out;
        for (const auto& m : e.models()) {
            if (m.get() == this) continue;
            const LiveModelView mv = m->view(e);
            DeskSource s;
            s.name = mv.name;
            s.said = mv.state + (mv.signal.empty() ? "" : ", " + mv.signal);
            // Relative value (a pair's NIFTY leg is a hedge, not a view) never votes.
            const bool relative = mv.family.find("pairs") != std::string::npos || mv.family.find("stat-arb") != std::string::npos
                               || mv.family.find("arbitrage") != std::string::npos;
            int side = 0;
            bool agree = true, any = false, nifty = false;
            for (const auto& p : e.book().positions()) {
                if (p.model != mv.name) continue;
                any = true;
                if (p.inst.kind != LiveKind::Future || p.inst.underlying != "NIFTY") continue;
                if (nifty && p.side != side) agree = false;
                nifty = true;
                side = p.side;
            }
            if (!any) s.why = "holds nothing";
            else if (relative) s.why = "relative value: its NIFTY leg, if any, is a hedge";
            else if (!nifty) s.why = "holds no NIFTY future: not a view on NIFTY's direction";
            else if (!agree) s.why = "holds NIFTY futures both ways";
            else { s.vote = side > 0 ? 1 : -1; s.why = "holds NIFTY futures"; }
            out.push_back(std::move(s));
        }
        DeskSource t;
        t.name = "altair_trader (daily)";
        if (!call_.ok) {
            t.said = "no call";
            t.why = call_.why;
        } else {
            t.said = call_.model + " expects " + live_fmt::num(call_.expected_bp, 1) + " bp against a " + live_fmt::num(call_.cost_bp, 1)
                   + " bp round trip, at the close of " + day_text(call_.day) + " (backtest: " + std::to_string(call_.trips)
                   + " trips, net Rs " + live_fmt::num(call_.net, 0) + ")";
            const std::int64_t age = e.today() - call_.day;
            if (age < 1 || age > kFreshDays) t.why = "not this session's call: rerun altair_forecast_curriculum and altair_trader after the close";
            else if (!(std::fabs(call_.expected_bp) > call_.cost_bp)) t.why = "the move does not beat its cost";
            else { t.vote = call_.expected_bp > 0 ? 1 : -1; t.why = "beats its cost"; }
        }
        out.push_back(std::move(t));
        DeskSource vp;
        vp.name = "Vol premium (NIFTY)";
        vp.said = !vol_.ok ? "no reading" : "implied " + live_fmt::num(vol_.implied, 2) + " vs HAR " + live_fmt::num(vol_.har, 2) + " vol pts ("
                                               + (vol_.spread > 0 ? "rich" : "cheap") + " by " + live_fmt::num(std::fabs(vol_.spread), 2) + ") on " + day_text(vol_.day);
        vp.why = !vol_.ok ? vol_.why : "a volatility view, not a direction: shown only";
        out.push_back(std::move(vp));
        return out;
    }

    DeskTraderCall call_;
    DeskVolReading vol_;
    bool decided_ = false;
    std::string note_, signal_, votes_;
    std::vector<DeskSource> sources_;
};

} // namespace altair::live
