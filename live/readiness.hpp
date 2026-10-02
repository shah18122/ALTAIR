// live/readiness.hpp -- is the paper book ready to be trusted with money?
// Gates, each with the evidence it was judged on.
//
// Readiness is an ASSESSMENT. Nothing here enables an order: live order
// submission stays disabled in this tree whatever the verdict. What the
// verdict says is whether the evidence a human would need before deciding to
// enable it exists, and what it shows.
//
// Only LIVE sessions count (sessions.csv, source LIVE, not a replay): a SIM
// day proves the plumbing, not the market.
//
// OPERATIONAL (the whole engine):
//   sessions     enough LIVE sessions on record;
//   clean exits  every one ended with its rows written (exit code 0);
//   feed         trade frames lost are a negligible share, and few sessions
//                saw a gap at all;
//   latency      the worst session's p99, frame to processed and for the
//                frames that ran the models, inside the bounds;
//   halts        none in the most recent sessions;
//   tapes        enough sessions recorded in full;
//   replays      enough recorded sessions replayed IDENTICALLY, none not;
//   bundles      every session's model bundle is on disk.
// ECONOMIC (per model, on LIVE days only):
//   days         enough marked days of paper;
//   priced       no fill without expenses, few unmarked days;
//   edge         mean daily net above zero after Romano-Wolf, interval clear of zero;
//   stress       still above zero with expenses x1.5 and 2 bp slippage;
//   drawdown     the worst drawdown a bounded share of the capital used.
// And for everything: the charges checked against a contract note, and the
// capital measured with the exchange's SPAN -- which this build cannot do
// (live/margin.hpp is an estimate), so that gate stays shut until it can.

#pragma once

#include <live/report.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace altair::live::readiness {

enum class GateStatus : std::uint8_t { Pass, Fail, Unknown };
[[nodiscard]] inline const char* status_text(GateStatus s) noexcept {
    return s == GateStatus::Pass ? "PASS" : s == GateStatus::Fail ? "FAIL" : "NO EVIDENCE";
}

struct ReadinessGate {
    std::string scope;      ///< "operational", "economic", or a model's name
    std::string name;
    GateStatus status = GateStatus::Unknown;
    std::string evidence;   ///< what was measured
    std::string bar;        ///< what it had to meet
};

struct ReadinessBars {
    std::size_t min_sessions = 20;
    double max_missed_share = 1e-4;          ///< trade frames lost / frames
    double max_gap_session_share = 0.10;     ///< sessions with any gap
    double max_frame_p99_us = 5'000.0;
    double max_decision_p99_us = 50'000.0;
    std::size_t halt_free_recent = 10;
    std::size_t min_tapes = 5;
    std::size_t min_replays = 5;
    std::size_t min_days = 60;
    double max_unmarked_share = 0.05;
    double alpha = 0.05;
    double max_drawdown_share = 0.20;        ///< of peak capital
};

struct SessionRecord {
    std::string date, source, tape, bundle;
    bool replay = false, tape_complete = false, kill_seen = false;
    std::uint64_t frames = 0, missed = 0, gaps = 0, halts = 0;
    double frame_p99_us = report::kNaN, decision_p99_us = report::kNaN;
    int exit_code = 0;
    std::int64_t started_unix = 0;
};

[[nodiscard]] inline std::vector<SessionRecord> read_sessions(const std::string& path) {
    std::vector<SessionRecord> out;
    std::ifstream in(path);
    std::string line;
    if (!std::getline(in, line)) return out;
    while (std::getline(in, line)) {
        const auto c = report::csv_fields(line);
        if (c.size() < 23) continue;
        SessionRecord s;
        s.date = c[0];
        s.started_unix = std::atoll(c[1].c_str());
        s.source = c[3];
        s.replay = c[4] == "1";
        s.frames = std::strtoull(c[5].c_str(), nullptr, 10);
        s.missed = std::strtoull(c[6].c_str(), nullptr, 10);
        s.gaps = std::strtoull(c[7].c_str(), nullptr, 10);
        s.halts = std::strtoull(c[9].c_str(), nullptr, 10);
        s.kill_seen = c[10] == "1";
        s.frame_p99_us = c[12].empty() ? report::kNaN : std::atof(c[12].c_str());
        s.decision_p99_us = c[14].empty() ? report::kNaN : std::atof(c[14].c_str());
        s.tape = c[19];
        s.tape_complete = c[20] == "1";
        s.bundle = c[21];
        s.exit_code = std::atoi(c[22].c_str());
        out.push_back(s);
    }
    return out;
}

/// A replay's verdict (data/live/replay_checks/*.json), read for the two
/// fields the gate needs.
struct ReplayVerdict {
    std::string tape, source;
    bool identical = false, bundle_match = false;
};
[[nodiscard]] inline bool json_flag(const std::string& text, const std::string& key) {
    const auto at = text.find("\"" + key + "\": ");
    return at != std::string::npos && text.compare(at + key.size() + 4, 4, "true") == 0;
}
[[nodiscard]] inline std::string json_text(const std::string& text, const std::string& key) {
    const auto at = text.find("\"" + key + "\": \"");
    if (at == std::string::npos) return {};
    const auto from = at + key.size() + 5;
    const auto to = text.find('"', from);
    return to == std::string::npos ? std::string() : text.substr(from, to - from);
}

[[nodiscard]] inline std::string fmt(double v, int digits = 1) {
    if (!std::isfinite(v)) return "n/a";
    char b[64];
    std::snprintf(b, sizeof b, "%.*f", digits, v);
    return b;
}

/// The operational gates over the LIVE sessions (in file order: oldest first).
[[nodiscard]] inline std::vector<ReadinessGate> operational(const std::vector<SessionRecord>& all, const std::vector<ReplayVerdict>& replays,
                                                   const std::set<std::string>& bundles_on_disk, const ReadinessBars& b) {
    std::vector<SessionRecord> s;
    for (const auto& x : all) if (x.source == "LIVE" && !x.replay) s.push_back(x);
    std::vector<ReadinessGate> g;
    const auto add = [&](std::string name, GateStatus st, std::string ev, std::string bar) {
        g.push_back(ReadinessGate{"operational", std::move(name), st, std::move(ev), std::move(bar)});
    };
    add("sessions", s.size() >= b.min_sessions ? GateStatus::Pass : GateStatus::Fail, std::to_string(s.size()) + " LIVE session(s) on record",
        ">= " + std::to_string(b.min_sessions));
    if (s.empty()) {
        for (const char* n : {"clean exits", "feed", "latency", "halts", "tapes", "bundles"})
            add(n, GateStatus::Unknown, "no LIVE session yet", "");
    } else {
        std::size_t bad = 0, gapped = 0, tapes = 0, bundles = 0;
        std::uint64_t frames = 0, missed = 0;
        double worst_frame = 0.0, worst_decision = 0.0;
        bool latency_known = true;
        for (const auto& x : s) {
            bad += x.exit_code != 0 ? 1u : 0u;
            gapped += x.gaps > 0 ? 1u : 0u;
            frames += x.frames;
            missed += x.missed;
            if (!std::isfinite(x.frame_p99_us) || !std::isfinite(x.decision_p99_us)) latency_known = false;
            else { worst_frame = std::max(worst_frame, x.frame_p99_us); worst_decision = std::max(worst_decision, x.decision_p99_us); }
            tapes += !x.tape.empty() && x.tape_complete ? 1u : 0u;
            bundles += bundles_on_disk.count(x.bundle) != 0 ? 1u : 0u;
        }
        add("clean exits", bad == 0 ? GateStatus::Pass : GateStatus::Fail, std::to_string(bad) + " session(s) ended with unwritten rows or an error",
            "0");
        const double share = frames > 0 ? static_cast<double>(missed) / static_cast<double>(frames) : report::kNaN;
        const double gshare = static_cast<double>(gapped) / static_cast<double>(s.size());
        add("feed", std::isfinite(share) && share <= b.max_missed_share && gshare <= b.max_gap_session_share ? GateStatus::Pass : GateStatus::Fail,
            std::to_string(missed) + " of " + std::to_string(frames) + " trade frames lost; " + std::to_string(gapped) + " session(s) with a gap",
            "lost <= " + fmt(b.max_missed_share * 100.0, 3) + " %, sessions with a gap <= " + fmt(b.max_gap_session_share * 100.0, 0) + " %");
        add("latency", !latency_known ? GateStatus::Unknown
                       : worst_frame <= b.max_frame_p99_us && worst_decision <= b.max_decision_p99_us ? GateStatus::Pass : GateStatus::Fail,
            "worst session p99: frame " + fmt(worst_frame) + " us, decision " + fmt(worst_decision) + " us",
            "frame <= " + fmt(b.max_frame_p99_us, 0) + " us, decision <= " + fmt(b.max_decision_p99_us, 0) + " us");
        std::size_t recent_halts = 0;
        const std::size_t from = s.size() > b.halt_free_recent ? s.size() - b.halt_free_recent : 0;
        for (std::size_t i = from; i < s.size(); ++i) recent_halts += s[i].halts > 0 || s[i].kill_seen ? 1u : 0u;
        add("halts", recent_halts == 0 ? GateStatus::Pass : GateStatus::Fail,
            std::to_string(recent_halts) + " of the last " + std::to_string(s.size() - from) + " session(s) halted or saw a kill request",
            "0");
        add("tapes", tapes >= b.min_tapes ? GateStatus::Pass : GateStatus::Fail, std::to_string(tapes) + " session(s) recorded in full",
            ">= " + std::to_string(b.min_tapes));
        add("bundles", bundles == s.size() ? GateStatus::Pass : GateStatus::Fail,
            std::to_string(bundles) + " of " + std::to_string(s.size()) + " sessions' bundles on disk", "all");
    }
    std::size_t same = 0, differ = 0;
    for (const auto& r : replays) {
        if (r.source != "LIVE") continue;
        (r.identical && r.bundle_match ? same : differ) += 1;
    }
    add("replays", differ == 0 && same >= b.min_replays ? GateStatus::Pass : (differ > 0 ? GateStatus::Fail : (same == 0 ? GateStatus::Unknown : GateStatus::Fail)),
        std::to_string(same) + " LIVE session(s) replayed identically, " + std::to_string(differ) + " not",
        ">= " + std::to_string(b.min_replays) + " identical, none different");
    return g;
}

/// The economic gates for one model (or the book).
[[nodiscard]] inline std::vector<ReadinessGate> economic(const std::string& model, const report::DailyStats& net, const report::DailyStats& stressed,
                                                std::size_t days, std::size_t unmarked, bool unpriced, double peak_capital, const ReadinessBars& b) {
    std::vector<ReadinessGate> g;
    const auto add = [&](std::string name, GateStatus st, std::string ev, std::string bar) {
        g.push_back(ReadinessGate{model, std::move(name), st, std::move(ev), std::move(bar)});
    };
    add("days", net.days >= b.min_days ? GateStatus::Pass : GateStatus::Fail, std::to_string(net.days) + " marked LIVE day(s)",
        ">= " + std::to_string(b.min_days));
    const double um = days > 0 ? static_cast<double>(unmarked) / static_cast<double>(days) : 0.0;
    add("priced", !unpriced && um <= b.max_unmarked_share ? GateStatus::Pass : GateStatus::Fail,
        std::string(unpriced ? "some fill has no expenses; " : "every fill priced; ") + std::to_string(unmarked) + " unmarked day(s)",
        "no unpriced fill, unmarked <= " + fmt(b.max_unmarked_share * 100.0, 0) + " %");
    const bool tested = std::isfinite(net.p_rw) && std::isfinite(net.ci_lo);
    add("edge", !tested ? GateStatus::Unknown : net.p_rw <= b.alpha && net.ci_lo > 0.0 ? GateStatus::Pass : GateStatus::Fail,
        "mean " + fmt(net.mean, 0) + " a day, 95 % CI " + fmt(net.ci_lo, 0) + " to " + fmt(net.ci_hi, 0) + ", p (Romano-Wolf) " + fmt(net.p_rw, 3),
        "p <= " + fmt(b.alpha, 2) + " and the interval above zero");
    const bool stested = std::isfinite(stressed.p_rw) && std::isfinite(stressed.ci_lo);
    add("stress", !stested ? GateStatus::Unknown : stressed.ci_lo > 0.0 && stressed.p_rw <= b.alpha ? GateStatus::Pass : GateStatus::Fail,
        "stressed mean " + fmt(stressed.mean, 0) + ", CI low " + fmt(stressed.ci_lo, 0) + ", p " + fmt(stressed.p_rw, 3),
        "still above zero with expenses x1.5 and 2 bp slippage");
    const bool dd_known = std::isfinite(net.max_drawdown) && std::isfinite(peak_capital) && peak_capital > 0.0;
    add("drawdown", !dd_known ? GateStatus::Unknown : net.max_drawdown <= b.max_drawdown_share * peak_capital ? GateStatus::Pass : GateStatus::Fail,
        "max drawdown " + fmt(net.max_drawdown, 0) + " on peak capital " + fmt(peak_capital, 0),
        "<= " + fmt(b.max_drawdown_share * 100.0, 0) + " % of capital");
    return g;
}

} // namespace altair::live::readiness
