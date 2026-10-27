// app/trader_combos.cpp -- `altair_trader combos`: the trader as the desk's
// meta-trader. Every model's output is a variable; every strategy of the
// sell-only book (strategies/meta_book.hpp) at every time of day is a choice;
// the combination search (models/combo_search.hpp) tries every rule that
// joins them and trades, walk-forward, the ones that have been earning.
//
// WHAT IT READS (FYERS ONLY). --fyers DIR, as ops/fetch_fyers_meta.ps1 writes it:
//   DIR/SOURCE.txt                 must say FYERS (the fetch script writes it)
//   DIR/NIFTY/1m, DIR/BANKNIFTY/1m the indices, one-minute bars
//   DIR/INDIAVIX/1d                India VIX daily closes; DIR/INDIAVIX/1m if present
//   DIR/NIFTY_FUT/1m, DIR/BANKNIFTY_FUT/1m   continuous near futures, if present
// Without SOURCE.txt saying FYERS it refuses, unless --not-fyers is given;
// then every output is stamped NOT FYERS DATA and is a smoke test only.
// Optional, joined as of the moment each value was known:
//   --logs DIR      the forecast curriculum's out-of-sample logs (every base
//                   model's conviction on every track, and each track's mean)
//   --signals FILE  any other model's output: `known_at,<name>,...` (IST)
// --book futures keeps only the strategies priced from FYERS bars; --book
// options only the model-priced ones; the default searches both.
//
// THE VARIABLES ALWAYS PRESENT, per underlying and moment, from bars that ended
// by then: implied vol (VIX; BANKNIFTY's scaled by the two indices' trailing
// realised-vol ratio), the HAR forecast of today's realised vol, the IV-RV gap
// (the standing rule's variable), today's realised vol so far against IV,
// the opening gap, the move since the open, 1/5/20-session moves, the range so
// far against the expected one, VIX's change, sessions to expiry, the
// weekday, the futures basis, and the other index's move since the open.
//
// TIME OF DAY. Each slot (--slots, default 09:20 09:45 10:30 11:30 12:30 13:30
// 14:30 15:00) is its own group: a rule says what to deploy at that time.
//
// WHAT IT WRITES (--out, default data/verified/altair_trader/combos):
//   trades.csv    every walk-forward trade and the reasoning behind it: the
//                 rule, why it was picked (its trailing record when picked),
//                 why it fired (each condition's value against its median),
//                 the fills, the exit, gross, charges, net, and whether the
//                 prices were FYERS's or the option model's
//   days.csv      net P&L per session: all, futures-only (FYERS prices),
//                 option strategies (model prices), cumulative, drawdown
//   playbook.csv  the latest pick: what to deploy at which time, and when
//   picks.csv     every monthly pick
//   rules_top.csv the best rules in sample, with the deflated Sharpe
//   summary.txt   the totals, the split by strategy and slot, the holdout,
//                 the placebo p-value, the deflated Sharpe, the leak check
//   report.html   the same as a page, with the equity curves
//
// --leak-check rebuilds every input with all prices, VIX and model outputs
// after two cuts changed, reruns everything, and needs every variable, every
// outcome final before the cut, every pick and every trade before the cut to
// come out bit for bit the same. Exit 3 if one does not.
//
// PAPER ONLY: this decides and reports; it sends no order anywhere.

#include <app/data_audit.hpp>
#include <app/demo_costs.hpp>
#include <models/combo_search.hpp>
#include <risk/charges_toml.hpp>
#include <strategies/meta_book.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace da = altair::data_audit;
namespace cb = altair::combo;
namespace mb = altair::metabook;

constexpr std::int64_t kDay = 86'400;
constexpr std::int64_t kClose = 15 * 3600 + 30 * 60;
const double kNaN = std::numeric_limits<double>::quiet_NaN();

std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool q = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (q) {
            if (c == '"') { if (i + 1 < line.size() && line[i + 1] == '"') { cur += '"'; ++i; } else q = false; }
            else cur += c;
        } else if (c == '"') q = true;
        else if (c == ',') { out.push_back(cur); cur.clear(); }
        else if (c != '\r') cur += c;
    }
    out.push_back(cur);
    return out;
}

std::string csv_field(const std::string& s) {
    if (s.find_first_of(",\"\n") == std::string::npos) return s;
    std::string o = "\"";
    for (char c : s) { if (c == '"') o += '"'; o += c; }
    return o + "\"";
}

std::string when(std::int64_t t) { return da::format_audit_time(t, false); }
std::string date_of(std::int64_t t) { return da::format_audit_time(da::audit_day(t) * kDay, true); }
std::string hhmm(std::int64_t sec_of_day) {
    char b[8];
    std::snprintf(b, sizeof b, "%02d:%02d", static_cast<int>(sec_of_day / 3600), static_cast<int>(sec_of_day / 60 % 60));
    return b;
}

// ---- raw inputs ------------------------------------------------------------------------

/// Everything read from disk. The leak check builds a second one with the
/// future changed and runs the same pipeline on it.
struct Raw {
    std::vector<da::AuditBar> spot[2], fut[2], vix1d, vix1m;
    /// model outputs: name -> (stamp, value), stamp order
    std::vector<std::string> ext_names;
    std::vector<std::vector<std::pair<std::int64_t, double>>> ext;
    std::size_t ext_core = 0;   ///< the first this many ext columns are track means (paired)
};

const char* kUnderlying[2] = {"NIFTY", "BANKNIFTY"};

std::vector<da::AuditBar> load_series(const fs::path& dir, int tf) {
    if (!fs::is_directory(dir)) return {};
    auto s = da::load_audit_dir(dir, tf);
    return s.bars;
}

/// The curriculum's logs: per track, every model's conviction, plus the track's mean.
void read_logs(const fs::path& dir, Raw& raw) {
    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file() && e.path().extension() == ".csv") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    std::map<std::string, std::map<std::int64_t, double>> cols;     // name -> stamp -> value
    std::map<std::string, std::map<std::int64_t, std::pair<double, int>>> means;
    for (const auto& p : files) {
        std::ifstream in(p);
        std::string line;
        if (!std::getline(in, line)) continue;
        const auto head = split(line);
        int ct = -1, cm = -1, cp = -1, cc = -1;
        for (int i = 0; i < static_cast<int>(head.size()); ++i) {
            if (head[static_cast<std::size_t>(i)] == "time") ct = i;
            else if (head[static_cast<std::size_t>(i)] == "model") cm = i;
            else if (head[static_cast<std::size_t>(i)] == "p_up") cp = i;
            else if (head[static_cast<std::size_t>(i)] == "call") cc = i;
        }
        if (ct < 0 || cm < 0 || cp < 0 || cc < 0) continue;
        const std::string track = p.stem().string();
        while (std::getline(in, line)) {
            const auto f = split(line);
            if (f.size() <= static_cast<std::size_t>(std::max({ct, cm, cp, cc}))) continue;
            const std::string& time = f[static_cast<std::size_t>(ct)];
            const std::string& model = f[static_cast<std::size_t>(cm)];
            if (model == "Coin flip") continue;
            const auto t = da::parse_audit_time(time);
            if (!t) continue;
            // a daily row is the call made at that session's close
            const std::int64_t stamp = time.size() == 10 ? *t + kClose : *t;
            const std::string& pu = f[static_cast<std::size_t>(cp)];
            const std::string& call = f[static_cast<std::size_t>(cc)];
            double c = kNaN;
            if (!pu.empty()) c = 2.0 * std::atof(pu.c_str()) - 1.0;
            else if (call == "UP") c = 1.0;
            else if (call == "DOWN") c = -1.0;
            if (!std::isfinite(c)) continue;
            c = std::clamp(c, -1.0, 1.0);
            cols[track + ":" + model][stamp] = c;
            auto& m = means["mean:" + track][stamp];
            m.first += c;
            m.second += 1;
        }
    }
    for (const auto& [name, rows] : means) {
        raw.ext_names.push_back(name);
        auto& v = raw.ext.emplace_back();
        for (const auto& [t, m] : rows) v.emplace_back(t, m.first / m.second);
    }
    raw.ext_core = raw.ext_names.size();
    for (const auto& [name, rows] : cols) {
        raw.ext_names.push_back(name);
        auto& v = raw.ext.emplace_back();
        for (const auto& [t, c] : rows) v.emplace_back(t, c);
    }
}

/// `known_at,<name>,...`: any other model's output.
bool read_signals(const fs::path& p, Raw& raw, std::string& err) {
    std::ifstream in(p);
    std::string line;
    if (!in || !std::getline(in, line)) { err = "cannot read " + p.string(); return false; }
    const auto head = split(line);
    if (head.empty() || head[0] != "known_at") { err = p.string() + ": the first column must be known_at"; return false; }
    const std::size_t base = raw.ext.size();
    for (std::size_t i = 1; i < head.size(); ++i) {
        raw.ext_names.push_back(p.stem().string() + ":" + head[i]);
        raw.ext.emplace_back();
    }
    while (std::getline(in, line)) {
        const auto f = split(line);
        const auto t = da::parse_audit_time(f[0]);
        if (!t) continue;
        for (std::size_t i = 1; i < f.size() && i < head.size(); ++i) {
            if (f[i].empty()) continue;
            const double v = std::atof(f[i].c_str());
            if (std::isfinite(v)) raw.ext[base + i - 1].emplace_back(*t, v);
        }
    }
    for (std::size_t i = base; i < raw.ext.size(); ++i) std::sort(raw.ext[i].begin(), raw.ext[i].end());
    return true;
}

/// The leak check's other future: every price at or after `cut` follows a
/// different path, VIX and every model output known at or after it change.
Raw perturb(const Raw& r, std::int64_t cut, std::uint64_t seed) {
    Raw o = r;
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> n01;
    for (int u = 0; u < 2; ++u) {
        double f = 1.0;
        for (auto& b : o.spot[u]) if (b.t >= cut) { f *= std::exp(0.002 * n01(rng)); b.o *= f; b.h *= f * 1.001; b.l *= f * 0.999; b.c *= f; }
        f = 1.0;
        for (auto& b : o.fut[u]) if (b.t >= cut) { f *= std::exp(0.002 * n01(rng)); b.o *= f; b.h *= f * 1.001; b.l *= f * 0.999; b.c *= f; }
    }
    for (auto& b : o.vix1d) if (b.t + kClose >= cut) { const double f = std::exp(0.2 * n01(rng)); b.o *= f; b.h *= f; b.l *= f; b.c *= f; }
    for (auto& b : o.vix1m) if (b.t >= cut) { const double f = std::exp(0.2 * n01(rng)); b.o *= f; b.h *= f; b.l *= f; b.c *= f; }
    for (auto& col : o.ext) for (auto& [t, v] : col) if (t >= cut) v = n01(rng);
    return o;
}

// ---- calendar --------------------------------------------------------------------------

/// Session days present in the data, and weekdays beyond its end.
struct Calendar {
    std::vector<std::int64_t> days;
    [[nodiscard]] bool trading(std::int64_t d) const {
        if (!days.empty() && d > days.back()) return da::audit_weekday(d) < 5;
        return std::binary_search(days.begin(), days.end(), d);
    }
    [[nodiscard]] std::int64_t prev_trading(std::int64_t d) const {
        for (int i = 0; i < 10 && !trading(d); ++i) --d;
        return d;
    }
    [[nodiscard]] int sessions_between(std::int64_t a, std::int64_t b) const {   // (a, b]
        int n = 0;
        for (std::int64_t d = a + 1; d <= b; ++d) n += trading(d) ? 1 : 0;
        return n;
    }
};

std::int64_t civil(int y, int m, int d) { return da::audit_days_from_civil(y, static_cast<unsigned>(m), static_cast<unsigned>(d)); }

/// Last `weekday` (0 = Monday) of the month containing `day`.
std::int64_t last_weekday_of_month(std::int64_t day, int weekday) {
    const auto c = da::audit_civil_from_days(day);
    const int ny = c.m == 12 ? static_cast<int>(c.y) + 1 : static_cast<int>(c.y);
    const int nm = c.m == 12 ? 1 : static_cast<int>(c.m) + 1;
    std::int64_t d = civil(ny, nm, 1) - 1;
    while (da::audit_weekday(d) != weekday) --d;
    return d;
}

/// The nearest expiry on or after `day`: NIFTY weekly (monthly before
/// 2019-02-11), BANKNIFTY monthly; Thursday until 2025-08-31, Tuesday after;
/// a holiday moves it to the session before.
std::int64_t expiry_for(int u, std::int64_t day, const Calendar& cal) {
    const std::int64_t tuesday_from = civil(2025, 9, 1);
    const bool weekly = u == 0 && day >= civil(2019, 2, 11);
    for (std::int64_t probe = day, i = 0; i < 80; ++i) {
        std::int64_t target;
        if (weekly) {
            const int wd = probe >= tuesday_from ? 1 : 3;
            target = probe;
            while (da::audit_weekday(target) != wd) ++target;
        } else {
            target = last_weekday_of_month(probe, probe >= tuesday_from ? 1 : 3);
            if (target < probe) {
                const auto c = da::audit_civil_from_days(probe);
                const std::int64_t next = civil(c.m == 12 ? static_cast<int>(c.y) + 1 : static_cast<int>(c.y),
                                                c.m == 12 ? 1 : static_cast<int>(c.m) + 1, 1);
                target = last_weekday_of_month(next, next >= tuesday_from ? 1 : 3);
            }
        }
        const std::int64_t e = cal.prev_trading(target);
        if (e >= day) return e;
        probe = target + 1;
    }
    return day;
}

// ---- the pipeline ----------------------------------------------------------------------

struct Options {
    fs::path fyers, out = "data/verified/altair_trader/combos", logs;
    std::vector<fs::path> signals;
    std::vector<std::int64_t> slots{9 * 3600 + 20 * 60, 9 * 3600 + 45 * 60, 10 * 3600 + 30 * 60, 11 * 3600 + 30 * 60,
                                    12 * 3600 + 30 * 60, 13 * 3600 + 30 * 60, 14 * 3600 + 30 * 60, 15 * 3600};
    bool unverified = false, leak_check = false, not_fyers = false;
    std::string book = "all";   ///< all | futures (FYERS-priced only) | options (model-priced only)
    int placebo = 20;
    double holdout_share = 0.2;
    double lot[2] = {65, 30};
    cb::SearchConfig search;
};

struct StepInfo {
    int u = 0, slot = 0;
    std::size_t day_index = 0, bar = 0;
    std::vector<mb::BookResult> res;   ///< per strategy
};

struct Built {
    std::vector<std::string> names;          ///< every variable
    std::vector<cb::Step> steps;
    std::vector<StepInfo> info;
    std::vector<mb::BookDay> days[2];
    std::size_t core = 0;                    ///< variables paired in depth-2 rules
    bool fut_present[2] = {false, false};
};

/// Per session of one underlying: realised variance (1m returns + overnight gap).
struct DayVol { std::int64_t day; double rv; double close; double open; };

class Pipeline {
public:
    Pipeline(const Options& opt, const std::vector<altair::ChargeSchedule>& sched) : opt_(opt), sched_(sched) {}

    Built build(const Raw& raw) const {
        Built b;
        // sessions per underlying
        Calendar cal;
        std::map<std::int64_t, std::vector<mb::BookBar>> bars[2], fbars[2];
        for (int u = 0; u < 2; ++u) {
            for (const auto& x : raw.spot[u]) {
                const std::int64_t m = da::audit_minute_of_day(x.t);
                if (m < da::kSessionOpenMin || m >= da::kSessionCloseMin) continue;
                bars[u][da::audit_day(x.t)].push_back({x.t, x.o, x.h, x.l, x.c});
            }
            for (const auto& x : raw.fut[u]) fbars[u][da::audit_day(x.t)].push_back({x.t, x.o, x.h, x.l, x.c});
            for (const auto& [d, v] : bars[u]) cal.days.push_back(d);
            b.fut_present[u] = !raw.fut[u].empty();
        }
        std::sort(cal.days.begin(), cal.days.end());
        cal.days.erase(std::unique(cal.days.begin(), cal.days.end()), cal.days.end());

        // VIX: daily close known at 15:30; 1m close known at the bar's end
        std::vector<std::pair<std::int64_t, double>> vix_close;   // (known_at, level)
        for (const auto& x : raw.vix1d) vix_close.emplace_back(x.t + kClose, x.c);
        std::map<std::int64_t, double> vix_min;   // bar start -> close
        for (const auto& x : raw.vix1m) vix_min[x.t] = x.c;
        const auto vix_asof = [&](std::int64_t t) {   // last daily close known at or before t
            auto it = std::upper_bound(vix_close.begin(), vix_close.end(), std::make_pair(t, 1e300));
            return it == vix_close.begin() ? kNaN : std::prev(it)->second;
        };
        const auto vix_prev_close = [&](std::int64_t t, int back) {
            auto it = std::upper_bound(vix_close.begin(), vix_close.end(), std::make_pair(t, 1e300));
            const auto n = static_cast<long>(it - vix_close.begin());
            return n - 1 - back >= 0 ? vix_close[static_cast<std::size_t>(n - 1 - back)].second : kNaN;
        };

        // daily realised variance per underlying, HAR forecast from past sessions only
        std::map<std::int64_t, DayVol> dv[2];
        std::map<std::int64_t, double> har[2];   // day -> forecast variance for that day (known at prior close)
        for (int u = 0; u < 2; ++u) {
            double prev_close = kNaN;
            for (const auto& [d, v] : bars[u]) {
                double rv = 0;
                for (std::size_t i = 1; i < v.size(); ++i) { const double r = std::log(v[i].c / v[i - 1].c); rv += r * r; }
                if (std::isfinite(prev_close)) { const double g = std::log(v.front().o / prev_close); rv += g * g; }
                dv[u][d] = {d, rv, v.back().c, v.front().o};
                prev_close = v.back().c;
            }
            // HAR: rv_d = b0 + b1 rv_{d-1} + b2 mean5 + b3 mean22, OLS refit each session on earlier ones
            std::vector<double> hist;
            double xtx[16] = {0}, xty[4] = {0};
            std::vector<std::array<double, 4>> xs;
            for (const auto& [d, x] : dv[u]) {
                const std::size_t n = hist.size();
                if (n >= 22) {
                    const double r1 = hist[n - 1];
                    double m5 = 0, m22 = 0;
                    for (std::size_t k = 1; k <= 5; ++k) m5 += hist[n - k];
                    for (std::size_t k = 1; k <= 22; ++k) m22 += hist[n - k];
                    const std::array<double, 4> f{1.0, r1, m5 / 5, m22 / 22};
                    // forecast for d from the fit on earlier pairs
                    if (xs.size() >= 60) {
                        double a[16], bb[4], w[4];
                        std::copy(xtx, xtx + 16, a);
                        std::copy(xty, xty + 4, bb);
                        for (int i = 0; i < 4; ++i) a[i * 4 + i] += 1e-12 * (1 + a[i * 4 + i]);
                        if (solve4(a, bb, w)) {
                            double fc = 0;
                            for (int i = 0; i < 4; ++i) fc += w[i] * f[static_cast<std::size_t>(i)];
                            har[u][d] = std::max(fc, 0.25 * m22 / 22);
                        }
                    }
                    for (int i = 0; i < 4; ++i) {
                        for (int j = 0; j < 4; ++j) xtx[i * 4 + j] += f[static_cast<std::size_t>(i)] * f[static_cast<std::size_t>(j)];
                        xty[i] += f[static_cast<std::size_t>(i)] * x.rv;
                    }
                    xs.push_back(f);
                }
                hist.push_back(x.rv);
            }
        }
        // BANKNIFTY's IV: VIX times the trailing 60-session ratio of the two realised vols
        std::map<std::int64_t, double> bnf_ratio;
        {
            std::vector<double> rn, rb;
            for (const auto& [d, x] : dv[1]) {
                if (rn.size() >= 20) {
                    double sn = 0, sb = 0;
                    const std::size_t k = std::min<std::size_t>(60, rn.size());
                    for (std::size_t i = rn.size() - k; i < rn.size(); ++i) { sn += rn[i]; sb += rb[i]; }
                    if (sn > 0) bnf_ratio[d] = std::sqrt(sb / sn);
                }
                const auto it = dv[0].find(d);
                if (it != dv[0].end()) { rn.push_back(it->second.rv); rb.push_back(x.rv); }
            }
        }

        // book days
        for (int u = 0; u < 2; ++u) {
            for (const auto& [d, v] : bars[u]) {
                mb::BookDay bd;
                bd.day = d;
                bd.spot = v;
                const auto fit = fbars[u].find(d);
                if (fit != fbars[u].end()) {
                    std::map<std::int64_t, mb::BookBar> fm;
                    for (const auto& x : fit->second) fm[x.t] = x;
                    std::size_t miss = 0;
                    std::vector<mb::BookBar> al;
                    for (const auto& s : v) {
                        auto f = fm.find(s.t);
                        if (f != fm.end()) al.push_back(f->second);
                        else if (!al.empty()) { ++miss; auto c = al.back(); c.t = s.t; c.o = c.h = c.l = c.c; al.push_back(c); }
                        else { ++miss; al.push_back({s.t, kNaN, kNaN, kNaN, kNaN}); }
                    }
                    bool ok = miss * 20 <= v.size();
                    for (const auto& x : al) ok = ok && std::isfinite(x.o);
                    if (ok) bd.fut = std::move(al);
                }
                const std::int64_t open = d * kDay + da::kSessionOpenMin * 60;
                const double ratio = u == 0 ? 1.0 : (bnf_ratio.count(d) ? bnf_ratio.at(d) : kNaN);
                const double v_open = vix_asof(open) / 100.0 * ratio;
                bd.iv.resize(v.size());
                for (std::size_t i = 0; i < v.size(); ++i) {
                    const auto m = vix_min.find(v[i].t);
                    bd.iv[i] = m != vix_min.end() ? m->second / 100.0 * ratio : v_open;
                }
                const auto h = har[u].find(d);
                bd.stop_vol = h != har[u].end() ? std::sqrt(h->second * 252.0) : kNaN;
                bd.expiry_day = expiry_for(u, d, cal);
                bd.sessions_to_expiry = cal.sessions_between(d, bd.expiry_day);
                b.days[u].push_back(std::move(bd));
            }
        }

        // variables
        // logs of annual vols; the gaps are log ratios (0: equal)
        b.names = {"iv_minus_har", "log_iv", "log_har_rv", "rv_today_minus_iv", "log_rv_today", "gap_bp", "ret_open_bp",
                   "ret_1d_bp", "ret_5d_bp", "ret_20d_bp", "range_ratio", "vix_chg_1d", "sessions_to_expiry",
                   "weekday", "basis_bp", "other_ret_open_bp"};
        const std::size_t n_core = b.names.size();
        for (std::size_t i = 0; i < raw.ext_core; ++i) b.names.push_back(raw.ext_names[i]);
        b.core = b.names.size();
        for (std::size_t i = raw.ext_core; i < raw.ext_names.size(); ++i) b.names.push_back(raw.ext_names[i]);

        mb::BookConfig bc[2];
        bc[0].lot = opt_.lot[0];
        bc[0].strike_step = 50;
        bc[1].lot = opt_.lot[1];
        bc[1].strike_step = 100;
        bc[1].fut_slip_pts = 2.0;
        bc[1].opt_slip_min = 1.0;
        const mb::BookCost cost = [this](bool option, bool buy, double qty, double px, std::int64_t t) {
            const auto c = altair::demo_costs::fill(option ? altair::Segment::Opt : altair::Segment::Fut,
                                                    buy ? altair::Side::Buy : altair::Side::Sell, qty, px, t, sched_);
            return c.priced ? c.total : kNaN;
        };
        const mb::MetaBook book0(b.days[0], bc[0], cost), book1(b.days[1], bc[1], cost);
        const mb::MetaBook* books[2] = {&book0, &book1};

        std::map<std::int64_t, std::size_t> day_index[2];
        for (int u = 0; u < 2; ++u) for (std::size_t i = 0; i < b.days[u].size(); ++i) day_index[u][b.days[u][i].day] = i;
        const auto ext_asof = [&](std::size_t col, std::int64_t t) {
            const auto& v = raw.ext[col];
            auto it = std::upper_bound(v.begin(), v.end(), std::make_pair(t, 1e300));
            return it == v.begin() ? kNaN : std::prev(it)->second;
        };

        // steps in time order: day, slot, underlying
        for (const auto d : cal.days) {
            for (std::size_t h = 0; h < opt_.slots.size(); ++h) {
                const std::int64_t t = d * kDay + opt_.slots[h];
                for (int u = 0; u < 2; ++u) {
                    const auto di = day_index[u].find(d);
                    if (di == day_index[u].end()) continue;
                    const mb::BookDay& bd = b.days[u][di->second];
                    std::size_t e = 0;
                    while (e < bd.spot.size() && bd.spot[e].t < t) ++e;
                    if (e == 0 || e >= bd.spot.size() || bd.spot[e].t != t) continue;
                    cb::Step st;
                    st.t = t;
                    st.group = static_cast<int>(u * static_cast<int>(opt_.slots.size()) + static_cast<int>(h));
                    st.x.assign(b.names.size(), kNaN);
                    const double s_now = bd.spot[e - 1].c;
                    const double iv = bd.iv[e - 1];
                    const auto hv = har[u].find(d);
                    const double har_vol = hv != har[u].end() ? std::sqrt(hv->second * 252.0) : kNaN;
                    double rv = 0, hi = bd.spot[0].h, lo = bd.spot[0].l;
                    for (std::size_t i = 1; i < e; ++i) {
                        const double r = std::log(bd.spot[i].c / bd.spot[i - 1].c);
                        rv += r * r;
                        hi = std::max(hi, bd.spot[i].h);
                        lo = std::min(lo, bd.spot[i].l);
                    }
                    const double share = static_cast<double>(e) / mb::kSessionMinutes * (1.0 - mb::kOvernightShare);
                    const double rv_ann = e >= 5 ? std::sqrt(rv / share * 252.0) : kNaN;
                    const auto prev_close = [&](int back) {   // close `back` sessions before today
                        const std::size_t i = di->second;
                        return i >= static_cast<std::size_t>(back) ? b.days[u][i - static_cast<std::size_t>(back)].spot.back().c : kNaN;
                    };
                    const double pc1 = prev_close(1);
                    st.x[0] = std::log(iv) - std::log(har_vol);
                    st.x[1] = std::log(iv);
                    st.x[2] = std::log(har_vol);
                    st.x[3] = std::log(rv_ann) - std::log(iv);
                    st.x[4] = std::log(rv_ann);
                    st.x[5] = 1e4 * std::log(bd.spot[0].o / pc1);
                    st.x[6] = 1e4 * std::log(s_now / bd.spot[0].o);
                    st.x[7] = 1e4 * std::log(pc1 / prev_close(2));
                    st.x[8] = 1e4 * std::log(pc1 / prev_close(6));
                    st.x[9] = 1e4 * std::log(pc1 / prev_close(21));
                    st.x[10] = (hi - lo) / (s_now * iv * std::sqrt(share / 252.0));
                    st.x[11] = std::log(vix_prev_close(t, 0) / vix_prev_close(t, 1));
                    st.x[12] = bd.sessions_to_expiry;
                    st.x[13] = da::audit_weekday(d);
                    st.x[14] = bd.fut.empty() ? kNaN : 1e4 * (bd.fut[e - 1].c - s_now) / s_now;
                    {
                        const int o = 1 - u;
                        const auto dj = day_index[o].find(d);
                        if (dj != day_index[o].end()) {
                            const auto& od = b.days[o][dj->second];
                            std::size_t k = 0;
                            while (k < od.spot.size() && od.spot[k].t < t) ++k;
                            if (k > 0) st.x[15] = 1e4 * std::log(od.spot[k - 1].c / od.spot[0].o);
                        }
                    }
                    for (std::size_t c = 0; c < raw.ext.size(); ++c) st.x[n_core + c] = ext_asof(c, t);
                    for (double& x : st.x) if (!std::isfinite(x)) x = kNaN;

                    StepInfo info;
                    info.u = u;
                    info.slot = static_cast<int>(h);
                    info.day_index = di->second;
                    info.bar = e;
                    st.y.resize(mb::kBookStrategies);
                    info.res.resize(mb::kBookStrategies);
                    for (int s = 0; s < mb::kBookStrategies; ++s) {
                        const bool opt_leg = mb::book_uses_options(s);
                        if ((opt_.book == "futures" && opt_leg) || (opt_.book == "options" && !opt_leg)) {
                            info.res[static_cast<std::size_t>(s)] = mb::BookResult{};
                            st.y[static_cast<std::size_t>(s)] = cb::Outcome{};
                            continue;
                        }
                        auto r = books[u]->run(s, di->second, e);
                        if (!std::isfinite(r.pnl)) r = mb::BookResult{};
                        st.y[static_cast<std::size_t>(s)] = cb::Outcome{r.pnl, r.known_at};
                        info.res[static_cast<std::size_t>(s)] = std::move(r);
                    }
                    b.steps.push_back(std::move(st));
                    b.info.push_back(std::move(info));
                }
            }
        }
        return b;
    }

private:
    static bool solve4(double* a, double* b, double* x) {
        for (int c = 0; c < 4; ++c) {
            int p = c;
            for (int r = c + 1; r < 4; ++r) if (std::fabs(a[r * 4 + c]) > std::fabs(a[p * 4 + c])) p = r;
            if (std::fabs(a[p * 4 + c]) < 1e-300) return false;
            if (p != c) { for (int k = 0; k < 4; ++k) std::swap(a[c * 4 + k], a[p * 4 + k]); std::swap(b[c], b[p]); }
            for (int r = c + 1; r < 4; ++r) {
                const double f = a[r * 4 + c] / a[c * 4 + c];
                for (int k = c; k < 4; ++k) a[r * 4 + k] -= f * a[c * 4 + k];
                b[r] -= f * b[c];
            }
        }
        for (int r = 3; r >= 0; --r) {
            double s = b[r];
            for (int k = r + 1; k < 4; ++k) s -= a[r * 4 + k] * x[k];
            x[r] = s / a[r * 4 + r];
        }
        return true;
    }

    const Options& opt_;
    const std::vector<altair::ChargeSchedule>& sched_;
};

// ---- reporting -------------------------------------------------------------------------

struct Perf {
    double net = 0, gross = 0, costs = 0, real = 0, synth = 0;
    std::size_t trades = 0, wins = 0;
    double sharpe = 0, max_dd = 0, psr = kNaN;
    std::vector<std::pair<std::int64_t, double>> daily;   ///< day -> net
};

Perf perf_of(const cb::Search& s, const Built& b, std::int64_t from_day, std::int64_t to_day) {
    Perf p;
    std::map<std::int64_t, double> by_day;
    std::set<std::int64_t> sessions;
    for (const auto& st : b.steps) {
        const std::int64_t d = st.t / kDay;
        if (d >= from_day && d <= to_day) sessions.insert(d);
    }
    for (const auto& d : sessions) by_day[d] = 0;
    for (const auto& tr : s.trades()) {
        const std::int64_t d = tr.known_at / kDay;
        if (d < from_day || d > to_day) continue;
        const auto& r = b.info[tr.step].res[static_cast<std::size_t>(s.rules()[tr.rule].strategy)];
        by_day[d] += tr.pnl;
        p.net += tr.pnl;
        p.gross += r.gross;
        p.costs += r.costs;
        (r.synthetic ? p.synth : p.real) += tr.pnl;
        ++p.trades;
        p.wins += tr.pnl > 0 ? 1 : 0;
    }
    std::vector<double> v;
    double cum = 0, peak = 0;
    for (const auto& [d, x] : by_day) {
        v.push_back(x);
        p.daily.emplace_back(d, x);
        cum += x;
        peak = std::max(peak, cum);
        p.max_dd = std::max(p.max_dd, peak - cum);
    }
    const auto m = cb::moments(v);
    if (m.sd > 0) {
        p.sharpe = m.mean / m.sd * std::sqrt(252.0);
        p.psr = cb::probabilistic_sharpe(m.mean / m.sd, 0.0, m.n, m.skew, m.kurt);
    }
    return p;
}

std::string rule_text(const cb::Rule& r, const Built& b, const Options& opt) {
    const int h = r.group % static_cast<int>(opt.slots.size()), u = r.group / static_cast<int>(opt.slots.size());
    std::string s = std::string(kUnderlying[u]) + " " + hhmm(opt.slots[static_cast<std::size_t>(h)]) + " " +
                    mb::book_strategy_name(r.strategy);
    const auto cond = [&](int c) {
        return b.names[static_cast<std::size_t>(c / 2)] + (c % 2 == 0 ? " above median" : " below median");
    };
    if (r.c1 < 0) return s + " always";
    s += " when " + cond(r.c1);
    if (r.c2 >= 0) s += " and " + cond(r.c2);
    return s;
}

/// `v` with `prec` decimals; prec < 0: four significant digits.
std::string fmt(int prec, double v) {
    char b[64];
    if (prec < 0) std::snprintf(b, sizeof b, "%.4g", v);
    else std::snprintf(b, sizeof b, "%.*f", prec, v);
    return b;
}

void write_svg_curve(std::ostream& o, const std::vector<std::pair<std::string, std::vector<double>>>& series,
                     const std::vector<std::string>& labels) {
    const double w = 900, h = 300, pad = 50;
    double lo = 0, hi = 0;
    std::size_t n = 0;
    for (const auto& [name, v] : series) { for (double x : v) { lo = std::min(lo, x); hi = std::max(hi, x); } n = std::max(n, v.size()); }
    if (hi - lo < 1) hi = lo + 1;
    o << "<svg viewBox='0 0 " << w << " " << h << "' role='img' aria-label='Cumulative net P&L'>";
    const auto y = [&](double v) { return h - pad - (v - lo) / (hi - lo) * (h - 2 * pad); };
    o << "<line x1='" << pad << "' x2='" << w - 10 << "' y1='" << y(0) << "' y2='" << y(0) << "' class='axis'/>";
    for (int k = 0; k <= 4; ++k) {
        const double v = lo + (hi - lo) * k / 4;
        o << "<text x='4' y='" << y(v) + 4 << "' class='tick'>" << fmt(0, v / 1000) << "k</text>";
    }
    if (n > 1 && !labels.empty()) {
        for (int k = 0; k <= 4; ++k) {
            const std::size_t i = (n - 1) * static_cast<std::size_t>(k) / 4;
            o << "<text x='" << pad + (w - pad - 10) * static_cast<double>(i) / static_cast<double>(n - 1) - 30 << "' y='" << h - 15
              << "' class='tick'>" << labels[std::min(i, labels.size() - 1)] << "</text>";
        }
    }
    int k = 0;
    for (const auto& [name, v] : series) {
        o << "<polyline class='s" << k++ << "' fill='none' points='";
        for (std::size_t i = 0; i < v.size(); ++i)
            o << pad + (w - pad - 10) * static_cast<double>(i) / static_cast<double>(std::max<std::size_t>(1, n - 1)) << ","
              << y(v[i]) << " ";
        o << "'/>";
    }
    o << "</svg>";
}

void usage(const char* exe) {
    std::printf(
        "%s combos --fyers DIR [--logs DIR] [--signals FILE]... [--out DIR]\n"
        "    [--slots 09:20,10:30,...] [--top-k 10] [--depth 2] [--pair-signals 40]\n"
        "    [--half-life 365] [--min-trades 30] [--warmup-days 365] [--placebo 20]\n"
        "    [--holdout 0.2] [--lot-nifty 65] [--lot-banknifty 30] [--unverified-costs]\n"
        "    [--book all|futures|options] [--leak-check] [--not-fyers]\n\n"
        "The meta-trader: every model's output is a variable; every sell-only strategy\n"
        "at every slot is a choice; rules joining them are searched walk-forward and the\n"
        "best are traded on paper. Option legs are model-priced (FYERS keeps no expired\n"
        "options) and reported apart from futures. See app/trader_combos.cpp.\n",
        exe);
}

} // namespace

int combos_main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has = i + 1 < argc;
        if (a == "--fyers" && has) opt.fyers = argv[++i];
        else if (a == "--out" && has) opt.out = argv[++i];
        else if (a == "--logs" && has) opt.logs = argv[++i];
        else if (a == "--signals" && has) opt.signals.emplace_back(argv[++i]);
        else if (a == "--slots" && has) {
            opt.slots.clear();
            std::stringstream ss(argv[++i]);
            std::string x;
            while (std::getline(ss, x, ',')) {
                int hh = 0, mm = 0;
                if (std::sscanf(x.c_str(), "%d:%d", &hh, &mm) != 2 || hh < 9 || hh > 15) { usage(argv[0]); return 2; }
                opt.slots.push_back(hh * 3600 + mm * 60);
            }
            std::sort(opt.slots.begin(), opt.slots.end());
        }
        else if (a == "--top-k" && has) opt.search.top_k = static_cast<std::size_t>(std::max(1, std::atoi(argv[++i])));
        else if (a == "--depth" && has) opt.search.depth = std::clamp(std::atoi(argv[++i]), 0, 2);
        else if (a == "--pair-signals" && has) opt.search.pair_signals = static_cast<std::size_t>(std::max(0, std::atoi(argv[++i])));
        else if (a == "--half-life" && has) opt.search.half_life_days = std::max(20.0, std::atof(argv[++i]));
        else if (a == "--min-trades" && has) opt.search.min_trades = std::max(5.0, std::atof(argv[++i]));
        else if (a == "--warmup-days" && has) opt.search.warmup_days = std::max(30, std::atoi(argv[++i]));
        else if (a == "--placebo" && has) opt.placebo = std::max(0, std::atoi(argv[++i]));
        else if (a == "--holdout" && has) opt.holdout_share = std::clamp(std::atof(argv[++i]), 0.0, 0.9);
        else if (a == "--lot-nifty" && has) opt.lot[0] = std::max(1.0, std::atof(argv[++i]));
        else if (a == "--lot-banknifty" && has) opt.lot[1] = std::max(1.0, std::atof(argv[++i]));
        else if (a == "--book" && has) {
            opt.book = argv[++i];
            if (opt.book != "all" && opt.book != "futures" && opt.book != "options") { usage(argv[0]); return 2; }
        }
        else if (a == "--unverified-costs") opt.unverified = true;
        else if (a == "--leak-check") opt.leak_check = true;
        else if (a == "--not-fyers") opt.not_fyers = true;
        else if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
        else { usage(argv[0]); return 2; }
    }
    if (opt.fyers.empty()) { usage(argv[0]); return 2; }

    // ---- provenance: FYERS only ------------------------------------------------------
    std::string source;
    {
        std::ifstream in(opt.fyers / "SOURCE.txt");
        std::getline(in, source);
    }
    const bool is_fyers = source.find("FYERS") != std::string::npos;
    if (!is_fyers && !opt.not_fyers) {
        std::printf("%s has no SOURCE.txt saying FYERS. The meta-trader reads FYERS data only\n"
                    "(ops/fetch_fyers_meta.ps1 writes it). --not-fyers runs a smoke test on other data,\n"
                    "stamped NOT FYERS DATA, never a result.\n", opt.fyers.string().c_str());
        return 2;
    }
    const std::string stamp = is_fyers ? "FYERS data" : "NOT FYERS DATA - smoke test only, not a result";

    // ---- charges ---------------------------------------------------------------------
    std::vector<altair::ChargeSchedule> sched;
    const auto rep = altair::load_charges_file("config/charges.toml", sched);
    if (!rep) { std::printf("config/charges.toml did not load: %s\n", altair::charges_error_text(rep.error())); return 2; }
    std::string cost_tag = "verified";
    if (!rep->verified) {
        if (!opt.unverified) { std::printf("config/charges.toml is UNVERIFIED: pass --unverified-costs to price expenses anyway\n"); return 2; }
        for (auto& s : sched) s.verified = true;
        cost_tag = "UNVERIFIED";
    }

    // ---- inputs ----------------------------------------------------------------------
    Raw raw;
    for (int u = 0; u < 2; ++u) {
        raw.spot[u] = load_series(opt.fyers / kUnderlying[u] / "1m", 1);
        raw.fut[u] = load_series(opt.fyers / (std::string(kUnderlying[u]) + "_FUT") / "1m", 1);
    }
    raw.vix1d = load_series(opt.fyers / "INDIAVIX" / "1d", da::kDailyTf);
    raw.vix1m = load_series(opt.fyers / "INDIAVIX" / "1m", 1);
    if (raw.spot[0].empty() || raw.vix1d.empty()) {
        std::printf("need %s/NIFTY/1m and %s/INDIAVIX/1d\n", opt.fyers.string().c_str(), opt.fyers.string().c_str());
        return 2;
    }
    if (!opt.logs.empty()) read_logs(opt.logs, raw);
    for (const auto& p : opt.signals) {
        std::string err;
        if (!read_signals(p, raw, err)) { std::printf("%s\n", err.c_str()); return 2; }
    }
    std::printf("altair_trader combos (%s): NIFTY %zu bars, BANKNIFTY %zu, futures %zu/%zu, VIX %zu days + %zu minutes, %zu model columns\n",
                stamp.c_str(), raw.spot[0].size(), raw.spot[1].size(), raw.fut[0].size(), raw.fut[1].size(),
                raw.vix1d.size(), raw.vix1m.size(), raw.ext.size());

    const Pipeline pipe(opt, sched);
    const Built b = pipe.build(raw);
    if (b.steps.empty()) { std::printf("no decision steps: no session covers the slots\n"); return 2; }
    auto cfg = opt.search;
    cfg.pair_signals = std::min(cfg.pair_signals, b.core);
    const std::size_t groups = 2 * opt.slots.size();
    cb::Search search(groups, mb::kBookStrategies, b.names.size(), cfg);
    std::printf("  %zu steps, %zu variables (%zu paired), %d strategies, %zu slots: %zu rules\n", b.steps.size(),
                b.names.size(), cfg.pair_signals, mb::kBookStrategies, opt.slots.size(), search.rules().size());
    search.run(b.steps);

    const std::int64_t first_day = b.steps.front().t / kDay, last_day = b.steps.back().t / kDay;
    const std::int64_t trade_from = search.picks().empty() ? last_day + 1 : search.picks().front().t / kDay;
    const std::int64_t hold_from = last_day - static_cast<std::int64_t>(static_cast<double>(last_day - trade_from) * opt.holdout_share);
    const Perf all = perf_of(search, b, trade_from, last_day);
    const Perf pre = perf_of(search, b, trade_from, hold_from - 1);
    const Perf hold = perf_of(search, b, hold_from, last_day);

    // ---- placebo ---------------------------------------------------------------------
    std::vector<double> placebo;
    for (int k = 0; k < opt.placebo; ++k) {
        cb::Search p(groups, mb::kBookStrategies, b.names.size(), cfg);
        p.run(cb::shuffle_outcomes(b.steps, 1000 + static_cast<std::uint64_t>(k)));
        double net = 0;
        for (const auto& tr : p.trades()) net += tr.pnl;
        placebo.push_back(net);
        std::printf("  placebo %d/%d: %.0f\r", k + 1, opt.placebo, net);
        std::fflush(stdout);
    }
    if (opt.placebo > 0) std::printf("\n");
    double p_value = kNaN;
    if (!placebo.empty()) {
        std::size_t ge = 0;
        for (double x : placebo) ge += x >= all.net ? 1 : 0;
        p_value = static_cast<double>(1 + ge) / static_cast<double>(1 + placebo.size());
    }

    // ---- in-sample rules and the deflated Sharpe -------------------------------------
    std::set<std::int64_t> sess;
    for (const auto& st : b.steps) sess.insert(st.t / kDay);
    const double T = static_cast<double>(sess.size());
    std::vector<std::pair<double, std::size_t>> ranked;
    std::vector<double> srs;
    for (std::size_t r = 0; r < search.rules().size(); ++r) {
        const auto& st = search.stats()[r];
        if (st.n < cfg.min_trades) continue;
        const double m = st.us / T, var = st.uq / T - m * m;
        if (!(var > 0)) continue;
        const double sr = m / std::sqrt(var);
        srs.push_back(sr);
        ranked.emplace_back(sr, r);
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& c) { return a.first > c.first; });
    const auto srm = cb::moments(srs);
    const double sr0 = cb::expected_max_sharpe(static_cast<double>(srs.size()), srm.sd * srm.sd);
    const double best_sr = ranked.empty() ? 0.0 : ranked.front().first;
    const double dsr = cb::probabilistic_sharpe(best_sr, sr0, T, 0.0, 3.0);

    // ---- leak check ------------------------------------------------------------------
    std::string leak = "not run (pass --leak-check)";
    int leak_bad = 0;
    if (opt.leak_check) {
        const std::size_t ns = sess.size();
        std::vector<std::int64_t> sv(sess.begin(), sess.end());
        for (const double frac : {0.4, 0.7}) {
            const std::int64_t cut = sv[static_cast<std::size_t>(static_cast<double>(ns) * frac)] * kDay + 11 * 3600;
            const Raw other = perturb(raw, cut, static_cast<std::uint64_t>(frac * 1000));
            const Built ob = pipe.build(other);
            cb::Search os(groups, mb::kBookStrategies, ob.names.size(), cfg);
            os.run(ob.steps);
            int bad = 0;
            const auto same = [](double x, double y) { return (std::isnan(x) && std::isnan(y)) || x == y; };
            std::size_t j = 0;
            for (; j < b.steps.size() && b.steps[j].t < cut; ++j) {
                if (j >= ob.steps.size() || ob.steps[j].t != b.steps[j].t) { ++bad; break; }
                for (std::size_t i = 0; i < b.steps[j].x.size(); ++i) bad += same(b.steps[j].x[i], ob.steps[j].x[i]) ? 0 : 1;
                for (std::size_t s = 0; s < b.steps[j].y.size(); ++s) {
                    const auto& y0 = b.steps[j].y[s];
                    const auto& y1 = ob.steps[j].y[s];
                    if (y0.known_at < cut || y1.known_at < cut)
                        bad += same(y0.pnl, y1.pnl) && y0.known_at == y1.known_at ? 0 : 1;
                }
            }
            std::vector<std::pair<std::size_t, std::size_t>> ta, tb;
            for (const auto& tr : search.trades()) if (b.steps[tr.step].t < cut) ta.emplace_back(tr.step, tr.rule);
            for (const auto& tr : os.trades()) if (ob.steps[tr.step].t < cut) tb.emplace_back(tr.step, tr.rule);
            bad += ta == tb ? 0 : 1;
            for (std::size_t i = 0; i < search.picks().size() && search.picks()[i].t < cut; ++i)
                bad += i < os.picks().size() && os.picks()[i].rules == search.picks()[i].rules ? 0 : 1;
            std::printf("  leak check at %s: %zu steps, %zu trades before the cut, %d differences\n", when(cut).c_str(), j,
                        ta.size(), bad);
            leak_bad += bad;
        }
        leak = leak_bad == 0 ? "passed: nothing after either cut moved any variable, outcome, pick or trade before it"
                             : "BAD: " + std::to_string(leak_bad) + " differences before a cut";
    }

    // ---- outputs ---------------------------------------------------------------------
    std::error_code ec;
    fs::create_directories(opt.out, ec);
    {
        std::ofstream o(opt.out / "trades.csv");
        o << "entry_time,underlying,slot,strategy,lots,pricing,rule,picked_on,why_picked,why_now,detail,final_at,gross,charges,net\n";
        for (const auto& tr : search.trades()) {
            const auto& st = b.steps[tr.step];
            const auto& ru = search.rules()[tr.rule];
            const auto& in = b.info[tr.step];
            const auto& r = in.res[static_cast<std::size_t>(ru.strategy)];
            std::int64_t picked_on = 0;
            for (const auto& p : search.picks()) if (p.t <= st.t) picked_on = p.t;
            std::string now;
            for (int c : {ru.c1, ru.c2}) {
                if (c < 0) continue;
                const auto i = static_cast<std::size_t>(c / 2);
                if (!now.empty()) now += "; ";
                now += b.names[i] + " = " + fmt(-1, st.x[i]) + (c % 2 == 0 ? " > " : " < ") +
                       fmt(-1, search.thresholds()[tr.step][i]) + " median";
            }
            if (now.empty()) now = "no condition: the rule trades every session at this time";
            const std::string why = "best rule for this slot and strategy at the pick: trailing mean Rs " +
                                    fmt(0, tr.mean_at_pick) + " per trade, t " + fmt(2, tr.t_at_pick) +
                                    " over " + fmt(0, tr.n_at_pick) + " effective trades";
            o << when(st.t) << ',' << kUnderlying[in.u] << ',' << hhmm(opt.slots[static_cast<std::size_t>(in.slot)]) << ','
              << mb::book_strategy_name(ru.strategy) << ",1," << (r.synthetic ? "synthetic options" : "FYERS") << ','
              << csv_field(rule_text(ru, b, opt)) << ',' << date_of(picked_on) << ',' << csv_field(why) << ','
              << csv_field(now) << ',' << csv_field(r.why) << ',' << when(tr.known_at) << ',' << fmt(2, r.gross)
              << ',' << fmt(2, r.costs) << ',' << fmt(2, tr.pnl) << '\n';
        }
    }
    std::vector<double> cum_all, cum_real, cum_syn;
    std::vector<std::string> labels;
    {
        std::map<std::int64_t, std::array<double, 3>> by;
        std::map<std::int64_t, int> cnt;
        for (const auto& [d, x] : all.daily) by[d] = {0, 0, 0};
        for (const auto& tr : search.trades()) {
            const std::int64_t d = tr.known_at / kDay;
            if (!by.count(d)) continue;
            const bool syn = b.info[tr.step].res[static_cast<std::size_t>(search.rules()[tr.rule].strategy)].synthetic;
            by[d][0] += tr.pnl;
            by[d][syn ? 2 : 1] += tr.pnl;
            ++cnt[d];
        }
        std::ofstream o(opt.out / "days.csv");
        o << "date,trades_closed,net,net_futures_fyers,net_options_synthetic,cum,drawdown\n";
        double c0 = 0, c1 = 0, c2 = 0, peak = 0;
        for (const auto& [d, v] : by) {
            c0 += v[0]; c1 += v[1]; c2 += v[2];
            peak = std::max(peak, c0);
            o << date_of(d * kDay) << ',' << cnt[d] << ',' << fmt(2, v[0]) << ',' << fmt(2, v[1]) << ','
              << fmt(2, v[2]) << ',' << fmt(2, c0) << ',' << fmt(2, peak - c0) << '\n';
            cum_all.push_back(c0); cum_real.push_back(c1); cum_syn.push_back(c2);
            labels.push_back(date_of(d * kDay).substr(0, 7));
        }
    }
    {
        std::ofstream o(opt.out / "picks.csv");
        o << "picked_on,rank,rule,trailing_mean,trailing_t,effective_trades\n";
        for (const auto& p : search.picks()) {
            for (std::size_t k = 0; k < p.rules.size(); ++k)
                o << date_of(p.t) << ',' << k + 1 << ',' << csv_field(rule_text(search.rules()[p.rules[k]], b, opt)) << ','
                  << fmt(2, p.mean[k]) << ',' << fmt(3, p.t_stat[k]) << ',' << fmt(1, p.n_eff[k]) << '\n';
        }
    }
    std::vector<std::array<std::string, 5>> playbook;
    if (!search.picks().empty()) {
        for (std::size_t r : search.picks().back().rules) {
            const auto& ru = search.rules()[r];
            const auto& st = search.stats()[r];
            const int h = ru.group % static_cast<int>(opt.slots.size()), u = ru.group / static_cast<int>(opt.slots.size());
            playbook.push_back({kUnderlying[u], hhmm(opt.slots[static_cast<std::size_t>(h)]), mb::book_strategy_name(ru.strategy),
                                rule_text(ru, b, opt), "mean Rs " + fmt(0, st.mean()) + ", t " + fmt(2, st.t()) +
                                                           ", " + fmt(0, st.n) + " trades in sample"});
        }
        std::sort(playbook.begin(), playbook.end(), [](const auto& a, const auto& c) { return a[1] != c[1] ? a[1] < c[1] : a[0] < c[0]; });
        std::ofstream o(opt.out / "playbook.csv");
        o << "underlying,time,strategy,rule,record\n";
        for (const auto& p : playbook) o << p[0] << ',' << p[1] << ',' << p[2] << ',' << csv_field(p[3]) << ',' << csv_field(p[4]) << '\n';
    }
    {
        std::ofstream o(opt.out / "rules_top.csv");
        o << "rank,rule,trades,mean_per_trade,in_sample_t,sharpe_per_session,deflated_sharpe_prob\n";
        for (std::size_t i = 0; i < ranked.size() && i < 200; ++i) {
            const auto& st = search.stats()[ranked[i].second];
            o << i + 1 << ',' << csv_field(rule_text(search.rules()[ranked[i].second], b, opt)) << ',' << st.n << ','
              << fmt(2, st.us / st.n) << ',' << fmt(2, st.plain_t()) << ',' << fmt(4, ranked[i].first) << ','
              << fmt(4, cb::probabilistic_sharpe(ranked[i].first, sr0, T, 0.0, 3.0)) << '\n';
        }
    }
    // by strategy and slot
    std::map<std::string, std::pair<double, int>> by_strat, by_slot;
    for (const auto& tr : search.trades()) {
        const auto& ru = search.rules()[tr.rule];
        const int u = ru.group / static_cast<int>(opt.slots.size()), h = ru.group % static_cast<int>(opt.slots.size());
        auto& a = by_strat[std::string(kUnderlying[u]) + " " + mb::book_strategy_name(ru.strategy)];
        a.first += tr.pnl; ++a.second;
        auto& c = by_slot[hhmm(opt.slots[static_cast<std::size_t>(h)])];
        c.first += tr.pnl; ++c.second;
    }
    // open lots at once (trades overlap: nothing caps them yet)
    std::size_t max_open[2] = {0, 0};
    {
        std::vector<std::pair<std::int64_t, int>> ev[2];
        for (const auto& tr : search.trades()) {
            const int u = b.info[tr.step].u;
            ev[u].emplace_back(b.steps[tr.step].t, +1);
            ev[u].emplace_back(tr.known_at, -1);
        }
        for (int u = 0; u < 2; ++u) {
            std::sort(ev[u].begin(), ev[u].end());   // a close at the same second goes first
            long open = 0;
            for (const auto& [t, d] : ev[u]) { open += d; max_open[u] = std::max(max_open[u], static_cast<std::size_t>(std::max(0L, open))); }
        }
    }
    // the unconditional baseline: each strategy every session at every slot, in sample
    struct Base { double n = 0, s = 0, q = 0; };
    std::map<std::string, Base> base;
    for (std::size_t r = 0; r < search.rules().size(); ++r) {
        const auto& ru = search.rules()[r];
        if (ru.c1 >= 0) continue;
        const int u = ru.group / static_cast<int>(opt.slots.size());
        auto& x = base[std::string(kUnderlying[u]) + " " + mb::book_strategy_name(ru.strategy)];
        x.n += search.stats()[r].n;
        x.s += search.stats()[r].us;
        x.q += search.stats()[r].uq;
    }
    std::ostringstream sum;
    sum << "altair_trader combos -- " << stamp << ", charges " << cost_tag << "\n"
        << "PAPER ONLY. Option legs are priced by Black-Scholes on the index price at VIX-based IV\n"
        << "(FYERS keeps no expired options): option results are a model, evidence of nothing until\n"
        << "recorded chains replace it. Futures results use the input's prices (" << stamp << ").\n"
        << "Trades are unlimited: every pick that fires opens one more lot, so positions overlap.\n\n"
        << "data: " << date_of(first_day * kDay) << " .. " << date_of(last_day * kDay) << ", " << b.steps.size()
        << " decision steps, " << b.names.size() << " variables, " << mb::kBookStrategies << " strategies, "
        << opt.slots.size() << " slots, " << search.rules().size() << " rules tried\n"
        << "book: " << opt.book << "\n"
        << "walk-forward: monthly pick of the top " << cfg.top_k << " (best per slot+strategy by trailing t, half-life "
        << cfg.half_life_days << "d, >= " << cfg.min_trades << " trades), first pick " << date_of(trade_from * kDay) << "\n\n";
    const auto block = [&](const char* name, const Perf& p) {
        sum << name << ": trades " << p.trades << ", win rate " << fmt(1, p.trades ? 100.0 * static_cast<double>(p.wins) / static_cast<double>(p.trades) : 0.0)
            << "%, net Rs " << fmt(0, p.net) << " (gross " << fmt(0, p.gross) << ", charges " << fmt(0, p.costs)
            << "), futures (market prices) Rs " << fmt(0, p.real) << ", options (model prices) Rs " << fmt(0, p.synth)
            << ", Sharpe " << fmt(2, p.sharpe) << ", max drawdown Rs " << fmt(0, p.max_dd)
            << ", P(SR>0) " << fmt(3, p.psr) << "\n";
    };
    block("all walk-forward", all);
    block("before the holdout", pre);
    block(("holdout from " + date_of(hold_from * kDay)).c_str(), hold);
    sum << "\nplacebo (" << placebo.size() << " runs on outcomes shuffled across sessions): ";
    if (!placebo.empty()) {
        const auto pm = cb::moments(placebo);
        sum << "mean Rs " << fmt(0, pm.mean) << ", best Rs " << fmt(0, *std::max_element(placebo.begin(), placebo.end()))
            << "; p-value of the real run " << fmt(3, p_value) << "\n"
            << "  (the placebo keeps each strategy's average and breaks only the link to the variables: a high\n"
            << "  p-value says the timing adds nothing over the strategy's own average, not that it loses)\n";
    } else {
        sum << "not run\n";
    }
    sum << "deflated Sharpe: best in-sample rule per-session SR " << fmt(4, best_sr) << " vs " << fmt(4, sr0)
        << " expected from luck over " << srs.size() << " rules; probability it is real " << fmt(3, dsr) << "\n"
        << "  (trials are correlated, so this is a conservative bound)\n"
        << "leak check: " << leak << "\n"
        << "most lots open at once: NIFTY " << max_open[0] << ", BANKNIFTY " << max_open[1] << "\n\nby strategy (walk-forward):\n";
    for (const auto& [k, v] : by_strat) sum << "  " << k << ": " << v.second << " trades, net Rs " << fmt(0, v.first) << "\n";
    sum << "by time of day:\n";
    for (const auto& [k, v] : by_slot) sum << "  " << k << ": " << v.second << " trades, net Rs " << fmt(0, v.first) << "\n";
    sum << "\nbaseline in sample (the strategy at every slot of every session, no condition):\n";
    for (const auto& [k, v] : base) {
        if (v.n < 2) continue;
        const double m = v.s / v.n, sd = std::sqrt(std::max(0.0, v.q / v.n - m * m));
        sum << "  " << k << ": " << fmt(0, v.n) << " trades, mean Rs " << fmt(0, m) << ", t "
            << fmt(2, sd > 0 ? m / sd * std::sqrt(v.n) : 0.0) << "\n";
    }
    sum << "\nplaybook (latest pick: what to deploy when):\n";
    for (const auto& p : playbook) sum << "  " << p[3] << "  [" << p[4] << "]\n";
    {
        std::ofstream o(opt.out / "summary.txt");
        o << sum.str();
    }
    std::printf("%s", sum.str().c_str());

    // the page
    {
        std::ofstream o(opt.out / "report.html");
        o << "<!doctype html><html lang='en'><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
             "<title>Meta-trader performance</title><style>"
             ":root{--bg:#fbfaf7;--fg:#1d1d1b;--mut:#6b6a66;--line:#d9d6cf;--a:#2f6db5;--b:#3a8a5c;--c:#c0763a}"
             "@media (prefers-color-scheme:dark){:root{--bg:#161615;--fg:#ecebe7;--mut:#a3a19b;--line:#3a3936;--a:#6ea4e6;--b:#6cc08f;--c:#e2a36c}}"
             "body{background:var(--bg);color:var(--fg);font:15px/1.5 system-ui,sans-serif;margin:0 auto;max-width:960px;padding:16px}"
             "h1{font-size:22px}h2{font-size:17px;margin-top:28px}pre{white-space:pre-wrap;font-size:13px}"
             "table{border-collapse:collapse;width:100%;font-size:13px}td,th{border-bottom:1px solid var(--line);padding:4px 6px;text-align:left}"
             ".axis{stroke:var(--line)}.tick{fill:var(--mut);font-size:11px}.s0{stroke:var(--a);stroke-width:2}.s1{stroke:var(--b);stroke-width:1.5}.s2{stroke:var(--c);stroke-width:1.5}"
             ".key span{display:inline-block;margin-right:16px}.warn{color:var(--c)}svg{width:100%;height:auto}"
             "</style></head><body><h1>altair_trader meta-trader</h1><p class='warn'>"
          << stamp << ". Paper only. Option strategies are priced by a model (no FYERS option history), so their line is not evidence.</p>"
          << "<h2>Cumulative net P&amp;L, walk-forward</h2><p class='key'><span style='color:var(--a)'>all</span>"
             "<span style='color:var(--b)'>futures, market prices</span><span style='color:var(--c)'>options, model prices</span></p>";
        write_svg_curve(o, {{"all", cum_all}, {"fut", cum_real}, {"opt", cum_syn}}, labels);
        o << "<h2>Playbook: what to deploy when</h2><table><tr><th>Underlying</th><th>Time</th><th>Strategy</th><th>Rule</th><th>Record</th></tr>";
        for (const auto& p : playbook) o << "<tr><td>" << p[0] << "</td><td>" << p[1] << "</td><td>" << p[2] << "</td><td>" << p[3] << "</td><td>" << p[4] << "</td></tr>";
        o << "</table><h2>Summary</h2><pre>" << sum.str() << "</pre></body></html>\n";
    }
    std::printf("wrote %s\n", opt.out.string().c_str());
    return leak_bad == 0 ? 0 : 3;
}
