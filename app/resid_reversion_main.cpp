// app/resid_reversion_main.cpp -- altair_resid_reversion.
//
// Cross-sectional statistical arbitrage over a stock universe
// (strategies/residual_reversion.hpp, after Avellaneda & Lee 2010): every
// stock's residual against the market and its sector peers, traded on its
// s-score, many small hedged positions at once.
//
// READ THE ASSUMPTIONS BEFORE THE NUMBERS (meta.csv repeats them):
//   * the universe is TODAY's NIFTY 50 (config/universe_nifty50.csv): the
//     stocks that survived into the index. Backtested over a decade, that is
//     survivorship bias, and it flatters the result;
//   * each position is `--notional` rupees of stock futures against beta x
//     that in NIFTY futures and its sector peers -- a fractional, notional
//     book. Real stock futures trade in whole lots of a few lakh each;
//   * every leg is charged as futures through risk/cost.hpp at entry and at
//     exit (Rs 20 or 0.03 % brokerage, the lower), REFUSED while
//     config/charges.toml is unverified unless --unverified-costs.
//
// Stock data is broker data in data/pairs/ (git-ignored): ops/fetch_universe.ps1.
// Writes <out>/resid_reversion/{trades,daily}_<variant>.csv, summary.csv, meta.csv.

#include <app/demo_costs.hpp>
#include <app/forecast_tracks.hpp>
#include <risk/charges_toml.hpp>
#include <strategies/residual_reversion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace ft = altair::forecast_tracks;
namespace da = altair::data_audit;
namespace dc = altair::demo_costs;

void usage(const char* exe) {
    std::printf(
        "  Trade every stock's residual against the market and its sector peers (Avellaneda-Lee).\n\n"
        "    %s [--universe FILE] [--market DIR] [--out DIR] [--notional RS] [--window N]\n"
        "       [--entry S] [--unverified-costs]\n\n"
        "    --universe FILE       default config/universe_nifty50.csv (symbol,fyers,sector,dir)\n"
        "    --market DIR          the market's daily bars (default dataset/spot/nifty/1d)\n"
        "    --out DIR             default data/verified (writes resid_reversion/)\n"
        "    --notional RS         rupees per position (default 1000000)\n"
        "    --window N            estimation window in sessions (default 60)\n"
        "    --entry S             |s-score| to open (default 1.25)\n"
        "    --unverified-costs    price expenses from an UNVERIFIED config/charges.toml, stamped\n", exe);
}

bool parse_double(std::string_view s, double& out) {
    const std::string v{s};
    char* end = nullptr;
    out = std::strtod(v.c_str(), &end);
    return end != v.c_str() && *end == '\0' && std::isfinite(out);
}

std::string fixed(double v, int d) {
    if (!std::isfinite(v)) { return {}; }
    char b[64];
    std::snprintf(b, sizeof b, "%.*f", d, v);
    return b;
}

std::string iso_day(std::int64_t day) { return da::format_audit_time(day * 86'400, true).substr(0, 10); }

struct Stock { std::string symbol, sector, dir; };

} // namespace

int main(int argc, char** argv) {
    fs::path universe = "config/universe_nifty50.csv", market_dir = "dataset/spot/nifty/1d", out = "data/verified";
    double notional = 1'000'000.0, window = 60, entry = 1.25;
    bool unverified = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a{argv[i]};
        const bool has = i + 1 < argc;
        if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
        if (a == "--universe" && has) { universe = argv[++i]; continue; }
        if (a == "--market" && has) { market_dir = argv[++i]; continue; }
        if (a == "--out" && has) { out = argv[++i]; continue; }
        if (a == "--notional" && has) { if (!parse_double(argv[++i], notional) || !(notional > 0.0)) { usage(argv[0]); return 2; } continue; }
        if (a == "--window" && has) { if (!parse_double(argv[++i], window) || window < 20 || window > 500) { usage(argv[0]); return 2; } continue; }
        if (a == "--entry" && has) { if (!parse_double(argv[++i], entry) || !(entry > 0.75) || entry > 5.0) { usage(argv[0]); return 2; } continue; }
        if (a == "--unverified-costs") { unverified = true; continue; }
        usage(argv[0]);
        return 2;
    }

    std::vector<altair::ChargeSchedule> schedules;
    const auto rep = altair::load_charges_file("config/charges.toml", schedules);
    bool costs_allowed = false, costs_verified = false;
    std::string cost_note;
    if (!rep) {
        cost_note = std::string{"config/charges.toml did not load: "} + altair::charges_error_text(rep.error());
    } else if (rep->verified) {
        costs_allowed = costs_verified = true;
        cost_note = "config/charges.toml, verified";
    } else if (unverified) {
        for (auto& s : schedules) { s.verified = true; }   // --unverified-costs: the caller's explicit choice
        costs_allowed = true;
        cost_note = "config/charges.toml is UNVERIFIED: expenses priced anyway (--unverified-costs); every expense and net figure is UNVERIFIED";
    } else {
        cost_note = "COSTING REFUSED: config/charges.toml is UNVERIFIED (block_on_unverified_schedule). Gross P&L only; "
                    "verify the rates and set last_verified, or pass --unverified-costs";
    }
    const std::string cost_tag = costs_allowed ? (costs_verified ? "verified" : "UNVERIFIED") : "refused";

    // Universe.
    std::vector<Stock> stocks;
    {
        std::ifstream in(universe);
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#' || line.rfind("symbol,", 0) == 0) { continue; }
            std::vector<std::string> f;
            std::stringstream ss(line);
            for (std::string c; std::getline(ss, c, ',');) { f.push_back(c); }
            if (f.size() >= 4) { stocks.push_back({f[0], f[2], f[3]}); }
        }
    }
    if (stocks.empty()) { std::printf("  no stocks in %s\n", universe.string().c_str()); return 2; }

    // Closes by day: the market's days are the panel's days.
    const auto closes = [](const fs::path& dir) {
        ft::TrackInfo info;
        auto bars = ft::load_bars(dir, da::kDailyTf, info);
        ft::clean_bars(bars, info);
        std::map<std::int64_t, double> m;
        for (const auto& b : bars) { if (b.c > 0.0) { m[ft::bar_day(b)] = b.c; } }
        return m;
    };
    const auto mkt = closes(market_dir);
    if (mkt.size() < 100) { std::printf("  no market data under %s\n", market_dir.string().c_str()); return 1; }
    std::vector<Stock> have;
    std::vector<std::map<std::int64_t, double>> px;
    std::vector<std::string> missing;
    for (const auto& s : stocks) {
        auto c = closes(s.dir);
        if (c.size() < 100) { missing.push_back(s.symbol + " (" + s.dir + ")"); continue; }
        have.push_back(s);
        px.push_back(std::move(c));
    }
    std::printf("Residual reversion: %zu of %zu stocks have data\n  %s\n", have.size(), stocks.size(), cost_note.c_str());
    if (!missing.empty()) {
        std::printf("  missing: ");
        for (std::size_t k = 0; k < missing.size(); ++k) { std::printf("%s%s", k ? ", " : "", missing[k].c_str()); }
        std::printf("\n  fetch with: powershell -ExecutionPolicy Bypass -File ops\\fetch_universe.ps1 -Go\n");
    }
    if (have.size() < 10) { std::printf("  fewer than 10 stocks: not run\n"); return 1; }

    // The panel: the market's days from the first day any stock has data.
    std::int64_t first = mkt.rbegin()->first;
    for (const auto& c : px) { first = std::min(first, c.begin()->first); }
    altair::RrPanel p;
    std::vector<std::vector<double>> close(have.size());
    std::vector<double> mclose;
    for (auto it = mkt.lower_bound(first); it != mkt.end(); ++it) {
        p.day.push_back(it->first);
        mclose.push_back(it->second);
        for (std::size_t i = 0; i < have.size(); ++i) {
            const auto c = px[i].find(it->first);
            close[i].push_back(c == px[i].end() ? std::nan("") : c->second);
        }
    }
    const std::size_t T = p.day.size();
    p.market.assign(T, std::nan(""));
    p.ret.assign(have.size(), std::vector<double>(T, std::nan("")));
    for (std::size_t t = 1; t < T; ++t) {
        p.market[t] = std::log(mclose[t] / mclose[t - 1]);
        for (std::size_t i = 0; i < have.size(); ++i) {
            if (std::isfinite(close[i][t]) && std::isfinite(close[i][t - 1])) { p.ret[i][t] = std::log(close[i][t] / close[i][t - 1]); }
        }
    }
    std::map<std::string, int> sector_id;
    for (const auto& s : have) {
        if (!sector_id.contains(s.sector)) { sector_id[s.sector] = static_cast<int>(sector_id.size()); }
        p.sector.push_back(sector_id[s.sector]);
    }

    std::ostringstream meta;
    meta << "key,value\ncosts," << cost_tag << "\ncost_note,\"" << cost_note << "\"\n";
    meta << "universe,\"" << universe.string() << ": " << have.size() << " of " << stocks.size() << " stocks with data\"\n";
    meta << "survivorship,\"TODAY's index members backtested over the past: survivors only, biased upward\"\n";
    meta << "sizing,\"Rs " << fixed(notional, 0) << " of stock futures a position, hedged with beta x that in NIFTY futures and sector peers; fractional, not whole lots\"\n";
    meta << "window," << window << "\nentry," << entry << "\nexit_long,0.50\nexit_short,0.75\nmin_half_life_days,30\n";

    const fs::path dir = out / "resid_reversion";
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::ofstream summary(dir / "summary.csv", std::ios::trunc);
    summary << "variant,trades,wins,win_pct,avg_days_held,gross_pnl,expenses,net_pnl,net_per_trade,avg_positions,"
               "daily_sharpe_annualised,daily_t_stat,first,last,costs\n";
    std::printf("  %-16s %7s %6s %6s %13s %13s %10s %8s %6s\n", "variant", "trades", "win %", "held", "gross Rs", "net Rs", "net/tr",
                "Sharpe", "t");

    const auto fut_cost = [&](double rupees, bool buy, std::int64_t day) {
        // Notional-priced futures leg: charges scale with notional, brokerage capped at Rs 20.
        return dc::fill(altair::Segment::Fut, buy ? altair::Side::Buy : altair::Side::Sell, std::fabs(rupees) / 100.0, 100.0,
                        day * 86'400 + ft::kCloseSec, schedules);
    };

    for (const bool sector : {true, false}) {
        const std::string id = sector ? "market+sector" : "market-only";
        altair::RrPolicy pol;
        pol.window = static_cast<std::size_t>(window);
        pol.entry = entry;
        pol.sector_factor = sector;
        const auto r = altair::rr_run(p, pol);
        if (!r) { std::printf("  %s: refused: %s\n", id.c_str(), altair::rr_error_text(r.error())); continue; }
        std::vector<double> day_cost(T, 0.0);
        std::ofstream tf(dir / ("trades_" + id + ".csv"), std::ios::trunc);
        tf << "symbol,sector,side,entry_date,exit_date,days_held,s_entry,s_exit,beta_market,beta_sector,kappa,hedged_return_bp,"
              "gross_pnl,expenses,net_pnl,exit_reason,costs\n";
        double gross = 0, exp = 0, held = 0;
        std::size_t wins = 0;
        for (const auto& t : r->trades) {
            const double g = t.ret * notional;
            dc::Costs c;
            bool priced = costs_allowed;
            if (costs_allowed) {
                const auto add = [&](const dc::Costs& x) { c.add(x); priced = priced && x.priced; };
                const bool long_stock = t.side > 0;
                for (const auto& [leg, buy_in] : {std::pair{notional, long_stock}, std::pair{t.beta_m * notional, !long_stock},
                                                  std::pair{t.beta_s * notional, !long_stock}}) {
                    if (std::fabs(leg) < 1.0) { continue; }
                    const bool buy = leg > 0 ? buy_in : !buy_in;   // a negative beta hedges the other way
                    add(fut_cost(leg, buy, p.day[t.entry]));
                    add(fut_cost(leg, !buy, p.day[t.exit]));
                }
                day_cost[t.exit] += c.total;   // charged on the day the trade closes
            }
            const double net = priced ? g - c.total : g;
            gross += g;
            exp += priced ? c.total : 0.0;
            wins += net > 0.0 ? 1 : 0;
            held += static_cast<double>(t.exit - t.entry);
            tf << have[t.stock].symbol << ',' << have[t.stock].sector << ',' << (t.side > 0 ? "long" : "short") << ','
               << iso_day(p.day[t.entry]) << ',' << iso_day(p.day[t.exit]) << ',' << (t.exit - t.entry) << ',' << fixed(t.s_entry, 2)
               << ',' << fixed(t.s_exit, 2) << ',' << fixed(t.beta_m, 3) << ',' << fixed(t.beta_s, 3) << ',' << fixed(t.kappa, 1) << ','
               << fixed(1e4 * t.ret, 1) << ',' << fixed(g, 2) << ',' << (priced ? fixed(c.total, 2) : std::string{}) << ','
               << (priced ? fixed(net, 2) : std::string{}) << ',' << t.why << ',' << cost_tag << '\n';
        }
        // Daily book: hedged P&L of every open position, expenses on closing days.
        std::ofstream df(dir / ("daily_" + id + ".csv"), std::ios::trunc);
        df << "date,longs,shorts,gross_pnl,expenses,net_pnl,cumulative_net\n";
        double cum = 0, s1 = 0, s2 = 0, pos = 0;
        std::size_t n = 0;
        std::size_t start = static_cast<std::size_t>(window);
        for (std::size_t t = start; t < T; ++t) {
            const double g = r->daily[t].ret * notional;
            const double net = g - day_cost[t];
            cum += net;
            s1 += net;
            s2 += net * net;
            pos += static_cast<double>(r->daily[t].longs + r->daily[t].shorts);
            ++n;
            df << iso_day(p.day[t]) << ',' << r->daily[t].longs << ',' << r->daily[t].shorts << ',' << fixed(g, 2) << ','
               << fixed(day_cost[t], 2) << ',' << fixed(net, 2) << ',' << fixed(cum, 2) << '\n';
        }
        const double nt = static_cast<double>(r->trades.size()), nd = static_cast<double>(std::max<std::size_t>(n, 1));
        const double mean = s1 / nd, sd = std::sqrt(std::max(0.0, s2 / nd - mean * mean));
        const double sharpe = sd > 0.0 ? mean / sd * std::sqrt(252.0) : 0.0;
        const double tstat = sd > 0.0 ? mean / (sd / std::sqrt(nd)) : 0.0;
        const double net_total = gross - exp;
        summary << id << ',' << r->trades.size() << ',' << wins << ',' << fixed(nt > 0 ? 100.0 * static_cast<double>(wins) / nt : 0.0, 1)
                << ',' << fixed(nt > 0 ? held / nt : 0.0, 1) << ',' << fixed(gross, 2) << ','
                << (costs_allowed ? fixed(exp, 2) : std::string{}) << ',' << (costs_allowed ? fixed(net_total, 2) : std::string{}) << ','
                << (costs_allowed && nt > 0 ? fixed(net_total / nt, 2) : std::string{}) << ',' << fixed(pos / nd, 1) << ','
                << fixed(sharpe, 2) << ',' << fixed(tstat, 2) << ',' << (T > start ? iso_day(p.day[start]) : std::string{}) << ','
                << iso_day(p.day.back()) << ',' << cost_tag << '\n';
        std::printf("  %-16s %7zu %6.1f %6.1f %13.0f %13s %10s %8.2f %6.2f\n", id.c_str(), r->trades.size(),
                    nt > 0 ? 100.0 * static_cast<double>(wins) / nt : 0.0, nt > 0 ? held / nt : 0.0, gross,
                    costs_allowed ? fixed(net_total, 0).c_str() : "refused",
                    costs_allowed && nt > 0 ? fixed(net_total / nt, 0).c_str() : "-", sharpe, tstat);
    }
    { std::ofstream f(dir / "meta.csv", std::ios::trunc); f << meta.str(); }
    std::printf("  wrote %s\n", dir.string().c_str());
    return 0;
}
