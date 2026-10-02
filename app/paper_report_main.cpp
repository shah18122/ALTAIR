// altair_paper_report -- what the paper book earned, per model and in total,
// with intervals that respect serial dependence and p-values that respect
// having tried every model at once (live/report.hpp says how).
//
//   altair_paper_report [--root DIR] [--paper DIR] [--out DIR]
//                       [--cost-mult 1.5] [--slip-bp 2] [--min-days 5]
//
// Reads data/live/paper/{journal,marks,margin}.csv and India VIX daily closes
// from dataset/; writes data/live/report/{daily.csv,summary.csv,summary.json,
// report.md}. The numbers are PAPER: fills at the quoted touch after the
// engine's latency, expenses from config/charges.toml, margin an estimate.

#include <live/readiness.hpp>
#include <live/report.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace rp = altair::live::report;

std::string num(double v, int digits = 2) {
    if (!std::isfinite(v)) return "";
    char b[64];
    std::snprintf(b, sizeof b, "%.*f", digits, v);
    return b;
}
std::string jnum(double v) {
    if (!std::isfinite(v)) return "null";
    char b[64];
    std::snprintf(b, sizeof b, "%.10g", v);
    return b;
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
std::string csv(const std::string& s) {
    std::string o = "\"";
    for (const char c : s) o += c == '"' ? std::string("\"\"") : std::string(1, c);
    return o + "\"";
}

struct Row {
    std::string model;
    rp::DailyStats net, stressed;
    std::size_t unmarked = 0;
    bool unpriced = false;
    double turnover = 0.0, peak_capital = 0.0;
    std::int64_t open_units = 0;
    std::map<std::string, std::pair<double, std::size_t>> regime;   ///< regime -> (sum net, days)
};

void usage(const char* exe) {
    std::printf(
        "  The paper book's daily mark-to-market P&L, per model and in total.\n\n"
        "    %s [--root DIR] [--paper DIR] [--out DIR] [--cost-mult X] [--slip-bp B] [--min-days N]\n\n"
        "    --root       the tree holding data/ and dataset/ (default: the source tree)\n"
        "    --paper      the ledger directory (default <root>/data/live/paper)\n"
        "    --out        where the report goes (default <root>/data/live/report)\n"
        "    --cost-mult  the stress case's expenses multiple (default 1.5)\n"
        "    --slip-bp    the stress case's extra slippage per rupee traded, bp (default 2)\n"
        "    --min-days   fewer marked days than this: no interval, no test (default 5)\n"
        "    --live-only  only days the engine ran on a LIVE feed (sessions.csv); SIM days prove\n"
        "                 the plumbing, not the market\n",
        exe);
}

}  // namespace

int main(int argc, char** argv) {
    fs::path root = ALTAIR_SOURCE_DIR, paper, out;
    double cost_mult = 1.5, slip_bp = 2.0;
    std::size_t min_days = 5;
    bool live_only = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has = i + 1 < argc;
        if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
        if (a == "--root" && has) { root = argv[++i]; continue; }
        if (a == "--paper" && has) { paper = argv[++i]; continue; }
        if (a == "--out" && has) { out = argv[++i]; continue; }
        if (a == "--cost-mult" && has) { cost_mult = std::atof(argv[++i]); continue; }
        if (a == "--slip-bp" && has) { slip_bp = std::atof(argv[++i]); continue; }
        if (a == "--min-days" && has) { min_days = static_cast<std::size_t>(std::atoll(argv[++i])); continue; }
        if (a == "--live-only") { live_only = true; continue; }
        std::printf("unknown argument %s\n", a.c_str());
        usage(argv[0]);
        return 2;
    }
    if (!(cost_mult >= 1.0) || !(slip_bp >= 0.0) || min_days < 2) {
        std::printf("--cost-mult must be >= 1, --slip-bp >= 0, --min-days >= 2\n");
        return 2;
    }
    if (paper.empty()) paper = root / "data/live/paper";
    if (out.empty()) out = root / "data/live/report";

    // Which feed each day ran on: LIVE, SIM, both ("mixed"), or not on record.
    std::map<std::int64_t, std::string> source;
    for (const auto& x : altair::live::readiness::read_sessions((paper.parent_path() / "sessions.csv").string())) {
        if (x.replay) continue;
        std::string& v = source[altair::live::parse_day(x.date)];
        v = v.empty() || v == x.source ? x.source : std::string("mixed");
    }
    const auto source_of = [&source](std::int64_t d) {
        const auto it = source.find(d);
        return it == source.end() ? std::string("unknown") : it->second;
    };
    std::size_t bad = 0;
    auto fills = rp::read_journal((paper / "journal.csv").string(), &bad);
    const auto marks = rp::read_marks((paper / "marks.csv").string());
    auto margin = rp::read_margin((paper / "margin.csv").string());
    if (live_only) {
        std::erase_if(fills, [&](const rp::JournalFill& f) { return source_of(rp::day_of_ns(f.ns)) != "LIVE"; });
        std::erase_if(margin.days, [&](std::int64_t d) { return source_of(d) != "LIVE"; });
    }
    if (fills.empty() && margin.days.empty()) {
        std::printf("nothing to report: no fills in %s and no session in %s\n", (paper / "journal.csv").string().c_str(),
                    (paper / "margin.csv").string().c_str());
        return 1;
    }
    auto series = rp::daily_mtm(fills, marks, margin.days);
    std::set<std::int64_t> days(margin.days);
    for (const auto& f : fills) days.insert(rp::day_of_ns(f.ns));
    const auto regimes = rp::vix_regimes(days, rp::read_daily_closes((root / "dataset/spot/indiavix/1d").string()));

    // Models that ran but never filled earned zero on every day: they belong
    // in the family tested, or the adjustment flatters the ones that traded.
    // decisions.csv names every model that decided (or declined) anything.
    std::set<std::string> ran;
    {
        std::ifstream in(paper / "decisions.csv");
        std::string line;
        std::getline(in, line);
        while (std::getline(in, line)) {
            const auto c = rp::csv_fields(line);
            if (c.size() >= 3 && !c[2].empty() && c[2] != "engine") ran.insert(c[2]);
        }
        for (const auto& [key, v] : margin.peak) if (key.second != "ALL") ran.insert(key.second);
    }
    std::set<std::string> seen;
    for (const auto& s : series) seen.insert(s.model);
    for (const auto& model : ran) {
        if (seen.count(model) != 0) continue;
        seen.insert(model);
        rp::ModelDaily z;
        z.model = model;
        for (const std::int64_t d : days) { rp::DayPnl day; day.day = d; z.days.push_back(day); }
        series.push_back(std::move(z));
    }

    std::vector<std::vector<double>> nets, stress;
    std::vector<Row> rows;
    std::vector<double> book(days.size(), 0.0), book_stress(days.size(), 0.0);
    for (const auto& s : series) {
        Row r;
        r.model = s.model;
        r.unmarked = s.unmarked;
        r.unpriced = s.unpriced;
        r.open_units = s.open_units_at_end;
        std::vector<double> n, st;
        for (std::size_t k = 0; k < s.days.size(); ++k) {
            const auto& d = s.days[k];
            const double net = d.net();
            const double str = d.gross - cost_mult * d.expenses - slip_bp * 1e-4 * d.turnover;
            n.push_back(net);
            st.push_back(str);
            book[k] += net;   // NaN sticks: a book day is known only when every model's is
            book_stress[k] += str;
            r.turnover += d.turnover;
            const auto pk = margin.peak.find({d.day, s.model});
            if (pk != margin.peak.end()) r.peak_capital = std::isfinite(pk->second) && std::isfinite(r.peak_capital)
                                                              ? std::max(r.peak_capital, pk->second) : rp::kNaN;
            if (std::isfinite(net)) {
                auto& g = r.regime[regimes.at(d.day)];
                g.first += net;
                ++g.second;
            }
        }
        r.net = rp::describe(n);
        r.stressed = rp::describe(st);
        // A Sharpe ratio from a handful of days is noise with a decimal point.
        if (r.net.days < min_days) r.net.sharpe = rp::kNaN;
        if (r.stressed.days < min_days) r.stressed.sharpe = rp::kNaN;
        nets.push_back(std::move(n));
        stress.push_back(std::move(st));
        rows.push_back(std::move(r));
    }
    {
        std::vector<rp::DailyStats> a, b;
        for (const auto& r : rows) { a.push_back(r.net); b.push_back(r.stressed); }
        rp::bootstrap(nets, a, 20261002, 4000, min_days);
        rp::bootstrap(stress, b, 20261003, 4000, min_days);
        for (std::size_t i = 0; i < rows.size(); ++i) { rows[i].net = a[i]; rows[i].stressed = b[i]; }
    }
    Row all;
    all.model = "ALL (the book)";
    {
        all.net = rp::describe(book);
        all.stressed = rp::describe(book_stress);
        if (all.net.days < min_days) all.net.sharpe = rp::kNaN;
        std::vector<rp::DailyStats> a{all.net}, b{all.stressed};
        rp::bootstrap({book}, a, 20261004, 4000, min_days);
        rp::bootstrap({book_stress}, b, 20261005, 4000, min_days);
        all.net = a[0];
        all.stressed = b[0];
        for (const auto& r : rows) { all.turnover += r.turnover; all.unmarked += r.unmarked; all.unpriced = all.unpriced || r.unpriced; }
        for (const std::int64_t d : days) {
            const auto pk = margin.peak.find({d, std::string("ALL")});
            if (pk != margin.peak.end()) all.peak_capital = std::isfinite(pk->second) && std::isfinite(all.peak_capital)
                                                                ? std::max(all.peak_capital, pk->second) : rp::kNaN;
        }
    }

    std::error_code ec;
    fs::create_directories(out, ec);
    {
        std::ofstream f(out / "daily.csv", std::ios::binary | std::ios::trunc);
        f << "day,source,model,regime,gross,expenses,net,stressed_net,turnover,fills,marked,peak_margin\n";
        for (const auto& s : series)
            for (const auto& d : s.days) {
                const auto pk = margin.peak.find({d.day, s.model});
                f << altair::live::day_text(d.day) << "," << source_of(d.day) << "," << csv(s.model) << "," << regimes.at(d.day) << ","
                  << num(d.gross) << ","
                  << num(d.expenses) << "," << num(d.net()) << ","
                  << num(d.gross - cost_mult * d.expenses - slip_bp * 1e-4 * d.turnover) << "," << num(d.turnover) << "," << d.fills
                  << "," << (d.marked ? 1 : 0) << "," << (pk != margin.peak.end() ? num(pk->second, 0) : std::string()) << "\n";
            }
    }
    const auto regime_mean = [](const Row& r, const char* g) {
        const auto it = r.regime.find(g);
        return it == r.regime.end() || it->second.second == 0 ? rp::kNaN : it->second.first / static_cast<double>(it->second.second);
    };
    const auto regime_n = [](const Row& r, const char* g) {
        const auto it = r.regime.find(g);
        return it == r.regime.end() ? std::size_t{0} : it->second.second;
    };
    std::vector<const Row*> listed;
    for (const auto& r : rows) listed.push_back(&r);
    listed.push_back(&all);
    {
        std::ofstream f(out / "summary.csv", std::ios::binary | std::ios::trunc);
        f << "model,days,unmarked,unpriced,total_net,mean_daily_net,sd_daily_net,sharpe,max_drawdown,hit_rate,ci_lo,ci_hi,p,p_holm,"
             "p_romano_wolf,stressed_mean,stressed_ci_lo,stressed_p_romano_wolf,turnover,peak_capital,return_on_capital,"
             "low_vix_mean,low_vix_days,mid_vix_mean,mid_vix_days,high_vix_mean,high_vix_days\n";
        for (const Row* r : listed) {
            f << csv(r->model) << "," << r->net.days << "," << r->unmarked << "," << (r->unpriced ? 1 : 0) << "," << num(r->net.total)
              << "," << num(r->net.mean) << "," << num(r->net.sd) << "," << num(r->net.sharpe, 3) << "," << num(r->net.max_drawdown)
              << "," << num(r->net.hit_rate, 4) << "," << num(r->net.ci_lo) << "," << num(r->net.ci_hi) << "," << num(r->net.p, 4)
              << "," << num(r->net.p_holm, 4) << "," << num(r->net.p_rw, 4) << "," << num(r->stressed.mean) << ","
              << num(r->stressed.ci_lo) << "," << num(r->stressed.p_rw, 4) << "," << num(r->turnover) << ","
              << num(r->peak_capital, 0) << "," << num(r->peak_capital > 0.0 ? r->net.total / r->peak_capital : rp::kNaN, 4);
            for (const char* g : {"low", "mid", "high"}) f << "," << num(regime_mean(*r, g)) << "," << regime_n(*r, g);
            f << "\n";
        }
    }
    {
        std::ofstream f(out / "summary.json", std::ios::binary | std::ios::trunc);
        f << "{\"days\": " << days.size() << ", \"live_only\": " << (live_only ? "true" : "false") << ", \"first\": " << jstr(days.empty() ? "" : altair::live::day_text(*days.begin()))
          << ", \"last\": " << jstr(days.empty() ? "" : altair::live::day_text(*days.rbegin())) << ", \"cost_mult\": " << jnum(cost_mult)
          << ", \"slip_bp\": " << jnum(slip_bp) << ", \"bad_journal_rows\": " << bad << ", \"models\": [";
        for (std::size_t i = 0; i < listed.size(); ++i) {
            const Row& r = *listed[i];
            f << (i ? ",\n  " : "\n  ") << "{\"model\": " << jstr(r.model) << ", \"book\": " << (&r == &all ? "true" : "false")
              << ", \"days\": " << r.net.days << ", \"unmarked\": " << r.unmarked << ", \"unpriced\": " << (r.unpriced ? "true" : "false")
              << ", \"total_net\": " << jnum(r.net.total) << ", \"mean\": " << jnum(r.net.mean) << ", \"ci_lo\": " << jnum(r.net.ci_lo)
              << ", \"ci_hi\": " << jnum(r.net.ci_hi) << ", \"p\": " << jnum(r.net.p) << ", \"p_holm\": " << jnum(r.net.p_holm)
              << ", \"p_rw\": " << jnum(r.net.p_rw) << ", \"sharpe\": " << jnum(r.net.sharpe) << ", \"max_drawdown\": "
              << jnum(r.net.max_drawdown) << ", \"stressed_mean\": " << jnum(r.stressed.mean) << ", \"stressed_ci_lo\": "
              << jnum(r.stressed.ci_lo) << ", \"stressed_p_rw\": " << jnum(r.stressed.p_rw) << ", \"turnover\": " << jnum(r.turnover)
              << ", \"peak_capital\": " << jnum(r.peak_capital) << "}";
        }
        f << "\n]}\n";
    }
    {
        std::ofstream f(out / "report.md", std::ios::binary | std::ios::trunc);
        std::map<std::string, std::size_t> by_source;
        for (const std::int64_t d : days) ++by_source[source_of(d)];
        f << "# Paper report\n\n" << days.size() << " day(s)";
        if (!days.empty()) f << ", " << altair::live::day_text(*days.begin()) << " to " << altair::live::day_text(*days.rbegin());
        f << " (";
        bool first_src = true;
        for (const auto& [src, n] : by_source) { f << (first_src ? "" : ", ") << n << " " << src; first_src = false; }
        f << ")" << (live_only ? ", LIVE days only" : "")
          << (by_source.count("SIM") || by_source.count("mixed") ? ". SIM days are the simulator's random walk: plumbing, not evidence" : "");
        f << ". PAPER: fills at the quoted touch after the engine's latency; expenses from config/charges.toml; capital is the "
             "margin ESTIMATE (live/margin.hpp), not SPAN. Intervals: 95 % moving-block bootstrap over days. p: one-sided, mean "
             "daily net <= 0; Holm and Romano-Wolf adjust for all "
          << rows.size() << " models tested together. Stress: expenses x" << num(cost_mult, 2) << " plus " << num(slip_bp, 1)
          << " bp of every rupee traded.\n\n";
        f << "| model | days | total net | mean/day | 95% CI | p (RW) | Sharpe | max DD | stressed mean | peak capital | low/mid/high VIX mean |\n"
             "|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---|\n";
        for (const Row* r : listed) {
            f << "| " << r->model << (r->unpriced ? " (net UNPRICED)" : "") << (r->unmarked ? " (" + std::to_string(r->unmarked) + " unmarked)" : "")
              << " | " << r->net.days << " | " << num(r->net.total, 0) << " | " << num(r->net.mean, 0) << " | ";
            if (std::isfinite(r->net.ci_lo)) f << num(r->net.ci_lo, 0) << " to " << num(r->net.ci_hi, 0);
            else f << "too few days";
            f << " | " << num(r->net.p_rw, 3) << " | " << num(r->net.sharpe, 2) << " | " << num(r->net.max_drawdown, 0) << " | "
              << num(r->stressed.mean, 0) << " | " << num(r->peak_capital, 0) << " | " << num(regime_mean(*r, "low"), 0) << " / "
              << num(regime_mean(*r, "mid"), 0) << " / " << num(regime_mean(*r, "high"), 0) << " |\n";
        }
        if (bad > 0) f << "\n" << bad << " journal row(s) could not be read and are NOT in these numbers.\n";
    }
    std::printf("%zu day(s), %zu model(s), %zu fill(s)%s -> %s\n", days.size(), rows.size(), fills.size(),
                bad ? (" (" + std::to_string(bad) + " unreadable journal rows)").c_str() : "", out.string().c_str());
    for (const Row* r : listed)
        std::printf("  %-34s %3zu d  net %12s  mean %10s  CI [%s, %s]  p_rw %s%s\n", r->model.c_str(), r->net.days, num(r->net.total, 0).c_str(),
                    num(r->net.mean, 0).c_str(), num(r->net.ci_lo, 0).c_str(), num(r->net.ci_hi, 0).c_str(), num(r->net.p_rw, 3).c_str(),
                    r->unpriced ? "  (expenses UNPRICED)" : "");
    return 0;
}
