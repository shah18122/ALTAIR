// app/live_engine_main.cpp -- altair_live_engine: the models, live, paper-trading.
//
//     altair_live_engine [--port 7421] [--until HH:MM] [--seconds N] [--root DIR] [--unverified-costs]
//                        [--latency-ms 250] [--quote-age-s 10] [--entry-timeout-s 60]
//                        [--max-positions 80] [--max-gross 1e8] [--max-daily-loss 2e5] [--gate-z 1]
//
// Subscribes to altair_price_service (FYERS live, or --sim), builds bars from
// the ticks, runs every live model on them and paper-trades their signals:
//   Vol band (HAR)                 the range, recomputed every minute; trades nothing
//   Strangle 80% NIFTY/BANKNIFTY   sell the band's edges at 09:20 (and a stop2x variant)
//   Direction 10:15 <model>        ARMA, logistic, ridge, GBDT and their vote, behind the
//                                  magnitude gate: q·gain − (1−q)·loss − cost, clear of zero by --gate-z se
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
#include <app/live_bundle.hpp>
#include <app/live_direction.hpp>
#include <analytics/har_rv.hpp>
#include <app/live_feed_reader.hpp>
#include <live/engine.hpp>
#include <live/feed_consumer.hpp>
#include <live/file_lock.hpp>
#include <live/latency.hpp>
#include <live/ledger.hpp>
#include <live/models.hpp>
#include <live/report.hpp>
#include <live/tape.hpp>
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
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// The commit the build was configured at (app/CMakeLists.txt): written beside a
// bundle, never into its id -- a rebuild that changes no decision keeps the id.
#ifndef ALTAIR_GIT_REV
#define ALTAIR_GIT_REV "unknown"
#endif

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
[[nodiscard]] lv::LiveVolInputs vol_inputs(const std::string& under, const std::vector<DayRv>& rv, std::string& note) {
    lv::LiveVolInputs v;
    v.under = under;
    if (rv.size() < 60) { note += under + ": too little 5-minute history for HAR. "; return v; }
    std::vector<double> r;
    for (const auto& x : rv) r.push_back(x.rv);
    const auto f = altair::har_forecast(r, r.size() - 1, 1);
    if (!f) { note += under + ": HAR refused (" + std::string(altair::har_error_text(f.error())) + "). "; return v; }
    v.sigma_day = std::sqrt(f->variance);
    double intra = 0.0, total = 0.0;
    for (std::size_t i = rv.size() > 250 ? rv.size() - 250 : 0; i < rv.size(); ++i) {
        intra += rv[i].rv - rv[i].gap2;
        total += rv[i].rv;
    }
    v.intraday_share = total > 0.0 ? intra / total : 0.75;
    v.prev_close = rv.back().close;
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
        "    --max-daily-loss RUPEES  no new entries past this loss today (default 2e5)\n"
        "    --max-margin RUPEES  estimated margin of everything held or working (default 1e7; a\n"
        "                         conservative estimate, not SPAN: live/margin.hpp)\n"
        "    --gate-z Z           a direction call trades only when its value clears zero by Z\n"
        "                         standard errors (default 1; 0 is the point estimate)\n"
        "    --record             also write the session tape to data/live/tapes/: every byte off the\n"
        "                         bus and every control event, for --replay (a live day is GBs)\n"
        "    --replay TAPE        run a recorded session again instead of reading the bus: same\n"
        "                         options, same starting ledger, same bundle; writes to --replay-out\n"
        "                         (default data/live/replay/<tape>) and must decide exactly as it did\n"
        "    --replay-out DIR     where a replay writes its files\n"
        "    --force-replay       replay even if the inputs no longer match the recorded bundle\n"
        "    --verify-against DIR after a replay, compare its journal, decisions, margin and marks with\n"
        "                         the recorded session's rows in DIR (default <root>/data/live/paper);\n"
        "                         the verdict goes to data/live/replay_checks/<tape>.json\n\n"
        "  Start the feed first: altair_price_service --live --go (FYERS, else Kite) or --sim.\n"
        "  Writes data/live/engine_state.json and data/live/paper/*.csv. Places no orders.\n"
        "  New entries stop while data/kill_request.json exists (the desktop's Kill Switch).\n",
        exe);
}

}  // namespace

int main(int argc, char** argv) {
    const std::int64_t started_unix = unix_now();
    unsigned short port = 7421;
    int until = 15 * 60 + 35, seconds = 0;
    bool unverified_costs = false;
    std::string date;   // a simulated past day (altair_price_service --sim --date)
    lv::LiveExecPolicy policy;
    lv::LiveRiskLimits limits;
    double gate_z = 1.0;   // standard errors a direction call's value must clear
    bool record = false, force_replay = false;
    fs::path replay_path, replay_out, verify_against;
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
        if (a == "--max-margin" && has) { limits.max_margin = std::atof(argv[++i]); continue; }
        if (a == "--gate-z" && has) { gate_z = std::atof(argv[++i]); continue; }
        if (a == "--record") { record = true; continue; }
        if (a == "--replay" && has) { replay_path = argv[++i]; continue; }
        if (a == "--replay-out" && has) { replay_out = argv[++i]; continue; }
        if (a == "--force-replay") { force_replay = true; continue; }
        if (a == "--verify-against" && has) { verify_against = argv[++i]; continue; }
        std::printf("unknown argument %s\n", a.c_str());
        usage(argv[0]);
        return 2;
    }
    // ---- a replay takes its options and starting files from the tape -------
    const bool replaying = !replay_path.empty();
    std::optional<lv::TapeReader> tape_in;
    lv::TapeSections start;   // what this session starts from (written to a recorded tape; read from a replayed one)
    if (replaying) {
        if (record) { std::printf("--record and --replay are exclusive\n"); return 2; }
        tape_in.emplace(replay_path.string());
        lv::TapeRecord first;
        if (!tape_in->ok() || !tape_in->next(first) || first.kind != lv::TapeKind::Start || !lv::tape_unpack(first.text(), start)) {
            std::printf("%s is not a session tape (or its first record is not the session's start)\n", replay_path.string().c_str());
            return 2;
        }
        const std::string* args = lv::tape_section(start, "args");
        if (args == nullptr) { std::printf("the tape's start record has no options\n"); return 2; }
        std::istringstream in(*args);
        for (std::string line; std::getline(in, line);) {
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            if (k == "date") date = v;
            else if (k == "until") until = std::atoi(v.c_str());
            else if (k == "latency_ns") policy.latency_ns = std::atoll(v.c_str());
            else if (k == "max_quote_age_ns") policy.max_quote_age_ns = std::atoll(v.c_str());
            else if (k == "entry_timeout_ns") policy.entry_timeout_ns = std::atoll(v.c_str());
            else if (k == "max_positions") limits.max_positions = static_cast<std::size_t>(std::atoll(v.c_str()));
            else if (k == "max_per_model") limits.max_per_model = static_cast<std::size_t>(std::atoll(v.c_str()));
            else if (k == "max_gross_notional") limits.max_gross_notional = std::strtod(v.c_str(), nullptr);
            else if (k == "max_daily_loss") limits.max_daily_loss = std::strtod(v.c_str(), nullptr);
            else if (k == "max_margin") limits.max_margin = std::strtod(v.c_str(), nullptr);
            else if (k == "gate_z") gate_z = std::strtod(v.c_str(), nullptr);
            else if (k == "unverified_costs") unverified_costs = v == "1";
        }
        std::printf("replaying %s: the session of %s, with its own options\n", replay_path.string().c_str(), date.c_str());
    }
    if (until < 0) { std::printf("--until must be HH:MM\n"); return 2; }
    if (policy.latency_ns < 0 || policy.max_quote_age_ns <= 0 || policy.entry_timeout_ns <= 0 || limits.max_positions == 0
        || !(limits.max_gross_notional > 0.0) || !(limits.max_daily_loss > 0.0) || !(limits.max_margin > 0.0) || !(gate_z >= 0.0)) {
        std::printf("--latency-ms must be >= 0; the ages, timeout and limits must be > 0\n");
        return 2;
    }
    if (!date.empty() && lv::parse_day(date) == 0) { std::printf("--date must be YYYY-MM-DD\n"); return 2; }
    // History stops the day before the session being traded: today, or the
    // simulated day.
    const std::int64_t today = date.empty() ? lv::ist_today(unix_now()) : lv::parse_day(date);
    if (date.empty()) date = lv::day_text(today);
    // A replay writes beside the live files, never over them, and starts from
    // the files the recorded session started from.
    const fs::path live_dir = !replaying ? root / "data/live"
                            : !replay_out.empty() ? replay_out : root / "data/live/replay" / replay_path.stem();
    const fs::path paper_dir = live_dir / "paper";
    fs::path charges_path = root / "config/charges.toml";
    std::error_code ec;
    if (replaying) {
        if (fs::exists(paper_dir / "engine.lock", ec)) {
            const lv::LiveFileLock probe((paper_dir / "engine.lock").string());
            if (!probe.held()) { std::printf("refused: another replay is writing %s\n", live_dir.string().c_str()); return 3; }
        }
        // Cleared before each replay -- so only ever a directory a replay made
        // (it carries the marker), or a new or empty one.
        const bool exists = fs::exists(live_dir, ec);
        if (exists && !fs::is_empty(live_dir, ec) && !fs::exists(live_dir / "REPLAY", ec)) {
            std::printf("refused: %s is not empty and not a replay's directory; name a new one with --replay-out\n",
                        live_dir.string().c_str());
            return 2;
        }
        fs::remove_all(live_dir, ec);
        fs::create_directories(paper_dir, ec);
        std::ofstream(live_dir / "REPLAY") << "written by altair_live_engine --replay " << replay_path.string()
                                           << "; cleared by the next replay into this directory\n";
        const auto put = [&](const fs::path& p, const std::string& name) {
            if (const std::string* body = lv::tape_section(start, name)) {
                std::ofstream f(p, std::ios::binary | std::ios::trunc);
                f << *body;
            }
        };
        put(live_dir / "universe.csv", "universe.csv");
        put(live_dir / "charges.toml", "charges.toml");
        put(paper_dir / "journal.csv", "journal.csv");
        put(paper_dir / "open_positions.csv", "open_positions.csv");
        charges_path = live_dir / "charges.toml";
    }
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
    if (const auto rep = altair::load_charges_file(charges_path.string().c_str(), schedules); !rep) {
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
    const auto nrv = daily_rv(nifty5), brv = daily_rv(bnf5);
    const auto nv = vol_inputs("NIFTY", nrv, note);
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
    std::string statarb_digest;
    {
        lv::Fingerprint fp;
        for (const auto d : sa.history.day) fp.i64(d);
        for (const double m : sa.history.market) fp.f64(m);
        for (const auto& r : sa.history.ret) for (const double x : r) fp.f64(x);
        for (const int sec : sa.history.sector) fp.i64(sec);
        for (const auto& sym : sa.symbols) fp.text(sym);
        for (const double c : sa.last_close) fp.f64(c);
        fp.f64(sa.market_last_close);
        statarb_digest = "{\"ok\": " + std::string(sa.ok ? "true" : "false") + ", \"stocks\": " + std::to_string(sa.symbols.size())
                       + ", \"days\": " + std::to_string(sa.history.day.size()) + ", \"digest\": \"" + fp.hex() + "\"}";
    }

    auto dir = std::make_shared<altair::live_direction::DirectionShared>();
    {
        // The last three years: what runs live, and a walk-forward that finishes in seconds.
        const std::int64_t from = today - 3 * 365;
        for (const auto& x : nifty5) if (da::audit_day(x.t) >= from) dir->nifty5.push_back(x);
        for (const auto& x : vix5) if (da::audit_day(x.t) >= from) dir->vix5.push_back(x);
        dir->gate_z = gate_z;
        const auto t0 = std::chrono::steady_clock::now();
        altair::live_direction::calibrate(*dir);
        std::printf("  direction: %s (%.1f s; today's fit %.2f s, before the session)\n",
                    dir->ok ? (std::to_string(dir->history_days) + " days walked forward").c_str() : dir->why.c_str(),
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), dir->fit_seconds);
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

    // ---- the bundle: what this session is built from ------------------------
    namespace lb = altair::live_bundle;
    const std::string universe_text = lb::file_text(live_dir / "universe.csv");
    const std::string charges_text = lb::file_text(charges_path);
    const std::string journal_text = have_journal ? lb::file_text(journal) : std::string();
    const std::string positions_text = have_journal ? std::string() : lb::file_text(paper_dir / "open_positions.csv");
    std::string args_text;
    {
        char text[1024];
        std::snprintf(text, sizeof text,
                      "date=%s\nuntil=%d\nlatency_ns=%lld\nmax_quote_age_ns=%lld\nentry_timeout_ns=%lld\nmax_positions=%zu\n"
                      "max_per_model=%zu\nmax_gross_notional=%.17g\nmax_daily_loss=%.17g\nmax_margin=%.17g\ngate_z=%.17g\nunverified_costs=%d\n",
                      date.c_str(), until, static_cast<long long>(policy.latency_ns), static_cast<long long>(policy.max_quote_age_ns),
                      static_cast<long long>(policy.entry_timeout_ns), limits.max_positions, limits.max_per_model,
                      limits.max_gross_notional, limits.max_daily_loss, limits.max_margin, gate_z, unverified_costs ? 1 : 0);
        args_text = text;
    }
    const auto vol_json = [](const lv::LiveVolInputs& v) {
        return "{\"under\": " + lb::str(v.under) + ", \"sigma_day\": " + lb::num(v.sigma_day) + ", \"intraday_share\": "
             + lb::num(v.intraday_share) + ", \"prev_close\": " + lb::num(v.prev_close) + "}";
    };
    const std::string bundle_body =
        "{\n  \"day\": " + lb::str(date) + ",\n  \"history_through\": " + lb::str(lv::day_text(today - 1))
        + ",\n  \"inputs\": {\"nifty_5m\": " + lb::bars_digest(nifty5) + ", \"banknifty_5m\": " + lb::bars_digest(bnf5)
        + ", \"vix_5m\": " + lb::bars_digest(vix5) + ",\n    \"nifty_1d\": " + lb::closes_digest(nd) + ", \"banknifty_1d\": "
        + lb::closes_digest(bd) + ", \"statarb\": " + statarb_digest + ",\n    \"universe\": " + lb::text_digest(universe_text)
        + ", \"charges\": " + lb::text_digest(charges_text) + ", \"costs\": " + lb::str(costs_label)
        + ", \"resumed_from\": " + lb::text_digest(have_journal ? journal_text : positions_text) + "}"
        + ",\n  \"options\": " + lb::str(args_text)
        + ",\n  \"models\": {\"vol_band\": [" + vol_json(nv) + ", " + vol_json(bv) + "], \"ratio_mean\": " + lb::num(rmean)
        + ", \"ratio_sd\": " + lb::num(rsd) + ",\n    \"pairs\": {\"ok\": " + std::string(pf.ok ? "true" : "false") + ", \"beta\": "
        + lb::num(pf.beta) + ", \"sd\": " + lb::num(pf.sd) + ", \"half_life\": " + lb::num(pf.half_life) + "},\n    \"direction\": "
        + lb::direction_json(*dir) + "}\n}\n";
    const std::string bundle_id = lv::Fingerprint{}.text(bundle_body).hex();
    if (replaying) {
        const std::string* recorded = lv::tape_section(start, "bundle_id");
        const std::string* recorded_body = lv::tape_section(start, "bundle");
        if (recorded == nullptr || *recorded != bundle_id) {
            std::printf("THE INPUTS DIFFER from the recorded session's (bundle %s, now %s)%s\n",
                        recorded ? recorded->c_str() : "missing", bundle_id.c_str(),
                        force_replay ? "; replaying anyway (--force-replay)" : "; refusing (--force-replay to run anyway)");
            if (recorded_body != nullptr) {
                // Name the lines that changed: a dataset revised, a model fitted differently.
                std::istringstream was(*recorded_body), now(bundle_body);
                std::string la, lb_;
                int shown = 0;
                while (std::getline(was, la) && std::getline(now, lb_) && shown < 6)
                    if (la != lb_) { std::printf("  was: %.200s\n  now: %.200s\n", la.c_str(), lb_.c_str()); ++shown; }
            }
            if (!force_replay) return 5;
        } else {
            std::printf("bundle %s: the inputs are the recorded session's\n", bundle_id.c_str());
        }
        std::ofstream(live_dir / "bundle.json", std::ios::binary | std::ios::trunc) << bundle_body;
    } else {
        // Content-addressed: the same inputs give the same file, written once.
        const fs::path bdir = root / "data/live/bundles" / date;
        fs::create_directories(bdir, ec);
        const fs::path bpath = bdir / ("bundle-" + bundle_id + ".json");
        if (!fs::exists(bpath, ec)) {
            std::ofstream f(bpath, std::ios::binary | std::ios::trunc);
            f << "{\"id\": \"" << bundle_id << "\", \"written_unix\": " << unix_now() << ", \"code\": \"" << ALTAIR_GIT_REV
              << "\",\n\"body\": " << bundle_body << "}\n";
            if (!lb::write_oos_csv(bdir / ("oos-" + bundle_id + ".csv"), *dir))
                std::printf("could not write the out-of-sample calls beside the bundle\n");
        }
        std::printf("bundle %s (%s)\n", bundle_id.c_str(), bpath.string().c_str());
    }

    // ---- the tape: everything this session is told, in order ---------------
    std::optional<lv::TapeWriter> tape_out;
    std::string tape_path;
    if (record) {
        const fs::path tdir = root / "data/live/tapes";
        fs::create_directories(tdir, ec);
        char stamp[32];
        const std::int64_t ist = unix_now() + 19800;
        std::snprintf(stamp, sizeof stamp, "%02lld%02lld%02lld", static_cast<long long>((ist / 3600) % 24),
                      static_cast<long long>((ist / 60) % 60), static_cast<long long>(ist % 60));
        const fs::path tpath = tdir / (date + "-" + stamp + ".tape");
        tape_out.emplace(tpath.string());
        tape_path = tpath.string();
        if (!tape_out->ok()) { std::printf("cannot write the tape %s\n", tpath.string().c_str()); return 2; }
        start = {{"args", args_text}, {"universe.csv", universe_text}, {"charges.toml", charges_text}};
        if (have_journal) start.emplace_back("journal.csv", journal_text);
        else if (fs::exists(paper_dir / "open_positions.csv", ec)) start.emplace_back("open_positions.csv", positions_text);
        start.emplace_back("bundle_id", bundle_id);
        start.emplace_back("bundle", bundle_body);
        const std::string packed = lv::tape_pack(start);
        tape_out->write(lv::TapeKind::Start, unix_now() * 1'000'000'000LL, packed);
        std::printf("recording the session to %s\n", tpath.string().c_str());
    }

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
    // slow minute here never makes the bus drop frames for this reader. A
    // replay reads the tape instead.
    std::optional<altair::live_feed::FeedReader> reader;
    if (!replaying) reader.emplace(port);
    bool connected = false;
    std::vector<std::uint8_t> buf;
    buf.reserve(1 << 20);
    // Where each received chunk ends in `buf`, and when it came off the
    // socket: a frame's receipt time is that of the chunk holding its last byte.
    std::deque<std::pair<std::size_t, std::int64_t>> marks;
    lv::LatencyHistogram lat_frame;     // receipt -> processed, every frame (queueing included)
    lv::LatencyHistogram lat_decision;  // receipt -> processed, frames that ran the models
    lv::LatencyHistogram lat_fill;      // decision -> fill, on the feed's clock (the paper latency plus the wait for a quote)
    const auto steady_ns = [] {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    const auto wall_ns = [] {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    };
    auto last_frame = std::chrono::steady_clock::now();
    auto last_write = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    std::uint64_t frames = 0;
    lv::LiveFeedConsumer consumer(engine);
    if (replaying)
        std::printf("engine replaying the tape -- %zu models, %zu instruments\n", engine.models().size(), universe.size());
    else
        std::printf("engine on 127.0.0.1:%u until %s IST -- %zu models, %zu instruments\n", static_cast<unsigned>(port),
                    lv::live_fmt::hhmm(until).c_str(), engine.models().size(), universe.size());
    std::fflush(stdout);

    lv::LivePaperLedger ledger(paper_dir, costs_label);
    if (!have_journal) {
        // The first run with a journal: the snapshot's positions become its
        // opening rows, so the next restart has them on the record.
        for (const auto& p : engine.book().held()) {
            lv::LivePaperFill f;
            f.model = p.model; f.token = p.inst.token; f.symbol = p.inst.symbol; f.side = p.side; f.qty = p.qty;
            f.price = p.entry; f.expenses = p.entry_expenses; f.ns = p.entry_ns; f.submit_ns = p.decided_ns;
            f.reason = p.why_in; f.role = lv::LiveFillRole::Open; f.carry = p.carry;
            ledger.journal.add(lv::live_journal_row(f));
        }
    }
    std::deque<std::string> events;   // the last few cancellations, for the page
    const fs::path kill_file = root / "data/kill_request.json";

    // The tape. A write that fails stops the recording, not the session: the
    // paper ledger is the record that matters; the tape is evidence about it.
    bool tape_failed = false;
    const auto tape_lost = [&] {
        tape_failed = true;
        std::printf("TAPE WRITE FAILED: the rest of this session is not recorded (the session goes on)\n");
        note += " The session tape failed; it is incomplete.";
    };
    const auto tape = [&](lv::TapeKind k, std::int64_t at, const std::uint8_t* p, std::size_t n) {
        if (!tape_out || tape_failed) return;
        tape_out->write(k, at, p, n);
        if (!tape_out->ok()) tape_lost();
    };
    const auto tape_text = [&](lv::TapeKind k, std::int64_t at, const std::string& t) {
        tape(k, at, reinterpret_cast<const std::uint8_t*>(t.data()), t.size());
    };

    // The controls the machine sets -- the halt and the kill request -- are
    // changed here in a live session and put on the tape where they change; a
    // replay takes them from the tape and only reports its own write failures.
    std::string replay_unwritten;
    std::uint64_t halts = 0;
    bool kill_seen = false;
    const auto flush_outputs = [&] {
        for (const auto& f : ledger.collect(engine)) lat_fill.record(f.ns - f.submit_ns);
        for (auto& c : engine.book().take_cancelled()) {
            std::printf("  %s\n", c.c_str());
            events.push_back(std::move(c));
            if (events.size() > 5) events.pop_front();
        }
        // The journal first: it is the record. Then the views of it.
        const std::string failed = ledger.flush(engine);
        if (replaying) {
            if (!failed.empty() && replay_unwritten.empty())
                std::printf("cannot write %s: this replay's files are incomplete (rows kept; retrying)\n", failed.c_str());
            replay_unwritten = failed;
        } else {
            const std::string halt = lv::LivePaperLedger::halt_text(failed);
            if (halt != engine.halt()) {
                if (engine.halt().empty()) {
                    std::printf("HALTED: cannot write %s; rows kept, retrying every second\n", failed.c_str());
                    ++halts;
                }
                else if (halt.empty()) std::printf("ledger writes resumed\n");
                tape_text(lv::TapeKind::Halt, wall_ns(), halt);
                engine.set_halt(halt);
            }
            std::error_code kec;
            const bool kill = fs::exists(kill_file, kec);
            kill_seen = kill_seen || kill;
            if (kill != engine.kill()) {
                tape_text(lv::TapeKind::Kill, wall_ns(), kill ? "1" : "0");
                engine.set_kill(kill);
            }
            if (tape_out && !tape_failed && !tape_out->flush()) tape_lost();
        }
        engine.set_metrics("{\"frame_us\": " + lat_frame.json(1e3) + ", \"decision_us\": " + lat_decision.json(1e3)
                           + ", \"fill_ms_feed\": " + lat_fill.json(1e6) + "}");
        std::string page = note;
        for (const auto& e : events) page += " " + e + ".";
        if (!connected && !replaying) page += " Not connected to the price service.";
        (void)write_atomic(live_dir / "engine_state.json", engine.state_json(page));
        std::fflush(stdout);
    };

    // What the bus says, one event at a time, each processed whole before the
    // next: the order on the tape is the order the engine saw.
    bool resync = false;   // the stream lost its framing: skip to the next connection
    const auto on_connected = [&] {
        resync = false;
        connected = true;
        buf.clear();
        marks.clear();
        consumer.on_reconnect();
        last_frame = std::chrono::steady_clock::now();
        std::printf("connected to the price service\n");
        std::fflush(stdout);
    };
    const auto on_disconnected = [&](const std::string& why) {
        connected = false;
        buf.clear();
        marks.clear();
        std::printf("price service went away: %s\n", why.c_str());
        std::fflush(stdout);
    };
    const auto on_data = [&](const std::uint8_t* p, std::size_t n, std::int64_t recv_ns) {
        if (resync || n == 0) return;
        buf.insert(buf.end(), p, p + n);
        marks.emplace_back(buf.size(), recv_ns);
        std::size_t at = 0;
        while (buf.size() - at >= altair::kFrameHeaderBytes) {
            const auto h = altair::decode_header(buf.data() + at, buf.size() - at);
            if (!h) {
                // Not frame-aligned and not recoverable by guessing: drop what is buffered and
                // treat it as a gap (the consumer's next frame is checked against the last seen).
                buf.clear(); marks.clear(); at = 0; resync = true;
                if (reader) reader->reconnect();
                std::printf("stream misaligned; reconnecting\n");
                return;
            }
            const std::size_t need = altair::kFrameHeaderBytes + h->payload_len;
            if (buf.size() - at < need) break;
            const std::uint64_t decisions_before = engine.decision_minutes();
            (void)consumer.on_frame(*h, buf.data() + at + altair::kFrameHeaderBytes);
            ++frames;
            at += need;
            while (!marks.empty() && marks.front().first < at) marks.pop_front();
            if (!marks.empty()) {
                const std::int64_t took = steady_ns() - marks.front().second;
                lat_frame.record(took);
                if (engine.decision_minutes() != decisions_before) lat_decision.record(took);
            }
        }
        if (at > 0) {
            buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(at));
            for (auto& m : marks) m.first -= at;
        }
    };

    std::uint64_t diverged = 0;
    if (!replaying) {
        while (!g_stop.load() && unix_now() < deadline && !past_until()) {
            bool got = false;
            for (auto& ev : reader->take(std::chrono::milliseconds(5))) {
                const std::int64_t at = wall_ns();
                if (ev.kind == altair::live_feed::FeedEvent::Connected) {
                    tape(lv::TapeKind::Connected, at, nullptr, 0);
                    on_connected();
                    continue;
                }
                if (ev.kind == altair::live_feed::FeedEvent::Disconnected) {
                    tape_text(lv::TapeKind::Disconnected, at, ev.note);
                    on_disconnected(ev.note);
                    continue;
                }
                got = got || (!resync && !ev.bytes.empty());
                tape(lv::TapeKind::Data, at, ev.bytes.data(), ev.bytes.size());
                on_data(ev.bytes.data(), ev.bytes.size(), ev.recv_ns);
            }
            const auto now = std::chrono::steady_clock::now();
            if (got) last_frame = now;
            const bool stale = !connected || now - last_frame > std::chrono::seconds(30);
            if (stale != engine.stale()) {
                tape_text(lv::TapeKind::Stale, wall_ns(), stale ? "1" : "0");
                engine.set_stale(stale);
            }
            // The feed's clock stops with the feed; the exits it owes do not wait for it.
            const std::int64_t at = wall_ns();
            if (engine.watchdog(at)) tape(lv::TapeKind::Watchdog, at, nullptr, 0);
            if (now - last_write >= std::chrono::seconds(1)) {
                last_write = now;
                flush_outputs();
            }
            // After the close the market stops printing, so the feed's clock
            // stops short of --until: a quiet minute past 15:30 ends the day.
            const std::int64_t c = engine.clock_ns();
            if (c > 0 && lv::live_minute_of_day(lv::live_ist_minute_index(c)) >= lv::kLiveCloseMinute
                && now - last_frame > std::chrono::seconds(60)) {
                std::printf("the market has closed and the feed has been quiet for a minute: stopping\n");
                break;
            }
        }
    } else {
        // Every record in order, into the same engine; the views are written
        // every second of the recorded session's clock.
        lv::TapeRecord rec;
        std::int64_t last_flush = std::numeric_limits<std::int64_t>::min();
        bool second_start = false;
        std::int64_t first_clock = 0;
        while (!g_stop.load() && tape_in->next(rec)) {
            switch (rec.kind) {
            case lv::TapeKind::Data: on_data(rec.bytes.data(), rec.bytes.size(), steady_ns()); break;
            case lv::TapeKind::Connected: on_connected(); break;
            case lv::TapeKind::Disconnected: on_disconnected(rec.text()); break;
            case lv::TapeKind::Stale: engine.set_stale(rec.text() == "1"); break;
            case lv::TapeKind::Kill: engine.set_kill(rec.text() == "1"); break;
            case lv::TapeKind::Halt: engine.set_halt(rec.text()); break;
            case lv::TapeKind::Watchdog:
                // It acted when recorded; on the same state it must act again.
                if (!engine.watchdog(rec.wall_ns)) ++diverged;
                break;
            case lv::TapeKind::Start: second_start = true; break;
            }
            if (second_start) { std::printf("a second start record: the tape is two sessions; stopping at the first\n"); break; }
            if (first_clock == 0) first_clock = engine.clock_ns();
            if (rec.wall_ns > 0 && (last_flush == std::numeric_limits<std::int64_t>::min() || rec.wall_ns - last_flush >= 1'000'000'000LL)) {
                last_flush = rec.wall_ns;
                flush_outputs();
            }
        }
        std::printf("replayed %llu record(s)\n", static_cast<unsigned long long>(tape_in->records()));
        if (tape_in->truncated() > 0)
            std::printf("the tape ends inside a record (%llu bytes dropped): the session's last moment is not on it\n",
                        static_cast<unsigned long long>(tape_in->truncated()));
        if (!tape_in->ok()) std::printf("the tape holds a record of no known kind; the replay stopped there\n");
        if (diverged > 0)
            std::printf("REPLAY DIVERGED: %llu watchdog action(s) recorded did not repeat\n", static_cast<unsigned long long>(diverged));
        flush_outputs();
        // The verdict: this replay's rows against the recorded session's, over
        // the feed time the tape covered. Sessions never overlap in a live
        // feed's time; two SIM runs of one day can, and then this says so.
        const fs::path original = !verify_against.empty() ? verify_against : root / "data/live/paper";
        if (first_clock == 0) {
            std::printf("the tape holds no market data: nothing to verify\n");
        } else if (fs::exists(original / "journal.csv", ec) || fs::exists(original / "decisions.csv", ec)) {
            const std::int64_t lo = first_clock, hi = engine.clock_ns();
            const auto rows_in = [lo, hi](const fs::path& p, std::size_t col) {
                std::vector<std::string> out;
                std::ifstream in(p);
                std::string line;
                std::getline(in, line);
                while (std::getline(in, line)) {
                    const auto c = lv::report::csv_fields(line);
                    if (c.size() <= col) continue;
                    const std::int64_t ns = std::atoll(c[col].c_str());
                    if (ns >= lo && ns <= hi) out.push_back(line);
                }
                return out;
            };
            bool identical = true;
            std::string first_diff, counts;
            for (const auto& [file, col] : std::vector<std::pair<std::string, std::size_t>>{
                     {"journal.csv", 0}, {"decisions.csv", 1}, {"margin.csv", 1}, {"marks.csv", 1}}) {
                const auto mine = rows_in(paper_dir / file, col), theirs = rows_in(original / file, col);
                counts += (counts.empty() ? "" : ", ") + std::string("\"") + file + "\": [" + std::to_string(mine.size()) + ", "
                        + std::to_string(theirs.size()) + "]";
                if (mine == theirs) continue;
                identical = false;
                if (!first_diff.empty()) continue;
                std::size_t k = 0;
                while (k < mine.size() && k < theirs.size() && mine[k] == theirs[k]) ++k;
                first_diff = file + " row " + std::to_string(k + 1) + ": replay " + (k < mine.size() ? mine[k] : "(none)")
                           + " / recorded " + (k < theirs.size() ? theirs[k] : "(none)");
            }
            const std::string* recorded_id = lv::tape_section(start, "bundle_id");
            const bool bundle_match = recorded_id != nullptr && *recorded_id == bundle_id;
            std::string verdict = "{\"tape\": " + lb::str(replay_path.string()) + ", \"date\": " + lb::str(date) + ", \"source\": \""
                                + (engine.simulated() ? "SIM" : "LIVE") + "\", \"bundle_match\": " + (bundle_match ? "true" : "false")
                                + ", \"identical\": " + (identical ? "true" : "false") + ", \"feed_from_ns\": " + std::to_string(lo)
                                + ", \"feed_to_ns\": " + std::to_string(hi) + ", \"rows\": {" + counts + "}, \"first_difference\": "
                                + lb::str(first_diff) + ", \"checked_unix\": " + std::to_string(unix_now()) + "}\n";
            std::printf("%s the recorded session (%s)%s%s\n", identical ? "IDENTICAL to" : "DIFFERS from", original.string().c_str(),
                        first_diff.empty() ? "" : ": ", first_diff.substr(0, 300).c_str());
            std::ofstream(live_dir / "replay_check.json", std::ios::binary | std::ios::trunc) << verdict;
            const fs::path checks = root / "data/live/replay_checks";
            fs::create_directories(checks, ec);
            std::ofstream(checks / (replay_path.stem().string() + ".json"), std::ios::binary | std::ios::trunc) << verdict;
            if (!identical && diverged == 0) diverged = 1;   // exit 6: the replay did not reproduce the session
        } else {
            std::printf("no recorded ledger at %s to compare with (--verify-against DIR)\n", original.string().c_str());
        }
    }
    flush_outputs();
    if (tape_out) {
        if (!tape_failed && !tape_out->flush()) tape_lost();
        std::printf("tape: %.1f MB%s\n", static_cast<double>(tape_out->bytes_written()) / 1e6, tape_failed ? " (INCOMPLETE)" : "");
    }
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
    std::printf("latency, frame received -> processed:  %s\n", lat_frame.text(1e3, "us").c_str());
    std::printf("latency, frames that ran the models:  %s\n", lat_decision.text(1e3, "us").c_str());
    std::printf("latency, decision -> fill (feed clock): %s\n", lat_fill.text(1e6, "ms").c_str());
    int rc = 0;
    if (!engine.halt().empty() && !replaying) { std::printf("UNWRITTEN ROWS REMAIN: %s\n", engine.halt().c_str()); rc = 4; }
    else if (!replay_unwritten.empty()) { std::printf("UNWRITTEN ROWS REMAIN: %s\n", replay_unwritten.c_str()); rc = 4; }
    else if (diverged > 0) rc = 6;

    // One row per session: the operational record the readiness gates read
    // (altair_readiness). A replay writes its own beside its files.
    {
        lv::LiveCsvLog sessions(live_dir / "sessions.csv",
                                "date,started_unix,ended_unix,source,replay,frames,trade_frames_missed,trade_gaps,late_prints,halts,"
                                "kill_seen,frame_p50_us,frame_p99_us,frame_p999_us,decision_p99_us,fill_p99_ms,round_trips,"
                                "open_positions,working_orders,tape,tape_complete,bundle,exit_code");
        const auto us = [](std::uint64_t ns) { return lv::live_fmt::num(static_cast<double>(ns) / 1e3, 1); };
        sessions.add(date + "," + std::to_string(started_unix) + "," + std::to_string(unix_now()) + ","
                     + (engine.simulated() ? "SIM" : "LIVE") + "," + (replaying ? "1" : "0") + "," + std::to_string(frames) + ","
                     + std::to_string(consumer.missed(altair::kTopicTrades)) + "," + std::to_string(engine.trade_gaps()) + ","
                     + std::to_string(engine.late_prints()) + "," + std::to_string(halts) + "," + (kill_seen ? "1" : "0") + ","
                     + us(lat_frame.quantile(0.5)) + "," + us(lat_frame.quantile(0.99)) + "," + us(lat_frame.quantile(0.999)) + ","
                     + us(lat_decision.quantile(0.99)) + "," + lv::live_fmt::num(static_cast<double>(lat_fill.quantile(0.99)) / 1e6, 1)
                     + "," + std::to_string(engine.book().trades().size()) + "," + std::to_string(engine.book().positions().size())
                     + "," + std::to_string(engine.book().working_orders()) + "," + lv::live_csv_text(tape_path) + ","
                     + (tape_out ? (tape_failed ? "0" : "1") : "") + "," + bundle_id + "," + std::to_string(rc));
        if (!sessions.flush()) std::printf("could not append to %s\n", (live_dir / "sessions.csv").string().c_str());
    }
    return rc;
}
