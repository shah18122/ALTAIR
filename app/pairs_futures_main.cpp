// app/pairs_futures_main.cpp -- altair_pairs_futures.
//
// Every pair in config/pairs.csv, walked forward by strategies/pairs_futures.hpp:
// a formation year, a quarter traded on it, the relationship re-estimated and
// re-tested before each quarter. Hedged with futures in whole lots (lot sizes
// from data/instruments.csv), costed through risk/cost.hpp -- entry, exit and
// every roll across a monthly expiry -- and REFUSED while config/charges.toml
// is unverified, unless --unverified-costs says otherwise.
//
// Also the ratio each pair is usually watched by (leg_b / leg_a), with the
// pair's ratio_cap hypothesis counted against the data.
//
// Offline and read-only. Writes <out>/pairs_futures/{trades,windows,summary,ratio}.csv.

#include <app/forecast_tracks.hpp>
#include <risk/charges_toml.hpp>
#include <risk/cost.hpp>
#include <strategies/pairs_futures.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace ft = altair::forecast_tracks;
namespace da = altair::data_audit;

void usage(const char* exe) {
    std::printf(
        "  Walk every pair in config/pairs.csv forward, hedged with futures in whole lots.\n\n"
        "    %s [--pairs FILE] [--out DIR] [--formation N] [--trading N] [--entry Z]\n"
        "       [--exit Z] [--stop Z] [--max-half-life D] [--unverified-costs]\n\n"
        "    --pairs FILE          default config/pairs.csv\n"
        "    --out DIR             default data/verified (writes pairs_futures/)\n"
        "    --formation N         trading days the relationship is estimated on (default 250)\n"
        "    --trading N           days it is traded before re-estimating (default 60)\n"
        "    --entry/--exit/--stop z-score thresholds (default 2.0 / 0.5 / 4.0)\n"
        "    --max-half-life D     a spread slower than D days to half-revert is not traded (default 30)\n"
        "    --unverified-costs    price expenses from an UNVERIFIED config/charges.toml anyway,\n"
        "                          stamping every expense and net figure UNVERIFIED\n", exe);
}

bool parse_double(std::string_view s, double& out) {
    const std::string v{s};
    char* end = nullptr;
    out = std::strtod(v.c_str(), &end);
    return end != v.c_str() && *end == '\0' && std::isfinite(out);
}

std::string fixed(double v, int d) {
    char b[64];
    std::snprintf(b, sizeof b, "%.*f", d, v);
    return b;
}

std::string iso_day(std::int64_t day) { return da::format_audit_time(day * 86'400, true).substr(0, 10); }

std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> f;
    std::string cur;
    bool quoted = false;
    for (const char c : line) {
        if (c == '"') { quoted = !quoted; continue; }
        if (c == ',' && !quoted) { f.push_back(cur); cur.clear(); continue; }
        cur += c;
    }
    f.push_back(cur);
    return f;
}

/// Today's futures lot size for `name` from the instrument master.
std::optional<double> futures_lot(const fs::path& master, const std::string& name) {
    std::ifstream in(master);
    std::string line;
    std::getline(in, line);
    std::string best_expiry;
    double lot = 0.0;
    while (std::getline(in, line)) {
        const auto f = split(line);
        if (f.size() < 12 || f[3] != name || f[9] != "FUT" || f[10] != "NFO-FUT") { continue; }
        double v = 0.0;
        if (!parse_double(f[8], v) || !(v > 0.0)) { continue; }
        if (best_expiry.empty() || f[5] < best_expiry) { best_expiry = f[5]; lot = v; }
    }
    return lot > 0.0 ? std::optional<double>{lot} : std::nullopt;
}

struct Spec { std::string pair, a, a_dir, b, b_dir; double cap = 0.0; };

std::vector<Spec> read_pairs(const fs::path& file) {
    std::vector<Spec> out;
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') { continue; }
        const auto f = split(line);
        if (f.size() < 8) { continue; }
        Spec s{f[0], f[1], f[3], f[4], f[6], 0.0};
        double cap = 0.0;
        if (parse_double(f[7], cap)) { s.cap = cap; }
        out.push_back(s);
    }
    return out;
}

struct Costs { bool priced = false; double total = 0.0; };

/// One futures order through risk/cost.hpp. Brokerage is Rs 20 or 0.03 %,
/// whichever is lower -- commercial, not in charges.toml; the literal
/// desktop/cost_panel.hpp states for futures.
bool add_order(Costs& c, const std::vector<altair::ChargeSchedule>& schedules, altair::Side side, double qty,
               double price, std::int64_t day) {
    altair::BrokerageRule br{};
    br.flat_per_order = altair::Notional{2'000};
    br.pct = altair::rate_from(0.0003L);
    br.take_lower = true;
    altair::Trade t{};
    t.segment = altair::Segment::Fut;
    t.exchange = altair::Exchange::NSE;
    t.side = side;
    t.qty = altair::Qty{static_cast<std::int64_t>(std::llround(qty))};
    t.price = altair::Price{static_cast<std::int64_t>(std::llround(price * 100.0))};
    t.trade_ts = altair::Timestamp{(day * 86'400 + ft::kCloseSec) * 1'000'000'000LL};
    const auto* s = altair::schedule_for(schedules.data(), schedules.size(), t.trade_ts);
    if (s == nullptr) { return false; }
    const auto b = altair::compute_cost(t, *s, br);
    if (!b) { return false; }
    c.total += static_cast<double>(b->total.raw()) / 100.0;
    return true;
}

} // namespace

int main(int argc, char** argv) {
    fs::path pairs_file = "config/pairs.csv", out = "data/verified";
    altair::PairsPolicy pol;
    bool unverified = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a{argv[i]};
        const bool has = i + 1 < argc;
        double v = 0.0;
        if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
        if (a == "--pairs" && has) { pairs_file = argv[++i]; continue; }
        if (a == "--out" && has) { out = argv[++i]; continue; }
        if (a == "--unverified-costs") { unverified = true; continue; }
        if (has && parse_double(argv[i + 1], v)) {
            if (a == "--formation" && v >= 30 && v <= 2048) { pol.formation = static_cast<std::size_t>(v); ++i; continue; }
            if (a == "--trading" && v >= 1) { pol.trading = static_cast<std::size_t>(v); ++i; continue; }
            if (a == "--entry") { pol.entry_z = v; ++i; continue; }
            if (a == "--exit") { pol.exit_z = v; ++i; continue; }
            if (a == "--stop") { pol.stop_z = v; ++i; continue; }
            if (a == "--max-half-life") { pol.max_half_life = v; ++i; continue; }
        }
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

    ft::TrackInfo ninfo;
    auto nifty = ft::load_bars("dataset/spot/nifty/1d", da::kDailyTf, ninfo);
    ft::clean_bars(nifty, ninfo);
    const auto rolls = ft::nifty_expiries(nifty);   // monthly F&O expiries; stock futures share them

    const auto specs = read_pairs(pairs_file);
    if (specs.empty()) { std::printf("  no pairs in %s\n", pairs_file.string().c_str()); return 1; }
    const fs::path dir = out / "pairs_futures";
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::ofstream trades(dir / "trades.csv", std::ios::trunc), windows(dir / "windows.csv", std::ios::trunc),
        summary(dir / "summary.csv", std::ios::trunc), ratio(dir / "ratio.csv", std::ios::trunc);
    trades << "pair,window,entry_date,exit_date,days_held,position,z_entry,z_exit,lots_a,qty_a,a_entry,a_exit,lots_b,qty_b,"
              "b_entry,b_exit,hedge_beta,held_ratio,rolls,exit_reason,gross_pnl,expenses,net_pnl,costs\n";
    windows << "pair,formation_from,trade_from,trade_to,traded,reason,beta,adf_t,critical_5pct,half_life_days,correlation\n";
    summary << "pair,status,days,first,last,windows,windows_traded,trades,wins,win_pct,gross_pnl,expenses,net_pnl,"
               "net_per_trade,avg_days_held,max_drawdown,lot_a,lot_b,costs\n";
    ratio << "pair,ratio,days,min,min_date,max,max_date,last,last_date,p05,p50,p95,cap,days_above_cap,first_above,last_above\n";

    std::printf("Pairs with futures (formation %zu, trading %zu, entry %.2f, exit %.2f, stop %.2f)\n  %s\n",
                pol.formation, pol.trading, pol.entry_z, pol.exit_z, pol.stop_z, cost_note.c_str());
    for (const Spec& s : specs) {
        ft::TrackInfo ia, ib;
        auto a = ft::load_bars(s.a_dir, da::kDailyTf, ia);
        auto b = ft::load_bars(s.b_dir, da::kDailyTf, ib);
        ft::clean_bars(a, ia);
        ft::clean_bars(b, ib);
        const auto lot_a = futures_lot("data/instruments.csv", s.a);
        const auto lot_b = futures_lot("data/instruments.csv", s.b);
        if (a.empty() || b.empty()) {
            const std::string why = "no data: run ops\\fetch_pairs.ps1 -Go (" + (a.empty() ? s.a_dir : s.b_dir) + ")";
            summary << s.pair << ",\"" << why << "\",,,,,,,,,,,,,,,,," << cost_tag << '\n';
            std::printf("  %-22s %s\n", s.pair.c_str(), why.c_str());
            continue;
        }
        if (!lot_a || !lot_b) {
            summary << s.pair << ",\"no futures lot size in data/instruments.csv\",,,,,,,,,,,,,,,,," << cost_tag << '\n';
            std::printf("  %-22s no futures lot size in data/instruments.csv\n", s.pair.c_str());
            continue;
        }
        std::map<std::int64_t, double> bc;
        for (const auto& x : b) { bc[ft::bar_day(x)] = x.c; }
        std::vector<std::int64_t> day;
        std::vector<double> pa, pb;
        for (const auto& x : a) {
            const auto it = bc.find(ft::bar_day(x));
            if (it == bc.end()) { continue; }
            day.push_back(ft::bar_day(x));
            pa.push_back(x.c);
            pb.push_back(it->second);
        }

        // The ratio, and the hypothesis written next to the pair.
        {
            std::vector<std::pair<double, std::size_t>> r;
            for (std::size_t i = 0; i < day.size(); ++i) { r.push_back({pb[i] / pa[i], i}); }
            if (!r.empty()) {
                auto sorted = r;
                std::sort(sorted.begin(), sorted.end());
                const auto q = [&sorted](double p) { return sorted[static_cast<std::size_t>(p * static_cast<double>(sorted.size() - 1))].first; };
                std::size_t above = 0;
                std::string first_above, last_above;
                for (const auto& [v, i] : r) {
                    if (s.cap > 0.0 && v > s.cap) {
                        ++above;
                        if (first_above.empty()) { first_above = iso_day(day[i]); }
                        last_above = iso_day(day[i]);
                    }
                }
                ratio << s.pair << ',' << s.b << '/' << s.a << ',' << r.size() << ',' << fixed(sorted.front().first, 4) << ','
                      << iso_day(day[sorted.front().second]) << ',' << fixed(sorted.back().first, 4) << ','
                      << iso_day(day[sorted.back().second]) << ',' << fixed(r.back().first, 4) << ',' << iso_day(day.back())
                      << ',' << fixed(q(0.05), 4) << ',' << fixed(q(0.5), 4) << ',' << fixed(q(0.95), 4) << ','
                      << (s.cap > 0.0 ? fixed(s.cap, 3) : std::string{}) << ',' << (s.cap > 0.0 ? std::to_string(above) : std::string{})
                      << ',' << first_above << ',' << last_above << '\n';
                if (s.cap > 0.0) {
                    std::printf("  %-22s hypothesis %s/%s never above %.2f: %s -- max %.3f on %s, %zu days above (%s to %s), last %.3f\n",
                                s.pair.c_str(), s.b.c_str(), s.a.c_str(), s.cap, above == 0 ? "HOLDS" : "FALSE",
                                sorted.back().first, iso_day(day[sorted.back().second]).c_str(), above,
                                first_above.c_str(), last_above.c_str(), r.back().first);
                }
            }
        }

        const auto res = altair::pairs_walk_forward(day, pa, pb, *lot_a, *lot_b, rolls, pol);
        if (!res) {
            summary << s.pair << ",\"refused: " << altair::pairs_error_text(res.error()) << "\",,,,,,,,,,,,,,,,," << cost_tag << '\n';
            continue;
        }
        for (const auto& w : res->windows) {
            windows << s.pair << ',' << iso_day(day[w.start - pol.formation]) << ',' << iso_day(day[w.start]) << ','
                    << iso_day(day[w.end - 1]) << ',' << (w.traded ? "yes" : "no") << ",\"" << w.reason << "\","
                    << fixed(w.beta, 4) << ',' << fixed(w.adf_t, 3) << ',' << fixed(w.critical, 3) << ','
                    << (std::isfinite(w.half_life) ? fixed(w.half_life, 1) : std::string{"inf"}) << ','
                    << fixed(w.correlation, 3) << '\n';
        }
        std::size_t traded = 0, wins = 0;
        double gross = 0.0, expenses = 0.0, net = 0.0, held = 0.0, cum = 0.0, peak = 0.0, dd = 0.0;
        for (const auto& w : res->windows) { traded += w.traded ? 1 : 0; }
        for (const auto& t : res->trades) {
            const double qa = static_cast<double>(t.lots_a) * *lot_a, qb = static_cast<double>(t.lots_b) * *lot_b;
            const auto buy = altair::Side::Buy, sell = altair::Side::Sell;
            const auto a_open = t.side > 0 ? buy : sell, a_close = t.side > 0 ? sell : buy;
            const auto b_open = t.side > 0 ? sell : buy, b_close = t.side > 0 ? buy : sell;
            Costs c;
            bool ok = costs_allowed;
            if (ok) {
                ok = add_order(c, schedules, a_open, qa, t.a_in, day[t.entry]) && add_order(c, schedules, b_open, qb, t.b_in, day[t.entry])
                  && add_order(c, schedules, a_close, qa, t.a_out, day[t.exit]) && add_order(c, schedules, b_close, qb, t.b_out, day[t.exit]);
                for (const std::size_t r : t.rolls) {   // close and reopen both legs at that close
                    ok = ok && add_order(c, schedules, a_close, qa, pa[r], day[r]) && add_order(c, schedules, a_open, qa, pa[r], day[r])
                         && add_order(c, schedules, b_close, qb, pb[r], day[r]) && add_order(c, schedules, b_open, qb, pb[r], day[r]);
                }
            }
            c.priced = ok;
            const double pnl = c.priced ? t.gross - c.total : t.gross;
            wins += pnl > 0.0 ? 1 : 0;
            gross += t.gross;
            expenses += c.priced ? c.total : 0.0;
            net += pnl;
            cum += pnl;
            peak = std::max(peak, cum);
            dd = std::max(dd, peak - cum);
            held += static_cast<double>(day[t.exit] - day[t.entry]);
            trades << s.pair << ',' << t.window << ',' << iso_day(day[t.entry]) << ',' << iso_day(day[t.exit]) << ','
                   << (day[t.exit] - day[t.entry]) << ",\"" << (t.side > 0 ? "long " + s.a + " / short " + s.b : "short " + s.a + " / long " + s.b)
                   << "\"," << fixed(t.z_entry, 2) << ',' << fixed(t.z_exit, 2) << ',' << t.lots_a << ',' << fixed(qa, 0) << ','
                   << fixed(t.a_in, 2) << ',' << fixed(t.a_out, 2) << ',' << t.lots_b << ',' << fixed(qb, 0) << ','
                   << fixed(t.b_in, 2) << ',' << fixed(t.b_out, 2) << ',' << fixed(t.hedge_beta, 4) << ','
                   << fixed(t.held_ratio, 4) << ',' << t.rolls.size() << ',' << t.exit_reason << ',' << fixed(t.gross, 2) << ','
                   << (c.priced ? fixed(c.total, 2) : std::string{}) << ',' << (c.priced ? fixed(pnl, 2) : std::string{}) << ','
                   << (c.priced ? cost_tag : std::string{"refused"}) << '\n';
        }
        const std::size_t n = res->trades.size();
        summary << s.pair << ",ok," << day.size() << ',' << iso_day(day.front()) << ',' << iso_day(day.back()) << ','
                << res->windows.size() << ',' << traded << ',' << n << ',' << wins << ','
                << (n > 0 ? fixed(100.0 * static_cast<double>(wins) / static_cast<double>(n), 1) : std::string{}) << ','
                << fixed(gross, 2) << ',' << (costs_allowed ? fixed(expenses, 2) : std::string{}) << ','
                << (costs_allowed ? fixed(net, 2) : std::string{}) << ','
                << (costs_allowed && n > 0 ? fixed(net / static_cast<double>(n), 2) : std::string{}) << ','
                << (n > 0 ? fixed(held / static_cast<double>(n), 1) : std::string{}) << ',' << fixed(dd, 2) << ','
                << fixed(*lot_a, 0) << ',' << fixed(*lot_b, 0) << ',' << cost_tag << '\n';
        std::printf("  %-22s %4zu days, %2zu of %2zu windows cointegrated, %3zu trades, win %5.1f%%, gross Rs %10.0f, expenses %s, net %s\n",
                    s.pair.c_str(), day.size(), traded, res->windows.size(), n,
                    n > 0 ? 100.0 * static_cast<double>(wins) / static_cast<double>(n) : 0.0, gross,
                    costs_allowed ? fixed(expenses, 0).c_str() : "refused", costs_allowed ? fixed(net, 0).c_str() : "-");
    }
    std::printf("  wrote %s\n", dir.string().c_str());
    return 0;
}
