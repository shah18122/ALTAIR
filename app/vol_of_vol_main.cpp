// app/vol_of_vol_main.cpp -- altair_vol_of_vol: sell vol-of-vol on NSE closes.
//
// strategies/vol_of_vol.hpp on the NSE F&O bhavcopy (app/bhavcopy.hpp; fetch
// with ops/fetch_bhavcopy.ps1): each week, short the 25-delta strangle of the
// first monthly expiry at least 10 sessions out, long the ATM straddle lots
// that cancel its vega, delta hedged with futures at every close. Three
// variants side by side, so the result can be read against its own controls:
//   short-always      sell vol-of-vol every week
//   short-rich-wings  only when the 25-delta butterfly is above its median of
//                     the previous 60 closes (the wings dear)
//   long-always       the mirror: what buying vol-of-vol would have done
// Expenses from config/charges.toml (app/demo_costs.hpp), refused while that
// schedule is unverified unless --unverified-costs. Slippage is charged on
// every option fill on top. Closes are last trades, the wings thinner than
// the money: a stale wing close is a price nobody may have dealt at.
//
// Writes <out>/vol_of_vol/: trades_<instrument>_<variant>.csv, summary.csv.

#include <app/bhavcopy.hpp>
#include <app/demo_costs.hpp>
#include <app/spec_today.hpp>
#include <risk/charges_toml.hpp>
#include <strategies/vol_of_vol.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace vov = altair::vol_of_vol;
namespace dc = altair::demo_costs;

void usage(const char* exe) {
    std::printf(
        "  Sell vol-of-vol: short the 25-delta strangle, long the ATM straddle, vega-neutral, delta hedged.\n\n"
        "    %s [--bhavcopy-dir DIR] [--out DIR] [--instrument NIFTY|BANKNIFTY|both]\n"
        "       [--strangle-lots N] [--hold N] [--slippage-pts P] [--rate R] [--unverified-costs]\n\n"
        "    --bhavcopy-dir DIR    default data/bhavcopy (ops/fetch_bhavcopy.ps1 fills it)\n"
        "    --out DIR             default data/verified\n"
        "    --strangle-lots N     lots of the short strangle (default 5); the straddle is sized to its vega\n"
        "    --hold N              closes each position is held before it is rolled (default 5)\n"
        "    --slippage-pts P      premium points given up per option fill (default 1.0)\n"
        "    --unverified-costs    price expenses from an UNVERIFIED config/charges.toml, stamped\n", exe);
}

bool parse_double(std::string_view s, double& out) {
    try {
        std::size_t used = 0;
        out = std::stod(std::string(s), &used);
        return used == s.size() && std::isfinite(out);
    } catch (...) {
        return false;
    }
}

std::string day_text(std::int64_t day) {
    std::int64_t z = day + 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const std::int64_t doe = z - era * 146097;
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::int64_t mp = (5 * doy + 2) / 153;
    const std::int64_t d = doy - (153 * mp + 2) / 5 + 1;
    const std::int64_t m = mp < 10 ? mp + 3 : mp - 9;
    const std::int64_t y = yoe + era * 400 + (m <= 2 ? 1 : 0);
    char b[48];
    std::snprintf(b, sizeof b, "%04lld-%02lld-%02lld", static_cast<long long>(y), static_cast<long long>(m), static_cast<long long>(d));
    return b;
}

struct Variant { std::string id; vov::VovGate gate; int side; };

} // namespace

int main(int argc, char** argv) {
    fs::path out = "data/verified", bhav_dir = "data/bhavcopy";
    std::string which = "both";
    double rate = 0.065, slip = 1.0, lots = 5, hold = 5;
    bool unverified = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a{argv[i]};
        const bool has = i + 1 < argc;
        if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
        if (a == "--out" && has) { out = argv[++i]; continue; }
        if (a == "--instrument" && has) { which = argv[++i]; continue; }
        if (a == "--bhavcopy-dir" && has) { bhav_dir = argv[++i]; continue; }
        if (a == "--rate" && has) { if (!parse_double(argv[++i], rate) || std::fabs(rate) > 1.0) { usage(argv[0]); return 2; } continue; }
        if (a == "--slippage-pts" && has) { if (!parse_double(argv[++i], slip) || slip < 0.0) { usage(argv[0]); return 2; } continue; }
        if (a == "--strangle-lots" && has) { if (!parse_double(argv[++i], lots) || lots < 1 || lots > 100) { usage(argv[0]); return 2; } continue; }
        if (a == "--hold" && has) { if (!parse_double(argv[++i], hold) || hold < 1 || hold > 20) { usage(argv[0]); return 2; } continue; }
        if (a == "--unverified-costs") { unverified = true; continue; }
        usage(argv[0]);
        return 2;
    }
    if (which != "NIFTY" && which != "BANKNIFTY" && which != "both") { usage(argv[0]); return 2; }

    std::vector<altair::ChargeSchedule> schedules;
    const auto rep = altair::load_charges_file("config/charges.toml", schedules);
    bool costs_allowed = false;
    std::string cost_note;
    if (!rep) {
        cost_note = std::string{"config/charges.toml did not load: "} + altair::charges_error_text(rep.error());
    } else if (rep->verified) {
        costs_allowed = true;
        cost_note = "config/charges.toml, verified";
    } else if (unverified) {
        for (auto& s : schedules) s.verified = true;   // --unverified-costs: the caller's explicit choice
        costs_allowed = true;
        cost_note = "config/charges.toml is UNVERIFIED: expenses priced anyway (--unverified-costs); every expense and net is UNVERIFIED";
    } else {
        cost_note = "COSTING REFUSED: config/charges.toml is UNVERIFIED. Gross P&L only; verify it, or pass --unverified-costs";
    }
    std::printf("altair_vol_of_vol: %s\n", cost_note.c_str());

    const fs::path dir = out / "vol_of_vol";
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::ofstream summary(dir / "summary.csv");
    summary << "instrument,variant,first,last,trades,won,gross,expenses,net,net_per_trade,sd_per_trade,t_stat,sharpe_annual,"
               "max_drawdown,worst_trade,mean_butterfly_vol_pts,mean_volga_per_volpt2,costs\n";
    const std::vector<Variant> variants{{"short-always", vov::VovGate::Always, -1},
                                        {"short-rich-wings", vov::VovGate::RichWings, -1},
                                        {"long-always", vov::VovGate::Always, 1}};
    std::vector<std::string> names;
    if (which == "both" || which == "NIFTY") names.push_back("NIFTY");
    if (which == "both" || which == "BANKNIFTY") names.push_back("BANKNIFTY");
    for (const auto& name : names) {
        const auto lot = altair::spec_today::lot_size("config/lot_size_history.csv", name);
        if (!lot) { std::printf("  %s: no verified lot size -- not run\n", name.c_str()); continue; }
        altair::bhavcopy::LoadReport br;
        const auto bhav = std::make_shared<std::map<std::int64_t, altair::bhavcopy::Day>>(altair::bhavcopy::load_dir(bhav_dir, name, br));
        std::printf("  %s bhavcopy: %zu files, %zu option and %zu futures rows, %zu untraded skipped\n", name.c_str(), br.files,
                    br.option_rows, br.future_rows, br.untraded);
        if (bhav->empty()) {
            std::printf("  %s: no bhavcopy under %s -- run ops\\fetch_bhavcopy.ps1 -Go first\n", name.c_str(), bhav_dir.string().c_str());
            continue;
        }
        vov::VovMarket m;
        for (const auto& [day, bd] : *bhav) {
            if (bd.fut.empty()) continue;
            m.day.push_back(day);
            for (const auto& [e, f] : bd.fut) m.expiries.insert(e);
        }
        const auto days = m.day;
        m.forward = [days, bhav](std::size_t i, std::int64_t e) {
            const auto& d = bhav->at(days[i]);
            const auto f = d.fut.find(e);
            return f == d.fut.end() ? std::nan("") : f->second;
        };
        m.chain = [days, bhav](std::size_t i, std::int64_t e) {
            std::vector<vov::VovStrike> outv;
            const auto& d = bhav->at(days[i]);
            const auto o = d.opt.find(e);
            if (o == d.opt.end()) return outv;
            for (const auto& [k, leg] : o->second) outv.push_back({k, leg.call, leg.put});
            return outv;
        };
        for (const auto& v : variants) {
            vov::VovPolicy p;
            p.lot_size = static_cast<double>(*lot);
            p.strangle_lots = static_cast<int>(lots);
            p.hold_sessions = static_cast<std::size_t>(hold);
            p.side = v.side;
            p.gate = v.gate;
            p.rate = rate;
            p.slippage_pts = slip;
            const auto r = vov::run(m, p);
            if (!r) { std::printf("  %s %s: refused: %s\n", name.c_str(), v.id.c_str(), vov::error_text(r.error())); continue; }
            std::ofstream tf(dir / ("trades_" + name + "_" + v.id + ".csv"));
            tf << "entry,exit,expiry,k_put,k_atm,k_call,vol_put,vol_atm,vol_call,butterfly_vol_pts,strangle_units,straddle_units,"
                  "vega_left_per_volpt,volga_per_volpt2,option_pnl,hedge_pnl,gross,expenses,net,exit_reason\n";
            double gross = 0, exp_sum = 0, net = 0, peak = 0, cum = 0, dd = 0, worst = 0, fly = 0, volga = 0;
            std::size_t won = 0;
            bool unpriced = !costs_allowed;
            std::vector<double> nets;
            for (const auto& t : r->trades) {
                double e = 0.0;
                bool priced = costs_allowed;
                if (costs_allowed)
                    for (const auto& f : t.fills) {
                        const auto c = dc::fill(f.future ? altair::Segment::Fut : altair::Segment::Opt,
                                                f.units > 0 ? altair::Side::Buy : altair::Side::Sell, std::fabs(f.units), f.price,
                                                m.day[f.i] * 86'400 + 15 * 3600 + 30 * 60, schedules);
                        if (!c.priced) { priced = false; break; }
                        e += c.total;
                    }
                if (!priced) unpriced = true;
                const double n = priced ? t.gross - e : std::nan("");
                gross += t.gross;
                if (priced) { exp_sum += e; net += n; nets.push_back(n); }
                const double x = priced ? n : t.gross;
                if (x > 0) ++won;
                worst = std::min(worst, x);
                cum += x;
                peak = std::max(peak, cum);
                dd = std::min(dd, cum - peak);
                fly += t.butterfly;
                volga += t.volga;
                char line[640];
                std::snprintf(line, sizeof line, "%s,%s,%s,%.2f,%.2f,%.2f,%.4f,%.4f,%.4f,%.3f,%.0f,%.0f,%.1f,%.2f,%.2f,%.2f,%.2f,%s,%s,%s\n",
                              day_text(m.day[t.entry]).c_str(), day_text(m.day[t.exit]).c_str(), day_text(t.expiry).c_str(), t.k_put,
                              t.k_atm, t.k_call, t.vol_put, t.vol_atm, t.vol_call, t.butterfly * 100.0, t.strangle_units,
                              t.straddle_units, t.vega_left, t.volga, t.option_pnl, t.hedge_pnl, t.gross,
                              priced ? std::to_string(e).c_str() : "unpriced", priced ? std::to_string(n).c_str() : "unpriced",
                              t.exit_reason);
                tf << line;
            }
            const std::size_t nt = r->trades.size();
            double mean = 0, sd = 0;
            const auto& series = nets.size() == nt && nt > 0 ? nets : std::vector<double>{};
            std::vector<double> gross_series;
            for (const auto& t : r->trades) gross_series.push_back(t.gross);
            const auto& s = series.empty() ? gross_series : series;
            for (const double x : s) mean += x;
            mean = s.empty() ? 0 : mean / static_cast<double>(s.size());
            for (const double x : s) sd += (x - mean) * (x - mean);
            sd = s.size() > 1 ? std::sqrt(sd / static_cast<double>(s.size() - 1)) : 0;
            const double years = nt > 0 ? static_cast<double>(m.day[r->trades.back().exit] - m.day[r->trades.front().entry]) / 365.25 : 0;
            const double per_year = years > 0 ? static_cast<double>(nt) / years : 0;
            const double t_stat = sd > 0 ? mean / (sd / std::sqrt(static_cast<double>(s.size()))) : 0;
            const double sharpe = sd > 0 ? mean / sd * std::sqrt(per_year) : 0;
            char row[768];
            std::snprintf(row, sizeof row, "%s,%s,%s,%s,%zu,%zu,%.0f,%s,%s,%.0f,%.0f,%.2f,%.2f,%.0f,%.0f,%.3f,%.1f,%s\n", name.c_str(),
                          v.id.c_str(), nt ? day_text(m.day[r->trades.front().entry]).c_str() : "", nt ? day_text(m.day[r->trades.back().exit]).c_str() : "",
                          nt, won, gross, unpriced ? "unpriced" : std::to_string(exp_sum).c_str(), unpriced ? "unpriced" : std::to_string(net).c_str(),
                          mean, sd, t_stat, sharpe, dd, worst, nt ? fly / static_cast<double>(nt) * 100.0 : 0.0,
                          nt ? volga / static_cast<double>(nt) : 0.0, unpriced ? "gross only" : cost_note.c_str());
            summary << row;
            std::printf("  %-9s %-17s %4zu trades, %3zu won; gross Rs %12.0f; %s %s; per trade Rs %9.0f (t %.2f); Sharpe %.2f; max DD Rs %.0f\n",
                        name.c_str(), v.id.c_str(), nt, won, gross, unpriced ? "gross only" : "net Rs",
                        unpriced ? "" : std::to_string(static_cast<long long>(net)).c_str(), mean, t_stat, sharpe, dd);
        }
    }
    std::printf("altair_vol_of_vol: wrote %s\n", dir.string().c_str());
    return 0;
}
