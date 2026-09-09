// app/kite_update_main.cpp -- bring dataset/ up to date, in one call.
//
// P34-01. Smit asked that linking a Kite token update the whole tree: NIFTY
// and BANKNIFTY at every timeframe, current, and the Live Grid pointed at it.
//
// WHY THIS IS NOT altair_kite_fetch IN A LOOP.
//
// The fetcher writes WHOLE MONTH FILES and refuses to overwrite one that
// exists, because overwriting good data on a whim is worse than not having
// new data. That is right for a bulk backfill and exactly wrong for a daily
// top-up: the month you are appending to is always the month already on disk,
// so every incremental fetch would skip the only file it needed to touch.
//
// So this MERGES. It reads what is there, unions the fetched candles in by
// timestamp, and rewrites the month. Nothing already stored is lost.
//
// A STORED BAR IS ONLY REPLACED WHEN THE DIFFERENCE IS MATERIAL.
//
// dataset/ carries two decimals and Kite's index dailies carry one, so a bar
// re-fetched a week later routinely differs by 0.05 points -- 0.02 bps on a
// 23,000 index. Replacing on ANY difference would overwrite the finer value
// with the coarser one and report it as a correction.
//
// One basis point separates the two cases. Below it is decimal places and the
// stored value stays. Above it is a bar that was captured mid-session and
// frozen -- India VIX's 2026-09-04 close sat in dataset/ as 10.80 against a
// settled 10.68, which is 110 bps -- and the fresh value wins. Both are
// counted and both are printed.
//
// AND TODAY'S BAR IS NOT WRITTEN UNTIL IT HAS CLOSED.
//
// The same rule the Live Forecast page keeps. While the session is open,
// today's daily candle exists and its close is wherever the price happens to
// be. Writing it produces exactly the frozen partial bar the paragraph above
// exists to repair, so an unfinished bar is dropped and counted.
//
// A daily bar is stamped 00:00 and settles at 15:30 IST, so "start + one day"
// would call today's partial bar finished for the whole afternoon -- which is
// precisely when somebody links a token and presses this.
//
// IT IS READ-ONLY AGAINST THE BROKER. /instruments/historical is a GET. This
// program has no order vocabulary and could not place one if asked.

#include <broker/https_client.hpp>
#include <broker/kite_historical.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace {

[[nodiscard]] const char* arg_value(int argc, char** argv, const char* flag) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], flag) == 0) { return argv[i + 1]; }
    }
    return nullptr;
}

[[nodiscard]] bool has_flag(int argc, char** argv, const char* flag) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], flag) == 0) { return true; }
    }
    return false;
}

[[nodiscard]] std::string env_or_empty(const char* name) {
#if defined(_WIN32)
    char* buf = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&buf, &len, name) != 0 || buf == nullptr) { return {}; }
    std::string v(buf);
    std::free(buf);
    return v;
#else
    const char* v = std::getenv(name);
    return v == nullptr ? std::string{} : std::string{v};
#endif
}

/// Read `access_token` out of data/kite_session.json.
///
/// Its own reader rather than a JSON library, for the reason every other
/// binary here gives: the file holds a LIVE TRADING CREDENTIAL and the
/// smallest amount of code that can see it is the right amount. Never
/// printed, never logged, never put in an error message.
[[nodiscard]] bool read_access_token(const char* path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { return false; }
    const std::string all((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
    const std::string key = "\"access_token\"";
    const auto k = all.find(key);
    if (k == std::string::npos) { return false; }
    const auto q1 = all.find('"', all.find(':', k) + 1);
    if (q1 == std::string::npos) { return false; }
    const auto q2 = all.find('"', q1 + 1);
    if (q2 == std::string::npos) { return false; }
    out = all.substr(q1 + 1, q2 - q1 - 1);
    return !out.empty();
}

struct Series {
    const char* sym;        // dataset directory
    std::int64_t token;     // Kite instrument_token
    const char* dir;        // interval directory
    const char* interval;   // Kite interval name
    std::int64_t ns;        // bar width in nanoseconds
};

/// One stored row, keyed by its stamp so a merge is a map insert.
struct Row {
    std::string line;       // the CSV row, verbatim
    double close = 0.0;
};

[[nodiscard]] double close_of(const std::string& line) {
    std::size_t at = 0;
    for (int f = 0; f < 4; ++f) {
        at = line.find(',', at);
        if (at == std::string::npos) { return 0.0; }
        ++at;
    }
    return std::atof(line.c_str() + at);
}

/// Is the bar starting at `start_ns` finished, at `now_ns`?
///
/// A DAILY BAR IS NOT FINISHED AT MIDNIGHT. Stamped 00:00, it covers 09:15 to
/// 15:30 IST, so start + one day would call today's partial bar complete for
/// the whole afternoon.
[[nodiscard]] bool bar_is_complete(std::int64_t start_ns, std::int64_t iv_ns,
                                  std::int64_t now_ns) {
    if (iv_ns >= 86'400'000'000'000LL) {
        const std::int64_t ist = start_ns + altair::detail::kIstOffsetNs;
        const std::int64_t day = ist / 86'400'000'000'000LL;
        const std::int64_t settle =
            day * 86'400'000'000'000LL + (15 * 3600 + 30 * 60) * 1'000'000'000LL
            - altair::detail::kIstOffsetNs;
        return now_ns >= settle;
    }
    return start_ns + iv_ns <= now_ns;
}

/// The key two stamps must share to be the SAME BAR.
///
/// P34-01, AND THE FIRST VERSION GOT THIS WRONG. dataset/spot/nifty/1d holds
/// `2026-09-08` and Kite returns `2026-09-08T00:00:00+05:30`. Keyed on the
/// full string those are different bars, so a top-up ADDED a duplicate of
/// every day it re-fetched instead of matching it -- 09-08 ended up in the
/// tree twice, in two files, in two formats.
///
/// A daily bar is identified by its DATE. The time on it is decoration, and
/// the two sources decorate differently.
[[nodiscard]] std::string bar_key(const std::string& stamp,
                                  std::int64_t iv_ns) {
    return iv_ns >= 86'400'000'000'000LL && stamp.size() >= 10
               ? stamp.substr(0, 10)
               : stamp;
}

/// Is this a `YYYY-MM.csv` partition file?
[[nodiscard]] bool is_month_file(const std::string& name) {
    if (name.size() != 11 || name.compare(7, 4, ".csv") != 0) { return false; }
    for (std::size_t i : {0u, 1u, 2u, 3u, 5u, 6u}) {
        if (name[i] < '0' || name[i] > '9') { return false; }
    }
    return name[4] == '-';
}

void usage(const char* exe) {
    std::printf(
        "  Bring dataset/ up to date from Kite, merging into what is there.\n\n"
        "    %s [--out DIR] [--symbols nifty,banknifty] [--days N] --go\n\n"
        "    --out       dataset root (default dataset)\n"
        "    --symbols   default nifty,banknifty,indiavix\n"
        "    --days      how far back to ask when a series is EMPTY\n"
        "                (default 400; an existing series is topped up from\n"
        "                 its own last bar and ignores this)\n"
        "    --go        ACTUALLY CALL THE API.\n\n"
        "  Read-only: /instruments/historical is a GET and this program has\n"
        "  no order vocabulary.\n", exe);
}

} // namespace

int main(int argc, char** argv)
{
    if (has_flag(argc, argv, "--help")) { usage(argv[0]); return 0; }

    const char* out_s = arg_value(argc, argv, "--out");
    const char* syms_s = arg_value(argc, argv, "--symbols");
    const char* days_s = arg_value(argc, argv, "--days");
    const bool go = has_flag(argc, argv, "--go");
    const std::string root = out_s != nullptr ? out_s : "dataset";
    const int back = days_s != nullptr ? std::atoi(days_s) : 400;

    struct Sym { const char* name; std::int64_t token; };
    const Sym kAll[] = {{"nifty", 256265}, {"banknifty", 260105},
                        {"indiavix", 264969}};
    std::vector<Sym> syms;
    {
        const std::string want =
            syms_s != nullptr ? syms_s : "nifty,banknifty,indiavix";
        for (const Sym& s : kAll) {
            if (want.find(s.name) != std::string::npos) { syms.push_back(s); }
        }
    }
    if (syms.empty()) {
        std::printf("  --symbols matched nothing\n");
        return 2;
    }

    struct Iv { const char* dir; const char* name; std::int64_t ns; };
    const Iv kIvs[] = {{"1m", "minute", 60'000'000'000LL},
                       {"5m", "5minute", 300'000'000'000LL},
                       {"15m", "15minute", 900'000'000'000LL},
                       {"60m", "60minute", 3'600'000'000'000LL},
                       {"1d", "day", 86'400'000'000'000LL}};

    std::vector<Series> plan;
    for (const Sym& s : syms) {
        for (const Iv& iv : kIvs) {
            plan.push_back({s.name, s.token, iv.dir, iv.name, iv.ns});
        }
    }

    const auto now = std::chrono::system_clock::now();
    const std::int64_t now_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count();
    const std::int64_t today =
        (now_ns + altair::detail::kIstOffsetNs) / 86'400'000'000'000LL;

    std::printf("\n  Updating %zu series under %s/\n", plan.size(),
                root.c_str());

    if (!go) {
        for (const Series& s : plan) {
            std::printf("    spot/%s/%s\n", s.sym, s.dir);
        }
        std::printf("\n  DRY RUN. Nothing was fetched and nothing written.\n"
                    "  Add --go to update for real.\n\n");
        return 0;
    }

    const std::string api_key = env_or_empty("ALTAIR_KITE_API_KEY");
    if (api_key.empty()) {
        std::printf("\n  ALTAIR_KITE_API_KEY is not set.\n");
        return 3;
    }
    std::string access;
    if (!read_access_token("data/kite_session.json", access)) {
        std::printf("\n  no usable data/kite_session.json -- run "
                    "altair_kite_login. Kite tokens are DAILY.\n");
        return 3;
    }
    const std::string auth = std::string("token ") + api_key + ":" + access;

    std::size_t series_done = 0, added_total = 0, replaced_total = 0;
    std::size_t rounding_total = 0, partial_total = 0, failed = 0;

    for (const Series& s : plan) {
        const std::string dir = root + "/spot/" + s.sym + "/" + s.dir;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);

        // ---- what is already here, and how far it reaches ----------------
        std::map<std::string, std::map<std::string, Row>> files;
        std::map<std::string, std::string> where;   // bar key -> file it is in
        std::string last_stamp;
        std::string header = "time,open,high,low,close,volume";
        // The file NEW rows belong in. `all.csv` when the series is stored as
        // one file, otherwise the month partition. Detected rather than
        // assumed: nifty/1d is a single all.csv and nifty/1m is monthly, and
        // the first version wrote month files into both -- which left
        // 1d/all.csv AND a 1d/2026-09.csv holding the same day twice.
        std::string single_file;
        bool stamp_is_date_only = false;
        for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
            if (!e.is_regular_file()) { continue; }
            const std::string name = e.path().filename().string();
            if (name.size() < 5
                || name.compare(name.size() - 4, 4, ".csv") != 0) {
                continue;
            }
            if (!is_month_file(name) && (single_file.empty()
                                         || name == "all.csv")) {
                single_file = name;
            }
            std::ifstream in(e.path());
            if (!in) { continue; }
            std::string line;
            std::getline(in, line);
            if (!line.empty() && line[0] == 't') { header = line; }
            while (std::getline(in, line)) {
                if (line.empty()) { continue; }
                const std::string stamp = line.substr(0, line.find(','));
                if (stamp.size() < 10) { continue; }
                const std::string key = bar_key(stamp, s.ns);
                files[name][key] = Row{line, close_of(line)};
                where[key] = name;
                if (stamp.size() == 10) { stamp_is_date_only = true; }
                // MAX ACROSS EVERY FILE, not the last one by name.
                // banknifty/1d holds all.csv and vendor_pre2015.csv, and the
                // vendor file sorts LAST -- the same trap the Data Flow
                // inventory hit in P30-03.
                if (stamp > last_stamp) { last_stamp = stamp; }
            }
        }

        // ---- the window to ask for ---------------------------------------
        std::int64_t from_day = today - back;
        if (last_stamp.size() >= 10) {
            std::int64_t d = 0;
            if (altair::parse_date(last_stamp.substr(0, 10), d)) {
                // From the last STORED day, not the day after: the last bar
                // of a partitioned intraday series is mid-session, and asking
                // from the next day would leave the rest of that day missing
                // for good.
                from_day = d;
            }
        }
        if (from_day > today) { from_day = today; }

        const auto chunks = altair::chunk_requests(
            altair::date_string(from_day),
            altair::date_string(today), s.interval);
        if (!chunks) {
            std::printf("    spot/%-10s %-4s  could not plan a request\n",
                        s.sym, s.dir);
            ++failed;
            continue;
        }

        // ---- fetch --------------------------------------------------------
        std::vector<altair::RawCandle> got;
        bool ok = true;
        for (std::size_t i = 0; i < chunks->size(); ++i) {
            const auto uri = altair::historical_uri(s.token, s.interval,
                                                    (*chunks)[i], false, false);
            if (!uri) { ok = false; break; }
            const auto r = altair::https_get_auth("api.kite.trade", *uri, auth);
            if (!r || r->status != 200) { ok = false; break; }
            const auto part = altair::parse_candles(r->body);
            if (!part) { ok = false; break; }
            got.insert(got.end(), part->begin(), part->end());
            if (i + 1 < chunks->size()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(400));
            }
        }
        if (!ok) {
            std::printf("    spot/%-10s %-4s  FETCH FAILED\n", s.sym, s.dir);
            ++failed;
            continue;
        }

        // ---- merge --------------------------------------------------------
        std::size_t added = 0, replaced = 0, rounding = 0, partial = 0;
        for (const altair::RawCandle& c : got) {
            if (!bar_is_complete(c.ts_ns, s.ns, now_ns)) {
                ++partial;
                continue;
            }
            const std::string full = altair::format_ist(c.ts_ns);
            const std::string key = bar_key(full, s.ns);
            // Written in the shape the series already uses. Mixing
            // `2026-09-08` and `2026-09-08T00:00:00+05:30` in one file makes
            // every reader's stamp parser the arbiter of what a bar is.
            const std::string stamp =
                (stamp_is_date_only && s.ns >= 86'400'000'000'000LL)
                    ? full.substr(0, 10) : full;

            std::string line = stamp;
            char buf[128];
            std::snprintf(buf, sizeof(buf), ",%g,%g,%g,%g,", c.open, c.high,
                          c.low, c.close);
            line += buf;      // volume left EMPTY: an index reports none

            const auto w = where.find(key);
            const std::string file =
                w != where.end() ? w->second
                : (!single_file.empty() ? single_file
                                        : full.substr(0, 7) + ".csv");
            auto& m = files[file];
            const auto it = m.find(key);
            if (it == m.end()) {
                m[key] = Row{line, c.close};
                where[key] = file;
                if (stamp > last_stamp) { last_stamp = stamp; }
                ++added;
                continue;
            }
            // MATERIAL or DECIMAL PLACES -- see the header.
            const double a = it->second.close, b = c.close;
            const double d = (a > 0.0 && b > 0.0)
                ? std::fabs(10'000.0 * std::log(b / a)) : 0.0;
            if (d > 1.0) {
                it->second = Row{line, b};
                ++replaced;
            } else if (d > 0.0) {
                ++rounding;
            }
        }

        // ---- rewrite the months that changed ------------------------------
        if (added > 0 || replaced > 0) {
            for (const auto& [file, rows] : files) {
                const std::string path = dir + "/" + file;
                const std::string tmp = path + ".tmp";
                std::ofstream f(tmp, std::ios::trunc);
                if (!f) { continue; }
                f << header << "\n";
                for (const auto& [stamp, row] : rows) { f << row.line << "\n"; }
                f.close();
                std::remove(path.c_str());
                std::rename(tmp.c_str(), path.c_str());
            }
        }

        std::printf("    spot/%-10s %-4s  +%-6zu ~%-4zu  (=%zu rounding, "
                    "%zu unfinished)  -> %s\n",
                    s.sym, s.dir, added, replaced, rounding, partial,
                    last_stamp.empty() ? "-" : last_stamp.c_str());
        ++series_done;
        added_total += added;
        replaced_total += replaced;
        rounding_total += rounding;
        partial_total += partial;
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    }

    std::printf("\n  %zu series updated, %zu failed.\n"
                "  %zu bars added, %zu replaced as materially different,\n"
                "  %zu left alone as decimal places, %zu dropped unfinished.\n",
                series_done, failed, added_total, replaced_total,
                rounding_total, partial_total);
    if (replaced_total > 0) {
        std::printf("\n  A REPLACED BAR IS A BAR THAT HAD BEEN CAPTURED\n"
                    "  MID-SESSION AND FROZEN. Worth knowing about, not\n"
                    "  worth alarm -- it is now the settled value.\n");
    }
    return failed == 0 ? 0 : 4;
}
