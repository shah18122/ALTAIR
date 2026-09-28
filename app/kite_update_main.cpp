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
//
// CX02-A2. THE MERGE NOW LIVES IN app/dataset_merge.hpp, WHERE IT IS TESTED.
//
// It used to live in this main(), untested, and the audit found it could lose
// a stored month five different ways (C14-005), merged windows Kite had
// truncated without the coverage check the fetcher applies (C14-006), and
// wrote prices at six significant digits (C14-001). The rules above -- merge,
// 1 bp, unfinished bars dropped -- are unchanged. What changed is how a file
// is replaced, which files are replaced, and what is refused.

#include <app/dataset_merge.hpp>
#include <broker/https_client.hpp>
#include <broker/kite_historical.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
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

struct SeriesPlan {
    const char* sym;        // dataset directory
    std::int64_t token;     // Kite instrument_token
    const char* dir;        // interval directory
    const char* interval;   // Kite interval name
    std::int64_t ns;        // bar width in nanoseconds
};

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

namespace ds = altair::dataset;

int main(int argc, char** argv)
{
    if (has_flag(argc, argv, "--help")) { usage(argv[0]); return 0; }

    const char* out_s = arg_value(argc, argv, "--out");
    const char* syms_s = arg_value(argc, argv, "--symbols");
    const char* days_s = arg_value(argc, argv, "--days");
    const bool go = has_flag(argc, argv, "--go");
    const std::string root = out_s != nullptr ? out_s : "dataset";
    int back = 400;
    if (days_s != nullptr) {
        // C14-016: atoi read "40x" as 40 and "x" as 0.
        const auto d = ds::parse_count(days_s, 1, 20'000);
        if (!d) {
            std::printf("  --days must be a whole number from 1 to 20000\n");
            return 2;
        }
        back = *d;
    }

    struct Sym { const char* name; std::int64_t token; };
    const Sym kAll[] = {{"nifty", 256265}, {"banknifty", 260105},
                        {"indiavix", 264969}};
    std::vector<Sym> syms;
    {
        // EXACT names. A substring match made `--symbols banknifty` update
        // nifty as well (C14-016).
        std::vector<std::string_view> names;
        for (const Sym& s : kAll) { names.emplace_back(s.name); }
        const auto sel = ds::select_names(
            syms_s != nullptr ? syms_s : "nifty,banknifty,indiavix", names);
        if (!sel.unknown.empty()) {
            std::printf("  --symbols: \"%s\" is not one of nifty, banknifty, "
                        "indiavix\n", sel.unknown.front().c_str());
            return 2;
        }
        for (const std::size_t i : sel.picked) { syms.push_back(kAll[i]); }
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

    std::vector<SeriesPlan> plan;
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
        for (const SeriesPlan& s : plan) {
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

    for (const SeriesPlan& s : plan) {
        const std::string dir = root + "/spot/" + s.sym + "/" + s.dir;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            std::printf("    spot/%-10s %-4s  COULD NOT CREATE SERIES DIRECTORY; "
                        "nothing merged\n", s.sym, s.dir);
            ++failed;
            continue;
        }
        // Hold one lock across the complete read -> fetch -> merge -> replace
        // sequence. Atomic file renames alone still allow two updater
        // processes to read the same old series and have the last writer drop
        // the other process's newly merged bars.
        ds::UpdateStateLock series_lock;
        if (!series_lock.acquire(
                std::filesystem::path(dir) / ".kite_update_series.lock")) {
            std::printf("    spot/%-10s %-4s  SERIES UPDATE LOCK FAILED; "
                        "another updater may be active; nothing merged\n",
                        s.sym, s.dir);
            ++failed;
            continue;
        }

        // ---- what is already here, and how far it reaches ----------------
        //
        // `all.csv` versus month partitions is detected, not assumed: nifty/1d
        // is a single all.csv and nifty/1m is monthly, and the first version
        // wrote month files into both.
        ds::Series store;
        bool unreadable = false;
        ec.clear();
        for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
            if (!e.is_regular_file()) { continue; }
            const std::string name = e.path().filename().string();
            if (name.size() < 5
                || name.compare(name.size() - 4, 4, ".csv") != 0) {
                continue;
            }
            std::ifstream in(e.path());
            if (!in) {
                unreadable = true;
                break;
            }
            ds::load_file(store, name, in, s.ns);
            if (in.bad()) {
                unreadable = true;
                break;
            }
        }
        if (ec || unreadable) {
            // An unreadable stored file is not an empty one: merging without
            // it would put its bars into a second file beside it.
            std::printf("    spot/%-10s %-4s  COULD NOT READ what is stored; "
                        "nothing merged\n", s.sym, s.dir);
            ++failed;
            continue;
        }

        // ---- the window to ask for ---------------------------------------
        // From the last STORED day, not the day after: the last bar of a
        // partitioned intraday series is mid-session, and asking from the
        // next day would leave the rest of that day missing for good.
        const auto from_day_result =
            ds::update_from_day(store.last_stamp, today, back);
        if (!from_day_result) {
            if (from_day_result.error() == ds::UpdateWindowError::FutureLastStamp) {
                // CX02-A2c (R-AB-028). Clamping a future stamp used to create
                // a zero-width query that returned empty and reported success
                // forever. Refuse the corrupt series visibly.
                std::printf(
                    "    spot/%-10s %-4s  STORED ROW DATED IN THE FUTURE "
                    "(%s); refusing rather than asking for a zero-width "
                    "window\n", s.sym, s.dir, store.last_stamp.c_str());
            } else {
                std::printf(
                    "    spot/%-10s %-4s  INVALID STORED LAST STAMP (%s); "
                    "nothing merged\n", s.sym, s.dir,
                    store.last_stamp.empty() ? "empty" : store.last_stamp.c_str());
            }
            ++failed;
            continue;
        }
        const std::int64_t from_day = *from_day_result;
        const std::string from_s = altair::date_string(from_day);
        const std::string to_s = altair::date_string(today);

        const auto chunks = altair::chunk_requests(from_s, to_s, s.interval);
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

        // ---- did Kite answer the whole window? (C14-006) ------------------
        //
        // Kite truncates an over-long request instead of refusing it, so a
        // short answer is not an error on the wire. The fetcher has always
        // refused one; this merged it, and the hole became permanent.
        const ds::Coverage cov = ds::judge_coverage(got, from_s, to_s, 5);
        const ds::EmptyAnswerEvent empty_event = !got.empty()
            ? ds::EmptyAnswerEvent::NonemptyResponse
            : cov == ds::Coverage::Incomplete
                ? ds::EmptyAnswerEvent::TradingWindowEmpty
                : ds::EmptyAnswerEvent::NoTradingSession;
        const auto empty_streak = ds::update_empty_answer_streak(
            std::filesystem::path(dir) / ".kite_update_empty_answers",
            empty_event);
        if (!empty_streak) {
            std::printf("    spot/%-10s %-4s  EMPTY-RESPONSE STATE FAILED: %s; "
                        "NOTHING MERGED\n", s.sym, s.dir,
                        ds::to_string(empty_streak.error()));
            ++failed;
            continue;
        }
        if (cov == ds::Coverage::Incomplete || cov == ds::Coverage::Malformed) {
            std::printf("    spot/%-10s %-4s  %s for %s..%s (%zu candles, "
                        "%llu consecutive empty response(s)); NOTHING MERGED\n",
                        s.sym, s.dir,
                        cov == ds::Coverage::Incomplete ? "INCOMPLETE COVERAGE"
                                                        : "MALFORMED ANSWER",
                        from_s.c_str(), to_s.c_str(), got.size(),
                        static_cast<unsigned long long>(*empty_streak));
            ++failed;
            continue;
        }

        // ---- merge --------------------------------------------------------
        std::size_t added = 0, replaced = 0, rounding = 0, partial = 0;
        std::size_t blocked = 0, bad_price = 0, unusable = 0;
        for (const altair::RawCandle& c : got) {
            if (!ds::bar_is_complete(c.ts_ns, s.ns, now_ns)) {
                ++partial;
                continue;
            }
            // Exact prices (C14-001). Volume is left EMPTY: an index reports
            // none.
            std::string prices;
            if (!ds::append_prices(prices, c)) {
                ++bad_price;
                continue;
            }
            switch (ds::merge_bar(store, altair::format_ist(c.ts_ns), prices,
                                  c.close, s.ns)) {
            case ds::MergeResult::Added:       ++added; break;
            case ds::MergeResult::Replaced:    ++replaced; break;
            case ds::MergeResult::Rounding:    ++rounding; break;
            case ds::MergeResult::Same:        break;
            case ds::MergeResult::BlockedFile: ++blocked; break;
            case ds::MergeResult::Unusable:    ++unusable; break;
            }
        }
        if (bad_price > 0 || blocked > 0 || unusable > 0) {
            // Rule 9. A NaN price is a malformed answer, and a bar that
            // belongs in a file this reader cannot fully key cannot be merged
            // without dropping the rows it could not key. Neither is written
            // around.
            std::printf("    spot/%-10s %-4s  %zu unwritable price(s), %zu "
                        "bar(s) for a file with unkeyable or duplicate rows, "
                        "%zu with an unusable close; NOTHING WRITTEN\n",
                        s.sym, s.dir, bad_price, blocked, unusable);
            ++failed;
            continue;
        }

        // ---- write back ONLY the files that changed (C14-005) -------------
        std::size_t write_failed = 0;
        for (const auto& [file, f] : store.files) {
            if (!f.dirty) { continue; }
            const auto r = ds::replace_file_checked(
                std::filesystem::path(dir) / file, ds::render(f));
            if (!r) {
                std::printf("    spot/%-10s %-4s  WRITE FAILED for %s: %s -- "
                            "that file is unchanged on disk\n", s.sym, s.dir,
                            file.c_str(), ds::to_string(r.error()));
                ++write_failed;
            }
        }

        std::printf("    spot/%-10s %-4s  +%-6zu ~%-4zu  (=%zu rounding, "
                    "%zu unfinished)  -> %s\n",
                    s.sym, s.dir, added, replaced, rounding, partial,
                    store.last_stamp.empty() ? "-" : store.last_stamp.c_str());
        if (store.cross_file_duplicates > 0) {
            std::printf("      note: %zu bar(s) are stored in two files; left "
                        "as they are\n", store.cross_file_duplicates);
        }
        if (write_failed > 0) {
            ++failed;
        } else {
            ++series_done;
        }
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
