// app/live_engine_main.cpp -- altair_live_engine: the models, live, paper-trading.
//
//     altair_live_engine [--port 7421] [--until HH:MM] [--seconds N] [--root DIR] [--unverified-costs]
//                        [--latency-ms 250] [--quote-age-s 10] [--entry-timeout-s 60]
//                        [--max-positions 80] [--max-gross 1e8] [--max-daily-loss 2e5]
//
// Subscribes to altair_price_service (FYERS live, or --sim), builds bars from
// the ticks, runs every live model on them and paper-trades their signals:
//   Vol band (HAR)                 the range, recomputed every minute; trades nothing
//   Strangle 80% NIFTY/BANKNIFTY   sell the band's edges at 09:20 (and a stop2x variant)
//   Direction 10:15 <model>        ARMA, logistic, ridge, GBDT and their vote, behind the
//                                  magnitude gate: (2q-1)E|r| must beat the cost
//   Pairs BANKNIFTY/NIFTY          the 250-day spread at 15:15, carried
//   Stat-arb NIFTY 50              Avellaneda-Lee s-scores at 15:15, carried
//
// WHAT IT WRITES (data/live/, git-ignored):
//   engine_state.json          every second: each model's state, signal and reason,
//                              and the open positions -- the desktop's Live Models page
//   paper/journal.csv          THE RECORD: every fill, appended and flushed; a restart
//                              rebuilds the book from it
//   paper/trades.csv           every round trip: model, instrument, side, quantity,
//                              entry and exit, gross, expenses, net, why in, why out
//   paper/fills.csv            every fill, at the bid or ask it dealt at
//   paper/open_positions.csv   what is held, for people (replaced in one step)
//   paper/engine.lock          held while running: one engine per ledger
//
// ORDERS FILL LIKE ORDERS (live/paper.hpp): after --latency-ms, against a quote
// no older than --quote-age-s, at the touch, for the size shown; an entry not
// filled in --entry-timeout-s is cancelled (and its other legs unwound); an
// exit works until it fills. Every entry passes one risk check (live/engine.hpp):
// the limits above, a stale feed, a trade gap, a ledger that cannot be written,
// and data/kill_request.json (the desktop's Kill Switch) all refuse new entries.
//
// A WRITE IS DONE WHEN THE FILE SAYS SO. Rows that fail to append are kept and
// retried, and until they land the engine is halted: no new entries.
//
// EXPENSES come from config/charges.toml through risk/cost.hpp (app/demo_costs.hpp),
// on every fill -- refused while that file is unverified, like every research
// demo, unless --unverified-costs (the desktop's "Price UNVERIFIED expenses")
// says to price them anyway; every row and the page then say UNVERIFIED.
//
// IT CANNOT TRADE. It reads a loopback socket and writes files. It links no
// broker and no OMS, and there is no order anywhere in it.

#include <app/data_audit.hpp>
#include <app/demo_costs.hpp>
#include <app/forecast_tracks.hpp>
#include <app/live_direction.hpp>
#include <analytics/har_rv.hpp>
#include <app/live_feed_reader.hpp>
#include <live/engine.hpp>
#include <live/feed_consumer.hpp>
#include <live/file_lock.hpp>
#include <live/models.hpp>
#include <live/universe.hpp>
#include <risk/charges_toml.hpp>
#include <server/price_payload.hpp>
#include <server/protocol.hpp>
#include <server/quote_payload.hpp>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace ft = altair::forecast_tracks;
namespace da = altair::data_audit;
namespace lv = altair::live;

std::atomic<bool> g_stop{false};
extern "C" void on_stop(int) { g_stop.store(true); }

[[nodiscard]] std::int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

[[nodiscard]] int parse_hhmm(const std::string& s) {
    if (s.size() != 5 || s[2] != ':') return -1;
    const int h = std::atoi(s.substr(0, 2).c_str()), m = std::atoi(s.substr(3, 2).c_str());
    return h >= 0 && h < 24 && m >= 0 && m < 60 ? h * 60 + m : -1;
}

[[nodiscard]] std::string ist_stamp(std::int64_t ns) {
    if (ns <= 0) return "";
    const std::int64_t s = ns / 1'000'000'000LL + 19800;
    const std::int64_t day = s / 86400, sec = s % 86400;
    char b[48];
    std::snprintf(b, sizeof b, "%s %02lld:%02lld:%02lld", lv::day_text(day).c_str(), static_cast<long long>(sec / 3600),
                  static_cast<long long>(sec / 60 % 60), static_cast<long long>(sec % 60));
    return b;
}

/// 5-minute bars of a dataset partition, cleaned, before `today` (IST days).
[[nodiscard]] std::vector<da::AuditBar> history_5m(const fs::path& dir, std::int64_t today) {
    ft::TrackInfo info;
    auto bars = ft::load_bars(dir, 5, info);
    ft::clean_bars(bars, info);
    bars.erase(std::remove_if(bars.begin(), bars.end(), [today](const da::AuditBar& b) { return da::audit_day(b.t) >= today; }),
               bars.end());
    return bars;
}

/// Per session day: realised variance (intraday + gap), and the gap's share.
struct DayRv { std::int64_t day = 0; double rv = 0.0, gap2 = 0.0, close = 0.0; };

[[nodiscard]] std::vector<DayRv> daily_rv(const std::vector<da::AuditBar>& bars) {
    std::vector<DayRv> out;
    std::size_t b = 0;
    double prev = 0.0;
    while (b < bars.size()) {
        const std::int64_t d = da::audit_day(bars[b].t);
        std::size_t e = b;
        std::vector<double> closes;
        while (e < bars.size() && da::audit_day(bars[e].t) == d) { closes.push_back(bars[e].c); ++e; }
        if (closes.size() >= 60) {   // a near-full session
            const double rv = altair::session_rv(closes, bars[b].o, prev);
            const double g = prev > 0.0 ? std::log(bars[b].o / prev) : 0.0;
            if (rv > 0.0) out.push_back({d, rv, g * g, closes.back()});
        }
        prev = closes.back();
        b = e;
    }
    return out;
}

/// HAR's one-day-ahead sigma, the session's share of daily variance, and the
/// last close, from 5-minute history.
[[nodiscard]] lv::LiveVolInputs vol_inputs(const std::string& under, const std::vector<DayRv>& rv, std::string& note,
                                           double* median_sigma = nullptr) {
    lv::LiveVolInputs v;
    v.under = under;
    if (rv.size() < 60) { note += under + ": too little 5-minute history for HAR. "; return v; }
    std::vector<double> r;
    for (const auto& x : rv) r.push_back(x.rv);
    const auto f = altair::har_forecast(r, r.size() - 1, 1);
    if (!f) { note += under + ": HAR refused (" + std::string(altair::har_error_text(f.error())) + "). "; return v; }
    v.sigma_day = std::sqrt(f->variance);
    double intra = 0.0, total = 0.0;
    std::vector<double> sig;
    for (std::size_t i = rv.size() > 250 ? rv.size() - 250 : 0; i < rv.size(); ++i) {
        intra += rv[i].rv - rv[i].gap2;
        total += rv[i].rv;
        sig.push_back(std::sqrt(rv[i].rv));
    }
    v.intraday_share = total > 0.0 ? intra / total : 0.75;
    v.prev_close = rv.back().close;
    if (median_sigma != nullptr && !sig.empty()) {
        std::nth_element(sig.begin(), sig.begin() + static_cast<std::ptrdiff_t>(sig.size() / 2), sig.end());
        *median_sigma = sig[sig.size() / 2];
    }
    return v;
}

/// Daily closes by IST day from a CSV partition (time,open,high,low,close,...).
[[nodiscard]] std::map<std::int64_t, double> daily_closes(const fs::path& dir, std::int64_t before) {
    std::map<std::int64_t, double> m;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.path().extension() != ".csv") continue;
        std::ifstream in(e.path());
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] < '0' || line[0] > '9') continue;
            const auto c = lv::split_csv(line);
            if (c.size() < 5) continue;
            const std::int64_t d = lv::parse_day(c[0]);
            const double close = std::atof(c[4].c_str());
            if (d > 0 && d < before && close > 0.0) m[d] = close;
        }
    }
    return m;
}

[[nodiscard]] lv::LiveStatArbInputs statarb_inputs(const fs::path& root, const std::map<std::int64_t, double>& market,
                                                   std::int64_t today) {
    lv::LiveStatArbInputs in;
    const auto stocks = lv::read_stock_universe((root / "config/universe_nifty50.csv").string());
    if (stocks.empty() || market.size() < 100) { in.why = "no stock list or too little NIFTY daily history"; return in; }
    std::vector<std::int64_t> days;
    for (auto it = market.rbegin(); it != market.rend() && days.size() < 121; ++it) days.push_back(it->first);
    std::reverse(days.begin(), days.end());
    std::map<std::string, int> sector_id;
    std::size_t with_data = 0;
    for (const auto& s : stocks) {
        std::string dir = s.symbol;
        for (auto& ch : dir) ch = static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
        const auto closes = daily_closes(root / "data/pairs" / dir / "1d", today);
        if (closes.size() < 70) continue;
        std::vector<double> ret(days.size() - 1, std::numeric_limits<double>::quiet_NaN());
        for (std::size_t k = 1; k < days.size(); ++k) {
            const auto a = closes.find(days[k - 1]), b = closes.find(days[k]);
            if (a != closes.end() && b != closes.end()) ret[k - 1] = std::log(b->second / a->second);
        }
        const auto sid = sector_id.emplace(s.sector, static_cast<int>(sector_id.size())).first->second;
        in.history.ret.push_back(std::move(ret));
        in.history.sector.push_back(sid);
        in.symbols.push_back(s.symbol);
        in.last_close.push_back(closes.rbegin()->second);
        ++with_data;
    }
    if (with_data < 20) {
        in.why = std::to_string(with_data) + " of " + std::to_string(stocks.size())
               + " stocks have daily closes in data/pairs/ (run ops/fetch_universe.ps1 -Go)";
        return in;
    }
    for (std::size_t k = 1; k < days.size(); ++k) {
        in.history.day.push_back(days[k]);
        in.history.market.push_back(std::log(market.at(days[k]) / market.at(days[k - 1])));
    }
    in.market_last_close = market.rbegin()->second;
    in.ok = true;
    return in;
}

/// Rows waiting to reach one CSV: appended, flushed and checked; on failure
/// they stay queued for the next attempt.
struct PendingCsv {
    fs::path path;
    const char* header = "";
    std::vector<std::string> rows;
    bool flush() {
        if (rows.empty()) return true;
        if (!lv::live_append_rows(path.string(), header, rows)) return false;
        rows.clear();
        return true;
    }
};

bool write_atomic(const fs::path& path, const std::string& body) {
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << body;
        f.flush();
        if (!f) return false;
    }
    return lv::live_replace_file(tmp.string(), path.string());
}

void usage(const char* exe) {
    std::printf(
        "  The live models, paper-trading on altair_price_service's stream.\n\n"
        "    %s [--port 7421] [--until HH:MM] [--seconds N] [--root DIR] [--unverified-costs] [--date YYYY-MM-DD]\n\n"
        "    --port     the price service's loopback port (default 7421)\n"
        "    --until    stop once the feed's own clock reaches this IST time (default 15:35)\n"
        "    --seconds  stop after N seconds (tests)\n"
        "    --root     the tree holding dataset/, config/ and data/ (default: the source tree)\n"
        "    --unverified-costs  price expenses from config/charges.toml although it is UNVERIFIED;\n"
        "               every expense and net figure is then marked UNVERIFIED\n"
        "    --date     the day being traded, for a simulated past day (--sim --date): history\n"
        "               stops the day before it\n"
        "    --latency-ms N       decision to order at the market (default 250)\n"
        "    --quote-age-s N      a quote older than this is not executable (default 10)\n"
        "    --entry-timeout-s N  an entry not filled by then is cancelled (default 60)\n"
        "    --max-positions N    held or working, all models (default 80)\n"
        "    --max-gross RUPEES   gross notional of everything held or working (default 1e8)\n"
        "    --max-daily-loss RUPEES  no new entries past this loss today (default 2e5)\n\n"
        "  Start the feed first: altair_price_service --live --go (FYERS, else Kite) or --sim.\n"
        "  Writes data/live/engine_state.json and data/live/paper/*.csv. Places no orders.\n"
        "  New entries stop while data/kill_request.json exists (the desktop's Kill Switch).\n",
        exe);
}

}  // namespace

int main(int argc, char** argv) {
    unsigned short port = 7421;
    int until = 15 * 60 + 35, seconds = 0;
    bool unverified_costs = false;
    std::string date;   // a simulated past day (altair_price_service --sim --date)
    lv::LiveExecPolicy policy;
    lv::LiveRiskLimits limits;
    fs::path root = ALTAIR_SOURCE_DIR;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has = i + 1 < argc;
        if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
        if (a == "--port" && has) { port = static_cast<unsigned short>(std::atoi(argv[++i])); continue; }
        if (a == "--until" && has) { until = parse_hhmm(argv[++i]); continue; }
        if (a == "--seconds" && has) { seconds = std::atoi(argv[++i]); continue; }
        if (a == "--root" && has) { root = argv[++i]; continue; }
        if (a == "--unverified-costs") { unverified_costs = true; continue; }
        if (a == "--date" && has) { date = argv[++i]; continue; }
        if (a == "--latency-ms" && has) { policy.latency_ns = std::atoll(argv[++i]) * 1'000'000LL; continue; }
        if (a == "--quote-age-s" && has) { policy.max_quote_age_ns = static_cast<std::int64_t>(std::atof(argv[++i]) * 1e9); continue; }
        if (a == "--entry-timeout-s" && has) { policy.entry_timeout_ns = static_cast<std::int64_t>(std::atof(argv[++i]) * 1e9); continue; }
        if (a == "--max-positions" && has) { limits.max_positions = static_cast<std::size_t>(std::atoll(argv[++i])); continue; }
        if (a == "--max-gross" && has) { limits.max_gross_notional = std::atof(argv[++i]); continue; }
        if (a == "--max-daily-loss" && has) { limits.max_daily_loss = std::atof(argv[++i]); continue; }
        std::printf("unknown argument %s\n", a.c_str());
        usage(argv[0]);
        return 2;
    }
    if (until < 0) { std::printf("--until must be HH:MM\n"); return 2; }
    if (policy.latency_ns < 0 || policy.max_quote_age_ns <= 0 || policy.entry_timeout_ns <= 0 || limits.max_positions == 0
        || !(limits.max_gross_notional > 0.0) || !(limits.max_daily_loss > 0.0)) {
        std::printf("--latency-ms must be >= 0; the ages, timeout and limits must be > 0\n");
        return 2;
    }
    if (!date.empty() && lv::parse_day(date) == 0) { std::printf("--date must be YYYY-MM-DD\n"); return 2; }
    // History stops the day before the session being traded: today, or the
    // simulated day.
    const std::int64_t today = date.empty() ? lv::ist_today(unix_now()) : lv::parse_day(date);
    const fs::path live_dir = root / "data/live", paper_dir = live_dir / "paper";
    std::error_code ec;
    fs::create_directories(paper_dir, ec);
    // One engine per ledger: a second one would interleave the journal.
    const lv::LiveFileLock lock((paper_dir / "engine.lock").string());
    if (!lock.held()) {
        std::printf("refused: %s -- is another altair_live_engine running on this tree?\n", lock.why().c_str());
        return 3;
    }

    // ---- the universe the feed streams -----------------------------------
    const auto universe = lv::read_universe((live_dir / "universe.csv").string());
    if (universe.empty()) {
        std::printf("no %s: start altair_price_service (--fyers --go, or --sim) first; it writes it.\n",
                    (live_dir / "universe.csv").string().c_str());
        return 2;
    }

    // ---- expenses ----------------------------------------------------------
    // Refused while config/charges.toml is unverified (risk/cost.hpp's
    // block_on_unverified_schedule), exactly as the research demos do, unless
    // --unverified-costs says to price them anyway -- then every figure says so.
    std::vector<altair::ChargeSchedule> schedules;
    std::string costs_label = "UNPRICED";
    std::string cost_note;
    if (const auto rep = altair::load_charges_file((root / "config/charges.toml").string().c_str(), schedules); !rep) {
        cost_note = std::string("Expenses unpriced: config/charges.toml did not load (") + altair::charges_error_text(rep.error()) + ").";
        schedules.clear();
    } else if (rep->verified) {
        costs_label = "VERIFIED";
        cost_note = "Expenses from config/charges.toml (verified).";
    } else if (unverified_costs) {
        for (auto& sch : schedules) sch.verified = true;   // --unverified-costs: the caller's explicit choice
        costs_label = "UNVERIFIED";
        cost_note = "Expenses priced from an UNVERIFIED config/charges.toml (--unverified-costs): every expense and net is UNVERIFIED.";
    } else {
        schedules.clear();
        cost_note = "Expenses REFUSED: config/charges.toml is UNVERIFIED. Gross P&L only -- verify the rates and set last_verified, "
                    "or start with --unverified-costs.";
    }
    std::printf("%s\n", cost_note.c_str());
    const lv::LiveCostFn cost = [&schedules](const lv::LiveInstrument& in, bool buy, double qty, double px, std::int64_t ns) {
        const altair::Segment seg = in.kind == lv::LiveKind::Future ? altair::Segment::Fut
                                  : (in.kind == lv::LiveKind::Equity ? altair::Segment::Cash : altair::Segment::Opt);
        const auto c = altair::demo_costs::fill(seg, buy ? altair::Side::Buy : altair::Side::Sell, qty, px,
                                                ns / 1'000'000'000LL + 19800, schedules);
        return c.priced ? c.total : std::numeric_limits<double>::quiet_NaN();
    };

    // ---- history inputs ----------------------------------------------------
    std::string note;
    std::printf("history through %s\n", lv::day_text(today - 1).c_str());
    const auto nifty5 = history_5m(root / "dataset/spot/nifty/5m", today);
    const auto bnf5 = history_5m(root / "dataset/spot/banknifty/5m", today);
    const auto vix5 = history_5m(root / "dataset/spot/indiavix/5m", today);
    double median_sigma = 0.0;
    const auto nrv = daily_rv(nifty5), brv = daily_rv(bnf5);
    const auto nv = vol_inputs("NIFTY", nrv, note, &median_sigma);
    const auto bv = vol_inputs("BANKNIFTY", brv, note);
    std::printf("  HAR sigma today: NIFTY %.3f %%, BANKNIFTY %.3f %%; session share %.2f / %.2f\n", 100 * nv.sigma_day,
                100 * bv.sigma_day, nv.intraday_share, bv.intraday_share);

    const auto nd = daily_closes(root / "dataset/spot/nifty/1d", today);
    const auto bd = daily_closes(root / "dataset/spot/banknifty/1d", today);
    std::vector<double> a, b, lr;
    for (const auto& [d, c] : nd) {
        const auto it = bd.find(d);
        if (it == bd.end()) continue;
        a.push_back(c);
        b.push_back(it->second);
        lr.push_back(std::log(it->second / c));
    }
    double rmean = 0.0, rsd = 0.0;
    if (lr.size() >= 60) {
        for (std::size_t i = lr.size() - 60; i < lr.size(); ++i) rmean += lr[i];
        rmean /= 60.0;
        for (std::size_t i = lr.size() - 60; i < lr.size(); ++i) rsd += (lr[i] - rmean) * (lr[i] - rmean);
        rsd = std::sqrt(rsd / 59.0);
    }
    const auto pf = lv::live_pairs_formation(a, b, 250);
    std::printf("  pairs formation: %s beta %.3f sd %.4f half-life %.1f d\n", pf.ok ? "ok" : pf.why.c_str(), pf.beta, pf.sd,
                pf.half_life);
    auto sa = statarb_inputs(root, nd, today);
    std::printf("  stat-arb: %s\n", sa.ok ? (std::to_string(sa.symbols.size()) + " stocks").c_str() : sa.why.c_str());

    auto dir = std::make_shared<altair::live_direction::DirectionShared>();
    {
        // The last three years: what runs live, and a walk-forward that finishes in seconds.
        const std::int64_t from = today - 3 * 365;
        for (const auto& x : nifty5) if (da::audit_day(x.t) >= from) dir->nifty5.push_back(x);
        for (const auto& x : vix5) if (da::audit_day(x.t) >= from) dir->vix5.push_back(x);
        dir->vol_ratio = median_sigma > 0.0 && nv.sigma_day > 0.0 ? nv.sigma_day / median_sigma : 1.0;
        const auto t0 = std::chrono::steady_clock::now();
        altair::live_direction::calibrate(*dir);
        std::printf("  direction: %s (%.1f s)\n", dir->ok ? (std::to_string(dir->history_days) + " days walked forward").c_str() : dir->why.c_str(),
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    }

    // ---- the engine --------------------------------------------------------
    lv::LiveEngine engine(universe, cost, policy);
    engine.set_limits(limits);
    engine.add_model(std::make_unique<lv::LiveVolBandModel>(std::vector<lv::LiveVolInputs>{nv, bv}, rmean, rsd));
    engine.add_model(std::make_unique<lv::LiveStrangleModel>(nv, 0.0));
    engine.add_model(std::make_unique<lv::LiveStrangleModel>(nv, 2.0));
    engine.add_model(std::make_unique<lv::LiveStrangleModel>(bv, 0.0));
    engine.add_model(std::make_unique<lv::LiveStrangleModel>(bv, 2.0));
    for (std::size_t i = 0; i <= dir->names.size(); ++i)
        engine.add_model(std::make_unique<altair::live_direction::LiveDirectionModel>(dir, i));
    if (dir->names.empty()) engine.add_model(std::make_unique<altair::live_direction::LiveDirectionModel>(dir, 0));
    engine.add_model(std::make_unique<lv::LivePairsModel>(pf));
    engine.add_model(std::make_unique<lv::LiveStatArbModel>(std::move(sa)));

    // Carried positions from the last run, rebuilt from the journal (the
    // record); a tree from before the journal falls back to the snapshot. An
    // intraday one from an earlier day was left open by an engine that stopped
    // before 15:20: reported, not resumed.
    std::vector<std::string> orphans;
    const fs::path journal = paper_dir / "journal.csv";
    std::size_t journal_rows = 0;
    const bool have_journal = fs::exists(journal);
    const auto resumed = have_journal ? lv::live_replay_journal(journal.string(), universe, orphans, &journal_rows)
                                      : lv::live_read_positions((paper_dir / "open_positions.csv").string(), universe, orphans);
    std::printf("resuming from %s: %zu position(s)%s\n", have_journal ? "paper/journal.csv" : "paper/open_positions.csv",
                resumed.size(), have_journal ? (" (" + std::to_string(journal_rows) + " fills)").c_str() : "");
    for (const auto& p : resumed) {
        const std::int64_t d = lv::live_day_of(lv::live_ist_minute_index(p.entry_ns));
        if (!p.carry && d != today) { orphans.push_back(p.model + " " + p.inst.symbol + " (intraday, " + lv::day_text(d) + ")"); continue; }
        engine.book().restore(p);
    }
    for (const auto& o : orphans) note += "Not resumed: " + o + ". ";
    note += cost_note;

    // ---- the stream ----------------------------------------------------------
    std::signal(SIGINT, on_stop);
    std::signal(SIGTERM, on_stop);
    // STOPS ON THE FEED'S CLOCK, like everything else here: when the trades it
    // reads are stamped past --until, not when the wall clock is. A simulated
    // session run in the evening still runs its whole day.
    const std::int64_t deadline = seconds > 0 ? unix_now() + seconds : std::numeric_limits<std::int64_t>::max();
    const auto past_until = [&engine, until] {
        const std::int64_t c = engine.clock_ns();
        return c > 0 && c >= (engine.today() * 86400 + static_cast<std::int64_t>(until) * 60 - 19800) * 1'000'000'000LL;
    };
    // The socket is drained on its own thread (app/live_feed_reader.hpp): a
    // slow minute here never makes the bus drop frames for this reader.
    altair::live_feed::FeedReader reader(port);
    bool connected = false;
    std::vector<std::uint8_t> buf;
    buf.reserve(1 << 20);
    auto last_frame = std::chrono::steady_clock::now();
    auto last_write = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    std::uint64_t frames = 0;
    lv::LiveFeedConsumer consumer(engine);
    const char* kTradesHeader = "date,model,symbol,token,side,qty,entry_time,entry,exit_time,exit,gross,expenses,net,why_in,why_out,source,costs";
    const char* kFillsHeader = "time,model,symbol,token,side,qty,price,expenses,at_quote,reason,source,costs";
    std::printf("engine on 127.0.0.1:%u until %s IST -- %zu models, %zu instruments\n", static_cast<unsigned>(port),
                lv::live_fmt::hhmm(until).c_str(), engine.models().size(), universe.size());
    std::fflush(stdout);

    PendingCsv journal_out{journal, lv::kLiveJournalHeader, {}};
    PendingCsv trades_out{paper_dir / "trades.csv", kTradesHeader, {}};
    PendingCsv fills_out{paper_dir / "fills.csv", kFillsHeader, {}};
    if (!have_journal) {
        // The first run with a journal: the snapshot's positions become its
        // opening rows, so the next restart has them on the record.
        for (const auto& p : engine.book().held()) {
            lv::LivePaperFill f;
            f.model = p.model; f.token = p.inst.token; f.symbol = p.inst.symbol; f.side = p.side; f.qty = p.qty;
            f.price = p.entry; f.expenses = p.entry_expenses; f.ns = p.entry_ns; f.submit_ns = p.decided_ns;
            f.reason = p.why_in; f.role = lv::LiveFillRole::Open; f.carry = p.carry;
            journal_out.rows.push_back(lv::live_journal_row(f));
        }
    }
    std::deque<std::string> events;   // the last few cancellations, for the page
    const fs::path kill_file = root / "data/kill_request.json";
    bool positions_dirty = true;

    const auto flush_outputs = [&] {
        const std::string src = engine.simulated() ? "SIM" : "LIVE";
        for (const auto& f : engine.take_new_fills()) {
            journal_out.rows.push_back(lv::live_journal_row(f));
            fills_out.rows.push_back(ist_stamp(f.ns) + ",\"" + f.model + "\"," + f.symbol + "," + std::to_string(f.token) + ","
                           + (f.side > 0 ? "buy" : "sell") + "," + std::to_string(f.qty) + "," + lv::live_fmt::num(f.price) + ","
                           + (std::isfinite(f.expenses) ? lv::live_fmt::num(f.expenses) : "") + "," + (f.at_quote ? "1" : "0")
                           + ",\"" + f.reason + "\"," + src + "," + costs_label);
        }
        auto& rows = trades_out.rows;
        for (const auto& t : engine.take_new_trades()) {
            const auto q = [](const std::string& s) { return "\"" + s + "\""; };
            rows.push_back(lv::day_text(lv::live_day_of(lv::live_ist_minute_index(t.exit_ns))) + "," + q(t.model) + "," + t.symbol
                           + "," + std::to_string(t.token) + "," + (t.side > 0 ? "long" : "short") + "," + std::to_string(t.qty)
                           + "," + ist_stamp(t.entry_ns) + "," + lv::live_fmt::num(t.entry) + "," + ist_stamp(t.exit_ns) + ","
                           + lv::live_fmt::num(t.exit) + "," + lv::live_fmt::num(t.gross) + ","
                           + (std::isfinite(t.expenses) ? lv::live_fmt::num(t.expenses) : "") + ","
                           + (std::isfinite(t.net) ? lv::live_fmt::num(t.net) : "") + "," + q(t.why_in) + "," + q(t.why_out)
                           + "," + src + "," + costs_label);
        }
        for (auto& c : engine.book().take_cancelled()) {
            std::printf("  %s\n", c.c_str());
            events.push_back(std::move(c));
            if (events.size() > 5) events.pop_front();
        }
        // The journal first: it is the record. Then the views of it.
        std::string failed;
        if (!journal_out.flush()) failed = journal_out.path.string();
        if (!trades_out.flush() && failed.empty()) failed = trades_out.path.string();
        if (!fills_out.flush() && failed.empty()) failed = fills_out.path.string();
        positions_dirty = engine.take_positions_changed() || positions_dirty;
        if (positions_dirty) {
            if (lv::live_write_positions((paper_dir / "open_positions.csv").string(), engine.book().held())) positions_dirty = false;
            else if (failed.empty()) failed = (paper_dir / "open_positions.csv").string();
        }
        if (!failed.empty()) {
            if (engine.halt().empty()) std::printf("HALTED: cannot write %s; rows kept, retrying every second\n", failed.c_str());
            engine.set_halt("cannot write " + failed + " (rows kept; retrying)");
        } else if (!engine.halt().empty()) {
            std::printf("ledger writes resumed\n");
            engine.set_halt({});
        }
        std::error_code kec;
        engine.set_kill(fs::exists(kill_file, kec));
        std::string page = note;
        for (const auto& e : events) page += " " + e + ".";
        if (!connected) page += " Not connected to the price service.";
        (void)write_atomic(live_dir / "engine_state.json", engine.state_json(page));
        std::fflush(stdout);
    };

    bool resync = false;   // the stream lost its framing: skip to the next connection
    while (!g_stop.load() && unix_now() < deadline && !past_until()) {
        bool got = false;
        for (auto& ev : reader.take(std::chrono::milliseconds(5))) {
            if (ev.kind == altair::live_feed::FeedEvent::Connected) {
                resync = false;
                connected = true;
                buf.clear();
                consumer.on_reconnect();
                last_frame = std::chrono::steady_clock::now();
                std::printf("connected to the price service\n");
                std::fflush(stdout);
                continue;
            }
            if (ev.kind == altair::live_feed::FeedEvent::Disconnected) {
                connected = false;
                buf.clear();
                std::printf("price service went away: %s\n", ev.note.c_str());
                std::fflush(stdout);
                continue;
            }
            if (resync) continue;
            got = got || !ev.bytes.empty();
            buf.insert(buf.end(), ev.bytes.begin(), ev.bytes.end());
        }
        const auto now = std::chrono::steady_clock::now();
        std::size_t at = 0;
        while (buf.size() - at >= altair::kFrameHeaderBytes) {
            const auto h = altair::decode_header(buf.data() + at, buf.size() - at);
            if (!h) {
                // Not frame-aligned and not recoverable by guessing: drop what is buffered and
                // treat it as a gap (the consumer's next frame is checked against the last seen).
                buf.clear(); at = 0; resync = true;
                reader.reconnect();
                std::printf("stream misaligned; reconnecting\n");
                break;
            }
            const std::size_t need = altair::kFrameHeaderBytes + h->payload_len;
            if (buf.size() - at < need) break;
            (void)consumer.on_frame(*h, buf.data() + at + altair::kFrameHeaderBytes);
            ++frames;
            at += need;
        }
        if (at > 0) buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(at));
        if (got) last_frame = now;
        engine.set_stale(!connected || now - last_frame > std::chrono::seconds(30));
        // The feed's clock stops with the feed; the exits it owes do not wait for it.
        engine.watchdog(unix_now() * 1'000'000'000LL);
        if (now - last_write >= std::chrono::seconds(1)) {
            last_write = now;
            flush_outputs();
        }
    }
    flush_outputs();
    double gross = 0.0, exp = 0.0;
    std::size_t unpriced = 0;
    for (const auto& t : engine.book().trades()) {
        gross += t.gross;
        if (std::isfinite(t.expenses)) exp += t.expenses; else ++unpriced;
    }
    // An unpriced expense is not zero: with one, there is no net to report.
    char net_text[96];
    if (unpriced == 0) std::snprintf(net_text, sizeof net_text, "net %.0f", gross - exp);
    else std::snprintf(net_text, sizeof net_text, "net unavailable: %zu round trip(s) unpriced", unpriced);
    std::printf("stopped: %llu frames (%llu trade frames missed), %zu round trips (gross %.0f, expenses %s, %s), %zu open, "
                "%zu working order(s)\n",
                static_cast<unsigned long long>(frames), static_cast<unsigned long long>(consumer.missed(altair::kTopicTrades)),
                engine.book().trades().size(), gross, unpriced == 0 ? lv::live_fmt::num(exp, 0).c_str() : "incomplete",
                net_text, engine.book().positions().size(), engine.book().working_orders());
    if (!engine.halt().empty()) { std::printf("UNWRITTEN ROWS REMAIN: %s\n", engine.halt().c_str()); return 4; }
    return 0;
}
