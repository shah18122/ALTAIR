// app/band_option_demo_main.cpp -- altair_band_option_demo.
//
// A demo of the one forecast that tested usable: the 80 % range band. For
// NIFTY and BANKNIFTY, every band model (models/band_curriculum.hpp) forecasts
// at 09:20 the band the session should close in, and each RULE sells options
// on it (strategies/band_option_fade.hpp), one lot, bought back by 15:20:
//   touch               wait for an edge to be touched, sell the option past it
//   strangle            sell both edges at 09:20, before any touch
//   strangle-stop2x     the same, a leg bought back once its premium doubles
//   strangle-hedged     the same, delta hedged with whole futures lots
//   expiry-day          the strangle on monthly expiry days, in that day's contract
//   expiry-day-stop2x   the same with the 2x stop
// Every trade records the model that placed it, the premiums, the expenses
// item by item, the net P&L, and where the gross came from: time decay, the
// underlying's move, the change in IV, and slippage.
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
//   * time to expiry runs on the VARIANCE clock by default: each day is split
//     between the overnight gap and the session in proportion to the trailing
//     250 sessions' variance (strategies/band_option_fade.hpp, FadeClock).
//     Calendar time credits an intraday seller a quarter of a day's decay,
//     trading time a whole day's; the result swings from loss to profit
//     between them, so --clock calendar|trading are there to show it;
//   * expenses come from config/charges.toml through risk/cost.hpp, and are
//     REFUSED while that schedule is unverified -- unless --unverified-costs
//     is passed, which prices them anyway and stamps every figure UNVERIFIED.
//
// Offline and read-only. Writes <out>/band_option_demo/: trades_<rule>.csv,
// summary.csv (every rule, instrument and model) and meta.csv.

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
        "  Sell the options a range band says will not pay: one lot, bought back by 15:20.\n\n"
        "    %s [--dataset DIR] [--out DIR] [--instrument NIFTY|BANKNIFTY|both]\n"
        "       [--rules R1,R2,...] [--clock variance|calendar|trading] [--rate R] [--slippage-pts P]\n"
        "       [--unverified-costs]\n\n"
        "    --dataset DIR         default dataset\n"
        "    --out DIR             default data/verified (writes band_option_demo/)\n"
        "    --instrument X        default both\n"
        "    --clock C             variance (default: each day's time split between the overnight gap and\n"
        "                          the session by the trailing 250 sessions' variance), calendar (INDIA\n"
        "                          VIX's convention) or trading (decay only while the market is open).\n"
        "                          Synthetic intraday P&L turns on this choice: run all three\n"
        "    --rules LIST          touch, strangle, strangle-stop2x, strangle-hedged, expiry-day,\n"
        "                          expiry-day-stop2x (default: all)\n"
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

/// Futures hedge fills through risk/cost.hpp: Rs 20 or 0.03 %, whichever is lower.
Costs price_hedge(const std::vector<altair::HedgeFill>& fills, double lot, const std::vector<altair::ChargeSchedule>& schedules,
                  bool allowed) {
    Costs c;
    if (!allowed) { return c; }
    if (fills.empty()) { c.priced = true; return c; }
    // FUTURES BROKERAGE: Rs 20 or 0.03 %, the lower -- commercial, as above.
    altair::BrokerageRule br{};
    br.flat_per_order = altair::Notional{2'000};
    br.pct = altair::rate_from(0.0003L);
    br.take_lower = true;
    for (const auto& f : fills) {
        altair::Trade tr{};
        tr.segment = altair::Segment::Fut;
        tr.exchange = altair::Exchange::NSE;
        tr.side = f.lots > 0 ? altair::Side::Buy : altair::Side::Sell;
        tr.qty = altair::Qty{static_cast<std::int64_t>(std::llround(std::fabs(static_cast<double>(f.lots)) * lot))};
        tr.price = altair::Price{static_cast<std::int64_t>(std::llround(f.price * 100.0))};
        tr.trade_ts = altair::Timestamp{f.t * 1'000'000'000LL};
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

/// Where a leg's gross came from, priced along the path entry -> exit:
/// time passing at the entry spot and IV, then the spot moving, then the IV
/// changing; slippage is what the fills gave up. The four add to the gross.
struct Decomp { double theta = 0, move = 0, iv = 0, slip = 0; };

Decomp decompose(const FadeTrade& t, double rate, double slip_pts, const altair::FadeClock& ck) {
    namespace fd = altair::fade_detail;
    Decomp d;
    const double p0 = fd::premium(t.side, t.entry_spot, t.strike, t.entry_t, t.expiry_ts, t.entry_iv, rate, ck);
    const double pt = fd::premium(t.side, t.entry_spot, t.strike, t.exit_t, t.expiry_ts, t.entry_iv, rate, ck);
    const double ps = fd::premium(t.side, t.exit_spot, t.strike, t.exit_t, t.expiry_ts, t.entry_iv, rate, ck);
    const double p1 = fd::premium(t.side, t.exit_spot, t.strike, t.exit_t, t.expiry_ts, t.exit_iv, rate, ck);
    if (!std::isfinite(p0) || !std::isfinite(pt) || !std::isfinite(ps) || !std::isfinite(p1)) { return d; }
    d.theta = (p0 - pt) * t.qty;
    d.move = (pt - ps) * t.qty;
    d.iv = (ps - p1) * t.qty;
    d.slip = -2.0 * slip_pts * t.qty;
    return d;
}

struct Rule {
    std::string id, text;
    bool touch = false, expiry_day = false;
    altair::StranglePolicy sp;
};

std::vector<Rule> all_rules() {
    const std::string strangle = "09:20: sell 1 lot CE at the first strike >= the upper edge and 1 lot PE at the first strike <= the lower edge";
    std::vector<Rule> r;
    r.push_back({"touch", "first touch of an edge after 09:20: sell 1 lot of the option at the first strike past it; buy back at 15:20", true, false, {}});
    r.push_back({"strangle", strangle + "; hold to 15:20", false, false, {}});
    r.push_back({"strangle-stop2x", strangle + "; buy a leg back at the first 5-minute close where its premium has doubled", false, false, {2.0, false}});
    r.push_back({"strangle-hedged", strangle + "; futures delta hedge in whole lots, reset at every 5-minute close", false, false, {0.0, true}});
    r.push_back({"expiry-day", "monthly expiry days only, in the contract expiring that day (0DTE): " + strangle + "; hold to 15:20", false, true, {}});
    r.push_back({"expiry-day-stop2x", "monthly expiry days only (0DTE): " + strangle + "; stop at double the premium", false, true, {2.0, false}});
    return r;
}

/// One instrument's sessions, prepared once and traded under every rule.
struct Session {
    std::int64_t day = 0;
    std::size_t row = 0;        ///< band_run row
    altair::FadeDay d;          ///< expiry: the next monthly after today (rolled on expiry day)
    bool expiry_today = false;
    double sessions_to_expiry = 0.0;   ///< full sessions after today through that expiry
    double intraday_share = 1.0;       ///< trailing share of daily variance inside the session
};
struct Prepared {
    std::string name;
    double lot = 0, step = 0;
    std::vector<Session> sessions;
    std::vector<std::string> models;
    std::vector<std::vector<float>> half;   ///< [model][row - first_row]
    std::vector<double> anchor;             ///< [row]
    std::size_t first_row = 0;
};

struct Leg { FadeTrade t; Costs c; Decomp dc; };
struct Row {
    std::size_t inst = 0, model = 0;
    std::int64_t day = 0, entry_t = 0;
    std::vector<Leg> legs;
    double band_low = 0, band_high = 0;
    std::vector<altair::HedgeFill> fills;
    double hedge = 0;
    Costs hc;
    double gross = 0, expenses = 0, net = 0;
    bool priced = false;
};

std::string leg_exit(const FadeTrade& t) {
    return std::string{t.exit_reason} == "stop" ? "stop " + hhmm(t.exit_t) : std::string{"15:20"};
}

} // namespace

int main(int argc, char** argv) {
    fs::path root = "dataset", out = "data/verified";
    std::string which = "both", rules_arg;
    double rate = 0.065, slip = 0.5;
    bool unverified = false, trading_clock = true, variance_clock = true;   // default: the variance clock
    for (int i = 1; i < argc; ++i) {
        const std::string_view a{argv[i]};
        const bool has = i + 1 < argc;
        if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
        if (a == "--clock" && has) {
            const std::string_view c{argv[++i]};
            if (c != "calendar" && c != "trading" && c != "variance") { usage(argv[0]); return 2; }
            trading_clock = c != "calendar";
            variance_clock = c == "variance";
            continue;
        }
        if (a == "--dataset" && has) { root = argv[++i]; continue; }
        if (a == "--out" && has) { out = argv[++i]; continue; }
        if (a == "--instrument" && has) { which = argv[++i]; continue; }
        if (a == "--rules" && has) { rules_arg = argv[++i]; continue; }
        if (a == "--rate" && has) { if (!parse_double(argv[++i], rate) || std::fabs(rate) > 1.0) { usage(argv[0]); return 2; } continue; }
        if (a == "--slippage-pts" && has) { if (!parse_double(argv[++i], slip) || slip < 0.0) { usage(argv[0]); return 2; } continue; }
        if (a == "--unverified-costs") { unverified = true; continue; }
        usage(argv[0]);
        return 2;
    }
    std::vector<Rule> rules;
    for (const Rule& r : all_rules()) {
        if (rules_arg.empty() || ("," + rules_arg + ",").find("," + r.id + ",") != std::string::npos) { rules.push_back(r); }
    }
    if (rules.empty()) { usage(argv[0]); return 2; }

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
    const std::string cost_tag = costs_allowed ? (costs_verified ? "verified" : "UNVERIFIED") : "refused";

    struct Inst { std::string name, dir; };
    std::vector<Inst> insts;
    if (which == "both" || which == "NIFTY") { insts.push_back({"NIFTY", "spot/nifty"}); }
    if (which == "both" || which == "BANKNIFTY") { insts.push_back({"BANKNIFTY", "spot/banknifty"}); }
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
    // Trading days in (from, to]: the dataset's sessions, then weekdays past its end.
    std::vector<std::int64_t> sessions;
    for (const auto& b : nifty_d) { sessions.push_back(ft::bar_day(b)); }
    const auto trading_days_between = [&](std::int64_t from, std::int64_t to) {
        const auto lo = std::upper_bound(sessions.begin(), sessions.end(), from);
        const auto hi = std::upper_bound(sessions.begin(), sessions.end(), to);
        std::int64_t n = hi - lo;
        for (std::int64_t d = std::max(from, sessions.back()) + 1; d <= to; ++d) {
            if (da::audit_weekday(d) < 5) { ++n; }
        }
        return n;
    };

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

    std::ostringstream meta;
    meta << "key,value\n";
    meta << "costs," << cost_tag << "\n";
    meta << "cost_note,\"" << cost_note << "\"\n";
    meta << "premiums,\"SYNTHETIC: Black-76 at INDIA VIX (BANKNIFTY: VIX x 20-day realised-vol ratio), flat across strikes; the dataset has no option-chain history\"\n";
    meta << "expiry,\"the current monthly contract, rolled to the next on expiry day (expiry-day rules: the contract expiring that day)\"\n";
    meta << "rate," << rate << "\nslippage_pts," << slip << "\n";
    meta << "clock,\"" << (variance_clock ? "VARIANCE: days / 252, each split between the overnight gap and the session by the trailing 250 sessions' variance"
                            : trading_clock ? "TRADING: time to expiry in sessions / 252; decay only while the market is open"
                                            : "CALENDAR: time to expiry / 365 days, INDIA VIX's convention") << "\"\n";
    for (const Rule& r : rules) { meta << "rule_" << r.id << ",\"" << r.text << "\"\n"; }
    meta << "rule,\"" << rules.front().text << "\"\n";
    std::size_t refused_days = 0, no_expiry = 0;

    // Prepare every instrument once: band forecasts and the sessions they trade.
    std::vector<Prepared> prep;
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
        Prepared p;
        p.name = in.name;
        p.lot = *lot;
        p.step = *step;
        p.models = br->models;
        p.half = br->half;
        p.first_row = br->first_row;
        p.anchor.assign(track.anchor.begin(), track.anchor.end());
        std::map<std::int64_t, std::size_t> first_bar;   // day -> index of its 09:15 bar
        for (std::size_t k = 0; k < own5.size(); ++k) {
            if (da::audit_minute_of_day(own5[k].t) == 555) { first_bar[ft::bar_day(own5[k])] = k; }
        }
        // Each day's share of variance inside the session, over the 250 days
        // BEFORE it: overnight gap = first open vs the previous last close.
        std::map<std::int64_t, double> share;
        {
            std::vector<std::pair<std::int64_t, std::pair<double, double>>> sq;   // day, (gap^2, session^2)
            double prev_close = 0.0;
            std::int64_t prev_day = -1;
            std::size_t k = 0;
            while (k < own5.size()) {
                const std::int64_t day = ft::bar_day(own5[k]);
                std::size_t e = k;
                while (e < own5.size() && ft::bar_day(own5[e]) == day) { ++e; }
                if (e - k >= 60 && prev_day >= 0 && prev_close > 0.0) {
                    const double g = std::log(own5[k].o / prev_close), sess = std::log(own5[e - 1].c / own5[k].o);
                    sq.push_back({day, {g * g, sess * sess}});
                }
                prev_close = own5[e - 1].c;
                prev_day = day;
                k = e;
            }
            double sg = 0.0, si = 0.0;
            constexpr std::size_t kWin = 250;
            for (std::size_t j = 0; j < sq.size(); ++j) {
                if (j >= 60 && sg + si > 0.0) { share[sq[j].first] = si / (sg + si); }   // known before day j opens
                sg += sq[j].second.first;
                si += sq[j].second.second;
                if (j >= kWin) {
                    sg -= sq[j - kWin].second.first;
                    si -= sq[j - kWin].second.second;
                }
            }
        }
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
            Session s;
            s.day = day;
            if (variance_clock) {
                const auto sh = share.find(day);
                if (sh == share.end()) { ++refused_days; continue; }
                s.intraday_share = sh->second;
            }
            s.row = i;
            s.expiry_today = expiries.contains(day);
            s.d.decide_bar = 0;
            s.d.exit_bar = kExitBar;
            s.d.expiry_ts = *ex * 86'400 + ft::kCloseSec;
            s.sessions_to_expiry = static_cast<double>(trading_days_between(day, *ex));
            double last_vix = 0.0;
            bool ok = true;
            for (std::size_t k = 0; k < kSessionBars; ++k) {
                const auto& b = own5[fb->second + k];
                if (ft::bar_day(b) != day) { ok = false; break; }
                const auto v = vix_at.find(b.t);
                if (v != vix_at.end() && v->second > 0.0) { last_vix = v->second; }
                if (!(last_vix > 0.0)) { ok = false; break; }
                s.d.bars.push_back({b.t, b.o, b.h, b.l, b.c, last_vix / 100.0 * scale});
            }
            if (!ok) { ++refused_days; continue; }
            p.sessions.push_back(std::move(s));
        }
        prep.push_back(std::move(p));
    }

    const fs::path dir = out / "band_option_demo";
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::remove(dir / "trades.csv", ec);   // the one-rule layout this replaced

    struct Sum {
        std::size_t n = 0, wins = 0;
        double gross = 0, exp = 0, net = 0, theta = 0, move = 0, iv = 0, hedge = 0, worst = 0, peak = 0, dd = 0, cum = 0;
        std::string first, last;
    };
    std::ofstream summary(dir / "summary.csv", std::ios::trunc);
    summary << "rule,instrument,model,trades,wins,win_pct,gross_pnl,expenses,net_pnl,net_per_trade,theta_per_trade,"
               "move_per_trade,iv_per_trade,hedge_per_trade,worst_trade,max_drawdown,first,last,costs\n";
    std::size_t total_rows = 0;
    std::printf("Band option demo: %zu rule(s)\n  %s\n", rules.size(), cost_note.c_str());
    std::printf("  %-18s %-10s %7s %6s %10s %10s %10s %10s %10s\n", "rule", "", "trades", "win %", "theta/tr", "move/tr",
                "gross/tr", "exp/tr", "net/tr");

    for (const Rule& rule : rules) {
        std::vector<Row> rows;
        altair::FadePolicy pol;
        pol.lots = 1;
        pol.rate = rate;
        pol.slippage_pts = slip;
        for (std::size_t pi = 0; pi < prep.size(); ++pi) {
            const Prepared& P = prep[pi];
            pol.strike_step = P.step;
            pol.lot_size = P.lot;
            for (const Session& s : P.sessions) {
                if (rule.expiry_day && !s.expiry_today) { continue; }
                altair::FadeDay d = s.d;
                d.clock.trading = trading_clock;
                d.clock.session_close = s.day * 86'400 + ft::kCloseSec;
                d.clock.sessions_after = s.sessions_to_expiry;
                d.clock.intraday_share = variance_clock ? s.intraday_share : 1.0;
                if (rule.expiry_day) {   // 0DTE: today's contract
                    d.expiry_ts = s.day * 86'400 + ft::kCloseSec;
                    d.clock.sessions_after = 0.0;
                }
                const double anchor = P.anchor[s.row];
                for (std::size_t m = 0; m < P.models.size(); ++m) {
                    const float h = P.half[m][s.row - P.first_row];
                    if (!(h > 0.0f)) { continue; }
                    const double half = static_cast<double>(h);
                    const auto add_leg = [&](Row& r, const FadeTrade& t) {
                        r.legs.push_back({t, price_costs(t, schedules, costs_allowed), decompose(t, rate, slip, d.clock)});
                    };
                    if (rule.touch) {
                        const auto trades = altair::fade_day(d, anchor, half, m, pol);
                        if (!trades) { continue; }
                        for (const auto& t : *trades) {
                            Row r;
                            r.inst = pi;
                            r.model = m;
                            r.day = s.day;
                            r.entry_t = t.entry_t;
                            r.band_low = anchor * std::exp(-half);
                            r.band_high = anchor * std::exp(half);
                            add_leg(r, t);
                            r.hc = price_hedge(r.fills, P.lot, schedules, costs_allowed);   // none: priced as zero
                            rows.push_back(std::move(r));
                        }
                        continue;
                    }
                    const auto sd = altair::strangle_day(d, anchor, half, m, pol, rule.sp);
                    if (!sd || !*sd) { continue; }
                    Row r;
                    r.inst = pi;
                    r.model = m;
                    r.day = s.day;
                    r.entry_t = (*sd)->call.entry_t;
                    r.band_low = anchor * std::exp(-half);
                    r.band_high = anchor * std::exp(half);
                    add_leg(r, (*sd)->call);
                    add_leg(r, (*sd)->put);
                    r.fills = (*sd)->fills;
                    r.hedge = (*sd)->hedge_gross;
                    r.hc = price_hedge(r.fills, P.lot, schedules, costs_allowed);
                    rows.push_back(std::move(r));
                }
            }
        }
        for (Row& r : rows) {
            r.gross = r.hedge;
            r.priced = costs_allowed && r.hc.priced;
            r.expenses = r.hc.total;
            for (const Leg& l : r.legs) {
                r.gross += l.t.gross;
                r.expenses += l.c.total;
                r.priced = r.priced && l.c.priced;
            }
            r.net = r.priced ? r.gross - r.expenses : r.gross;
        }
        std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.entry_t < b.entry_t; });
        total_rows += rows.size();

        std::ofstream f(dir / ("trades_" + rule.id + ".csv"), std::ios::trunc);
        f << "date,instrument,model,rule,entry_time,entry_spot,exit_spot,entry_iv_pct,band_low,band_high,expiry,"
             "call_strike,call_in,call_out,call_exit,put_strike,put_in,put_out,put_exit,qty,hedge_orders,hedge_pnl,"
             "theta_pnl,move_pnl,iv_pnl,slippage_pnl,gross_pnl,option_expenses,hedge_expenses,expenses,net_pnl,costs\n";
        std::map<std::pair<std::size_t, std::size_t>, Sum> sums;
        for (const Row& r : rows) {
            const FadeTrade& t0 = r.legs.front().t;
            const FadeTrade* call = nullptr;
            const FadeTrade* put = nullptr;
            Decomp dc;
            double opt_exp = 0;
            for (const Leg& l : r.legs) {
                (l.t.side == FadeSide::Call ? call : put) = &l.t;
                dc.theta += l.dc.theta;
                dc.move += l.dc.move;
                dc.iv += l.dc.iv;
                dc.slip += l.dc.slip;
                opt_exp += l.c.total;
            }
            const auto leg_cols = [&](const FadeTrade* t) {
                return t == nullptr ? std::string{",,,"}
                                    : fixed(t->strike, 0) + ',' + fixed(t->entry_premium, 2) + ',' + fixed(t->exit_premium, 2) + ','
                                          + leg_exit(*t);
            };
            f << iso_day(r.day) << ',' << prep[r.inst].name << ",\"" << prep[r.inst].models[r.model] << "\"," << rule.id << ','
              << hhmm(r.entry_t) << ',' << fixed(t0.entry_spot, 2) << ',' << fixed(r.legs.back().t.exit_spot, 2) << ','
              << fixed(100.0 * t0.entry_iv, 2) << ',' << fixed(r.band_low, 2) << ',' << fixed(r.band_high, 2) << ','
              << iso_day(da::audit_day(t0.expiry_ts)) << ',' << leg_cols(call) << ',' << leg_cols(put) << ',' << fixed(t0.qty, 0) << ','
              << r.fills.size() << ',' << fixed(r.hedge, 2) << ',' << fixed(dc.theta, 2) << ',' << fixed(dc.move, 2) << ','
              << fixed(dc.iv, 2) << ',' << fixed(dc.slip, 2) << ',' << fixed(r.gross, 2) << ',';
            if (r.priced) {
                f << fixed(opt_exp, 2) << ',' << fixed(r.hc.total, 2) << ',' << fixed(r.expenses, 2) << ',' << fixed(r.net, 2) << ','
                  << cost_tag << '\n';
            } else {
                f << ",,,,refused\n";
            }
            Sum& s = sums[{r.inst, r.model}];
            ++s.n;
            s.wins += r.net > 0.0 ? 1 : 0;
            s.gross += r.gross;
            s.exp += r.priced ? r.expenses : 0.0;
            s.net += r.net;
            s.theta += dc.theta;
            s.move += dc.move;
            s.iv += dc.iv;
            s.hedge += r.hedge;
            s.worst = std::min(s.worst, r.net);
            s.cum += r.net;
            s.peak = std::max(s.peak, s.cum);
            s.dd = std::max(s.dd, s.peak - s.cum);
            if (s.first.empty()) { s.first = iso_day(r.day); }
            s.last = iso_day(r.day);
        }
        std::map<std::size_t, Sum> by_inst;
        for (const auto& [k, s] : sums) {
            const double n = static_cast<double>(s.n);
            summary << rule.id << ',' << prep[k.first].name << ",\"" << prep[k.first].models[k.second] << "\"," << s.n << ','
                    << s.wins << ',' << fixed(100.0 * static_cast<double>(s.wins) / n, 1) << ',' << fixed(s.gross, 2) << ','
                    << (costs_allowed ? fixed(s.exp, 2) : std::string{}) << ',' << (costs_allowed ? fixed(s.net, 2) : std::string{})
                    << ',' << (costs_allowed ? fixed(s.net / n, 2) : std::string{}) << ',' << fixed(s.theta / n, 2) << ','
                    << fixed(s.move / n, 2) << ',' << fixed(s.iv / n, 2) << ',' << fixed(s.hedge / n, 2) << ','
                    << fixed(s.worst, 2) << ',' << fixed(s.dd, 2) << ',' << s.first << ',' << s.last << ',' << cost_tag << '\n';
            Sum& b = by_inst[k.first];
            b.n += s.n;
            b.wins += s.wins;
            b.gross += s.gross;
            b.exp += s.exp;
            b.net += s.net;
            b.theta += s.theta;
            b.move += s.move;
        }
        for (const auto& [pi, b] : by_inst) {
            const double n = static_cast<double>(b.n);
            std::printf("  %-18s %-10s %7zu %6.1f %10.0f %10.0f %10.0f %10s %10s\n", rule.id.c_str(), prep[pi].name.c_str(), b.n,
                        100.0 * static_cast<double>(b.wins) / n, b.theta / n, b.move / n, b.gross / n,
                        costs_allowed ? fixed(b.exp / n, 0).c_str() : "refused", costs_allowed ? fixed(b.net / n, 0).c_str() : "-");
        }
    }
    meta << "trades," << total_rows << "\ndays_skipped_no_bars_or_vix," << refused_days << "\ndays_skipped_no_expiry," << no_expiry << "\n";
    { std::ofstream f(dir / "meta.csv", std::ios::trunc); f << meta.str(); }
    std::printf("  wrote %s\n", dir.string().c_str());
    return 0;
}
