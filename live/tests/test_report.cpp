// The paper report's arithmetic, on books whose answers are known: daily
// mark-to-market across a carried position and an unmarked day, unpriced
// expenses that stay unknown, drawdown, the block bootstrap's intervals and
// p-values, the multiple-testing adjustments, the VIX terciles, and the
// readers.
//
// No check description here may contain the substring "F" "AIL" joined.

#include <live/report.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) { ++failures; }
}

using namespace altair::live;
using namespace altair::live::report;
namespace fs = std::filesystem;

const std::int64_t kD0 = parse_day("2026-09-01");
std::int64_t ns_at(std::int64_t day, int hh, int mm) { return ((day * 86400) + hh * 3600 + mm * 60 - 19800) * 1'000'000'000LL; }

JournalFill fill(std::int64_t day, int hh, int mm, const char* model, std::uint32_t tok, bool open, int side, std::int64_t qty,
                 double px, double exp) {
    JournalFill f;
    f.ns = ns_at(day, hh, mm); f.model = model; f.token = tok; f.symbol = "X"; f.open = open; f.side = side; f.qty = qty;
    f.price = px; f.expenses = exp;
    return f;
}
bool same_value(double a, double b) { return std::fabs(a - b) < 1e-9; }

} // namespace

int main() {
    std::printf("live report\n");

    // ---- daily mark-to-market ------------------------------------------------------
    {
        const std::vector<JournalFill> fills{
            fill(kD0, 10, 0, "A", 1, true, 1, 50, 100.0, 1.0),        // day 0: a round trip, +500 gross, 2 expenses
            fill(kD0, 11, 0, "A", 1, false, -1, 50, 110.0, 1.0),
            fill(kD0 + 1, 15, 0, "A", 2, true, 1, 10, 200.0, 0.5),    // day 1: carried, marked 210: +100
            fill(kD0 + 4, 10, 0, "A", 2, false, -1, 10, 220.0, 0.5),  // day 4: sold at 220
        };
        std::map<MarkKey, double> marks;
        marks[{kD0 + 1, "A", 2}] = 210.0;
        // day 2: no mark (the engine stopped early); day 3: marked 205.
        marks[{kD0 + 3, "A", 2}] = 205.0;
        const std::set<std::int64_t> days{kD0, kD0 + 1, kD0 + 2, kD0 + 3, kD0 + 4, kD0 + 5};
        const auto s = daily_mtm(fills, marks, days);
        check(s.size() == 1 && s[0].days.size() == 6, "one model, every report day");
        const auto& d = s[0].days;
        check(same_value(d[0].gross, 500.0) && same_value(d[0].expenses, 2.0) && same_value(d[0].net(), 498.0) && same_value(d[0].turnover, 10500.0),
              "an intraday round trip: gross, expenses, net and turnover");
        check(same_value(d[1].gross, 100.0), "a carried position is marked to the close");
        check(!d[2].marked && std::isnan(d[2].net()) && s[0].unmarked == 1, "a day with no mark is not guessed");
        check(same_value(d[3].gross, -50.0), "its move is carried into the next marked day");
        check(same_value(d[4].gross, 150.0) && same_value(d[5].gross, 0.0), "the exit day, then a day with nothing held");
        double total = 0.0;
        for (const auto& x : d) if (std::isfinite(x.gross)) total += x.gross;
        check(same_value(total, 700.0), "the days sum to the round trips' gross: nothing lost or counted twice");
        check(s[0].open_units_at_end == 0 && !s[0].unpriced, "flat at the end, every fill priced");

        std::vector<JournalFill> unpriced = fills;
        unpriced[1].expenses = kNaN;
        const auto u = daily_mtm(unpriced, marks, days);
        check(u[0].unpriced && std::isnan(u[0].days[0].net()) && same_value(u[0].days[0].gross, 500.0),
              "an unpriced fill: gross stands, net is unknown -- never zero expenses");
    }

    // ---- describe ----------------------------------------------------------------------
    {
        const auto s = describe({1.0, -2.0, kNaN, 3.0});
        check(s.days == 3 && same_value(s.total, 2.0) && same_value(s.mean, 2.0 / 3.0), "NaN days are left out, not zeroed");
        check(same_value(s.max_drawdown, 2.0) && same_value(s.hit_rate, 2.0 / 3.0), "drawdown peak to trough, and the share of up days");
    }

    // ---- the block bootstrap and the adjustments -------------------------------------
    {
        BlockRng g(7);
        std::vector<std::vector<double>> series(6, std::vector<double>(250));
        for (auto& sv : series)
            for (auto& x : sv) {
                // Roughly normal noise, sd ~ 1000.
                double z = 0.0;
                for (int k = 0; k < 12; ++k) z += static_cast<double>(g.below(1'000'000)) / 1e6;
                x = (z - 6.0) * 1000.0;
            }
        for (auto& x : series[0]) x += 400.0;   // one model with a real edge: 0.4 sd a day
        std::vector<DailyStats> st;
        for (const auto& sv : series) st.push_back(describe(sv));
        bootstrap(series, st, 11, 2000, 5);
        check(st[0].ci_lo > 0.0 && st[0].p < 0.01 && st[0].p_rw < 0.05, "a real edge: the interval clears zero and survives Romano-Wolf");
        bool nulls_fine = true, ordered = true;
        for (std::size_t i = 1; i < st.size(); ++i) {
            nulls_fine = nulls_fine && st[i].p_rw > 0.05;
            ordered = ordered && st[i].p_rw >= st[i].p - 1e-12 && st[i].p_holm >= st[i].p - 1e-12;
        }
        check(nulls_fine, "five models of pure noise: none survives the adjustment");
        check(ordered, "an adjusted p is never below the raw one");
        std::vector<DailyStats> again;
        for (const auto& sv : series) again.push_back(describe(sv));
        bootstrap(series, again, 11, 2000, 5);
        check(again[0].ci_lo == st[0].ci_lo && again[3].p_rw == st[3].p_rw, "the same inputs and seed: the same numbers, exactly");

        std::vector<std::vector<double>> few{{1.0, 2.0, 3.0}};
        std::vector<DailyStats> fs_{describe(few[0])};
        bootstrap(few, fs_, 1, 500, 5);
        check(std::isnan(fs_[0].ci_lo) && std::isnan(fs_[0].p), "too few days: no interval and no test, rather than a confident one");
    }

    // ---- the RNG's bounded draw --------------------------------------------------------
    {
        BlockRng a(42), b(42);
        bool same = true, inside = true;
        std::vector<int> hist(7, 0);
        for (int i = 0; i < 70000; ++i) {
            const auto x = a.below(7);
            same = same && x == b.below(7);
            inside = inside && x < 7;
            ++hist[static_cast<std::size_t>(x)];
        }
        bool flat = true;
        for (const int h : hist) flat = flat && std::abs(h - 10000) < 500;
        check(same && inside && flat, "the bounded draw: reproducible, in range, and even");
    }

    // ---- VIX terciles ------------------------------------------------------------------
    {
        std::map<std::int64_t, double> vix;
        std::set<std::int64_t> days;
        for (int k = 0; k < 9; ++k) { vix[kD0 + k] = 10.0 + k; days.insert(kD0 + k + 1); }
        days.insert(kD0 + 40);   // no close within a week before it
        const auto r = vix_regimes(days, vix);
        check(r.at(kD0 + 1) == "low" && r.at(kD0 + 5) == "mid" && r.at(kD0 + 9) == "high", "the previous close, in terciles");
        check(r.at(kD0 + 40) == "unknown", "no recent close: unknown, not guessed");
    }

    // ---- the readers -------------------------------------------------------------------
    {
        const fs::path dir = fs::temp_directory_path()
                           / ("altair_report_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_"
                              + std::to_string(std::random_device{}()));
        fs::create_directories(dir);
        {
            std::ofstream j(dir / "journal.csv");
            j << "ns,model,token,symbol,role,side,qty,price,expenses,carry,why,submit_ns\n"
              << ns_at(kD0, 10, 0) << ",A,1,X,OPEN,1,50,100.0000,1.0000,0,why,0\n"
              << ns_at(kD0, 11, 0) << ",A,1,X,CLOSE,-1,50,110.0000,,0,why,0\n"
              << "garbage\n";
            std::ofstream m(dir / "marks.csv");
            m << "time,ns,model,token,symbol,side,qty,mark,carry\n"
              << "t," << ns_at(kD0, 15, 29) << ",\"Pairs, hedged\",2,Y,1,10,205.0000,1\n"
              << "t," << ns_at(kD0, 15, 30) << ",\"Pairs, hedged\",2,Y,1,10,206.0000,1\n";
            std::ofstream g(dir / "margin.csv");
            g << "time,ns,model,margin_estimate,positions\n"
              << "t," << ns_at(kD0, 10, 0) << ",ALL,1000,1\n"
              << "t," << ns_at(kD0, 10, 0) << ",\"A\",1000,\n"
              << "t," << ns_at(kD0, 11, 0) << ",ALL,,1\n";
        }
        std::size_t bad = 0;
        const auto f = read_journal((dir / "journal.csv").string(), &bad);
        check(f.size() == 2 && bad == 1 && std::isnan(f[1].expenses) && f[1].side == -1, "the journal: rows read, a bad one counted");
        const auto mk = read_marks((dir / "marks.csv").string());
        check(mk.size() == 1 && same_value(mk.at({kD0, "Pairs, hedged", 2}), 206.0), "marks: the last of the day, quoted names intact");
        const auto mg = read_margin((dir / "margin.csv").string());
        check(mg.days.count(kD0) == 1 && std::isnan(mg.peak.at({kD0, "ALL"})) && same_value(mg.peak.at({kD0, "A"}), 1000.0),
              "margin: the day's peak, unknown when a sample was");
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    std::printf("%s\n", failures == 0 ? "all live report checks passed" : "live report checks did not pass");
    return failures == 0 ? 0 : 1;
}
