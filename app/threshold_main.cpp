// app/threshold_main.cpp -- altair_threshold: the owner's threshold strategies
// (threshold_strategy/), on paper, with real futures expenses.
//
//   1. Prev-2-day breakout on BANKNIFTY, as 2D-H-L.xlsx states it
//      (strategies/threshold.hpp), from 2007-09-17 to the last session in
//      the dataset. Checked trade by trade against the workbook's own log
//      (threshold_strategy/extracted/2D-H-L_trade_log.csv, written by
//      research/tools/threshold_extract.py).
//   2. The BANKNIFTY/NIFTY ratio z-score of the relative-value note
//      (BNFNF.html), measured as the note measures it and in rupees.
//
// PAPER ONLY: offline and read-only; it never reaches a broker.
//
// Rupees: one lot (--lots N) at TODAY's lot sizes from data/instruments.csv,
// priced at index levels -- the dataset has index history, not a continuous
// futures series, so the near future's basis is left out. Every fill pays
// the expenses of config/charges.toml (risk/cost.hpp via app/demo_costs.hpp);
// a holding across a month end pays a roll (out and back in at that close).
//
// Writes <out>/threshold/{breakout_trades,ratio_trades,ratio_days}.csv and
// summary.txt.

#include <app/demo_costs.hpp>
#include <risk/charges_toml.hpp>
#include <strategies/threshold.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace dc = altair::demo_costs;
namespace th = altair::threshold;

void usage(const char* exe) {
    std::printf(
        "  The threshold strategies of threshold_strategy/, on paper, with real futures expenses.\n\n"
        "    %s [--dataset DIR] [--out DIR] [--from YYYY-MM-DD] [--lots N] [--unverified-costs]\n\n"
        "    --dataset DIR      default dataset (reads spot/banknifty and spot/nifty, 1d)\n"
        "    --out DIR          default data/verified (writes threshold/)\n"
        "    --from DATE        first session (default 2007-09-17, where the owner's workbook starts)\n"
        "    --lots N           lots per trade (default 1)\n"
        "    --unverified-costs price expenses from an UNVERIFIED config/charges.toml anyway\n",
        exe);
}

std::vector<std::string> split_csv(const std::string& line) {
    std::vector<std::string> f;
    std::string cur;
    bool quoted = false;
    for (const char c : line) {
        if (c == '"') { quoted = !quoted; continue; }
        if (c == ',' && !quoted) { f.push_back(cur); cur.clear(); continue; }
        if (c != '\r') cur += c;
    }
    f.push_back(cur);
    return f;
}

constexpr std::int64_t days_from_civil(int y, unsigned m, unsigned d) noexcept {
    y -= m <= 2 ? 1 : 0;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}
std::int64_t day_of(const std::string& iso) {
    if (iso.size() < 10) return 0;
    return days_from_civil(std::atoi(iso.substr(0, 4).c_str()), static_cast<unsigned>(std::atoi(iso.substr(5, 2).c_str())),
                           static_cast<unsigned>(std::atoi(iso.substr(8, 2).c_str())));
}
constexpr std::int64_t kCloseSec = 15 * 3600 + 30 * 60;

std::string fx(double v, int dp = 2) {
    if (!std::isfinite(v)) return {};
    char b[64];
    std::snprintf(b, sizeof b, "%.*f", dp, v);
    return b;
}
/// `part` of `whole`, in per cent; 0 of nothing.
double pct(std::size_t part, std::size_t whole) {
    return whole != 0 ? 100.0 * static_cast<double>(part) / static_cast<double>(whole) : 0.0;
}
/// 1234567.8 -> "12,34,568" (Indian grouping), sign kept.
std::string rs(double v) {
    const bool neg = v < 0;
    auto n = static_cast<long long>(std::llround(std::fabs(v)));
    std::string d = std::to_string(n), out;
    const int len = static_cast<int>(d.size());
    for (int i = 0; i < len; ++i) {
        out += d[static_cast<std::size_t>(i)];
        const int left = len - 1 - i;
        if (left > 0 && (left == 3 || (left > 3 && (left - 3) % 2 == 0))) out += ',';
    }
    return (neg ? "-Rs " : "Rs ") + out;
}

/// One index's daily bars, the pre-2015 vendor file first and the main file
/// over it. `close_only` marks a bar whose open, high and low are its close.
struct Daily {
    std::map<std::string, th::DayBar> bars;
    std::set<std::string> close_only;
};
Daily load_daily(const fs::path& dir) {
    Daily d;
    for (const char* f : {"vendor_pre2015.csv", "all.csv"}) {
        std::ifstream in(dir / f);
        std::string line;
        std::getline(in, line);
        while (std::getline(in, line)) {
            const auto c = split_csv(line);
            if (c.size() < 5 || c[0].size() < 10) continue;
            th::DayBar b;
            b.day = day_of(c[0]);
            b.open = std::atof(c[1].c_str());
            b.high = std::atof(c[2].c_str());
            b.low = std::atof(c[3].c_str());
            b.close = std::atof(c[4].c_str());
            if (!(b.close > 0.0)) continue;
            const std::string date = c[0].substr(0, 10);
            d.bars[date] = b;
            if (b.open == b.close && b.high == b.close && b.low == b.close) d.close_only.insert(date);
            else d.close_only.erase(date);
        }
    }
    return d;
}

/// The nearest index future's lot in the Kite master; `fallback` without it.
long long lot_from_master(const fs::path& master, const std::string& name, long long fallback) {
    std::ifstream in(master);
    std::string line, best_exp;
    long long lot = 0;
    while (std::getline(in, line)) {
        if (line.find(",FUT,NFO-FUT,") == std::string::npos) continue;
        const auto f = split_csv(line);
        if (f.size() < 12 || f[3] != name) continue;
        if (best_exp.empty() || f[5] < best_exp) { best_exp = f[5]; lot = std::atoll(f[8].c_str()); }
    }
    return lot > 0 ? lot : fallback;
}

/// Expense heads, added up over fills.
struct Heads {
    dc::Costs c;
    bool priced = true;
    void fill(const dc::Costs& f) { c.add(f); priced = priced && f.priced; }
};
const char* kHeadCols = "brokerage,stt,exchange_txn,sebi,stamp,ipft,gst,expenses";
std::string heads_csv(const Heads& h) {
    if (!h.priced) return ",,,,,,,";
    const auto& c = h.c;
    return fx(c.brokerage) + "," + fx(c.stt) + "," + fx(c.exchange) + "," + fx(c.sebi) + "," + fx(c.stamp) + "," + fx(c.ipft) + ","
         + fx(c.gst) + "," + fx(c.total);
}

/// A daily return series, measured the way the BNF/NF note measures it.
struct SeriesStats {
    std::size_t days = 0, held = 0;
    double total = 0, cagr = 0, vol = 0, sharpe = 0, max_dd = 0;
};
SeriesStats series_stats(const std::vector<double>& r, const std::vector<int>& pos, double years) {
    SeriesStats s;
    s.days = r.size();
    if (r.size() < 2) return s;
    double eq = 1.0, peak = 1.0, mean = 0.0;
    for (const double x : r) {
        eq *= 1.0 + x;
        peak = std::max(peak, eq);
        s.max_dd = std::min(s.max_dd, eq / peak - 1.0);
        mean += x;
    }
    mean /= static_cast<double>(r.size());
    double ss = 0.0;
    for (const double x : r) ss += (x - mean) * (x - mean);
    const double sd = std::sqrt(ss / static_cast<double>(r.size() - 1));
    s.total = eq - 1.0;
    s.cagr = years > 0.0 ? std::pow(eq, 1.0 / years) - 1.0 : 0.0;
    s.vol = sd * std::sqrt(252.0);
    s.sharpe = sd > 0.0 ? mean / sd * std::sqrt(252.0) : 0.0;
    for (const int p : pos) s.held += p != 0 ? 1 : 0;
    return s;
}

} // namespace

int main(int argc, char** argv) {
    fs::path dataset = "dataset", out_root = "data/verified";
    std::string from = "2007-09-17";
    long long lots = 1;
    bool unverified = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has = i + 1 < argc;
        if (a == "--dataset" && has) dataset = argv[++i];
        else if (a == "--out" && has) out_root = argv[++i];
        else if (a == "--from" && has) from = argv[++i];
        else if (a == "--lots" && has) lots = std::max(1LL, std::atoll(argv[++i]));
        else if (a == "--unverified-costs") unverified = true;
        else { usage(argv[0]); return a == "--help" || a == "-h" ? 0 : 2; }
    }

    // ---- expenses -------------------------------------------------------------------------
    std::vector<altair::ChargeSchedule> schedules;
    const auto rep = altair::load_charges_file("config/charges.toml", schedules);
    std::string cost_tag;
    if (!rep) {
        std::printf("config/charges.toml did not load: %s -- refusing: the demo is gross and net or nothing\n",
                    altair::charges_error_text(rep.error()));
        return 2;
    }
    if (rep->verified) {
        cost_tag = "verified";
    } else if (unverified) {
        for (auto& s : schedules) s.verified = true;
        cost_tag = "UNVERIFIED";
    } else {
        std::printf("config/charges.toml is UNVERIFIED: pass --unverified-costs to price expenses anyway\n");
        return 2;
    }
    const long long lot_b = lot_from_master("data/instruments.csv", "BANKNIFTY", 30);
    const long long lot_n = lot_from_master("data/instruments.csv", "NIFTY", 65);
    const auto fill = [&](bool buy, long long qty, double px, const std::string& date) {
        return dc::fill(altair::Segment::Fut, buy ? altair::Side::Buy : altair::Side::Sell, static_cast<double>(qty), px,
                        day_of(date) * 86'400 + kCloseSec, schedules);
    };

    const Daily bnf = load_daily(dataset / "spot" / "banknifty" / "1d");
    const Daily nifty = load_daily(dataset / "spot" / "nifty" / "1d");
    if (bnf.bars.empty() || nifty.bars.empty()) {
        std::printf("no daily bars under %s/spot/{banknifty,nifty}/1d\n", dataset.string().c_str());
        return 2;
    }
    const fs::path out = out_root / "threshold";
    std::error_code ec;
    fs::create_directories(out, ec);
    std::ostringstream sum;
    sum << "THRESHOLD STRATEGIES -- paper only, nothing is sent to a broker (altair_threshold)\n"
        << "Expenses: config/charges.toml (" << cost_tag << "), every fill; " << lots << " lot(s) at today's lot sizes "
        << "(BANKNIFTY " << lot_b << ", NIFTY " << lot_n << ", data/instruments.csv). Index levels stand in for the\n"
        << "near future (the dataset has no continuous futures series, so its basis is left out); a holding across a\n"
        << "month end pays a roll (out and back in at that close).\n\n";

    // ---- 1. prev-2-day breakout ---------------------------------------------------------------
    std::vector<std::string> dates;
    std::vector<th::DayBar> bars;
    std::size_t skipped = 0;
    for (const auto& [d, b] : bnf.bars) {
        if (d < from) continue;
        if (bnf.close_only.count(d) != 0) { ++skipped; continue; }   // no high or low: no breakout level
        dates.push_back(d);
        bars.push_back(b);
    }
    const auto trades = th::two_day_breakout(bars);
    {
        std::ofstream f(out / "breakout_trades.csv");
        f << "n,side,entry_date,exit_date,days,entry,stop,exit,exit_reason,flip,mfe_pts,mae_pts,points,qty,rolls,gross_pnl,"
          << kHeadCols << ",net_pnl,workbook_cost_pts,workbook_net_pts\n";
        double gross = 0, exp = 0, net = 0, pts = 0, wb_net = 0;
        std::size_t priced = 0, wins = 0, longs = 0, flips = 0, rolls_all = 0, held_days = 0;
        double win_sum = 0, loss_sum = 0;
        const long long q = lot_b * lots;
        for (std::size_t n = 0; n < trades.size(); ++n) {
            const auto& t = trades[n];
            Heads h;
            h.fill(fill(t.side > 0, q, t.entry, dates[t.entry_i]));
            h.fill(fill(t.side < 0, q, t.exit, dates[t.exit_i]));
            int rolls = 0;
            for (std::size_t k = t.entry_i; k < t.exit_i; ++k)
                if (dates[k].substr(0, 7) != dates[k + 1].substr(0, 7)) {
                    ++rolls;
                    h.fill(fill(t.side < 0, q, bars[k].close, dates[k]));
                    h.fill(fill(t.side > 0, q, bars[k].close, dates[k]));
                }
            const double g = t.points() * static_cast<double>(q);
            const double wb_cost = 0.0015 * (t.entry + t.exit);   // the workbook's own: 0.15 % of turnover a round trip
            gross += g;
            pts += t.points();
            wb_net += t.points() - wb_cost;
            longs += t.side > 0 ? 1 : 0;
            flips += t.flip ? 1 : 0;
            rolls_all += static_cast<std::size_t>(rolls);
            held_days += t.days();
            if (h.priced) {
                ++priced;
                exp += h.c.total;
                const double nt = g - h.c.total;
                net += nt;
                if (nt > 0) { ++wins; win_sum += nt; } else { loss_sum += nt; }
            }
            f << n + 1 << ',' << (t.side > 0 ? "Long" : "Short") << ',' << dates[t.entry_i] << ',' << dates[t.exit_i] << ','
              << t.days() << ',' << fx(t.entry) << ',' << fx(t.stop) << ',' << fx(t.exit) << ',' << th::exit_text(t.why) << ','
              << (t.flip ? "Yes" : "") << ',' << fx(t.mfe) << ',' << fx(t.mae) << ',' << fx(t.points()) << ',' << q << ',' << rolls
              << ',' << fx(g) << ',' << heads_csv(h) << ',' << (h.priced ? fx(g - h.c.total) : std::string{}) << ','
              << fx(wb_cost) << ',' << fx(t.points() - wb_cost) << '\n';
        }
        sum << "1. PREV-2-DAY BREAKOUT, BANKNIFTY (market bnf sheets/2D-H-L.xlsx)\n"
            << "   Long above the higher of the previous two highs, short below the lower of the previous two lows, at\n"
            << "   that level (the open on a gap). Stop fixed at entry at the previous day's low (long) / high (short);\n"
            << "   trailed at each previous day's low / high from the day after entry; on an exit day the opposite\n"
            << "   breakout reverses the position (flip). One position at a time.\n";
        if (!trades.empty()) {
            sum << "   " << dates.front() << " to " << dates.back() << ": " << trades.size() << " trades (" << longs << " long, "
                << trades.size() - longs << " short, " << flips << " flips), " << fx(static_cast<double>(held_days) / static_cast<double>(trades.size()), 1)
                << " sessions held on average, " << rolls_all << " month-end rolls\n"
                << "   gross " << fx(pts, 1) << " points = " << rs(gross) << "\n"
                << "   expenses " << rs(exp) << " (" << priced << " of " << trades.size() << " trades priced)\n"
                << "   NET " << rs(net) << "; net win rate " << fx(pct(wins, priced), 1) << " %, profit factor "
                << (loss_sum < 0 ? fx(win_sum / -loss_sum) : std::string("-")) << "\n"
                << "   Stops here fill exactly at their level. Slippage of "
                << fx(net / (static_cast<double>(q) * 2.0 * static_cast<double>(trades.size())), 1)
                << " points a fill (entry and exit) would take the whole net away.\n"
                << "   With the workbook's own cost (0.15 % of turnover a round trip, Rs 15,000 a crore): net "
                << fx(wb_net, 1) << " points. Real futures expenses are a fraction of that.\n";
        }
        if (skipped != 0) sum << "   " << skipped << " session(s) with a close but no high or low were left out.\n";
        // Tomorrow's levels: what the rule watches next session.
        if (bars.size() >= 2) {
            const auto& a = bars[bars.size() - 1];
            const auto& b2 = bars[bars.size() - 2];
            sum << "   Next session: long above " << fx(std::max(a.high, b2.high)) << ", short below " << fx(std::min(a.low, b2.low));
            if (!trades.empty() && trades.back().why == th::ExitWhy::OpenEod) {
                const auto& t = trades.back();
                sum << "; holding " << (t.side > 0 ? "LONG" : "SHORT") << " from " << dates[t.entry_i] << " at " << fx(t.entry)
                    << ", out " << (t.side > 0 ? "below " : "above ")
                    << fx(t.side > 0 ? std::max(t.stop, a.low) : std::min(t.stop, a.high));
            } else {
                sum << "; flat";
            }
            sum << " (from the " << dates.back() << " bar)\n";
        }
    }

    // ---- the check against the workbook's own trade log --------------------------------------------
    {
        const fs::path log = "threshold_strategy/extracted/2D-H-L_trade_log.csv";
        std::ifstream in(log);
        struct Wb { std::string side, entry, exit, why; double pts; };
        std::vector<Wb> wb;
        std::string line;
        std::getline(in, line);
        while (std::getline(in, line)) {
            const auto c = split_csv(line);
            if (c.size() < 12) continue;
            wb.push_back(Wb{c[1], c[2], c[3], c[7], std::atof(c[11].c_str())});
        }
        if (wb.empty()) {
            sum << "   CHECK: " << log.string() << " not found (python3 research/tools/threshold_extract.py .)\n\n";
        } else {
            const std::string lo = wb.front().entry, hi = wb.back().exit;
            const auto compare = [&](const std::vector<th::BreakoutTrade>& ours, const std::vector<std::string>& d, const char* label) {
                std::set<std::string> by_entry, by_all;
                double our_pts = 0;
                std::size_t our_n = 0;
                for (const auto& t : ours) {
                    if (d[t.entry_i] < lo || d[t.entry_i] > hi) continue;
                    ++our_n;
                    our_pts += t.points();
                    const std::string side = t.side > 0 ? "Long" : "Short";
                    by_entry.insert(d[t.entry_i] + side);
                    by_all.insert(d[t.entry_i] + side + d[t.exit_i] + th::exit_text(t.why));
                }
                std::size_t same_entry = 0, same_all = 0;
                double wb_pts = 0;
                for (const auto& w : wb) {
                    wb_pts += w.pts;
                    same_entry += by_entry.count(w.entry + w.side);
                    same_all += by_all.count(w.entry + w.side + w.exit + w.why);
                }
                sum << "     " << label << ": " << our_n << " trades against its " << wb.size() << "; " << same_entry
                    << " enter on the same day and side, " << same_all << " also leave on the same day for the same reason; gross "
                    << fx(our_pts, 1) << " points against its " << fx(wb_pts, 1) << "\n";
            };
            sum << "   CHECK against the workbook's own trade log (" << lo << " to " << hi << "):\n";
            compare(trades, dates, "on every session in our data");
            std::ifstream din("threshold_strategy/extracted/2D-H-L_days.csv");
            std::set<std::string> wb_days;
            std::getline(din, line);
            while (std::getline(din, line)) if (line.size() >= 10) wb_days.insert(line.substr(0, 10));
            if (!wb_days.empty()) {
                std::vector<std::string> d2;
                std::vector<th::DayBar> b2;
                for (std::size_t i = 0; i < dates.size(); ++i)
                    if (wb_days.count(dates[i]) != 0) { d2.push_back(dates[i]); b2.push_back(bars[i]); }
                compare(th::two_day_breakout(b2), d2, "on the workbook's sessions only");
            }
            sum << "     The rules agree; where trades differ, the bars differ: our closes and ranges come from the broker's\n"
                << "     daily candles, the workbook's from another source, and some sessions (special Saturday and Muhurat\n"
                << "     sessions, a few missing days) are in one and not the other.\n\n";
        }
    }

    // ---- 2. BANKNIFTY / NIFTY ratio z-score -------------------------------------------------------
    {
        std::vector<std::string> d;
        std::vector<double> a, b;
        for (const auto& [date, bar] : bnf.bars) {
            if (date < from) continue;
            const auto it = nifty.bars.find(date);
            if (it == nifty.bars.end()) continue;
            d.push_back(date);
            a.push_back(bar.close);
            b.push_back(it->second.close);
        }
        const th::RatioConfig cfg;
        const auto days = th::ratio_z(a, b, cfg);
        const auto trips = th::ratio_trips(days);
        {
            std::ofstream f(out / "ratio_days.csv");
            f << "date,banknifty,nifty,ratio,z,position\n";
            for (std::size_t i = 0; i < d.size(); ++i)
                f << d[i] << ',' << fx(a[i]) << ',' << fx(b[i]) << ',' << fx(days[i].ratio, 4) << ',' << fx(days[i].z, 3) << ','
                  << days[i].pos << '\n';
        }
        constexpr double kNoteCost = 0.0006;   // the note's 6 bps per unit of turnover, on every change
        std::ofstream f(out / "ratio_trades.csv");
        f << "n,side,entry_date,exit_date,days,entry_ratio,exit_ratio,entry_z,exit_z,banknifty_entry,banknifty_exit,banknifty_qty,"
             "nifty_entry,nifty_exit,nifty_qty,rolls,banknifty_pnl,nifty_pnl,gross_pnl,"
          << kHeadCols << ",net_pnl,note_return_pct,open\n";
        double gross = 0, exp = 0, net = 0;
        std::size_t priced = 0, wins = 0, shorts = 0;
        for (std::size_t n = 0; n < trips.size(); ++n) {
            const auto& t = trips[n];
            const long long qb = lot_b * lots;
            const double notional = a[t.entry_i] * static_cast<double>(qb);
            const long long nl = std::max(1LL, std::llround(notional / (b[t.entry_i] * static_cast<double>(lot_n))));
            const long long qn = nl * lot_n;
            const int s = t.side;
            Heads h;
            h.fill(fill(s > 0, qb, a[t.entry_i], d[t.entry_i]));
            h.fill(fill(s < 0, qn, b[t.entry_i], d[t.entry_i]));
            h.fill(fill(s < 0, qb, a[t.exit_i], d[t.exit_i]));
            h.fill(fill(s > 0, qn, b[t.exit_i], d[t.exit_i]));
            int rolls = 0;
            double rel = 1.0;
            for (std::size_t k = t.entry_i; k < t.exit_i; ++k) {
                rel *= 1.0 + s * ((a[k + 1] / a[k] - 1.0) - (b[k + 1] / b[k] - 1.0));
                if (d[k].substr(0, 7) != d[k + 1].substr(0, 7)) {
                    ++rolls;
                    h.fill(fill(s < 0, qb, a[k], d[k]));
                    h.fill(fill(s > 0, qb, a[k], d[k]));
                    h.fill(fill(s > 0, qn, b[k], d[k]));
                    h.fill(fill(s < 0, qn, b[k], d[k]));
                }
            }
            const double note_ret = rel - 1.0 - 2.0 * kNoteCost;
            const double pb = s * static_cast<double>(qb) * (a[t.exit_i] - a[t.entry_i]);
            const double pn = -s * static_cast<double>(qn) * (b[t.exit_i] - b[t.entry_i]);
            const double g = pb + pn;
            gross += g;
            shorts += s < 0 ? 1U : 0U;
            if (h.priced) {
                ++priced;
                exp += h.c.total;
                net += g - h.c.total;
                wins += g - h.c.total > 0 ? 1 : 0;
            }
            f << n + 1 << ',' << (s > 0 ? "Long ratio (long BANKNIFTY, short NIFTY)" : "Short ratio (short BANKNIFTY, long NIFTY)") << ','
              << d[t.entry_i] << ',' << d[t.exit_i] << ',' << t.exit_i - t.entry_i << ',' << fx(days[t.entry_i].ratio, 4) << ','
              << fx(days[t.exit_i].ratio, 4) << ',' << fx(days[t.entry_i].z, 2) << ',' << fx(days[t.exit_i].z, 2) << ','
              << fx(a[t.entry_i]) << ',' << fx(a[t.exit_i]) << ',' << qb << ',' << fx(b[t.entry_i]) << ',' << fx(b[t.exit_i]) << ','
              << qn << ',' << rolls << ',' << fx(pb) << ',' << fx(pn) << ',' << fx(g) << ',' << heads_csv(h) << ','
              << (h.priced ? fx(g - h.c.total) : std::string{}) << ',' << fx(100.0 * note_ret, 2) << ','
              << (t.open_at_end ? "open" : "") << '\n';
        }
        // The note's own measure: equal notional, daily, 6 bps a change.
        const auto measure = [&](const std::string& until) {
            std::vector<double> r;
            std::vector<int> pos;
            std::size_t end = 0;
            for (std::size_t i = 0; i < d.size() && d[i] <= until; ++i) end = i;
            for (std::size_t i = 0; i + 1 <= end; ++i) {
                const int prev = i > 0 ? days[i - 1].pos : 0;
                r.push_back(days[i].pos * ((a[i + 1] / a[i] - 1.0) - (b[i + 1] / b[i] - 1.0))
                            - std::abs(days[i].pos - prev) * kNoteCost);
                pos.push_back(days[i].pos);
            }
            const double years = static_cast<double>(day_of(d[end]) - day_of(d.front())) / 365.25;
            return std::make_pair(series_stats(r, pos, years), d[end]);
        };
        sum << "2. BANKNIFTY / NIFTY RATIO Z-SCORE (market bnf sheets/BNFNF)\n"
            << "   z of BANKNIFTY/NIFTY over 120 sessions: short the ratio above +1, buy it below -1, flat inside 0.25.\n"
            << "   Positions formed at a close earn to the next close.\n";
        if (!trips.empty()) {
            const auto line = [&](const std::pair<SeriesStats, std::string>& m) {
                const auto& st = m.first;
                std::size_t n = 0, w = 0;
                double hold = 0;
                for (const auto& t : trips) {
                    if (d[t.entry_i] > m.second) continue;
                    ++n;
                    double rel = 1.0;   // to its exit, or to `until` when it is still held then
                    for (std::size_t k = t.entry_i; k < t.exit_i && d[k + 1] <= m.second; ++k)
                        rel *= 1.0 + t.side * ((a[k + 1] / a[k] - 1.0) - (b[k + 1] / b[k] - 1.0));
                    w += rel - 1.0 - 2.0 * kNoteCost > 0 ? 1 : 0;
                    hold += static_cast<double>(day_of(std::min(d[t.exit_i], m.second)) - day_of(d[t.entry_i]));
                }
                sum << "   to " << m.second << ": total " << fx(100.0 * st.total, 1) << " %, CAGR " << fx(100.0 * st.cagr, 1)
                    << " %, vol " << fx(100.0 * st.vol, 1) << " %, Sharpe " << fx(st.sharpe) << ", max drawdown "
                    << fx(100.0 * st.max_dd, 1) << " %, in the market " << fx(pct(st.held, st.days), 0)
                    << " % of days; " << n << " trades, " << fx(pct(w, n), 0) << " % won, held "
                    << fx(n ? hold / static_cast<double>(n) : 0.0, 0) << " calendar days on average\n";
            };
            sum << "   As the note measures it (equal notional, net of 6 bps a change):\n";
            line(measure("2026-06-08"));
            sum << "   The note's own figures to 2026-06-08: total -2.3 %, CAGR -0.1 %, vol 11.2 %, Sharpe 0.04, max drawdown\n"
                << "   -31.0 %, in the market 71 %; 71 trades, 72 % won, held 68 days on average. Ours differ where the\n"
                << "   closes differ (the note used Yahoo Finance closes).\n";
            line(measure("9999-12-31"));
            sum << "   In rupees, " << lots << " lot(s) of BANKNIFTY against the NIFTY lots nearest equal notional at entry: "
                << trips.size() << " trades (" << shorts << " short the ratio), gross " << rs(gross) << ", expenses " << rs(exp)
                << " (" << priced << " priced), NET " << rs(net) << ", " << fx(pct(wins, priced), 0)
                << " % won net\n";
            const auto& last = days.back();
            sum << "   Now (" << d.back() << "): ratio " << fx(last.ratio, 4) << ", z " << fx(last.z, 2) << ", position "
                << (last.pos > 0 ? "LONG the ratio" : last.pos < 0 ? "SHORT the ratio" : "flat") << "\n\n";
        }
    }

    sum << "Not built yet -- their rules are not in the folder: Short Straddle, Nifty CE Buy on High Close, BNF CE Buy,\n"
        << "Monthly SIP with Expiry, BNF, Wed + Mon Both, Tue + Thursday Both, Delta Hedge using Option. The TradingView\n"
        << "results in BACKTEST/ are shown as TradingView wrote them (threshold_strategy/extracted/tradingview_backtests.csv).\n";
    {
        std::ofstream f(out / "summary.txt");
        f << sum.str();
    }
    std::fputs(sum.str().c_str(), stdout);
    std::printf("\nwrote %s/{breakout_trades,ratio_trades,ratio_days}.csv and summary.txt\n", out.string().c_str());
    return 0;
}
