// app/kite_fetch_main.cpp -- fill dataset/ from Kite's historical API.
//
// P2-12c. The command Smit asked for: two years of 1-minute NIFTY.
//
// IT DRY-RUNS BY DEFAULT AND THAT IS NOT TIMIDITY.
//
// This program has never made a real call. `https_get_auth` was written for it
// and has never been exercised against the live API either, because there are
// no credentials on the test path. Untested network code, carrying a live
// trading credential, in a loop, against an API with rate limits, is exactly
// the shape of thing that should not run the first time by accident.
//
// So the default prints every request it WOULD make and exits. `--go` is
// required to send anything. The first real run should be one instrument and
// one month, checked by eye, before anyone asks it for two years.
//
// WHAT IT REFUSES TO DO.
//
//   * It never writes a partial file. Chunks are fetched, joined, and
//     coverage-checked FIRST; a window that came back short is reported and
//     nothing is written. A CSV that is quietly missing three weeks is worse
//     than no CSV, because the next reader has no way to tell.
//   * It never overwrites without --force. dataset/ is regenerable but it is
//     not cheap to regenerate, and a fetch that silently replaced a good file
//     with a truncated one would be the same failure as above with an extra
//     step.
//   * It never prints the access token. Not in a URL, not in an error, not in
//     a "here is what I sent" diagnostic.
//
// RATE LIMIT. Kite's historical endpoint is documented around 3 requests a
// second. This sleeps 400 ms between calls -- deliberately slower than the
// cap, because the cost of being slightly slow is seconds and the cost of
// being slightly fast is a ban.

#include <broker/https_client.hpp>
#include <broker/kite_historical.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

/// Read `access_token` out of data/kite_session.json.
///
/// A hand-rolled field scan rather than a JSON parser: the file is written by
/// altair_kite_login and has a fixed shape, and pulling in a parser for one
/// string would widen what touches a credential for no benefit.
[[nodiscard]] bool read_access_token(const char* path, std::string& out) {
    std::ifstream f(path);
    if (!f) { return false; }
    std::string all((std::istreambuf_iterator<char>(f)),
                    std::istreambuf_iterator<char>());
    const std::string key = "\"access_token\"";
    const std::size_t k = all.find(key);
    if (k == std::string::npos) { return false; }
    std::size_t i = all.find(':', k + key.size());
    if (i == std::string::npos) { return false; }
    ++i;
    while (i < all.size() && (all[i] == ' ' || all[i] == '\t')) { ++i; }
    if (i >= all.size() || all[i] != '"') { return false; }
    ++i;
    const std::size_t start = i;
    while (i < all.size() && all[i] != '"') { ++i; }
    if (i >= all.size() || i == start) { return false; }
    out = all.substr(start, i - start);
    return true;
}

/// Same shape as kite_login_main.cpp's. MSVC deprecates getenv and gate 1 is
/// zero warnings, so the platform split is explicit rather than suppressed
/// with _CRT_SECURE_NO_WARNINGS -- which would silence every other instance of
/// the check too.
[[nodiscard]] std::string env_or_empty(const char* name) {
#if defined(_MSC_VER)
    char* buf = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&buf, &len, name) != 0 || buf == nullptr) { return {}; }
    std::string out{buf};
    std::free(buf);
    return out;
#else
    const char* v = std::getenv(name);
    return v != nullptr ? std::string{v} : std::string{};
#endif
}

[[nodiscard]] const char* arg_value(int argc, char** argv, const char* name) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) { return argv[i + 1]; }
    }
    return nullptr;
}

[[nodiscard]] bool has_flag(int argc, char** argv, const char* name) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) { return true; }
    }
    return false;
}

void usage(const char* exe) {
    std::printf(
        "  Fill dataset/ from Kite's historical candle API.\n\n"
        "    %s --token <instrument_token> --out <dir> [options]\n\n"
        "    --token N        Kite instrument_token (NIFTY 50 spot is 256265)\n"
        "    --out DIR        e.g. dataset/spot/nifty/1m\n"
        "    --interval S     minute | 3minute | 5minute | 15minute |\n"
        "                     30minute | 60minute | day     (default minute)\n"
        "    --from D --to D  YYYY-MM-DD (default: the last 2 years to --to)\n"
        "    --oi             ask for open interest (derivatives only)\n"
        "    --volume-absent  this instrument reports no volume (an INDEX).\n"
        "                     Writes an empty field rather than a zero.\n"
        "    --volume-zero    0 really is a measurement here.\n"
        "    --continuous     DERIVATIVES only: stitch the near-month series\n"
        "                     back across contract rolls. A different series,\n"
        "                     not a longer one — the step at each roll is a\n"
        "                     contract change, not a market move.\n\n"
        "    --dump-instruments PATH   download the whole instrument master\n"
        "                     (a CSV) and stop. Needed to look up a token.\n"
        "    --go             ACTUALLY CALL THE API. Without it this prints\n"
        "                     the requests it would make and exits.\n"
        "    --force          overwrite existing files\n\n"
        "  Needs data/kite_session.json, which altair_kite_login writes.\n"
        "  The access token is read from there and never printed.\n", exe);
}

} // namespace

int main(int argc, char** argv) {
    // ---- --dump-instruments: the whole master, as Kite serves it ---------
    //
    // P2-12e. Needed before anything else can be fetched: a historical request
    // takes an instrument_token, and rule 1 says a token is not something you
    // write as a literal. `instruments/kite_dump.hpp` already PARSES this file
    // (P1-04); nothing could download it.
    //
    // Written to disk untouched so the existing parser and the three-way
    // reconciliation see exactly what Kite sent, rather than something this
    // program decided to keep.
    if (const char* dump = arg_value(argc, argv, "--dump-instruments")) {
        std::string access;
        if (!read_access_token("data/kite_session.json", access)) {
            std::printf("  no usable data/kite_session.json -- run "
                        "altair_kite_login first\n");
            return 1;
        }
        const std::string api_key = env_or_empty("ALTAIR_KITE_API_KEY");
        if (api_key.empty()) {
            std::printf("  ALTAIR_KITE_API_KEY is not set\n");
            return 1;
        }
        const std::string auth =
            std::string("token ") + api_key + ":" + access;
        std::printf("  GET https://api.kite.trade/instruments\n");
        const auto r = altair::https_get_auth("api.kite.trade", "/instruments",
                                              auth, "3",
                                              std::chrono::seconds{60});
        if (!r) {
            std::printf("  TRANSPORT FAILED\n");
            return 1;
        }
        if (r->status != 200) {
            std::printf("  HTTP %d\n    %s\n", r->status, r->body.c_str());
            return 1;
        }
        // ---- PARSE-VERIFY, THEN ATOMIC REPLACE (P1-08b) -----------------
        //
        // The first version wrote straight over the target after checking only
        // that the body started with "instrument_token". That is the same
        // defect the candle fetcher was built to avoid and I did not apply
        // here: a truncated download, a proxy's HTML error page, or a half
        // response replaces a good master with a broken one, and the next
        // reader has no way to tell.
        //
        // So: verify FIRST, write to a sibling temp, then rename. The rename
        // is atomic on NTFS and on POSIX, so the target is either the old file
        // or the whole new one and never a partial write.
        if (r->body.rfind("instrument_token", 0) != 0) {
            std::printf("  REFUSED: the body does not start with "
                        "\"instrument_token\", so it is not the instrument "
                        "master.\n  Nothing written.\n");
            return 1;
        }
        // Header field count, then every row against it. A truncated CSV ends
        // mid-row, which shows up as a short final line rather than as
        // anything the header check could catch.
        const std::size_t hdr_end = r->body.find('\n');
        if (hdr_end == std::string::npos) {
            std::printf("  REFUSED: one line only, so no rows.\n");
            return 1;
        }
        std::size_t want_fields = 1;
        for (std::size_t i = 0; i < hdr_end; ++i) {
            if (r->body[i] == ',') { ++want_fields; }
        }
        std::size_t rows = 0, bad = 0;
        std::size_t line_start = hdr_end + 1;
        while (line_start < r->body.size()) {
            std::size_t e = r->body.find('\n', line_start);
            if (e == std::string::npos) { e = r->body.size(); }
            std::size_t len = e - line_start;
            if (len > 0 && r->body[line_start + len - 1] == '\r') { --len; }
            if (len > 0) {
                std::size_t f = 1;
                for (std::size_t i = 0; i < len; ++i) {
                    if (r->body[line_start + i] == ',') { ++f; }
                }
                if (f != want_fields) { ++bad; }
                ++rows;
            }
            line_start = e + 1;
        }
        if (rows == 0 || bad > 0) {
            std::printf("  REFUSED: %zu rows, %zu with a field count other "
                        "than the header's %zu.\n  The existing %s is "
                        "UNTOUCHED.\n", rows, bad, want_fields, dump);
            return 1;
        }

        const std::string tmp = std::string(dump) + ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) {
                std::printf("  cannot write %s\n", tmp.c_str());
                return 1;
            }
            out << r->body;
            if (!out) {
                std::printf("  write to %s failed; %s is UNTOUCHED\n",
                            tmp.c_str(), dump);
                return 1;
            }
        }
        std::error_code rn;
        std::filesystem::rename(tmp, dump, rn);
        if (rn) {
            // Windows refuses rename onto an existing file on some paths;
            // remove and retry rather than leaving the temp behind.
            std::filesystem::remove(dump, rn);
            std::filesystem::rename(tmp, dump, rn);
        }
        if (rn) {
            std::printf("  could not replace %s (%s); the download is in %s\n",
                        dump, rn.message().c_str(), tmp.c_str());
            return 1;
        }
        std::printf("  wrote %s -- %zu bytes, %zu rows, %zu fields each, "
                    "verified then renamed\n",
                    dump, r->body.size(), rows, want_fields);
        return 0;
    }

    const char* token_s = arg_value(argc, argv, "--token");
    const char* out_dir = arg_value(argc, argv, "--out");
    if (token_s == nullptr || out_dir == nullptr) {
        usage(argv[0]);
        return 2;
    }
    const char* iv = arg_value(argc, argv, "--interval");
    const std::string interval = iv != nullptr ? iv : "minute";
    const bool go = has_flag(argc, argv, "--go");
    const bool force = has_flag(argc, argv, "--force");
    const bool want_oi = has_flag(argc, argv, "--oi");
    // CONTINUOUS: for a DERIVATIVE only, and it changes what the series means.
    //
    // A futures contract lives about three months and then expires. Asked
    // without this, Kite returns that one contract's own life and nothing
    // before it -- so a "10-year NIFTY futures history" fetched flat is
    // actually one quarter, and it looks like a full answer.
    //
    // With it, Kite stitches the near-month series back across rolls. That is
    // a DIFFERENT SERIES, not a longer one: it splices contracts at each roll,
    // so the price step across a roll is a contract change and not a market
    // move. Anything measuring returns across that boundary is measuring the
    // basis. The flag is explicit for that reason and is meaningless on cash.
    const bool continuous = has_flag(argc, argv, "--continuous");
    // See the all-zero-volume refusal below for why these exist and why
    // neither has a default.
    const bool vol_absent = has_flag(argc, argv, "--volume-absent");
    const bool vol_zero = has_flag(argc, argv, "--volume-zero");

    const std::int64_t token = std::atoll(token_s);
    if (token <= 0) {
        std::printf("  --token must be a positive instrument_token\n");
        return 2;
    }
    if (altair::interval_by_name(interval) == nullptr) {
        std::printf("  --interval \"%s\" is not one Kite serves\n",
                    interval.c_str());
        return 2;
    }

    const char* to_s = arg_value(argc, argv, "--to");
    const char* from_s = arg_value(argc, argv, "--from");
    std::string to = to_s != nullptr ? to_s : "";
    std::string from = from_s != nullptr ? from_s : "";
    if (to.empty() || from.empty()) {
        // Default: two years ending today. Computed from the system clock,
        // which is the ONE place a wall clock is legitimate -- this is an
        // operator tool choosing a fetch window, not a strategy reading time.
        const auto now = std::chrono::system_clock::now();
        const auto days = std::chrono::duration_cast<std::chrono::hours>(
                              now.time_since_epoch()).count() / 24;
        if (to.empty()) { to = altair::date_string(days); }
        if (from.empty()) { from = altair::date_string(days - 730); }
    }

    // CONTINUOUS IS DAY-ONLY, AND KITE SAYS SO WITH A 400.
    //
    // Measured: `--continuous` on 60minute returns HTTP 400 "invalid interval
    // for continuous data". Catching it here costs nothing and saves a round
    // trip; more to the point it names the CONSEQUENCE, which the API's
    // message does not -- without continuous, an intraday futures request
    // returns only the life of that one contract, about three months, and a
    // "ten-year history" fetched that way is one quarter that looks like a
    // full answer.
    if (continuous && interval != "day") {
        std::printf(
            "  REFUSED: --continuous works only with --interval day.\n"
            "  Kite answers anything else with 400 \"invalid interval for "
            "continuous data\".\n\n"
            "  Without it an intraday request returns ONLY the life of this\n"
            "  one contract -- about three months -- which is a real answer\n"
            "  and not the one you asked for. For a longer intraday futures\n"
            "  history you need each expired contract's own token, and the\n"
            "  current master does not carry them.\n");
        return 2;
    }

    const auto chunks = altair::chunk_requests(from, to, interval);
    if (!chunks) {
        std::printf("  refused: bad range or interval\n");
        return 2;
    }

    std::printf("  instrument %lld, %s, %s .. %s\n", static_cast<long long>(token),
                interval.c_str(), from.c_str(), to.c_str());
    std::printf("  %zu requests, 400 ms apart\n\n", chunks->size());

    if (!go) {
        for (const auto& c : *chunks) {
            const auto u = altair::historical_uri(token, interval, c,
                                                  continuous,
                                                  want_oi);
            std::printf("    GET https://api.kite.trade%s\n",
                        u ? u->c_str() : "<refused>");
        }
        std::printf(
            "\n  DRY RUN. Nothing was sent and nothing was written.\n"
            "  Add --go to make these calls for real.\n\n"
            "  This program and the GET beneath it have never made a live\n"
            "  call. Run ONE month first and look at the file before asking\n"
            "  for two years.\n");
        return 0;
    }

    std::string access;
    if (!read_access_token("data/kite_session.json", access)) {
        std::printf("  no usable data/kite_session.json.\n"
                    "  Run altair_kite_login first -- the browser step is "
                    "yours, not this program's.\n");
        return 1;
    }
    const std::string api_key = env_or_empty("ALTAIR_KITE_API_KEY");
    if (api_key.empty()) {
        std::printf("  ALTAIR_KITE_API_KEY is not set.\n");
        return 1;
    }
    // "token <api_key>:<access_token>". Built once, passed by reference, and
    // never printed -- not even redacted, because a redacted credential in a
    // log is still a credential-shaped thing somebody screenshots.
    const std::string auth =
        std::string("token ") + api_key + ":" + access;

    std::vector<altair::RawCandle> all;
    for (std::size_t i = 0; i < chunks->size(); ++i) {
        const auto& c = (*chunks)[i];
        const auto u = altair::historical_uri(token, interval, c,
                                              continuous,
                                              want_oi);
        if (!u) {
            std::printf("  refused building URI for %s..%s\n", c.from.c_str(),
                        c.to.c_str());
            return 1;
        }
        std::printf("  [%zu/%zu] %s .. %s ", i + 1, chunks->size(),
                    c.from.c_str(), c.to.c_str());
        std::fflush(stdout);

        const auto r = altair::https_get_auth("api.kite.trade", *u, auth);
        if (!r) {
            std::printf("TRANSPORT FAILED\n");
            return 1;
        }
        if (r->status != 200) {
            // The body carries Kite's machine-readable reason. Printed
            // because it is the only way to tell "token expired" from "no
            // historical subscription" -- and it contains no credential.
            std::printf("HTTP %d\n    %s\n", r->status, r->body.c_str());
            return 1;
        }
        const auto candles = altair::parse_candles(r->body);
        if (!candles) {
            std::printf("UNPARSEABLE\n");
            return 1;
        }
        std::printf("%zu candles\n", candles->size());
        all.insert(all.end(), candles->begin(), candles->end());

        if (i + 1 < chunks->size()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{400});
        }
    }

    // COVERAGE BEFORE WRITING. A short answer is Kite's way of saying the
    // window was too long, and it is not an error -- so it has to be caught
    // here or it becomes a file with a hole in it.
    //
    // 5 days: a weekend plus a holiday is normal, a working week missing is
    // not.
    const auto gap = altair::verify_coverage(all, from, to, 5);
    if (!gap) {
        std::printf("\n  INCOMPLETE COVERAGE. %zu candles came back for "
                    "%s..%s, with a gap\n  longer than 5 days or a window "
                    "that does not reach the ends.\n"
                    "  NOTHING WAS WRITTEN. Narrow the range and look at what "
                    "comes back.\n",
                    all.size(), from.c_str(), to.c_str());
        return 1;
    }
    std::printf("\n  %zu candles, largest gap %lld days\n", all.size(),
                static_cast<long long>(*gap));

    // ---- AN INDEX HAS NO VOLUME, AND 0 IS NOT HOW YOU SAY THAT ----------
    //
    // NIFTY 50 is an index. It does not trade, so Kite reports volume 0 on
    // every candle -- measured: 7,875 of 7,875 for August 2026. Writing a
    // literal 0 asserts "no trading happened in this minute", which is a claim
    // about the MARKET. The truth is "this instrument does not report volume",
    // which is a claim about the FEED.
    //
    // desktop/data/bar_csv.hpp already separates those -- zero_volume_rows
    // against absent_volume_rows -- and P11Q-06 had to unpick exactly this
    // confusion once, when 528 India VIX rows loaded as ZERO bars because an
    // empty volume field read as a parse error. An empty field is how the rest
    // of dataset/ says "not reported".
    //
    // Kite returns 0 for both cases and cannot distinguish them, so this
    // REFUSES rather than guessing (rule 9). The operator knows whether the
    // token is an index; the program does not.
    bool any_volume = false;
    for (const altair::RawCandle& c : all) {
        if (c.volume != 0.0) { any_volume = true; break; }
    }
    if (vol_absent && vol_zero) {
        std::printf("  --volume-absent and --volume-zero contradict each other.\n");
        return 2;
    }
    if (!any_volume && !vol_absent && !vol_zero) {
        std::printf(
            "\n  ALL %zu CANDLES HAVE VOLUME 0. NOTHING WAS WRITTEN.\n\n"
            "  That is what Kite returns for an INDEX -- an index does not\n"
            "  trade, so it has no volume to report. It is also what a\n"
            "  genuinely untraded contract looks like, and the API cannot tell\n"
            "  you which this is.\n\n"
            "  Writing 0 would assert \"no trading in this minute\", a claim\n"
            "  about the market. \"Not reported\" is a claim about the feed.\n"
            "  dataset/ says the second with an EMPTY field, and bar_csv.hpp\n"
            "  counts the two separately.\n\n"
            "  Pick one:\n"
            "    --volume-absent   an index, or a feed reporting none. Writes\n"
            "                      an empty field. This is what token 256265\n"
            "                      (NIFTY 50) needs.\n"
            "    --volume-zero     you know 0 is a real measurement here.\n",
            all.size());
        return 1;
    }

    // One file per month, matching what dataset/ already holds.
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    std::size_t written = 0, skipped = 0;
    std::string cur_month;
    std::ofstream f;
    for (const altair::RawCandle& c : all) {
        const std::string stamp = altair::format_ist(c.ts_ns);
        const std::string month = stamp.substr(0, 7);      // YYYY-MM
        if (month != cur_month) {
            if (f.is_open()) { f.close(); }
            const std::string path =
                std::string(out_dir) + "/" + month + ".csv";
            if (!force && std::filesystem::exists(path)) {
                cur_month = month;
                ++skipped;
                continue;
            }
            f.open(path, std::ios::trunc);
            if (!f) {
                std::printf("  cannot write %s\n", path.c_str());
                return 1;
            }
            f << "time,open,high,low,close,volume";
            if (want_oi) { f << ",oi"; }
            f << "\n";
            cur_month = month;
            ++written;
        }
        if (!f.is_open()) { continue; }              // skipped month
        f << stamp << ',' << c.open << ',' << c.high << ',' << c.low << ','
          << c.close << ',';
        // Empty field, not 0, when the instrument reports no volume.
        if (!vol_absent) { f << static_cast<long long>(c.volume); }
        if (want_oi) {
            // Absent OI writes an EMPTY field, not a zero. bar_csv.hpp already
            // distinguishes those for volume and the same rule applies here.
            f << ',';
            if (c.oi_known) { f << static_cast<long long>(c.oi); }
        }
        f << "\n";
    }
    if (f.is_open()) { f.close(); }

    std::printf("  wrote %zu month files to %s", written, out_dir);
    if (skipped > 0) {
        std::printf(", SKIPPED %zu that already existed (use --force)",
                    skipped);
    }
    std::printf("\n");
    return 0;
}
