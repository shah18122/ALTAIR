// app/vol_premium_main.cpp -- altair_vol_premium.
//
// Sell the variance premium the way an options desk does
// (strategies/vol_premium.hpp): each close, the at-the-money monthly
// straddle's implied volatility against a HAR forecast of realised volatility
// over the same sessions (analytics/har_rv.hpp, fitted only on what was known
// that day); sell one lot when implied is rich, hedge its delta with whole
// futures lots, buy it back before expiry.
//
// TWO SOURCES OF OPTION PRICES, NAMED ON EVERY OUTPUT:
//   synthetic  Black-76 at INDIA VIX (BANKNIFTY: VIX x the 20-day realised-vol
//              ratio), flat across strikes, NIFTY's monthly calendar. Then
//              implied IS VIX by construction, and the test is whether VIX
//              over-prices what the index then does.
//   bhavcopy   NSE F&O bhavcopy closes from data/bhavcopy/ (app/bhavcopy.hpp;
//              fetch with ops/fetch_bhavcopy.ps1): real at-the-money straddles,
//              real futures for the forward and the hedge, the exchange's own
//              expiries. Only traded strikes are priced.
//
// Several variants run side by side, so the forecast's contribution can be
// read off against a control that sells every month regardless:
//   always        sell every month, ignore the forecast
//   har-0 / -2 / -4   sell when implied - forecast >= 0 / 2 / 4 vol points
//   har-2-stop2x  the same, bought back once the straddle costs 2x the credit
//   har-2-unhedged    no futures hedge
//   har-2-vixveto skip entries the INDIA VIX direction forecast calls UP
//                 (--vix-log: the forecast curriculum's per-forecast log)
//
// Expenses come from config/charges.toml (app/demo_costs.hpp) and are refused
// while that schedule is unverified, unless --unverified-costs.
//
// Writes <out>/vol_premium/: trades_<variant>.csv, summary.csv,
// signals.csv, forecast_eval.csv, meta.csv.

#include <analytics/greeks.hpp>
#include <analytics/har_rv.hpp>
#include <app/bhavcopy.hpp>
#include <app/demo_costs.hpp>
#include <app/forecast_tracks.hpp>
#include <app/spec_today.hpp>
#include <risk/charges_toml.hpp>
#include <strategies/vol_premium.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
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
        "  Sell the at-the-money monthly straddle when implied vol beats a HAR forecast; delta hedged.\n\n"
        "    %s [--dataset DIR] [--out DIR] [--instrument NIFTY|BANKNIFTY|both]\n"
        "       [--source synthetic|bhavcopy] [--bhavcopy-dir DIR] [--min-sessions N] [--exit-sessions N]\n"
        "       [--slippage-pts P] [--rate R] [--vix-log FILE] [--vix-model NAME] [--unverified-costs]\n\n"
        "    --source S            synthetic (default; VIX-priced) or bhavcopy (NSE closes)\n"
        "    --bhavcopy-dir DIR    default data/bhavcopy (ops/fetch_bhavcopy.ps1 fills it)\n"
        "    --min-sessions N      sessions to expiry at entry (default 10)\n"
        "    --exit-sessions N     buy back once this many or fewer remain (default 1)\n"
        "    --slippage-pts P      premium points given up per option fill (default 1.0)\n"
        "    --vix-log FILE        forecast_log/india_vix_daily.csv from altair_forecast_curriculum\n"
        "    --vix-model NAME      the model whose INDIA VIX call vetoes entries (default Hedge)\n"
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

/// One session per day from the 5-minute bars: its last close and its
/// realised variance (bars from the open, plus the overnight gap).
struct Daily {
    std::vector<std::int64_t> day;
    std::vector<double> close, rv;
};

Daily daily_from_5m(const std::vector<da::AuditBar>& bars) {
    Daily d;
    double prev = 0.0;
    std::size_t k = 0;
    while (k < bars.size()) {
        const std::int64_t day = ft::bar_day(bars[k]);
        std::size_t e = k;
        std::vector<double> closes;
        while (e < bars.size() && ft::bar_day(bars[e]) == day) { closes.push_back(bars[e].c); ++e; }
        if (closes.size() >= 60) {   // a full session; a half day would understate the variance
            const double rv = altair::session_rv(closes, bars[k].o, prev);
            if (rv > 0.0) {
                d.day.push_back(day);
                d.close.push_back(closes.back());
                d.rv.push_back(rv);
            }
        }
        prev = closes.back();
        k = e;
    }
    return d;
}

struct Variant {
    std::string id, text;
    altair::VrpPolicy p;
    bool veto = false;
};

struct Priced {
    altair::VrpTrade t;
    dc::Costs c;
    bool priced = false;
    double net = 0.0;
};

} // namespace

int main(int argc, char** argv) {
    fs::path root = "dataset", out = "data/verified", bhav_dir = "data/bhavcopy";
    std::string which = "both", source = "synthetic", vix_log, vix_model = "Hedge";
    double rate = 0.065, slip = 1.0, min_s = 10, exit_s = 1;
    bool unverified = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a{argv[i]};
        const bool has = i + 1 < argc;
        if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
        if (a == "--dataset" && has) { root = argv[++i]; continue; }
        if (a == "--out" && has) { out = argv[++i]; continue; }
        if (a == "--instrument" && has) { which = argv[++i]; continue; }
        if (a == "--source" && has) { source = argv[++i]; continue; }
        if (a == "--bhavcopy-dir" && has) { bhav_dir = argv[++i]; continue; }
        if (a == "--vix-log" && has) { vix_log = argv[++i]; continue; }
        if (a == "--vix-model" && has) { vix_model = argv[++i]; continue; }
        if (a == "--rate" && has) { if (!parse_double(argv[++i], rate) || std::fabs(rate) > 1.0) { usage(argv[0]); return 2; } continue; }
        if (a == "--slippage-pts" && has) { if (!parse_double(argv[++i], slip) || slip < 0.0) { usage(argv[0]); return 2; } continue; }
        if (a == "--min-sessions" && has) { if (!parse_double(argv[++i], min_s) || min_s < 2 || min_s > 60) { usage(argv[0]); return 2; } continue; }
        if (a == "--exit-sessions" && has) { if (!parse_double(argv[++i], exit_s) || exit_s < 0 || exit_s > 20) { usage(argv[0]); return 2; } continue; }
        if (a == "--unverified-costs") { unverified = true; continue; }
        usage(argv[0]);
        return 2;
    }
    if (source != "synthetic" && source != "bhavcopy") { usage(argv[0]); return 2; }

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

    const auto load = [&](const std::string& dir, int tf) {
        ft::TrackInfo info;
        auto v = ft::load_bars(root / dir / (tf == da::kDailyTf ? std::string{"1d"} : std::to_string(tf) + "m"), tf, info);
        ft::clean_bars(v, info);
        return v;
    };
    const auto vix_d = daily_from_5m(load("spot/indiavix", 5));
    const auto nifty5 = load("spot/nifty", 5);
    const auto bank5 = load("spot/banknifty", 5);
    const auto nifty = daily_from_5m(nifty5), bank = daily_from_5m(bank5);
    if (nifty.day.empty() || vix_d.day.empty()) { std::printf("  no data under %s\n", root.string().c_str()); return 1; }
    const auto nifty_daily_bars = load("spot/nifty", da::kDailyTf);
    const auto expiries_nifty = ft::nifty_expiries(nifty_daily_bars);

    // INDIA VIX direction veto from the curriculum's out-of-sample log.
    std::set<std::int64_t> vix_up;
    if (!vix_log.empty()) {
        std::ifstream in(vix_log);
        std::string line;
        std::getline(in, line);
        std::size_t rows = 0;
        while (std::getline(in, line)) {
            // time,stage,learned_days,model,... ; the model is quoted
            const auto q1 = line.find('"'), q2 = line.find('"', q1 + 1);
            if (q1 == std::string::npos || q2 == std::string::npos || line.substr(q1 + 1, q2 - q1 - 1) != vix_model) { continue; }
            std::vector<std::string> f;
            std::stringstream ss(line.substr(q2 + 2));
            for (std::string c; std::getline(ss, c, ',');) { f.push_back(c); }
            // after the model: last_price,next_price,forecast_price,p_up,call,...
            if (f.size() < 5) { continue; }
            const auto day = altair::bhavcopy::detail::date(line.substr(0, 10));
            if (day && f[4] == "UP") { vix_up.insert(*day); }
            ++rows;
        }
        if (rows == 0) {
            std::printf("  --vix-log %s has no rows for model \"%s\"\n", vix_log.c_str(), vix_model.c_str());
            return 2;
        }
    }

    std::vector<Variant> variants;
    const auto base = [&](double spread) {
        altair::VrpPolicy p;
        p.rate = rate;
        p.entry_spread = spread;
        p.min_sessions = static_cast<std::size_t>(min_s);
        p.exit_sessions = static_cast<std::size_t>(exit_s);
        p.slippage_pts = slip;
        return p;
    };
    variants.push_back({"always", "sell every month, whatever the forecast says (the control)", base(-10.0), false});
    variants.push_back({"har-0", "sell when implied vol >= the HAR forecast", base(0.0), false});
    variants.push_back({"har-2", "sell when implied vol beats the HAR forecast by 2 vol points", base(0.02), false});
    variants.push_back({"har-4", "sell when implied vol beats the HAR forecast by 4 vol points", base(0.04), false});
    {
        auto p = base(0.02);
        p.stop_multiple = 2.0;
        variants.push_back({"har-2-stop2x", "har-2, bought back once the straddle costs 2x the credit", p, false});
    }
    {
        auto p = base(0.02);
        p.hedge = false;
        variants.push_back({"har-2-unhedged", "har-2 without the futures hedge", p, false});
    }
    if (!vix_log.empty()) {
        variants.push_back({"har-2-vixveto", "har-2, skipping days the " + vix_model + " model calls INDIA VIX UP", base(0.02), true});
    }

    std::ostringstream meta;
    meta << "key,value\nsource," << source << "\ncosts," << cost_tag << "\ncost_note,\"" << cost_note << "\"\n";
    meta << "prices,\"" << (source == "synthetic"
                                ? "SYNTHETIC: Black-76 at INDIA VIX (BANKNIFTY: VIX x 20-day realised-vol ratio), flat across strikes; NIFTY's monthly expiry calendar for both"
                                : "NSE F&O bhavcopy closes; traded strikes only; same-expiry futures for the forward and the hedge")
         << "\"\n";
    meta << "forecast,\"HAR (Corsi 2009) in logs on 5-minute realised variance incl. the overnight gap, refitted each day on the 1000 days before it\"\n";
    meta << "rate," << rate << "\nslippage_pts," << slip << "\nmin_sessions," << min_s << "\nexit_sessions," << exit_s << "\n";
    for (const auto& v : variants) { meta << "variant_" << v.id << ",\"" << v.text << "\"\n"; }

    const fs::path dir = out / "vol_premium";
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::map<std::string, std::vector<std::pair<std::string, Priced>>> by_variant;
    std::set<std::string> opened;
    std::ofstream signals(dir / "signals.csv", std::ios::trunc);
    signals << "instrument,date,expiry,sessions,implied_vol,har_vol,spread,realised_vol_after\n";
    std::ofstream eval(dir / "forecast_eval.csv", std::ios::trunc);
    eval << "instrument,horizon,days,har_rmse_vol_pts,trailing_rmse_vol_pts,har_bias_vol_pts,implied_minus_realised_vol_pts,"
            "share_implied_above_realised\n";

    struct Inst { std::string name; const Daily* d; };
    std::vector<Inst> insts;
    if (which == "both" || which == "NIFTY") { insts.push_back({"NIFTY", &nifty}); }
    if (which == "both" || which == "BANKNIFTY") { insts.push_back({"BANKNIFTY", &bank}); }
    std::printf("Volatility premium (%s prices)\n  %s\n", source.c_str(), cost_note.c_str());

    for (const Inst& in : insts) {
        const Daily& D = *in.d;
        if (D.day.empty()) { std::printf("  %s: no 5-minute data -- not run\n", in.name.c_str()); continue; }
        const auto lot = altair::spec_today::lot_size("config/lot_size_history.csv", in.name);
        const auto step = altair::spec_today::strike_step("data/instruments.csv", in.name);
        if (!lot || !step) {
            std::printf("  %s: no verified lot size or strike step -- not run\n", in.name.c_str());
            continue;
        }
        meta << in.name << "_lot_size," << *lot << "\n" << in.name << "_strike_step," << *step << "\n";
        std::map<std::int64_t, std::size_t> at;   // day -> index into D
        for (std::size_t k = 0; k < D.day.size(); ++k) { at[D.day[k]] = k; }

        // The HAR forecast at D-index t over h sessions, cached.
        std::map<std::pair<std::size_t, std::size_t>, double> har_cache;
        const auto har = [&](std::size_t t, std::size_t h) {
            const auto key = std::make_pair(t, h);
            if (const auto c = har_cache.find(key); c != har_cache.end()) { return c->second; }
            const auto f = altair::har_forecast(D.rv, t, h, 1000);
            const double v = f ? f->vol : std::nan("");
            har_cache[key] = v;
            return v;
        };
        // Forecast quality at a fixed 21-session horizon, before any trading.
        {
            constexpr std::size_t h = 21;
            double se_h = 0, se_n = 0, bias = 0, vrp = 0;
            std::size_t n = 0, above = 0;
            std::map<std::int64_t, double> vix_at;
            for (std::size_t k = 0; k < vix_d.day.size(); ++k) { vix_at[vix_d.day[k]] = vix_d.close[k] / 100.0; }
            for (std::size_t t = 300; t + h < D.rv.size(); ++t) {
                const double f = har(t, h);
                if (!std::isfinite(f)) { continue; }
                double fut = 0, past = 0;
                for (std::size_t k = t + 1; k <= t + h; ++k) { fut += D.rv[k]; }
                for (std::size_t k = t + 1 - 22; k <= t; ++k) { past += D.rv[k]; }
                const double real = std::sqrt(fut / h * 252.0), trail = std::sqrt(past / 22.0 * 252.0);
                se_h += (f - real) * (f - real);
                se_n += (trail - real) * (trail - real);
                bias += f - real;
                if (in.name == "NIFTY") {
                    if (const auto v = vix_at.find(D.day[t]); v != vix_at.end()) {
                        vrp += v->second - real;
                        above += v->second > real ? 1u : 0u;
                    }
                }
                ++n;
            }
            if (n > 0) {
                const double dn = static_cast<double>(n);
                eval << in.name << ',' << h << ',' << n << ',' << fixed(100 * std::sqrt(se_h / dn), 2) << ','
                     << fixed(100 * std::sqrt(se_n / dn), 2) << ',' << fixed(100 * bias / dn, 2) << ','
                     << (in.name == "NIFTY" ? fixed(100 * vrp / dn, 2) : std::string{}) << ','
                     << (in.name == "NIFTY" ? fixed(100.0 * static_cast<double>(above) / dn, 1) : std::string{}) << '\n';
                std::printf("  %s HAR, 21 sessions: RMSE %.2f vol pts (trailing 22-day %.2f), bias %+.2f%s\n", in.name.c_str(),
                            100 * std::sqrt(se_h / dn), 100 * std::sqrt(se_n / dn), 100 * bias / dn,
                            in.name == "NIFTY" ? (", VIX above realised " + fixed(100.0 * static_cast<double>(above) / dn, 1)
                                                  + " % of days by " + fixed(100 * vrp / dn, 2) + " pts on average").c_str() : "");
            }
        }

        // The market, from either source.
        altair::VrpMarket m;
        std::shared_ptr<std::map<std::int64_t, altair::bhavcopy::Day>> bhav;
        std::vector<std::size_t> d_index;   // market index -> D index
        if (source == "synthetic") {
            std::map<std::int64_t, double> vix_at;
            for (std::size_t k = 0; k < vix_d.day.size(); ++k) { vix_at[vix_d.day[k]] = vix_d.close[k] / 100.0; }
            // BANKNIFTY: VIX x the ratio of the two indices' 20-day realised vol, as of the day before.
            std::map<std::int64_t, double> scale;
            if (in.name == "BANKNIFTY") {
                std::map<std::int64_t, std::size_t> nat;
                for (std::size_t k = 0; k < nifty.day.size(); ++k) { nat[nifty.day[k]] = k; }
                for (std::size_t k = 21; k < D.day.size(); ++k) {
                    double sb = 0, sn = 0;
                    bool ok = true;
                    for (std::size_t j = k - 20; j < k && ok; ++j) {
                        const auto n2 = nat.find(D.day[j]);
                        if (n2 == nat.end()) { ok = false; break; }
                        sb += D.rv[j];
                        sn += nifty.rv[n2->second];
                    }
                    if (ok && sn > 0) { scale[D.day[k]] = std::sqrt(sb / sn); }
                }
            }
            std::vector<double> iv;
            for (std::size_t k = 0; k < D.day.size(); ++k) {
                const auto v = vix_at.find(D.day[k]);
                if (v == vix_at.end() || !(v->second > 0.0)) { continue; }
                double s = 1.0;
                if (in.name == "BANKNIFTY") {
                    const auto sc = scale.find(D.day[k]);
                    if (sc == scale.end()) { continue; }
                    s = sc->second;
                }
                m.day.push_back(D.day[k]);
                m.spot.push_back(D.close[k]);
                iv.push_back(v->second * s);
                d_index.push_back(k);
            }
            m.expiries = expiries_nifty;
            const auto days = m.day;
            const auto spot = m.spot;
            const double st = *step;
            m.forward = [days, spot, rate](std::size_t i, std::int64_t e) {
                return spot[i] * std::exp(rate * static_cast<double>(e - days[i]) / 365.0);
            };
            m.strike_near = [st](std::size_t, std::int64_t, double level) { return std::round(level / st) * st; };
            m.quote = [days, spot, iv, rate](std::size_t i, std::int64_t e, double k) {
                const double T = static_cast<double>(e - days[i]) / 365.0;
                const double F = spot[i] * std::exp(rate * T);
                altair::VrpQuote q;
                for (const bool call : {true, false}) {
                    const auto g = altair::black76(call ? altair::OptionRight::Call : altair::OptionRight::Put,
                                                   altair::Price{std::llround(F * 100.0)}, altair::Price{std::llround(k * 100.0)},
                                                   altair::Years{T}, altair::Vol{iv[i]}, rate);
                    (call ? q.call : q.put) = g ? g->price / 100.0 : std::nan("");
                }
                return q;
            };
        } else {
            altair::bhavcopy::LoadReport br;
            bhav = std::make_shared<std::map<std::int64_t, altair::bhavcopy::Day>>(
                altair::bhavcopy::load_dir(bhav_dir, in.name, br));
            std::printf("  %s bhavcopy: %zu files (%zu legacy, %zu UDiFF, %zu other), %zu option and %zu futures rows, %zu untraded skipped\n",
                        in.name.c_str(), br.files, br.legacy, br.udiff, br.unknown, br.option_rows, br.future_rows, br.untraded);
            if (bhav->empty()) {
                std::printf("  %s: no bhavcopy under %s -- run ops\\fetch_bhavcopy.ps1 -Go first\n", in.name.c_str(),
                            bhav_dir.string().c_str());
                continue;
            }
            meta << in.name << "_bhavcopy_files," << br.files << "\n";
            for (const auto& [day, bd] : *bhav) {
                const auto k = at.find(day);
                if (k == at.end()) { continue; }
                m.day.push_back(day);
                m.spot.push_back(D.close[k->second]);
                d_index.push_back(k->second);
                for (const auto& [e, f] : bd.fut) { m.expiries.insert(e); }
            }
            const auto days = m.day;
            const auto B = bhav;
            m.forward = [days, B](std::size_t i, std::int64_t e) {
                const auto& d = B->at(days[i]);
                const auto f = d.fut.find(e);
                return f == d.fut.end() ? std::nan("") : f->second;
            };
            m.strike_near = [days, B](std::size_t i, std::int64_t e, double level) {
                const auto& d = B->at(days[i]);
                const auto o = d.opt.find(e);
                double best = std::nan("");
                if (o == d.opt.end()) { return best; }
                for (const auto& [k, leg] : o->second) {
                    if (!std::isfinite(leg.call) || !std::isfinite(leg.put)) { continue; }
                    if (!std::isfinite(best) || std::fabs(k - level) < std::fabs(best - level)) { best = k; }
                }
                return best;
            };
            m.quote = [days, B](std::size_t i, std::int64_t e, double k) {
                altair::VrpQuote q;
                const auto& d = B->at(days[i]);
                const auto o = d.opt.find(e);
                if (o == d.opt.end()) { return q; }
                const auto leg = o->second.find(k);
                if (leg == o->second.end()) { return q; }
                q.call = leg->second.call;
                q.put = leg->second.put;
                return q;
            };
        }
        if (m.day.empty()) { std::printf("  %s: no priced days -- not run\n", in.name.c_str()); continue; }
        const auto di = d_index;
        m.rv_forecast = [&har, di](std::size_t i, std::size_t h) { return har(di[i], h); };

        // The signal every day it could be priced, from the plainest variant.
        bool signals_written = false;
        for (const Variant& v : variants) {
            auto p = v.p;
            p.lot_size = *lot;
            altair::VrpMarket mv = m;
            if (v.veto) {
                const auto days = m.day;
                mv.veto = [days, &vix_up](std::size_t i) { return vix_up.contains(days[i]); };
            }
            const auto r = altair::vrp_run(mv, p);
            if (!r) {
                std::printf("  %s %s: refused: %s\n", in.name.c_str(), v.id.c_str(), altair::vrp_error_text(r.error()));
                continue;
            }
            if (!signals_written) {
                for (const auto& s : r->signals) {
                    const std::size_t t = di[s.i];
                    double fut = 0;
                    std::size_t n = 0;
                    for (std::size_t k = t + 1; k <= t + s.sessions && k < D.rv.size(); ++k) { fut += D.rv[k]; ++n; }
                    signals << in.name << ',' << iso_day(m.day[s.i]) << ',' << iso_day(s.expiry) << ',' << s.sessions << ','
                            << fixed(100 * s.iv, 2) << ',' << fixed(100 * s.rv_fcst, 2) << ',' << fixed(100 * (s.iv - s.rv_fcst), 2)
                            << ',' << (n == s.sessions ? fixed(100 * std::sqrt(fut / static_cast<double>(n) * 252.0), 2) : std::string{})
                            << '\n';
                }
                signals_written = true;
            }
            for (const auto& t : r->trades) {
                Priced pr;
                pr.t = t;
                pr.priced = costs_allowed;
                const auto close_ts = [&](std::size_t i) { return m.day[i] * 86'400 + ft::kCloseSec; };
                if (costs_allowed) {
                    const auto add = [&](const dc::Costs& c) {
                        pr.c.add(c);
                        pr.priced = pr.priced && c.priced;
                    };
                    add(dc::fill(altair::Segment::Opt, altair::Side::Sell, t.qty, t.call_in - slip, close_ts(t.entry), schedules));
                    add(dc::fill(altair::Segment::Opt, altair::Side::Sell, t.qty, t.put_in - slip, close_ts(t.entry), schedules));
                    add(dc::fill(altair::Segment::Opt, altair::Side::Buy, t.qty, t.call_out + slip, close_ts(t.exit), schedules));
                    add(dc::fill(altair::Segment::Opt, altair::Side::Buy, t.qty, t.put_out + slip, close_ts(t.exit), schedules));
                    for (const auto& f : t.fills) {
                        add(dc::fill(altair::Segment::Fut, f.lots > 0 ? altair::Side::Buy : altair::Side::Sell,
                                     std::fabs(static_cast<double>(f.lots)) * *lot, f.price, close_ts(f.i), schedules));
                    }
                }
                pr.net = pr.priced ? t.gross - pr.c.total : t.gross;
                by_variant[v.id].push_back({in.name, pr});
            }
            // This instrument's rows, written while `m` can still turn day indices into dates.
            const auto& rows = by_variant[v.id];
            const fs::path f = dir / ("trades_" + v.id + ".csv");
            const bool fresh = opened.insert(f.string()).second;   // the first write this run starts the file
            std::ofstream o(f, fresh ? std::ios::trunc : std::ios::app);
            if (fresh) {
                o << "instrument,variant,entry_date,exit_date,expiry,strike,forward_in,implied_vol,har_vol,realised_vol,"
                     "call_in,put_in,credit,call_out,put_out,debit,qty,option_pnl,hedge_orders,hedge_pnl,gross_pnl,expenses,"
                     "net_pnl,exit_reason,source,costs\n";
            }
            for (const auto& [name, pr] : rows) {
                if (name != in.name) { continue; }
                const auto& t = pr.t;
                o << name << ',' << v.id << ',' << iso_day(m.day[t.entry]) << ',' << iso_day(m.day[t.exit]) << ','
                  << iso_day(t.expiry) << ',' << fixed(t.strike, 0) << ',' << fixed(t.fwd_in, 2) << ',' << fixed(100 * t.iv_in, 2)
                  << ',' << fixed(100 * t.rv_fcst, 2) << ',' << fixed(100 * t.rv_real, 2) << ',' << fixed(t.call_in, 2) << ','
                  << fixed(t.put_in, 2) << ',' << fixed(t.credit, 2) << ',' << fixed(t.call_out, 2) << ',' << fixed(t.put_out, 2)
                  << ',' << fixed(t.debit, 2) << ',' << fixed(t.qty, 0) << ',' << fixed(t.option_pnl, 2) << ','
                  << t.fills.size() << ',' << fixed(t.hedge_pnl, 2) << ',' << fixed(t.gross, 2) << ','
                  << (pr.priced ? fixed(pr.c.total, 2) : std::string{}) << ',' << (pr.priced ? fixed(pr.net, 2) : std::string{})
                  << ',' << t.exit_reason << ',' << source << ',' << cost_tag << '\n';
            }
        }
    }

    std::ofstream summary(dir / "summary.csv", std::ios::trunc);
    summary << "variant,instrument,trades,wins,win_pct,gross_pnl,expenses,net_pnl,net_per_trade,t_stat,avg_implied_vol,"
               "avg_har_vol,avg_realised_vol,worst_trade,max_drawdown,source,costs\n";
    std::printf("  %-16s %-10s %6s %6s %12s %12s %10s %6s %7s %7s %7s\n", "variant", "", "trades", "win %", "gross Rs",
                "net Rs", "net/tr", "t", "IV", "HAR", "real");
    for (const auto& v : variants) {
        std::map<std::string, std::vector<const Priced*>> per;
        for (const auto& [name, pr] : by_variant[v.id]) { per[name].push_back(&pr); }
        for (const auto& [name, list] : per) {
            const double n = static_cast<double>(list.size());
            double gross = 0, exp = 0, net = 0, ss = 0, iv = 0, hv = 0, rv = 0, worst = 0, cum = 0, peak = 0, dd = 0;
            std::size_t wins = 0;
            for (const Priced* p : list) {
                gross += p->t.gross;
                exp += p->priced ? p->c.total : 0.0;
                net += p->net;
                ss += p->net * p->net;
                iv += p->t.iv_in;
                hv += p->t.rv_fcst;
                rv += p->t.rv_real;
                worst = std::min(worst, p->net);
                wins += p->net > 0.0 ? 1 : 0;
                cum += p->net;
                peak = std::max(peak, cum);
                dd = std::max(dd, peak - cum);
            }
            const double mean = net / n, sd = std::sqrt(std::max(0.0, ss / n - mean * mean));
            const double t = sd > 0.0 ? mean / (sd / std::sqrt(n)) : 0.0;
            summary << v.id << ',' << name << ',' << list.size() << ',' << wins << ',' << fixed(100.0 * static_cast<double>(wins) / n, 1)
                    << ',' << fixed(gross, 2) << ',' << (costs_allowed ? fixed(exp, 2) : std::string{}) << ','
                    << (costs_allowed ? fixed(net, 2) : std::string{}) << ',' << (costs_allowed ? fixed(mean, 2) : std::string{})
                    << ',' << fixed(t, 2) << ',' << fixed(100 * iv / n, 2) << ',' << fixed(100 * hv / n, 2) << ','
                    << fixed(100 * rv / n, 2) << ',' << fixed(worst, 2) << ',' << fixed(dd, 2) << ',' << source << ',' << cost_tag << '\n';
            std::printf("  %-16s %-10s %6zu %6.1f %12.0f %12s %10s %6.2f %7.2f %7.2f %7.2f\n", v.id.c_str(), name.c_str(), list.size(),
                        100.0 * static_cast<double>(wins) / n, gross, costs_allowed ? fixed(net, 0).c_str() : "refused",
                        costs_allowed ? fixed(mean, 0).c_str() : "-", t, 100 * iv / n, 100 * hv / n, 100 * rv / n);
        }
    }
    { std::ofstream f(dir / "meta.csv", std::ios::trunc); f << meta.str(); }
    std::printf("  wrote %s\n", dir.string().c_str());
    return 0;
}
