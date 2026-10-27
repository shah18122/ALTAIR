// app/as_mm_main.cpp -- altair_as_mm: Avellaneda-Stoikov quoting on NIFTY or
// BANKNIFTY futures, simulated on FYERS one-minute bars, on paper
// (strategies/as_market_maker.hpp).
//
// FUTURES ONLY. A market maker's bid is a buy, and ALTAIR never buys an
// option, so this tool quotes the index future and nothing else. Options get
// only the sell-only ask of analytics/avellaneda_stoikov.hpp.
//
// WHAT IT WRITES (<out>, default data/verified/as_mm/<folder>):
//   fills.csv    every fill: time, side, lots, price, inventory before and
//                after, mid, reservation, spread, sigma, k, tau, gamma,
//                P(stress), expense, five-minute markout, and the reason
//   days.csv     every session: fills, gross, expenses, net, largest inventory
//   summary.txt  totals, Sharpe, drawdown, spread captured against adverse
//                selection, the leak check
//
// --regimes FILE takes altair_regimes' CSV: each session's gamma is scaled by
// (1 + P(stress)) as of the PREVIOUS close.
// --leak-check reruns with every bar after three cuts replaced and needs every
// quote and fill up to each cut unchanged. Exit 3 if one is not.
//
// EXPENSES. --cost-bp is every expense of ONE fill in bp of notional; the
// default is half of app/forecast_tracks.hpp's futures round trip at the
// latest session (STT as dated, plus 1 bp of everything else), applied to
// every session, which overcharges sessions before the STT rise.

#include <app/data_audit.hpp>
#include <app/forecast_tracks.hpp>
#include <app/fyers_source.hpp>
#include <strategies/as_market_maker.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace da = altair::data_audit;
namespace ft = altair::forecast_tracks;

void usage(const char* exe) {
    std::printf(
        "usage: %s --fyers DIR [--out DIR] [--lot N] [--gamma G] [--max-lots N] [--cost-bp B]\n"
        "          [--tick T] [--regimes FILE] [--leak-check] [--not-fyers]\n"
        "  --fyers      a folder of FYERS 1-minute futures bars (SOURCE.txt naming FYERS, or under a folder named fyers)\n"
        "  --out        where to write (default data/verified/as_mm/<folder>)\n"
        "  --lot        units per lot (default 65, NIFTY; BANKNIFTY is 30)\n"
        "  --gamma      risk aversion per point per lot (default 1e-4)\n"
        "  --max-lots   inventory cap either way (default 5)\n"
        "  --cost-bp    every expense of one fill, bp of notional (default: half the dated futures round trip)\n"
        "  --tick       tick size (default 0.05)\n"
        "  --regimes    altair_regimes CSV: gamma x (1 + yesterday's P(stress))\n"
        "  --leak-check rerun with another future after three cuts; exit 3 if any earlier quote or fill changes\n"
        "  --not-fyers  run on a folder that is not FYERS: a smoke test, stamped NOT FYERS, never a result\n",
        exe);
}

std::string stamp(std::int64_t t) {
    const auto c = da::audit_civil_from_days(da::audit_day(t));
    const auto m = da::audit_minute_of_day(t);
    char b[64];
    std::snprintf(b, sizeof b, "%04lld-%02u-%02u %02lld:%02lld", static_cast<long long>(c.y), c.m, c.d,
                  static_cast<long long>(m / 60), static_cast<long long>(m % 60));
    return b;
}

std::string csv_quote(const std::string& s) {
    std::string o = "\"";
    for (const char ch : s) { if (ch == '"') o += '"'; o += ch; }
    return o + "\"";
}

/// altair_regimes' CSV: date -> p_stress.
std::map<std::int64_t, double> load_regimes(const fs::path& f, std::size_t& bad) {
    std::map<std::int64_t, double> m;
    std::ifstream in(f);
    std::string line;
    std::getline(in, line);
    while (std::getline(in, line)) {
        const auto c1 = line.find(',');
        const auto c2 = c1 == std::string::npos ? c1 : line.find(',', c1 + 1);
        if (c2 == std::string::npos) { ++bad; continue; }
        const auto t = da::parse_audit_time(std::string_view(line).substr(0, c1));
        const double p = std::atof(line.substr(c1 + 1, c2 - c1 - 1).c_str());
        if (!t || !std::isfinite(p)) { ++bad; continue; }
        m[da::audit_day(*t)] = p;
    }
    return m;
}

} // namespace

int main(int argc, char** argv) {
    fs::path dir, out, regimes_file;
    altair::AsMmParams p;
    double cost_bp = -1.0;
    bool leak = false, not_fyers = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        const bool has = i + 1 < argc;
        if (a == "--fyers" && has) dir = argv[++i];
        else if (a == "--out" && has) out = argv[++i];
        else if (a == "--lot" && has) p.lot_units = std::max(1.0, std::atof(argv[++i]));
        else if (a == "--gamma" && has) p.gamma = std::atof(argv[++i]);
        else if (a == "--max-lots" && has) p.max_lots = std::max(1, std::atoi(argv[++i]));
        else if (a == "--cost-bp" && has) cost_bp = std::atof(argv[++i]);
        else if (a == "--tick" && has) p.tick = std::atof(argv[++i]);
        else if (a == "--regimes" && has) regimes_file = argv[++i];
        else if (a == "--leak-check") leak = true;
        else if (a == "--not-fyers") not_fyers = true;
        else if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
        else { usage(argv[0]); return 2; }
    }
    if (dir.empty() || !(p.gamma > 0.0) || !(p.tick > 0.0)) { usage(argv[0]); return 2; }
    const bool fyers = altair::app::is_fyers_folder(dir);
    if (!fyers && !not_fyers) {
        std::printf("%s is not marked as FYERS data (no SOURCE.txt naming FYERS, no 'fyers' folder in its path).\n"
                    "ALTAIR runs on FYERS data only. Fetch it with altair_fyers_history, or pass --not-fyers for a smoke test.\n",
                    dir.string().c_str());
        return 2;
    }

    ft::TrackInfo info;
    auto raw = ft::load_bars(dir, 1, info);
    ft::clean_bars(raw, info);
    if (raw.empty()) { std::printf("no 1-minute bars in %s\n", dir.string().c_str()); return 1; }
    std::vector<altair::AsBar> bars;
    bars.reserve(raw.size());
    for (const auto& b : raw) bars.push_back({b.t, b.o, b.h, b.l, b.c});
    p.cost_bp_per_fill = cost_bp >= 0.0 ? cost_bp : 0.5 * ft::futures_cost_bp(da::audit_day(bars.back().t), 1.0);

    std::map<std::int64_t, double> regimes;
    std::size_t bad_reg = 0;
    if (!regimes_file.empty()) {
        regimes = load_regimes(regimes_file, bad_reg);
        if (regimes.empty()) { std::printf("no regimes read from %s\n", regimes_file.string().c_str()); return 2; }
    }
    const auto* reg = regimes.empty() ? nullptr : &regimes;

    std::printf("altair_as_mm: %zu one-minute bars from %s%s; gamma %.3g, cap %d lots, %.2f bp a fill%s\n",
                bars.size(), dir.string().c_str(), fyers ? "" : " (NOT FYERS: smoke test only)", p.gamma, p.max_lots,
                p.cost_bp_per_fill, reg ? ", gamma scaled by the HMM regime" : "");
    const auto r = altair::run_as_market_maker(bars, p, reg);

    std::size_t leak_diff = 0;
    if (leak) {
        for (const std::size_t cut : {bars.size() / 3, bars.size() / 2 + 101, bars.size() * 9 / 10})
            leak_diff += altair::as_leak_check(bars, p, cut, reg);
        std::printf("leak check: %zu differences up to the cuts (%s)\n", leak_diff, leak_diff == 0 ? "pass" : "FAIL");
    }

    if (out.empty()) out = fs::path("data/verified/as_mm") / dir.filename();
    std::error_code ec;
    fs::create_directories(out, ec);
    {
        std::ofstream o(out / "fills.csv", std::ios::binary);
        o << "time,side,lots,price,q_before,q_after,mid,reservation,spread,sigma,k,tau_min,gamma,p_stress,expense_rs,markout_5m_pts,flatten,source,reason\n";
        for (const auto& f : r.fills) {
            char b[512];
            std::snprintf(b, sizeof b, "%s,%s,%d,%.2f,%d,%d,%.2f,%.2f,%.3f,%.4f,%.4f,%.0f,%.3g,%.4f,%.2f,%s,%d,%s,",
                          stamp(f.t).c_str(), f.side > 0 ? "BUY" : "SELL", std::abs(f.q_after - f.q_before), f.price,
                          f.q_before, f.q_after, f.mid, f.reservation, f.spread, f.sigma, f.k, f.tau, f.gamma, f.p_stress,
                          f.cost_rs, std::isfinite(f.markout_pts) ? std::to_string(f.markout_pts).c_str() : "",
                          f.flatten ? 1 : 0, fyers ? "FYERS" : "NOT FYERS");
            o << b << csv_quote(f.reason) << '\n';
        }
    }
    double net = 0.0, gross = 0.0, cost = 0.0, peak = 0.0, dd = 0.0, sum = 0.0, sum2 = 0.0;
    std::size_t up = 0, fills = 0;
    int max_q = 0;
    {
        std::ofstream o(out / "days.csv", std::ios::binary);
        o << "date,fills,gross_rs,expenses_rs,net_rs,max_abs_lots\n";
        for (const auto& d : r.days) {
            char b[160];
            std::snprintf(b, sizeof b, "%s,%zu,%.2f,%.2f,%.2f,%d\n", stamp(d.day * 86'400).substr(0, 10).c_str(), d.fills,
                          d.gross_rs, d.cost_rs, d.net_rs, d.max_abs_q);
            o << b;
            gross += d.gross_rs; cost += d.cost_rs; net += d.net_rs;
            sum += d.net_rs; sum2 += d.net_rs * d.net_rs;
            peak = std::max(peak, net);
            dd = std::max(dd, peak - net);
            if (d.net_rs > 0.0) ++up;
            fills += d.fills;
            max_q = std::max(max_q, d.max_abs_q);
        }
    }
    double mk = 0.0, half = 0.0;
    std::size_t nmk = 0, nq = 0;
    for (const auto& f : r.fills) {
        if (f.flatten) continue;
        half += f.side * (f.mid - f.price);   // edge against the mid at the fill
        ++nq;
        if (std::isfinite(f.markout_pts)) { mk += f.markout_pts; ++nmk; }
    }
    const double n = static_cast<double>(r.days.size());
    const double mean = n > 0 ? sum / n : 0.0;
    const double sd = n > 1 ? std::sqrt(std::max(0.0, (sum2 - n * mean * mean) / (n - 1.0))) : 0.0;
    std::ostringstream s;
    s << "altair_as_mm -- Avellaneda-Stoikov quoting on index futures, one-minute bars, paper only\n"
      << "source: " << dir.string() << (fyers ? " (FYERS)" : " (NOT FYERS: smoke test only, not a result)") << "\n";
    char b[1024];
    std::snprintf(b, sizeof b,
        "sessions %zu, fills %zu (lots), largest inventory %d lots\n"
        "gross Rs %.0f, expenses Rs %.0f, net Rs %.0f; Rs %.0f a session, sd %.0f, Sharpe %.2f (x sqrt 252)\n"
        "sessions up %zu of %zu; max drawdown Rs %.0f\n"
        "quoted edge against the mid: %.2f pts a fill on %zu quote fills\n"
        "five-minute markout: %.2f pts a fill on %zu fills (negative = adverse selection eats the edge)\n"
        "expenses %.2f bp a fill; gamma %.3g; cap %d lots; tick %.2f; lot %.0f%s\n",
        r.days.size(), fills, max_q, gross, cost, net, mean, sd, sd > 0 ? mean / sd * std::sqrt(252.0) : 0.0,
        up, r.days.size(), dd, nq ? half / static_cast<double>(nq) : 0.0, nq,
        nmk ? mk / static_cast<double>(nmk) : 0.0, nmk, p.cost_bp_per_fill, p.gamma, p.max_lots, p.tick, p.lot_units,
        reg ? "; gamma x (1 + yesterday's HMM P(stress))" : "");
    s << b;
    s << "Fills are simulated from bars (trade-through by a tick at the quote); the book, the queue and partial fills are not seen.\n";
    if (leak) s << "leak check: " << leak_diff << " differences up to three cuts (" << (leak_diff == 0 ? "pass" : "FAIL") << ")\n";
    if (bad_reg) s << bad_reg << " regime rows unreadable and skipped\n";
    std::ofstream(out / "summary.txt", std::ios::binary) << s.str();
    std::printf("%s-> %s\n", s.str().c_str(), out.string().c_str());
    return leak_diff == 0 ? 0 : 3;
}
