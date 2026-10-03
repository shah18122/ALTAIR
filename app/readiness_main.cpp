// altair_readiness -- the operational and economic gates before any money,
// each with its evidence (live/readiness.hpp says which and why).
//
//   altair_readiness [--root DIR] [--min-sessions N] [--min-days N] [--min-tapes N] [--min-replays N]
//
// Reads data/live/sessions.csv, data/live/replay_checks/, data/live/bundles/,
// the paper ledger (LIVE days only) and data/verified/charges_check.json;
// writes data/live/report/readiness.{md,json}. Exit 0 only when every gate a
// first live pilot needs is passed. It enables nothing: live order submission
// stays disabled in this tree whatever the verdict.

#include <live/readiness.hpp>
#include <live/report.hpp>
#include <live/universe.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace rp = altair::live::report;
namespace rd = altair::live::readiness;

std::string file_text(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream o;
    o << f.rdbuf();
    return o.str();
}
std::string jstr(const std::string& s) {
    std::string o = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (static_cast<unsigned char>(c) < 0x20) o += ' ';
        else o += c;
    }
    return o + "\"";
}

void usage(const char* exe) {
    std::printf(
        "  The gates before any money, each with its evidence. Enables nothing.\n\n"
        "    %s [--root DIR] [--min-sessions N] [--min-days N] [--min-tapes N] [--min-replays N]\n\n"
        "    --root          the tree holding data/ and config/ (default: the source tree)\n"
        "    --min-sessions  LIVE sessions on record (default 20)\n"
        "    --min-days      marked LIVE paper days per model (default 60)\n"
        "    --min-tapes     sessions recorded in full (default 5)\n"
        "    --min-replays   recorded LIVE sessions replayed identically (default 5)\n",
        exe);
}

}  // namespace

int main(int argc, char** argv) {
    fs::path root = ALTAIR_SOURCE_DIR;
    rd::ReadinessBars bars;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has = i + 1 < argc;
        if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
        if (a == "--root" && has) { root = argv[++i]; continue; }
        if (a == "--min-sessions" && has) { bars.min_sessions = static_cast<std::size_t>(std::atoll(argv[++i])); continue; }
        if (a == "--min-days" && has) { bars.min_days = static_cast<std::size_t>(std::atoll(argv[++i])); continue; }
        if (a == "--min-tapes" && has) { bars.min_tapes = static_cast<std::size_t>(std::atoll(argv[++i])); continue; }
        if (a == "--min-replays" && has) { bars.min_replays = static_cast<std::size_t>(std::atoll(argv[++i])); continue; }
        std::printf("unknown argument %s\n", a.c_str());
        usage(argv[0]);
        return 2;
    }
    const fs::path live = root / "data/live", paper = live / "paper";
    std::error_code ec;

    // ---- operational ----------------------------------------------------------
    const auto sessions = rd::read_sessions((live / "sessions.csv").string());
    std::vector<rd::ReplayVerdict> replays;
    for (const auto& e : fs::directory_iterator(live / "replay_checks", ec)) {
        if (e.path().extension() != ".json") continue;
        const std::string t = file_text(e.path());
        replays.push_back(rd::ReplayVerdict{rd::json_text(t, "tape"), rd::json_text(t, "source"), rd::json_flag(t, "identical"),
                                          rd::json_flag(t, "bundle_match")});
    }
    std::set<std::string> bundles;
    for (const auto& e : fs::recursive_directory_iterator(live / "bundles", ec)) {
        const std::string n = e.path().filename().string();
        if (n.rfind("bundle-", 0) == 0 && e.path().extension() == ".json") bundles.insert(n.substr(7, n.size() - 12));
    }
    std::vector<rd::ReadinessGate> gates = rd::operational(sessions, replays, bundles, bars);

    // ---- economic: the paper book on LIVE days only ---------------------------
    std::set<std::int64_t> live_days;
    for (const auto& s : sessions) if (s.source == "LIVE" && !s.replay) live_days.insert(altair::live::parse_day(s.date));
    std::size_t bad = 0;
    std::vector<rp::JournalFill> fills;
    for (auto& f : rp::read_journal((paper / "journal.csv").string(), &bad))
        if (live_days.count(rp::day_of_ns(f.ns)) != 0) fills.push_back(std::move(f));
    const auto marks = rp::read_marks((paper / "marks.csv").string());
    const auto margin = rp::read_margin((paper / "margin.csv").string());
    auto series = rp::daily_mtm(fills, marks, live_days);
    {
        // Every model that decided on a LIVE day is in the family tested.
        std::set<std::string> seen;
        for (const auto& s : series) seen.insert(s.model);
        std::ifstream in(paper / "decisions.csv");
        std::string line;
        std::getline(in, line);
        while (std::getline(in, line)) {
            const auto c = rp::csv_fields(line);
            if (c.size() < 3 || c[2].empty() || c[2] == "engine" || seen.count(c[2]) != 0) continue;
            if (live_days.count(rp::day_of_ns(std::atoll(c[1].c_str()))) == 0) continue;
            seen.insert(c[2]);
            rp::ModelDaily z;
            z.model = c[2];
            for (const std::int64_t d : live_days) { rp::DayPnl day; day.day = d; z.days.push_back(day); }
            series.push_back(std::move(z));
        }
    }
    std::vector<std::vector<double>> nets, stress;
    std::vector<rp::DailyStats> ns, ss;
    std::vector<double> peaks;
    for (const auto& s : series) {
        std::vector<double> n, st;
        double peak = 0.0;
        for (const auto& d : s.days) {
            n.push_back(d.net());
            st.push_back(d.gross - 1.5 * d.expenses - 2.0 * 1e-4 * d.turnover);
            const auto pk = margin.peak.find({d.day, s.model});
            if (pk != margin.peak.end()) peak = std::isfinite(pk->second) && std::isfinite(peak) ? std::max(peak, pk->second) : rp::kNaN;
        }
        ns.push_back(rp::describe(n));
        ss.push_back(rp::describe(st));
        nets.push_back(std::move(n));
        stress.push_back(std::move(st));
        peaks.push_back(peak);
    }
    rp::bootstrap(nets, ns, 20261002, 4000, 5);
    rp::bootstrap(stress, ss, 20261003, 4000, 5);

    // Global economic gates.
    {
        const fs::path cc = root / "data/verified/charges_check.json";
        const std::string t = file_text(cc);
        rd::ReadinessGate g{"economic", "charges", rd::GateStatus::Unknown, "no contract note checked (altair_charges_check --note ...)",
                   "a contract note agrees with config/charges.toml, head by head"};
        if (!t.empty()) {
            g.status = rd::json_flag(t, "ok") ? rd::GateStatus::Pass : rd::GateStatus::Fail;
            g.evidence = std::string(rd::json_flag(t, "ok") ? "agreed" : "DISAGREED") + " on the note of " + rd::json_text(t, "first_day")
                       + " to " + rd::json_text(t, "last_day") + " (" + cc.string() + ")";
        }
        gates.push_back(g);
        gates.push_back(rd::ReadinessGate{"economic", "capital", rd::GateStatus::Fail,
                                 "margin is an ESTIMATE (live/margin.hpp), not the exchange's SPAN: the SPAN files are not loaded",
                                 "capital measured with NSE SPAN plus exposure"});
    }
    std::vector<std::string> ready_models;
    for (std::size_t i = 0; i < series.size(); ++i) {
        const auto g = rd::economic(series[i].model, ns[i], ss[i], series[i].days.size(), series[i].unmarked, series[i].unpriced, peaks[i], bars);
        bool all = true;
        for (const auto& x : g) all = all && x.status == rd::GateStatus::Pass;
        if (all) ready_models.push_back(series[i].model);
        gates.insert(gates.end(), g.begin(), g.end());
    }
    bool global = true;
    for (const auto& g : gates)
        if (g.scope == "operational" || g.scope == "economic") global = global && g.status == rd::GateStatus::Pass;
    const bool ready = global && !ready_models.empty();

    // ---- out ------------------------------------------------------------------
    const fs::path out = live / "report";
    fs::create_directories(out, ec);
    {
        std::ofstream f(out / "readiness.md", std::ios::binary | std::ios::trunc);
        f << "# Readiness\n\n**" << (ready ? "READY for a supervised live pilot of: " : "NOT READY");
        for (std::size_t i = 0; i < ready_models.size() && ready; ++i) f << (i ? ", " : "") << ready_models[i];
        f << "**\n\nAn assessment only: live order submission stays disabled in this tree whatever it says. "
          << live_days.size() << " LIVE day(s) of paper; SIM sessions do not count.\n\n"
          << "| scope | gate | status | evidence | bar |\n|---|---|---|---|---|\n";
        for (const auto& g : gates)
            f << "| " << g.scope << " | " << g.name << " | " << rd::status_text(g.status) << " | " << g.evidence << " | " << g.bar << " |\n";
        if (bad > 0) f << "\n" << bad << " journal row(s) could not be read and are not in these numbers.\n";
    }
    {
        std::ofstream f(out / "readiness.json", std::ios::binary | std::ios::trunc);
        f << "{\"ready\": " << (ready ? "true" : "false") << ", \"live_days\": " << live_days.size() << ", \"ready_models\": [";
        for (std::size_t i = 0; i < ready_models.size(); ++i) f << (i ? ", " : "") << jstr(ready_models[i]);
        f << "], \"gates\": [";
        for (std::size_t i = 0; i < gates.size(); ++i) {
            const auto& g = gates[i];
            f << (i ? ",\n  " : "\n  ") << "{\"scope\": " << jstr(g.scope) << ", \"gate\": " << jstr(g.name) << ", \"status\": "
              << jstr(rd::status_text(g.status)) << ", \"evidence\": " << jstr(g.evidence) << ", \"bar\": " << jstr(g.bar) << "}";
        }
        f << "\n]}\n";
    }
    std::printf("%s -- %zu LIVE session(s), %zu LIVE day(s)\n", ready ? "READY (assessment only; nothing is enabled)" : "NOT READY",
                static_cast<std::size_t>(std::count_if(sessions.begin(), sessions.end(),
                                                       [](const rd::SessionRecord& s) { return s.source == "LIVE" && !s.replay; })),
                live_days.size());
    for (const auto& g : gates)
        if (g.scope == "operational" || g.scope == "economic")
            std::printf("  %-11s %-12s %-11s %s\n", g.scope.c_str(), g.name.c_str(), rd::status_text(g.status), g.evidence.c_str());
    std::printf("  per model: %zu model(s) judged, %zu passed every gate -> %s\n", series.size(), ready_models.size(),
                (out / "readiness.md").string().c_str());
    return ready ? 0 : 1;
}
