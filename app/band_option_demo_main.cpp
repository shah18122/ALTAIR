// app/band_option_demo_main.cpp -- altair_band_option_demo.
//
// A demo of the one forecast that tested usable: the 80 % range band. For
// NIFTY and BANKNIFTY, every band model (models/band_curriculum.hpp) forecasts
// at 09:20 the band the session should close in; when price touches an edge,
// strategies/band_option_fade.hpp sells one lot of the option beyond it and
// buys it back at 15:20. Every trade records the model that placed it, the
// premiums, the expenses item by item and the net P&L.
//
// READ THE ASSUMPTIONS BEFORE THE NUMBERS. They are in meta.csv and on the
// desktop page, and every one is stated rather than buried:
//   * premiums are SYNTHETIC: Black-76 at INDIA VIX, flat across strikes --
//     the dataset has no option-chain history;
//   * one lot is TODAY's contract size (config/lot_size_history.csv), applied
//     to every historical day, because earlier sizes are not transcribed;
//   * strikes step by TODAY's step (data/instruments.csv);
//   * the option is the current MONTHLY contract (the expiry calendar in
//     app/forecast_tracks.hpp), rolled to the next one on expiry day;
//   * expenses come from config/charges.toml through risk/cost.hpp, and are
//     REFUSED while that schedule is unverified -- unless --unverified-costs
//     is passed, which prices them anyway and stamps every figure UNVERIFIED.
//
// Offline and read-only. Writes <out>/band_option_demo/{trades,summary,meta}.csv.

#include <app/forecast_tracks.hpp>
#include <models/band_curriculum.hpp>
#include <risk/charges_toml.hpp>
#include <risk/cost.hpp>
#include <strategies/band_option_fade.hpp>

#include <algorithm>
#include <charconv>
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
using altair::FadeSide;
using altair::FadeTrade;

constexpr std::int64_t kDecideMinute = 560;   // 09:20, the close of the 09:15 bar
constexpr std::size_t kExitBar = 72;          // the 15:15 bar, which closes at 15:20
constexpr std::size_t kSessionBars = 75;

void usage(const char* exe) {
    std::printf(
        "  Sell the option a range band says will not pay: one lot, bought back at 15:20.\n\n"
        "    %s [--dataset DIR] [--out DIR] [--instrument NIFTY|BANKNIFTY|both]\n"
        "       [--rate R] [--slippage-pts P] [--unverified-costs]\n\n"
        "    --dataset DIR         default dataset\n"
        "    --out DIR             default data/verified (writes band_option_demo/)\n"
        "    --instrument X        default both\n"
        "    --rate R              continuously compounded rate for the forward (default 0.065)\n"
        "    --slippage-pts P      option premium points given up per fill (default 0.5)\n"
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
std::string hhmm(std::int64_t t) { return da::format_audit_time(t, false).substr(11, 5); }

/// Today's verified lot size from config/lot_size_history.csv: the OPEN row
/// whose `verified` column carries a verification date ("NO" otherwise).
/// History is not transcribed there, and is not guessed here.
std::optional<double> lot_size_today(const fs::path& file, const std::string& symbol) {
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') { continue; }
        std::vector<std::string> f;
        std::stringstream ss(line);
        for (std::string c; std::getline(ss, c, ',');) { f.push_back(c); }
        if (f.size() >= 7 && f[0] == symbol && f[2] == "OPEN" && f[6].size() == 10 && f[6][4] == '-') {
            double v = 0.0;
            if (parse_double(f[3], v) && v > 0.0) { return v; }
        }
    }
    return std::nullopt;
}

/// Today's strike step from the instrument master: the commonest gap between
/// adjacent strikes of the nearest expiry's options on `name`.
std::optional<double> strike_step_today(const fs::path& master, const std::string& name) {
    std::ifstream in(master);
    std::string line;
    std::map<std::string, std::set<double>> by_expiry;
    std::getline(in, line);   // header
    while (std::getline(in, line)) {
        // instrument_token,exchange_token,tradingsymbol,name,last_price,expiry,strike,tick_size,lot_size,instrument_type,segment,exchange
        std::vector<std::string> f;
        std::string cur;
        bool quoted = false;
        for (const char c : line) {
            if (c == '"') { quoted = !quoted; continue; }
            if (c == ',' && !quoted) { f.push_back(cur); cur.clear(); continue; }
            cur += c;
        }
        f.push_back(cur);
        if (f.size() < 12 || f[3] != name || f[10] != "NFO-OPT") { continue; }
        double k = 0.0;
        if (parse_double(f[6], k) && k > 0.0) { by_expiry[f[5]].insert(k); }
    }
    if (by_expiry.empty()) { return std::nullopt; }
    const auto& strikes = by_expiry.begin()->second;   // ISO dates sort: the nearest first
    std::map<double, int> gaps;
    double prev = -1.0;
    for (const double k : strikes) {
        if (prev > 0.0) { ++gaps[std::round((k - prev) * 100.0) / 100.0]; }
        prev = k;
    }
    double best = 0.0;
    int count = 0;
    for (const auto& [g, c] : gaps) { if (c > count) { best = g; count = c; } }
    return best > 0.0 ? std::optional<double>{best} : std::nullopt;
}

struct Costs {
    bool priced = false;
    double brokerage = 0, stt = 0, exchange = 0, sebi = 0, stamp = 0, ipft = 0, gst = 0, total = 0;
};

/// Both fills of one trade through risk/cost.hpp: the sale at entry, the
/// purchase at exit, each under the schedule live on its date.
Costs price_costs(const FadeTrade& t, const std::vector<altair::ChargeSchedule>& schedules, bool allowed) {
    Costs c;
    if (!allowed) { return c; }
    // OPTIONS BROKERAGE IS A FLAT Rs 20 AN ORDER -- commercial, not regulatory,
    // so not in charges.toml; the same literal desktop/cost_panel.hpp states.
    altair::BrokerageRule br{};
    br.flat_per_order = altair::Notional{2'000};
    br.pct = 0;
    br.take_lower = false;
    for (int leg = 0; leg < 2; ++leg) {
        altair::Trade tr{};
        tr.segment = altair::Segment::Opt;
        tr.exchange = altair::Exchange::NSE;
        tr.side = leg == 0 ? altair::Side::Sell : altair::Side::Buy;
        tr.qty = altair::Qty{static_cast<std::int64_t>(std::llround(t.qty))};
        const double prem = leg == 0 ? t.entry_premium : t.exit_premium;
        tr.price = altair::Price{static_cast<std::int64_t>(std::llround(prem * 100.0))};
        const std::int64_t ist = leg == 0 ? t.entry_t : t.exit_t;
        tr.trade_ts = altair::Timestamp{ist * 1'000'000'000LL};
        const auto* s = altair::schedule_for(schedules.data(), schedules.size(), tr.trade_ts);
        if (s == nullptr) { return Costs{}; }
        const auto b = altair::compute_cost(tr, *s, br);
        if (!b) { return Costs{}; }
        const auto rs = [](altair::Notional n) { return static_cast<double>(n.raw()) / 100.0; };
        c.brokerage += rs(b->brokerage);
        c.stt += rs(b->stt);
        c.exchange += rs(b->exchange_txn);
        c.sebi += rs(b->sebi);
        c.stamp += rs(b->stamp);
        c.ipft += rs(b->ipft);
        c.gst += rs(b->gst);
        c.total += rs(b->total);
    }
    c.priced = true;
    return c;
}

struct Row {
    std::string instrument, model;
    std::int64_t day = 0;
    FadeTrade t;
    Costs c;
};

} // namespace

int main(int argc, char** argv) {
    fs::path root = "dataset", out = "data/verified";
    std::string which = "both";
    double rate = 0.065, slip = 0.5;
    bool unverified = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a{argv[i]};
        const bool has = i + 1 < argc;
        if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
        if (a == "--dataset" && has) { root = argv[++i]; continue; }
        if (a == "--out" && has) { out = argv[++i]; continue; }
        if (a == "--instrument" && has) { which = argv[++i]; continue; }
        if (a == "--rate" && has) { if (!parse_double(argv[++i], rate) || std::fabs(rate) > 1.0) { usage(argv[0]); return 2; } continue; }
        if (a == "--slippage-pts" && has) { if (!parse_double(argv[++i], slip) || slip < 0.0) { usage(argv[0]); return 2; } continue; }
        if (a == "--unverified-costs") { unverified = true; continue; }
        usage(argv[0]);
        return 2;
    }

    // Charges: refused while unverified, unless the caller says otherwise.
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

    struct Inst { std::string name, dir, other_dir; };
    std::vector<Inst> insts;
    if (which == "both" || which == "NIFTY") { insts.push_back({"NIFTY", "spot/nifty", "spot/banknifty"}); }
    if (which == "both" || which == "BANKNIFTY") { insts.push_back({"BANKNIFTY", "spot/banknifty", "spot/nifty"}); }
    if (insts.empty()) { usage(argv[0]); return 2; }

    const auto load = [&](const std::string& dir, int tf) {
        ft::TrackInfo info;
        auto v = ft::load_bars(root / dir / (tf == da::kDailyTf ? std::string{"1d"} : std::to_string(tf) + "m"), tf, info);
        ft::clean_bars(v, info);
        return v;
    };
    const auto vix5 = load("spot/indiavix", 5);
    const auto nifty_d = load("spot/nifty", da::kDailyTf);
    const auto bank_d = load("spot/banknifty", da::kDailyTf);
    if (vix5.empty() || nifty_d.empty()) { std::printf("  no data under %s\n", root.string().c_str()); return 1; }
    std::map<std::int64_t, double> vix_at;
    for (const auto& b : vix5) { vix_at[b.t] = b.c; }
    const auto expiries = ft::nifty_expiries(nifty_d);

    // BANKNIFTY has no VIX of its own: scale INDIA VIX by the ratio of the two
    // indices' 20-day realised volatility, as of the previous close.
    std::map<std::int64_t, double> bank_scale;
    {
        std::map<std::int64_t, double> nc, bc;
        for (const auto& b : nifty_d) { nc[ft::bar_day(b)] = b.c; }
        for (const auto& b : bank_d) { bc[ft::bar_day(b)] = b.c; }
        std::vector<std::pair<std::int64_t, std::pair<double, double>>> both;
        for (const auto& [d, c] : nc) { if (bc.contains(d)) { both.push_back({d, {c, bc[d]}}); } }
        for (std::size_t k = 21; k < both.size(); ++k) {
            double sn = 0, snn = 0, sb = 0, sbb = 0;
            for (std::size_t j = k - 20; j < k; ++j) {
                const double rn = std::log(both[j].second.first / both[j - 1].second.first);
                const double rb = std::log(both[j].second.second / both[j - 1].second.second);
                sn += rn; snn += rn * rn; sb += rb; sbb += rb * rb;
            }
            const double vn = snn / 20 - (sn / 20) * (sn / 20), vb = sbb / 20 - (sb / 20) * (sb / 20);
            if (vn > 0 && vb > 0) { bank_scale[both[k].first] = std::sqrt(vb / vn); }   // known before day k opens
        }
    }

    std::vector<Row> rows;
    std::ostringstream meta;
    meta << "key,value\n";
    meta << "costs," << (costs_allowed ? (costs_verified ? "verified" : "UNVERIFIED") : "refused") << "\n";
    meta << "cost_note,\"" << cost_note << "\"\n";
    meta << "premiums,\"SYNTHETIC: Black-76 at INDIA VIX (BANKNIFTY: VIX x 20-day realised-vol ratio), flat across strikes; the dataset has no option-chain history\"\n";
    meta << "expiry,\"the current monthly contract, rolled to the next on expiry day\"\n";
    meta << "rate," << rate << "\nslippage_pts," << slip << "\n";
    meta << "rule,\"09:20 band per model; first bar high >= upper edge: sell 1 lot CE at the first strike >= edge; first low <= lower edge: sell 1 lot PE at the first strike <= edge; buy back at the 15:20 close\"\n";
    std::size_t refused_days = 0, no_expiry = 0;

    for (const Inst& in : insts) {
        const auto lot = lot_size_today("config/lot_size_history.csv", in.name);
        const auto step = strike_step_today("data/instruments.csv", in.name);
        if (!lot || !step) {
            std::printf("  %s: no verified lot size or strike step (config/lot_size_history.csv, data/instruments.csv) -- not run\n",
                        in.name.c_str());
            continue;
        }
        meta << in.name << "_lot_size," << *lot << "\n" << in.name << "_lot_note,\"TODAY's contract size, applied to every historical day: earlier sizes are not transcribed\"\n";
        meta << in.name << "_strike_step," << *step << "\n";
        const auto own5 = load(in.dir, 5);
        ft::TrackInfo info;
        const auto track = ft::build_session({in.name + " 0920-close", in.name, static_cast<int>(kDecideMinute), &own5, &vix5,
                                              1.3, nullptr, ""}, info);
        auto models = altair::band_default_models();
        const auto br = altair::band_run(track, models);
        if (!br) {
            std::printf("  %s: band run refused: %s\n", in.name.c_str(), altair::curriculum_error_text(br.error()));
            continue;
        }
        std::map<std::int64_t, std::size_t> first_bar;   // day -> index of its 09:15 bar
        for (std::size_t k = 0; k < own5.size(); ++k) {
            if (da::audit_minute_of_day(own5[k].t) == 555) { first_bar[ft::bar_day(own5[k])] = k; }
        }
        altair::FadePolicy pol;
        pol.strike_step = *step;
        pol.lot_size = *lot;
        pol.lots = 1;
        pol.rate = rate;
        pol.slippage_pts = slip;
        for (std::size_t i = br->first_row; i < track.rows(); ++i) {
            const std::int64_t day = da::audit_day(track.t[i]);
            const auto fb = first_bar.find(day);
            if (fb == first_bar.end() || fb->second + kSessionBars > own5.size()) { ++refused_days; continue; }
            const auto ex = expiries.upper_bound(day);   // the expiry after today: rolled on expiry day
            if (ex == expiries.end()) { ++no_expiry; continue; }
            double scale = 1.0;
            if (in.name == "BANKNIFTY") {
                const auto sc = bank_scale.find(day);
                if (sc == bank_scale.end()) { ++refused_days; continue; }
                scale = sc->second;
            }
            altair::FadeDay d;
            d.decide_bar = 0;
            d.exit_bar = kExitBar;
            d.expiry_ts = *ex * 86'400 + ft::kCloseSec;
            double last_vix = 0.0;
            bool ok = true;
            for (std::size_t k = 0; k < kSessionBars; ++k) {
                const auto& b = own5[fb->second + k];
                if (ft::bar_day(b) != day) { ok = false; break; }
                const auto v = vix_at.find(b.t);
                if (v != vix_at.end() && v->second > 0.0) { last_vix = v->second; }
                if (!(last_vix > 0.0)) { ok = false; break; }
                d.bars.push_back({b.t, b.o, b.h, b.l, b.c, last_vix / 100.0 * scale});
            }
            if (!ok) { ++refused_days; continue; }
            for (std::size_t m = 0; m < br->models.size(); ++m) {
                const float h = br->half[m][i - br->first_row];
                if (!(h > 0.0f)) { continue; }
                const auto trades = altair::fade_day(d, track.anchor[i], static_cast<double>(h), m, pol);
                if (!trades) { continue; }
                for (const auto& t : *trades) {
                    rows.push_back({in.name, br->models[m], day, t, price_costs(t, schedules, costs_allowed)});
                }
            }
        }
    }

    // Trades, oldest first.
    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.t.entry_t < b.t.entry_t; });
    const fs::path dir = out / "band_option_demo";
    std::error_code ec;
    fs::create_directories(dir, ec);
    {
        std::ofstream f(dir / "trades.csv", std::ios::trunc);
        f << "date,instrument,model,option,strike,expiry,entry_time,entry_spot,entry_iv_pct,entry_premium,exit_time,exit_spot,"
             "exit_iv_pct,exit_premium,lots,qty,band_edge,band_half_bp,gross_pnl,brokerage,stt,exchange,sebi,stamp,ipft,gst,"
             "expenses,net_pnl,costs\n";
        for (const Row& r : rows) {
            const auto& t = r.t;
            f << iso_day(r.day) << ',' << r.instrument << ",\"" << r.model << "\"," << (t.side == FadeSide::Call ? "CE" : "PE") << ','
              << fixed(t.strike, 0) << ',' << iso_day(da::audit_day(t.expiry_ts)) << ',' << hhmm(t.entry_t) << ','
              << fixed(t.entry_spot, 2) << ',' << fixed(100.0 * t.entry_iv, 2) << ',' << fixed(t.entry_premium, 2) << ','
              << hhmm(t.exit_t) << ',' << fixed(t.exit_spot, 2) << ',' << fixed(100.0 * t.exit_iv, 2) << ','
              << fixed(t.exit_premium, 2) << ",1," << fixed(t.qty, 0) << ',' << fixed(t.band_edge, 2) << ','
              << fixed(1e4 * t.half, 1) << ',' << fixed(t.gross, 2) << ',';
            if (r.c.priced) {
                f << fixed(r.c.brokerage, 2) << ',' << fixed(r.c.stt, 2) << ',' << fixed(r.c.exchange, 2) << ','
                  << fixed(r.c.sebi, 2) << ',' << fixed(r.c.stamp, 2) << ',' << fixed(r.c.ipft, 2) << ','
                  << fixed(r.c.gst, 2) << ',' << fixed(r.c.total, 2) << ',' << fixed(t.gross - r.c.total, 2) << ','
                  << (costs_verified ? "verified" : "UNVERIFIED") << '\n';
            } else {
                f << ",,,,,,,,,refused\n";
            }
        }
    }

    // Per instrument and model.
    struct Sum { std::size_t n = 0, wins = 0, calls = 0, puts = 0; double gross = 0, exp = 0, net = 0, peak = 0, dd = 0, cum = 0; std::string first, last; };
    std::map<std::pair<std::string, std::string>, Sum> sums;
    for (const Row& r : rows) {
        Sum& s = sums[{r.instrument, r.model}];
        const double pnl = r.c.priced ? r.t.gross - r.c.total : r.t.gross;
        ++s.n;
        s.wins += pnl > 0.0 ? 1 : 0;
        (r.t.side == FadeSide::Call ? s.calls : s.puts) += 1;
        s.gross += r.t.gross;
        s.exp += r.c.priced ? r.c.total : 0.0;
        s.net += pnl;
        s.cum += pnl;
        s.peak = std::max(s.peak, s.cum);
        s.dd = std::max(s.dd, s.peak - s.cum);
        if (s.first.empty()) { s.first = iso_day(r.day); }
        s.last = iso_day(r.day);
    }
    {
        std::ofstream f(dir / "summary.csv", std::ios::trunc);
        f << "instrument,model,trades,calls,puts,wins,win_pct,gross_pnl,expenses,net_pnl,net_per_trade,max_drawdown,first,last,costs\n";
        for (const auto& [k, s] : sums) {
            f << k.first << ",\"" << k.second << "\"," << s.n << ',' << s.calls << ',' << s.puts << ',' << s.wins << ','
              << fixed(100.0 * static_cast<double>(s.wins) / static_cast<double>(s.n), 1) << ',' << fixed(s.gross, 2) << ','
              << (costs_allowed ? fixed(s.exp, 2) : std::string{}) << ',' << (costs_allowed ? fixed(s.net, 2) : std::string{}) << ','
              << (costs_allowed ? fixed(s.net / static_cast<double>(s.n), 2) : std::string{}) << ',' << fixed(s.dd, 2) << ','
              << s.first << ',' << s.last << ',' << (costs_allowed ? (costs_verified ? "verified" : "UNVERIFIED") : "refused") << '\n';
        }
    }
    meta << "trades," << rows.size() << "\ndays_skipped_no_bars_or_vix," << refused_days << "\ndays_skipped_no_expiry," << no_expiry << "\n";
    { std::ofstream f(dir / "meta.csv", std::ios::trunc); f << meta.str(); }

    std::printf("Band option fade demo: %zu trades\n  %s\n", rows.size(), cost_note.c_str());
    std::printf("  %-10s %-28s %6s %6s %13s %12s %13s %12s\n", "", "model", "trades", "win %", "gross Rs", "expenses Rs", "net Rs", "max DD Rs");
    for (const auto& [k, s] : sums) {
        std::printf("  %-10s %-28s %6zu %6.1f %13.0f %12s %13s %12.0f\n", k.first.c_str(), k.second.c_str(), s.n,
                    100.0 * static_cast<double>(s.wins) / static_cast<double>(s.n), s.gross,
                    costs_allowed ? fixed(s.exp, 0).c_str() : "refused", costs_allowed ? fixed(s.net, 0).c_str() : "-", s.dd);
    }
    std::printf("  wrote %s\n", dir.string().c_str());
    return 0;
}
