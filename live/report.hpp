// live/report.hpp -- what the paper book earned, day by day, and how sure we can be.
//
// From the ledger the engine writes (live/ledger.hpp):
//   * journal.csv -- every fill: the cash that moved and the positions held;
//   * marks.csv   -- each held position's mark near the close (15:29), so a
//                    position carried overnight is marked to market daily;
//   * margin.csv  -- the margin estimate every minute, per model: the capital.
//
// DAILY MARK-TO-MARKET. A model's P&L for a day is the cash its fills moved
// that day (less their expenses) plus the change in what it holds, valued at
// that day's marks. A day on which something held has no mark (the engine
// stopped before 15:29) is UNMARKED: its P&L is not guessed, it is carried
// into the next marked day, and the report counts such days.
//
// EXPENSES ARE NEVER ZERO BY DEFAULT. A fill priced without a schedule has
// NaN expenses (the engine refused to guess); its day's net is NaN, and a
// model with any such day reports gross only.
//
// UNCERTAINTY. Daily P&L is serially dependent (positions carry, regimes
// persist), so resampling single days understates the error. Means get a
// moving-block bootstrap over days (block length n^(1/3)), drawn ONCE for all
// models together so their correlation survives; from the same draws:
// percentile intervals, one-sided p-values (H0: mean daily net <= 0), and the
// Romano-Wolf step-down adjustment for having tried every model at once --
// with Holm's alongside, which ignores the correlation and so is stricter.
//
// STRESS. The same series with expenses x cost_mult and an extra slippage of
// slip_bp on every rupee traded: what latency and a cost schedule too kind
// would take away.
//
// REGIMES. Each day is labelled by the previous close of India VIX, cut into
// terciles over the report's own days: low, mid, high.
//
// The RNG is a fixed-seed SplitMix64 with its own bounded draw, so the same
// inputs give the same intervals on every compiler and platform.

#pragma once

#include <live/universe.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace altair::live::report {

inline constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// ---- reading the ledger ----------------------------------------------------------

/// Split one CSV line; quoted fields may hold commas and doubled quotes.
[[nodiscard]] inline std::vector<std::string> csv_fields(const std::string& line) {
    std::vector<std::string> out;
    std::string f;
    bool quoted = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quoted) {
            if (c == '"' && i + 1 < line.size() && line[i + 1] == '"') { f += '"'; ++i; }
            else if (c == '"') quoted = false;
            else f += c;
        } else if (c == '"') quoted = true;
        else if (c == ',') { out.push_back(f); f.clear(); }
        else if (c != '\r') f += c;
    }
    out.push_back(f);
    return out;
}

/// The IST trading day (days since 1970-01-01) of a feed time; feed times
/// are after 1970, which the readers check.
[[nodiscard]] inline std::int64_t day_of_ns(std::int64_t ns) { return (ns / 1'000'000'000LL + 19800) / 86400; }

struct JournalFill {
    std::int64_t ns = 0;
    std::string model, symbol;
    std::uint32_t token = 0;
    bool open = true;
    int side = 0;              ///< +1 bought, -1 sold
    std::int64_t qty = 0;
    double price = 0.0;
    double expenses = kNaN;    ///< NaN: unpriced
    bool carry = false;
    std::int64_t submit_ns = 0;
};

/// journal.csv, in file order (the order the engine filled). Malformed rows
/// are counted in `bad`, never silently dropped.
[[nodiscard]] inline std::vector<JournalFill> read_journal(const std::string& path, std::size_t* bad = nullptr) {
    std::vector<JournalFill> out;
    std::ifstream in(path);
    std::string line;
    std::size_t skipped = 0;
    if (!std::getline(in, line) || line.rfind("ns,", 0) != 0) { if (bad) *bad = 0; return out; }
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        const auto c = csv_fields(line);
        if (c.size() < 11) { ++skipped; continue; }
        JournalFill f;
        f.ns = std::atoll(c[0].c_str());
        f.model = c[1];
        f.token = static_cast<std::uint32_t>(std::strtoul(c[2].c_str(), nullptr, 10));
        f.symbol = c[3];
        f.open = c[4] == "OPEN";
        f.side = std::atoi(c[5].c_str());
        f.qty = std::atoll(c[6].c_str());
        f.price = std::atof(c[7].c_str());
        f.expenses = c[8].empty() ? kNaN : std::atof(c[8].c_str());
        f.carry = c[9] == "1";
        f.submit_ns = c.size() > 11 ? std::atoll(c[11].c_str()) : f.ns;
        if (f.ns <= 0 || f.token == 0 || f.qty <= 0 || (f.side != 1 && f.side != -1) || !(f.price > 0.0)) { ++skipped; continue; }
        out.push_back(f);
    }
    if (bad) *bad = skipped;
    return out;
}

/// marks.csv: the last mark per (day, model, token).
using MarkKey = std::tuple<std::int64_t, std::string, std::uint32_t>;
[[nodiscard]] inline std::map<MarkKey, double> read_marks(const std::string& path) {
    std::map<MarkKey, double> out;
    std::ifstream in(path);
    std::string line;
    if (!std::getline(in, line)) return out;
    while (std::getline(in, line)) {
        const auto c = csv_fields(line);
        if (c.size() < 9 || c[7].empty()) continue;
        const std::int64_t ns = std::atoll(c[1].c_str());
        const double m = std::atof(c[7].c_str());
        if (ns <= 0 || !(m > 0.0)) continue;
        out[{day_of_ns(ns), c[2], static_cast<std::uint32_t>(std::strtoul(c[3].c_str(), nullptr, 10))}] = m;
    }
    return out;
}

/// margin.csv: the day's peak estimate per model ("ALL" for the book), and
/// the days the engine ran at all (it samples every minute it is up).
struct MarginRecord {
    std::map<std::pair<std::int64_t, std::string>, double> peak;   ///< NaN when a sample was unpriceable
    std::set<std::int64_t> days;
};
[[nodiscard]] inline MarginRecord read_margin(const std::string& path) {
    MarginRecord out;
    std::ifstream in(path);
    std::string line;
    if (!std::getline(in, line)) return out;
    while (std::getline(in, line)) {
        const auto c = csv_fields(line);
        if (c.size() < 4) continue;
        const std::int64_t ns = std::atoll(c[1].c_str());
        if (ns <= 0) continue;
        const std::int64_t d = day_of_ns(ns);
        out.days.insert(d);
        const double v = c[3].empty() ? kNaN : std::atof(c[3].c_str());
        auto [it, fresh] = out.peak.try_emplace({d, c[2]}, v);
        if (!fresh) it->second = std::isfinite(it->second) && std::isfinite(v) ? std::max(it->second, v) : kNaN;
    }
    return out;
}

/// Daily closes from a dataset partition (dataset/spot/<x>/1d/*.csv).
[[nodiscard]] inline std::map<std::int64_t, double> read_daily_closes(const std::string& dir) {
    std::map<std::int64_t, double> m;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (e.path().extension() != ".csv") continue;
        std::ifstream in(e.path());
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] < '0' || line[0] > '9') continue;
            const auto c = csv_fields(line);
            if (c.size() < 5) continue;
            const std::int64_t d = parse_day(c[0].substr(0, 10));
            const double close = std::atof(c[4].c_str());
            if (d > 0 && close > 0.0) m[d] = close;
        }
    }
    return m;
}

// ---- the daily series ----------------------------------------------------------------

struct DayPnl {
    std::int64_t day = 0;
    double gross = 0.0;        ///< rupees: cash moved before expenses + change in marked value
    double expenses = 0.0;     ///< NaN when a fill was unpriced
    double turnover = 0.0;     ///< rupees traded, both sides
    std::size_t fills = 0;
    bool marked = true;        ///< false: something held had no mark; P&L carried to the next marked day
    [[nodiscard]] double net() const { return gross - expenses; }
};

struct ModelDaily {
    std::string model;
    std::vector<DayPnl> days;     ///< every report day, in order (zeros on days it did nothing)
    std::size_t unmarked = 0;
    bool unpriced = false;     ///< some fill had no expenses: net is unavailable
    std::int64_t open_units_at_end = 0;
};

/// Daily mark-to-market for each model over `days` (sorted, unique). A day
/// outside `days` on which a fill happened is added: no fill goes unreported.
[[nodiscard]] inline std::vector<ModelDaily> daily_mtm(const std::vector<JournalFill>& fills, const std::map<MarkKey, double>& marks,
                                                         std::set<std::int64_t> days) {
    std::set<std::string> models;
    for (const auto& f : fills) { models.insert(f.model); days.insert(day_of_ns(f.ns)); }
    std::vector<ModelDaily> out;
    for (const auto& model : models) {
        ModelDaily ms;
        ms.model = model;
        std::map<std::uint32_t, std::int64_t> units;     // signed holdings
        double value_last = 0.0, pending = 0.0, pending_exp = 0.0;
        std::size_t k = 0;
        std::vector<const JournalFill*> mine;
        for (const auto& f : fills) if (f.model == model) mine.push_back(&f);
        std::stable_sort(mine.begin(), mine.end(), [](const JournalFill* a, const JournalFill* b) { return a->ns < b->ns; });
        for (const std::int64_t d : days) {
            DayPnl day;
            day.day = d;
            double cash = 0.0, exp = 0.0;
            for (; k < mine.size() && day_of_ns(mine[k]->ns) <= d; ++k) {
                const JournalFill& f = *mine[k];
                const double notional = static_cast<double>(f.qty) * f.price;
                cash -= static_cast<double>(f.side) * notional;
                exp += f.expenses;   // NaN sticks
                day.turnover += notional;
                ++day.fills;
                units[f.token] += static_cast<std::int64_t>(f.side) * f.qty;
            }
            double value = 0.0;
            bool marked = true;
            for (const auto& [tok, u] : units) {
                if (u == 0) continue;
                const auto it = marks.find({d, model, tok});
                if (it == marks.end()) { marked = false; break; }
                value += static_cast<double>(u) * it->second;
            }
            if (!marked) {
                pending += cash;
                pending_exp += exp;
                day.gross = kNaN;
                day.expenses = kNaN;
                day.marked = false;
                ++ms.unmarked;
            } else {
                day.gross = pending + cash + value - value_last;
                day.expenses = pending_exp + exp;
                pending = pending_exp = 0.0;
                value_last = value;
            }
            if (day.fills > 0 && !std::isfinite(exp)) ms.unpriced = true;
            ms.days.push_back(day);
        }
        for (const auto& [tok, u] : units) ms.open_units_at_end += u < 0 ? -u : u;
        out.push_back(std::move(ms));
    }
    return out;
}

// ---- statistics ----------------------------------------------------------------------

/// SplitMix64, with an exact bounded draw: the same numbers everywhere.
class BlockRng {
public:
    explicit BlockRng(std::uint64_t seed) : s_(seed) {}
    std::uint64_t next() {
        std::uint64_t z = (s_ += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
    /// Uniform on [0, n), n > 0, by rejection: no modulo bias.
    std::uint64_t below(std::uint64_t n) {
        const std::uint64_t limit = std::numeric_limits<std::uint64_t>::max() - std::numeric_limits<std::uint64_t>::max() % n;
        std::uint64_t x = next();
        while (x >= limit) x = next();
        return x % n;
    }

private:
    std::uint64_t s_;
};

/// Linear-interpolated quantile (type 7) of a sorted vector.
[[nodiscard]] inline double quantile_sorted(const std::vector<double>& v, double q) {
    if (v.empty()) return kNaN;
    const double h = (static_cast<double>(v.size()) - 1.0) * q;
    const auto lo = static_cast<std::size_t>(std::floor(h));
    const std::size_t hi = std::min(lo + 1, v.size() - 1);
    return v[lo] + (h - static_cast<double>(lo)) * (v[hi] - v[lo]);
}

struct DailyStats {
    std::size_t days = 0;          ///< finite days in the series
    double total = kNaN, mean = kNaN, sd = kNaN, sharpe = kNaN;   ///< sharpe: annualised, sqrt(252)
    double max_drawdown = kNaN;    ///< rupees, peak to trough of the cumulative series (>= 0)
    double hit_rate = kNaN;        ///< share of days with net > 0
    double ci_lo = kNaN, ci_hi = kNaN;   ///< 95 % block-bootstrap interval of the mean
    double se = kNaN;              ///< bootstrap standard error of the mean
    double p = kNaN;               ///< one-sided: H0 mean <= 0
    double p_holm = kNaN, p_rw = kNaN;   ///< adjusted for every model tested together
};

[[nodiscard]] inline DailyStats describe(const std::vector<double>& x) {
    DailyStats s;
    std::vector<double> v;
    for (const double a : x) if (std::isfinite(a)) v.push_back(a);
    s.days = v.size();
    if (v.empty()) return s;
    s.total = std::accumulate(v.begin(), v.end(), 0.0);
    s.mean = s.total / static_cast<double>(v.size());
    if (v.size() > 1) {
        double ss = 0.0;
        for (const double a : v) ss += (a - s.mean) * (a - s.mean);
        s.sd = std::sqrt(ss / static_cast<double>(v.size() - 1));
        s.sharpe = s.sd > 0.0 ? s.mean / s.sd * std::sqrt(252.0) : kNaN;
    }
    double cum = 0.0, peak = 0.0, dd = 0.0;
    std::size_t up = 0;
    for (const double a : v) {
        cum += a;
        peak = std::max(peak, cum);
        dd = std::max(dd, peak - cum);
        up += a > 0.0 ? 1u : 0u;
    }
    s.max_drawdown = dd;
    s.hit_rate = static_cast<double>(up) / static_cast<double>(v.size());
    return s;
}

/// Moving-block bootstrap of the mean for several series over the SAME days,
/// drawn jointly; fills ci/se/p in `out` and the Holm and Romano-Wolf
/// adjustments across the series. Series with fewer than `min_days` finite
/// days are left untested (NaN p).
inline void bootstrap(const std::vector<std::vector<double>>& series, std::vector<DailyStats>& out, std::uint64_t seed = 20261002,
                      std::size_t draws = 4000, std::size_t min_days = 5) {
    const std::size_t m = series.size();
    if (m == 0 || series[0].empty()) return;
    const std::size_t n = series[0].size();
    const auto block = std::max<std::size_t>(1, static_cast<std::size_t>(std::llround(std::cbrt(static_cast<double>(n)))));
    BlockRng rng(seed);
    std::vector<std::vector<double>> boot(m, std::vector<double>(draws, kNaN));
    std::vector<std::size_t> idx(n);
    for (std::size_t b = 0; b < draws; ++b) {
        for (std::size_t filled = 0; filled < n;) {
            const std::size_t start = static_cast<std::size_t>(rng.below(n));
            for (std::size_t j = 0; j < block && filled < n; ++j) idx[filled++] = (start + j) % n;   // circular blocks
        }
        for (std::size_t s = 0; s < m; ++s) {
            double sum = 0.0;
            std::size_t c = 0;
            for (const std::size_t i : idx) if (std::isfinite(series[s][i])) { sum += series[s][i]; ++c; }
            boot[s][b] = c > 0 ? sum / static_cast<double>(c) : kNaN;
        }
    }
    std::vector<std::size_t> tested;
    std::vector<double> t(m, kNaN);
    for (std::size_t s = 0; s < m; ++s) {
        DailyStats& st = out[s];
        if (st.days < min_days || !std::isfinite(st.mean)) continue;
        std::vector<double> v;
        for (const double a : boot[s]) if (std::isfinite(a)) v.push_back(a);
        if (v.size() < draws / 2) continue;
        std::sort(v.begin(), v.end());
        st.ci_lo = quantile_sorted(v, 0.025);
        st.ci_hi = quantile_sorted(v, 0.975);
        double mu = 0.0;
        for (const double a : v) mu += a;
        mu /= static_cast<double>(v.size());
        double ss = 0.0;
        for (const double a : v) ss += (a - mu) * (a - mu);
        st.se = std::sqrt(ss / static_cast<double>(v.size() - 1));
        if (!(st.se > 0.0)) continue;
        // Centred: how often does a resample move the mean up by as much as the mean itself?
        std::size_t hits = 0;
        for (const double a : v) hits += (a - st.mean) >= st.mean ? 1u : 0u;
        st.p = (1.0 + static_cast<double>(hits)) / (1.0 + static_cast<double>(v.size()));
        t[s] = st.mean / st.se;
        tested.push_back(s);
    }
    // Holm.
    std::vector<std::size_t> by_p = tested;
    std::sort(by_p.begin(), by_p.end(), [&](std::size_t a, std::size_t b) { return out[a].p < out[b].p; });
    double run = 0.0;
    for (std::size_t r = 0; r < by_p.size(); ++r) {
        run = std::max(run, std::min(1.0, static_cast<double>(by_p.size() - r) * out[by_p[r]].p));
        out[by_p[r]].p_holm = run;
    }
    // Romano-Wolf step-down on the studentised means, from the same joint draws.
    std::vector<std::size_t> by_t = tested;
    std::sort(by_t.begin(), by_t.end(), [&](std::size_t a, std::size_t b) { return t[a] > t[b]; });
    run = 0.0;
    for (std::size_t r = 0; r < by_t.size(); ++r) {
        std::size_t hits = 0, valid = 0;
        for (std::size_t b = 0; b < draws; ++b) {
            double mx = -std::numeric_limits<double>::infinity();
            bool any = false;
            for (std::size_t q = r; q < by_t.size(); ++q) {
                const std::size_t s = by_t[q];
                if (!std::isfinite(boot[s][b])) continue;
                mx = std::max(mx, (boot[s][b] - out[s].mean) / out[s].se);
                any = true;
            }
            if (!any) continue;
            ++valid;
            hits += mx >= t[by_t[r]] ? 1u : 0u;
        }
        const double p = (1.0 + static_cast<double>(hits)) / (1.0 + static_cast<double>(valid));
        run = std::max(run, p);
        out[by_t[r]].p_rw = run;
    }
}

// ---- regimes ---------------------------------------------------------------------------

/// "low" / "mid" / "high" by the previous India VIX close, in terciles over
/// `days`; "unknown" where no previous close is on file.
[[nodiscard]] inline std::map<std::int64_t, std::string> vix_regimes(const std::set<std::int64_t>& days,
                                                                     const std::map<std::int64_t, double>& vix_close) {
    std::map<std::int64_t, double> prev;
    for (const std::int64_t d : days) {
        auto it = vix_close.lower_bound(d);   // first close on or after d
        if (it == vix_close.begin()) continue;
        --it;                                 // the last close before d
        if (d - it->first <= 7) prev[d] = it->second;
    }
    std::vector<double> v;
    for (const auto& [d, x] : prev) v.push_back(x);
    std::sort(v.begin(), v.end());
    const double lo = quantile_sorted(v, 1.0 / 3.0), hi = quantile_sorted(v, 2.0 / 3.0);
    std::map<std::int64_t, std::string> out;
    for (const std::int64_t d : days) {
        const auto it = prev.find(d);
        out[d] = it == prev.end() ? "unknown" : it->second <= lo ? "low" : it->second <= hi ? "mid" : "high";
    }
    return out;
}

} // namespace altair::live::report
