// app/fyers_chain.hpp -- end-of-day option chains from FYERS, ALTAIR's own record.
//
// Smit's rule: research runs on FYERS data, never NSE bhavcopy. FYERS history
// serves a live contract's bars from its first traded day but refuses every
// expired option and future (HTTP 422), so the only chain history ALTAIR will
// have is the one it saves before each contract expires. altair_fyers_chain
// (app/fyers_chain_main.cpp; ops/fyers_chain.ps1 runs it daily at 15:45 IST)
// saves it; this header picks the contracts, turns a day's one-minute bars
// into a close, and reads the saved days back.
//
// THE FILES (data/store/fyers_chain/, git-ignored, SOURCE.txt says FYERS):
//   <YYYY-MM-DD>/<UNDERLYING>.csv      ticker,kind,expiry,strike,close,last_bar,bars
//   <YYYY-MM-DD>/<UNDERLYING>_1m.csv   ticker,time,open,high,low,close,volume
// kind is FUT, CE, PE, SPOT or VIX; expiry YYYY-MM-DD (empty for SPOT/VIX);
// last_bar HH:MM IST.
//
// A CLOSE IS A TRADE. The close is the last one-minute bar between 09:15 and
// 15:29 IST. A contract whose last bar is before 15:20 has no close that day:
// a price from 13:00 is not where the market shut. A contract that did not
// trade has no row at all -- never a model price.
//
// FALLBACK. A day with no saved bars but a REST snapshot from
// ops/chain_snapshot.ps1 (data/store/chain_snap/<day>/1527.csv) is read from
// the snapshot's last traded prices, counted apart in the report: FYERS data,
// but a 15:27 LTP, not a bar close.
//
// THE SAME SHAPE AS app/bhavcopy.hpp. load_dir returns bhavcopy::Day, so every
// tool that read the bhavcopy reads FYERS chains unchanged; by default it keeps
// what the bhavcopy loader kept (monthly expiries, strikes within +-20 % of
// that expiry's future).

#pragma once

#include <app/bhavcopy.hpp>
#include <app/data_audit.hpp>
#include <instruments/fyers_master.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <istream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace altair::fyers_chain {

namespace fs = std::filesystem;

inline constexpr std::int64_t kIstOffsetS = 19'800;    ///< +05:30
inline constexpr int kFirstMinute = 9 * 60 + 15;        ///< 09:15 IST
inline constexpr int kLastMinute = 15 * 60 + 29;        ///< 15:29 IST, the last bar of the session
inline constexpr int kCloseFrom = 15 * 60 + 20;         ///< a last bar before 15:20 is no close

/// IST calendar day of an instant, as days since 1970-01-01.
[[nodiscard]] inline std::int64_t ist_day(std::int64_t ts_ns) noexcept {
    const std::int64_t s = ts_ns / 1'000'000'000LL + kIstOffsetS;
    return s >= 0 ? s / 86'400 : (s - 86'399) / 86'400;
}

/// IST minute of the day of an instant, 0..1439.
[[nodiscard]] inline int ist_minute(std::int64_t ts_ns) noexcept {
    const std::int64_t s = ts_ns / 1'000'000'000LL + kIstOffsetS;
    const std::int64_t r = ((s % 86'400) + 86'400) % 86'400;
    return static_cast<int>(r / 60);
}

[[nodiscard]] inline std::string day_text(std::int64_t day) {
    const auto c = data_audit::audit_civil_from_days(day);
    char b[32];
    std::snprintf(b, sizeof b, "%04lld-%02u-%02u", static_cast<long long>(c.y), c.m, c.d);
    return b;
}

/// One listed contract, or SPOT / VIX.
struct Contract {
    std::string ticker;        ///< the FYERS symbol, e.g. NSE:NIFTY25N0425000CE
    std::string underlying;    ///< NIFTY, BANKNIFTY
    std::string kind;          ///< FUT, CE, PE, SPOT, VIX
    std::int64_t expiry = 0;   ///< IST day; 0 for SPOT / VIX
    double strike = 0.0;       ///< rupees; 0 unless an option
    long lot = 0;
};

/// The F&O contracts of `underlyings` in FYERS's public NSE_FO master
/// (instruments/fyers_master.hpp verifies every column). `refused` counts rows
/// the parser would not accept.
[[nodiscard]] inline std::vector<Contract> read_fo_master(std::istream& in, const std::set<std::string>& underlyings,
                                                          std::size_t* refused = nullptr) {
    std::vector<Contract> out;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') { line.pop_back(); }
        if (line.empty()) { continue; }
        const auto r = instruments::parse_fyers_master_row(line);
        if (!r) { if (refused) { ++*refused; } continue; }
        if (!underlyings.contains(r->underlying)) { continue; }
        if (r->segment != altair::Segment::Fut && r->segment != altair::Segment::Opt) { continue; }
        Contract c;
        c.ticker = r->ticker;
        c.underlying = r->underlying;
        c.kind = r->opt_type == altair::OptionType::CE ? "CE" : r->opt_type == altair::OptionType::PE ? "PE" : "FUT";
        c.expiry = ist_day(r->expiry.ns_since_epoch());
        c.strike = static_cast<double>(r->strike.raw()) / 100.0;
        c.lot = static_cast<long>(r->lot_size.raw());
        out.push_back(std::move(c));
    }
    return out;
}

/// The monthly expiries among `expiries`: the last one of each calendar month.
[[nodiscard]] inline std::set<std::int64_t> monthly_of(const std::set<std::int64_t>& expiries) {
    std::map<std::pair<std::int64_t, unsigned>, std::int64_t> last;
    for (const auto e : expiries) {
        const auto c = data_audit::audit_civil_from_days(e);
        auto& slot = last[{c.y, c.m}];
        slot = std::max(slot, e);
    }
    std::set<std::int64_t> out;
    for (const auto& [ym, e] : last) { out.insert(e); }
    return out;
}

struct Pick {
    std::size_t nearest = 3;     ///< the nearest option expiries (weeklies included)
    std::size_t monthlies = 1;   ///< plus the nearest monthly expiries
    std::size_t strikes = 40;    ///< strikes each side of the money, per expiry
};

/// The contracts to save for one underlying on `today`, centred on `spot`:
/// every listed future, and calls and puts of the chosen expiries at the
/// 2 x strikes + 1 listed strikes nearest the spot.
[[nodiscard]] inline std::vector<Contract> pick(const std::vector<Contract>& listed, const std::string& underlying,
                                                double spot, std::int64_t today, const Pick& p) {
    std::set<std::int64_t> opt_expiries;
    for (const auto& c : listed) {
        if (c.underlying == underlying && c.kind != "FUT" && c.expiry >= today) { opt_expiries.insert(c.expiry); }
    }
    std::set<std::int64_t> chosen;
    for (const auto e : opt_expiries) { if (chosen.size() < p.nearest) { chosen.insert(e); } }
    std::size_t m = 0;
    for (const auto e : monthly_of(opt_expiries)) { if (m++ < p.monthlies) { chosen.insert(e); } }

    std::vector<Contract> out;
    std::map<std::int64_t, std::set<double>> strikes;
    for (const auto& c : listed) {
        if (c.underlying != underlying || c.expiry < today) { continue; }
        if (c.kind == "FUT") { out.push_back(c); continue; }
        if (chosen.contains(c.expiry)) { strikes[c.expiry].insert(c.strike); }
    }
    std::map<std::int64_t, std::set<double>> keep;
    for (const auto& [e, ks] : strikes) {
        std::vector<double> v(ks.begin(), ks.end());
        std::sort(v.begin(), v.end(), [&](double a, double b) {
            return std::fabs(a - spot) != std::fabs(b - spot) ? std::fabs(a - spot) < std::fabs(b - spot) : a < b;
        });
        v.resize(std::min(v.size(), 2 * p.strikes + 1));
        keep[e].insert(v.begin(), v.end());
    }
    for (const auto& c : listed) {
        if (c.underlying == underlying && c.kind != "FUT" && keep.contains(c.expiry) && keep[c.expiry].contains(c.strike)) {
            out.push_back(c);
        }
    }
    return out;
}

/// A bar as the tool keeps it.
struct Bar {
    std::int64_t ts_ns = 0;
    double open = 0.0, high = 0.0, low = 0.0, close = 0.0, volume = 0.0;
};

struct DayClose {
    double close = 0.0;
    int last_minute = 0;     ///< IST minute of the bar the close came from
    std::size_t bars = 0;    ///< session bars that day
};

/// The close of one contract's bars on one IST day: the last bar from 09:15 to
/// 15:29; none if there is no such bar or the last one is before 15:20.
[[nodiscard]] inline std::optional<DayClose> day_close(const std::vector<Bar>& bars) {
    DayClose d;
    bool any = false;
    for (const auto& b : bars) {
        const int m = ist_minute(b.ts_ns);
        if (m < kFirstMinute || m > kLastMinute || !(b.close > 0.0) || !std::isfinite(b.close)) { continue; }
        ++d.bars;
        if (!any || m >= d.last_minute) { d.close = b.close; d.last_minute = m; any = true; }
    }
    if (!any || d.last_minute < kCloseFrom) { return std::nullopt; }
    return d;
}

[[nodiscard]] inline std::string closes_header() { return "ticker,kind,expiry,strike,close,last_bar,bars\n"; }

[[nodiscard]] inline std::string close_row(const Contract& c, const DayClose& d) {
    char b[256];
    std::snprintf(b, sizeof b, "%s,%s,%s,%.2f,%.2f,%02d:%02d,%zu\n", c.ticker.c_str(), c.kind.c_str(),
                  c.expiry > 0 ? day_text(c.expiry).c_str() : "", c.strike, d.close, d.last_minute / 60, d.last_minute % 60,
                  d.bars);
    return b;
}

struct LoadReport {
    std::size_t days = 0;            ///< days read from saved bars
    std::size_t snapshot_days = 0;   ///< days read from a 15:27 REST snapshot instead
    std::size_t option_rows = 0, future_rows = 0, skipped = 0;
};

namespace detail {

struct Row {
    std::string kind;
    std::int64_t expiry = 0;
    double strike = 0.0, close = 0.0;
};

[[nodiscard]] inline std::optional<std::int64_t> day_of_folder(const fs::path& p) {
    return bhavcopy::detail::date(p.filename().string());
}

/// <UNDERLYING>.csv rows (the tool's own format).
inline void read_closes(const fs::path& file, std::vector<Row>& rows, LoadReport& rep) {
    std::ifstream in(file);
    std::string line;
    if (!in || !std::getline(in, line)) { return; }
    const auto head = bhavcopy::detail::split(line);
    const auto col = [&](std::string_view n) {
        for (std::size_t k = 0; k < head.size(); ++k) { if (head[k] == n) { return static_cast<int>(k); } }
        return -1;
    };
    const int c_kind = col("kind"), c_exp = col("expiry"), c_k = col("strike"), c_close = col("close");
    if (c_kind < 0 || c_exp < 0 || c_k < 0 || c_close < 0) { ++rep.skipped; return; }
    while (std::getline(in, line)) {
        const auto f = bhavcopy::detail::split(line);
        const auto at = [&](int c) { return c < static_cast<int>(f.size()) ? f[static_cast<std::size_t>(c)] : std::string{}; };
        Row r;
        r.kind = at(c_kind);
        if (r.kind != "FUT" && r.kind != "CE" && r.kind != "PE") { continue; }
        const auto e = bhavcopy::detail::date(at(c_exp));
        const auto k = bhavcopy::detail::number(at(c_k));
        const auto px = bhavcopy::detail::number(at(c_close));
        if (!e || !k || !px || !(*px > 0.0)) { ++rep.skipped; continue; }
        r.expiry = *e;
        r.strike = *k;
        r.close = *px;
        rows.push_back(r);
    }
}

/// ops/chain_snapshot.ps1's 1527.csv: snap_time,fyers,underlying,kind,expiry,strike,ltp,...
inline void read_snapshot(const fs::path& file, const std::string& symbol, std::vector<Row>& rows, LoadReport& rep) {
    std::ifstream in(file);
    std::string line;
    if (!in || !std::getline(in, line)) { return; }
    const auto head = bhavcopy::detail::split(line);
    const auto col = [&](std::string_view n) {
        for (std::size_t k = 0; k < head.size(); ++k) { if (head[k] == n) { return static_cast<int>(k); } }
        return -1;
    };
    const int c_u = col("underlying"), c_kind = col("kind"), c_exp = col("expiry"), c_k = col("strike"), c_ltp = col("ltp");
    if (c_u < 0 || c_kind < 0 || c_exp < 0 || c_k < 0 || c_ltp < 0) { ++rep.skipped; return; }
    while (std::getline(in, line)) {
        const auto f = bhavcopy::detail::split(line);
        const auto at = [&](int c) { return c < static_cast<int>(f.size()) ? f[static_cast<std::size_t>(c)] : std::string{}; };
        if (at(c_u) != symbol) { continue; }
        const std::string kind = at(c_kind);
        Row r;
        r.kind = kind == "future" ? "FUT" : kind == "call" ? "CE" : kind == "put" ? "PE" : "";
        if (r.kind.empty()) { continue; }
        const auto e = bhavcopy::detail::date(at(c_exp));
        const auto px = bhavcopy::detail::number(at(c_ltp));
        const auto k = r.kind == "FUT" ? std::optional<double>{0.0} : bhavcopy::detail::number(at(c_k));
        if (!e || !k || !px || !(*px > 0.0)) { ++rep.skipped; continue; }
        r.expiry = *e;
        r.strike = *k;
        r.close = *px;
        rows.push_back(r);
    }
}

} // namespace detail

/// Every saved day under `dir` (data/store/fyers_chain) for `symbol`, plus the
/// days only `snap_dir` (data/store/chain_snap) has. `monthly_only` keeps what
/// app/bhavcopy.hpp kept; false keeps every expiry (the NIFTY weeklies).
[[nodiscard]] inline std::map<std::int64_t, bhavcopy::Day> load_dir(const fs::path& dir, const std::string& symbol,
                                                                     LoadReport& rep, bool monthly_only = true,
                                                                     const fs::path& snap_dir = {}) {
    std::map<std::int64_t, std::vector<detail::Row>> raw;
    std::error_code ec;
    if (fs::is_directory(dir, ec)) {
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            const auto day = detail::day_of_folder(e.path());
            if (!day || !e.is_directory()) { continue; }
            const fs::path f = e.path() / (symbol + ".csv");
            if (!fs::exists(f)) { continue; }
            detail::read_closes(f, raw[*day], rep);
            ++rep.days;
        }
    }
    if (!snap_dir.empty() && fs::is_directory(snap_dir, ec)) {
        for (const auto& e : fs::directory_iterator(snap_dir, ec)) {
            const auto day = detail::day_of_folder(e.path());
            if (!day || !e.is_directory() || raw.contains(*day)) { continue; }
            const fs::path f = e.path() / "1527.csv";
            if (!fs::exists(f)) { continue; }
            detail::read_snapshot(f, symbol, raw[*day], rep);
            ++rep.snapshot_days;
        }
    }
    std::map<std::int64_t, bhavcopy::Day> days;
    for (const auto& [day, rows] : raw) {
        std::set<std::int64_t> listed;
        for (const auto& r : rows) { listed.insert(r.expiry); }
        const auto monthly = monthly_of(listed);
        const auto keep = [&](std::int64_t e) { return !monthly_only || monthly.contains(e); };
        bhavcopy::Day d;
        for (const auto& r : rows) {
            if (r.kind == "FUT" && keep(r.expiry)) { d.fut[r.expiry] = r.close; ++rep.future_rows; }
        }
        for (const auto& r : rows) {
            if (r.kind == "FUT" || !keep(r.expiry)) { continue; }
            const auto fut = d.fut.find(r.expiry);
            if (monthly_only && (fut == d.fut.end() || std::fabs(r.strike / fut->second - 1.0) > 0.20)) { continue; }
            auto& leg = d.opt[r.expiry][r.strike];
            (r.kind == "CE" ? leg.call : leg.put) = r.close;
            ++rep.option_rows;
        }
        if (!d.fut.empty() || !d.opt.empty()) { days[day] = std::move(d); }
    }
    return days;
}

/// A tool's chain days for `symbol`: FYERS (the saved chains under
/// `chain_dir`, the 15:27 snapshots beside it in chain_snap/) unless the
/// caller opted in to the bhavcopy. `note` says which and how much, for the
/// tool to print.
[[nodiscard]] inline std::map<std::int64_t, bhavcopy::Day> load_source(bool use_bhavcopy, const fs::path& chain_dir,
                                                                        const fs::path& bhav_dir, const std::string& symbol,
                                                                        std::string& note) {
    char b[256];
    if (use_bhavcopy) {
        bhavcopy::LoadReport br;
        auto days = bhavcopy::load_dir(bhav_dir, symbol, br);
        std::snprintf(b, sizeof b, "%s bhavcopy (opt-in, not FYERS data): %zu file(s) under %s", symbol.c_str(), br.files,
                      bhav_dir.string().c_str());
        note = b;
        return days;
    }
    LoadReport fr;
    auto days = load_dir(chain_dir, symbol, fr, true, chain_dir.parent_path() / "chain_snap");
    std::snprintf(b, sizeof b, "%s FYERS chains: %zu saved day(s), %zu from 15:27 snapshots, under %s%s", symbol.c_str(), fr.days,
                  fr.snapshot_days, chain_dir.string().c_str(), days.empty() ? " -- altair_fyers_chain --go saves them daily" : "");
    note = b;
    return days;
}

} // namespace altair::fyers_chain
