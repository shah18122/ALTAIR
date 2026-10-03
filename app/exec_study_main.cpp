// altair_exec_study -- execution labels for every paper fill, from the
// session tapes that saw them (live/exec_study.hpp says what each label is).
//
//   altair_exec_study --tape T [--tape T2 ...] [--journal J] [--out DIR]
//
// Each tape is replayed (no models: the market only) against the fills of
// its day; a fill belongs to the tape whose frames span it. Writes
// <out>/exec_fills.csv (one row per fill) and <out>/exec_summary.csv (by
// model and entry/exit): shortfall, markouts, and how often a passive order
// at the touch would have filled -- the labels for execution research.

#include <live/exec_study.hpp>
#include <live/report.hpp>
#include <live/tape.hpp>
#include <live/universe.hpp>
#include <server/protocol.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace lv = altair::live;
namespace rp = altair::live::report;
namespace ex = altair::live::execution;

std::string num(double v, int digits = 2) {
    if (!std::isfinite(v)) return "";
    char b[64];
    std::snprintf(b, sizeof b, "%.*f", digits, v);
    return b;
}
std::string csv(const std::string& s) {
    std::string o = "\"";
    for (const char c : s) o += c == '"' ? std::string("\"\"") : std::string(1, c);
    return o + "\"";
}

/// One tape, replayed into a study of `fills` (that day's); the labels of
/// the fills its frames span.
bool study_tape(const fs::path& tape, const fs::path& scratch, const std::vector<rp::JournalFill>& all,
                std::vector<ex::FillLabel>& out) {
    lv::TapeReader in(tape.string());
    lv::TapeRecord rec;
    lv::TapeSections start;
    if (!in.ok() || !in.next(rec) || rec.kind != lv::TapeKind::Start || !lv::tape_unpack(rec.text(), start)) {
        std::printf("%s: not a session tape\n", tape.string().c_str());
        return false;
    }
    const std::string* uni = lv::tape_section(start, "universe.csv");
    const std::string* args = lv::tape_section(start, "args");
    if (uni == nullptr || args == nullptr) { std::printf("%s: the start record lacks the universe or the options\n", tape.string().c_str()); return false; }
    std::string date;
    {
        const auto at = args->find("date=");
        if (at != std::string::npos) date = args->substr(at + 5, 10);
    }
    const std::int64_t day = lv::parse_day(date);
    std::error_code ec;
    fs::create_directories(scratch, ec);
    const fs::path upath = scratch / "universe.csv";
    { std::ofstream(upath, std::ios::binary | std::ios::trunc) << *uni; }
    const auto universe = lv::read_universe(upath.string());
    std::vector<rp::JournalFill> fills;
    for (const auto& f : all) if (rp::day_of_ns(f.ns) == day) fills.push_back(f);
    ex::ExecStudy study(universe, fills);

    std::vector<std::uint8_t> buf;
    bool resync = false;
    std::uint64_t frames = 0;
    while (in.next(rec)) {
        if (rec.kind == lv::TapeKind::Connected) { buf.clear(); resync = false; study.on_reconnect(); continue; }
        if (rec.kind == lv::TapeKind::Disconnected) { buf.clear(); continue; }
        if (rec.kind != lv::TapeKind::Data || resync) continue;
        buf.insert(buf.end(), rec.bytes.begin(), rec.bytes.end());
        std::size_t at = 0;
        while (buf.size() - at >= altair::kFrameHeaderBytes) {
            const auto h = altair::decode_header(buf.data() + at, buf.size() - at);
            if (!h) { buf.clear(); at = 0; resync = true; break; }   // as the engine does: wait for the next connection
            const std::size_t need = altair::kFrameHeaderBytes + h->payload_len;
            if (buf.size() - at < need) break;
            study.on_frame(*h, buf.data() + at + altair::kFrameHeaderBytes);
            ++frames;
            at += need;
        }
        if (at > 0) buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(at));
    }
    const auto [first, last] = study.span();
    std::size_t kept = 0;
    for (auto& l : study.finish()) {
        if (l.fill.ns < first || l.fill.ns > last) continue;   // another session's fill
        out.push_back(std::move(l));
        ++kept;
    }
    std::printf("%s: %s, %llu frames, %zu of the day's %zu fill(s)%s\n", tape.filename().string().c_str(), date.c_str(),
                static_cast<unsigned long long>(frames), kept, fills.size(),
                in.truncated() ? " (the tape ends mid-record)" : "");
    return true;
}

void usage(const char* exe) {
    std::printf(
        "  Execution labels for the paper fills, from the session tapes.\n\n"
        "    %s --tape T [--tape T2 ...] [--journal J] [--out DIR] [--root DIR]\n\n"
        "    --tape     a session tape (altair_live_engine --record); repeat for several\n"
        "    --journal  the fills (default <root>/data/live/paper/journal.csv)\n"
        "    --out      where the labels go (default <root>/data/live/report)\n",
        exe);
}

}  // namespace

int main(int argc, char** argv) {
    fs::path root = ALTAIR_SOURCE_DIR, journal, out;
    std::vector<fs::path> tapes;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has = i + 1 < argc;
        if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
        if (a == "--tape" && has) { tapes.emplace_back(argv[++i]); continue; }
        if (a == "--journal" && has) { journal = argv[++i]; continue; }
        if (a == "--out" && has) { out = argv[++i]; continue; }
        if (a == "--root" && has) { root = argv[++i]; continue; }
        std::printf("unknown argument %s\n", a.c_str());
        usage(argv[0]);
        return 2;
    }
    if (tapes.empty()) { usage(argv[0]); return 2; }
    if (journal.empty()) journal = root / "data/live/paper/journal.csv";
    if (out.empty()) out = root / "data/live/report";
    std::size_t bad = 0;
    const auto fills = rp::read_journal(journal.string(), &bad);
    if (bad > 0) std::printf("%zu journal row(s) unreadable: not studied\n", bad);

    std::vector<ex::FillLabel> labels;
    for (const auto& t : tapes)
        if (!study_tape(t, out / "exec_scratch", fills, labels)) return 1;
    std::error_code ec;
    fs::remove_all(out / "exec_scratch", ec);
    std::sort(labels.begin(), labels.end(), [](const ex::FillLabel& a, const ex::FillLabel& b) { return a.fill.ns < b.fill.ns; });

    fs::create_directories(out, ec);
    {
        std::ofstream f(out / "exec_fills.csv", std::ios::binary | std::ios::trunc);
        f << "time,ns,model,symbol,token,role,side,qty,price,decision_mid,decision_spread_bp,fill_mid,shortfall_bp";
        for (const auto h : ex::kMarkoutSeconds) f << ",markout_" << h << "s_bp";
        f << ",passive_price,queue_ahead";
        for (const auto h : ex::kPassiveSeconds) f << ",passive_fill_" << h << "s";
        f << "\n";
        for (const auto& l : labels) {
            const auto& x = l.fill;
            f << lv::day_text(rp::day_of_ns(x.ns)) << "," << x.ns << "," << csv(x.model) << "," << x.symbol << "," << x.token << ","
              << (x.open ? "entry" : "exit") << "," << x.side << "," << x.qty << "," << num(x.price, 4) << "," << num(l.decision_mid, 4)
              << "," << num(l.decision_spread_bp) << "," << num(l.fill_mid, 4) << "," << num(l.shortfall_bp);
            for (const double m : l.markout_bp) f << "," << num(m);
            f << "," << num(l.passive_price, 4) << "," << (l.queue_ahead >= 0 ? std::to_string(l.queue_ahead) : std::string());
            for (const int p : l.passive_filled) f << "," << (p < 0 ? std::string() : std::to_string(p));
            f << "\n";
        }
    }
    // By model and role.
    struct Agg {
        std::size_t n = 0;
        std::vector<double> shortfall, spread;
        std::array<std::vector<double>, ex::kMarkoutSeconds.size()> markout;
        std::array<std::pair<std::size_t, std::size_t>, ex::kPassiveSeconds.size()> passive{};   // (filled, known)
    };
    std::map<std::pair<std::string, std::string>, Agg> agg;
    for (const auto& l : labels) {
        for (const std::string key : {l.fill.model, std::string("ALL")}) {
            Agg& a = agg[{key, l.fill.open ? "entry" : "exit"}];
            ++a.n;
            if (std::isfinite(l.shortfall_bp)) a.shortfall.push_back(l.shortfall_bp);
            if (std::isfinite(l.decision_spread_bp)) a.spread.push_back(l.decision_spread_bp);
            for (std::size_t h = 0; h < l.markout_bp.size(); ++h) if (std::isfinite(l.markout_bp[h])) a.markout[h].push_back(l.markout_bp[h]);
            for (std::size_t h = 0; h < l.passive_filled.size(); ++h)
                if (l.passive_filled[h] >= 0) { a.passive[h].first += static_cast<std::size_t>(l.passive_filled[h]); ++a.passive[h].second; }
        }
    }
    const auto mean = [](const std::vector<double>& v) {
        if (v.empty()) return rp::kNaN;
        double s = 0.0;
        for (const double x : v) s += x;
        return s / static_cast<double>(v.size());
    };
    const auto median = [](std::vector<double> v) {
        if (v.empty()) return rp::kNaN;
        std::sort(v.begin(), v.end());
        return rp::quantile_sorted(v, 0.5);
    };
    {
        std::ofstream f(out / "exec_summary.csv", std::ios::binary | std::ios::trunc);
        f << "model,role,fills,shortfall_mean_bp,shortfall_median_bp,spread_mean_bp";
        for (const auto h : ex::kMarkoutSeconds) f << ",markout_" << h << "s_mean_bp";
        for (const auto h : ex::kPassiveSeconds) f << ",passive_fill_" << h << "s_rate,passive_" << h << "s_known";
        f << "\n";
        for (const auto& [k, a] : agg) {
            f << csv(k.first) << "," << k.second << "," << a.n << "," << num(mean(a.shortfall)) << "," << num(median(a.shortfall)) << ","
              << num(mean(a.spread));
            for (const auto& m : a.markout) f << "," << num(mean(m));
            for (const auto& [filled, known] : a.passive)
                f << "," << (known ? num(static_cast<double>(filled) / static_cast<double>(known), 3) : std::string()) << "," << known;
            f << "\n";
        }
    }
    std::printf("%zu fill(s) labelled -> %s\n", labels.size(), (out / "exec_fills.csv").string().c_str());
    for (const auto& [k, a] : agg) {
        if (k.first != "ALL") continue;
        std::printf("  all %-5s %4zu fills  shortfall %s bp (median %s)  spread %s bp  markout 1s/60s/300s %s / %s / %s bp  passive fill 5s/60s %s / %s\n",
                    k.second.c_str(), a.n, num(mean(a.shortfall)).c_str(), num(median(a.shortfall)).c_str(), num(mean(a.spread)).c_str(),
                    num(mean(a.markout[0])).c_str(), num(mean(a.markout[3])).c_str(), num(mean(a.markout[4])).c_str(),
                    a.passive[1].second ? num(static_cast<double>(a.passive[1].first) / static_cast<double>(a.passive[1].second), 2).c_str() : "",
                    a.passive[3].second ? num(static_cast<double>(a.passive[3].first) / static_cast<double>(a.passive[3].second), 2).c_str() : "");
    }
    return 0;
}
