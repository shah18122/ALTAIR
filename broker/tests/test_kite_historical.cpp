// broker/tests/test_kite_historical.cpp -- P2-12.
//
// THE PARSER HAS NEVER SEEN A REAL KITE RESPONSE, AND THIS FILE SAYS SO.
//
// There are no credentials here and no network, so every fixture below is
// constructed from `research/reference/gokiteconnect/market.go` -- Zerodha's
// own client, which is the authority for the shape. That is much better than a
// fixture invented from memory and it is still not the live API. The first
// real call may find a field this rejects, and when it does the right reaction
// is to fix the parser, not to loosen it: a scanner that skips what it does
// not understand turns a schema change into missing candles.

#include <broker/kite_historical.hpp>

#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %-4s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) { ++failures; }
}

} // namespace

int main() {
    std::printf("P2-12 kite historical\n");

    // ---- the offset trap --------------------------------------------------
    //
    // market.go:236 parses with layout "2006-01-02T15:04:05-0700", so Kite
    // sends "+0530" with NO colon. Every CSV in dataset/ carries "+05:30".
    // Both must land on the same instant or a fetched file and a stored file
    // are different data wearing the same column headers.
    std::int64_t a = 0, b = 0;
    const bool pa = altair::parse_stamp("2026-08-31T09:15:00+0530", a);
    const bool pb = altair::parse_stamp("2026-08-31T09:15:00+05:30", b);
    check(pa && pb, "both offset forms parse");
    check(a == b, "\"+0530\" (Kite) and \"+05:30\" (our CSVs) are the SAME "
                  "instant");

    // 09:15 IST is 03:45 UTC. Asserted against arithmetic, not against the
    // parser's own output -- otherwise the test only proves self-consistency.
    const std::int64_t day = altair::detail::days_from_civil(2026, 8, 31);
    const std::int64_t expect =
        day * 86'400'000'000'000LL + (3 * 3600 + 45 * 60) * 1'000'000'000LL;
    check(a == expect, "09:15 IST is stored as 03:45 UTC");

    check(altair::format_ist(a) == "2026-08-31T09:15:00+05:30",
          "formatting emits the COLON form, matching dataset/");

    check(!altair::parse_stamp("2026-08-31T09:15:00+5:30", a),
          "a malformed offset is refused, not guessed at");
    check(!altair::parse_stamp("2026-13-31T09:15:00+0530", a),
          "month 13 is refused");

    // ---- chunking ---------------------------------------------------------
    //
    // Two years of one-minute at a 60-day cap. The seam is where data is lost:
    // an overlap duplicates candles, a gap drops them.
    const auto ch = altair::chunk_requests("2024-09-01", "2026-08-31",
                                           "minute");
    check(ch.has_value(), "two years of 1-minute chunks");
    if (ch) {
        std::printf("    %zu requests for 2024-09-01 .. 2026-08-31 at "
                    "60 days each\n", ch->size());
        check(ch->size() >= 12 && ch->size() <= 14,
              "about thirteen requests, as 730/60 implies");
        check(ch->front().from == "2024-09-01", "starts where asked");
        check(ch->back().to == "2026-08-31", "ends where asked");

        bool contiguous = true;
        for (std::size_t i = 1; i < ch->size(); ++i) {
            std::int64_t prev_to = 0, cur_from = 0;
            const bool p1 = altair::parse_date((*ch)[i - 1].to, prev_to);
            const bool p2 = altair::parse_date((*ch)[i].from, cur_from);
            if (!p1 || !p2 || cur_from != prev_to + 1) { contiguous = false; }
        }
        check(contiguous,
              "seams are exactly one day apart -- no overlap, no gap");
    }

    // A day-interval request for the same span is ONE call, because the cap is
    // 2000 days. If this ever splits, the caps table has been edited wrongly.
    const auto chd = altair::chunk_requests("2024-09-01", "2026-08-31", "day");
    check(chd && chd->size() == 1, "the same span at daily is a single call");

    check(!altair::chunk_requests("2026-01-01", "2025-01-01", "minute"),
          "a backwards range is refused");
    check(!altair::chunk_requests("2024-09-01", "2026-08-31", "7minute"),
          "an interval Kite does not serve is refused");

    // ---- the URI ----------------------------------------------------------
    const auto uri = altair::historical_uri(
        256265, "minute", altair::Chunk{"2026-07-01", "2026-08-29"}, false,
        false);
    check(uri.has_value(), "URI built");
    if (uri) {
        std::printf("    %s\n", uri->c_str());
        check(uri->find("/instruments/historical/256265/minute") == 0,
              "path is /instruments/historical/:token/:interval");
        check(uri->find("from=2026-07-01") != std::string::npos
                  && uri->find("to=2026-08-29") != std::string::npos,
              "from and to are in the query");
    }
    check(!altair::historical_uri(0, "minute", altair::Chunk{"2026-07-01",
                                                             "2026-08-29"},
                                  false, false),
          "instrument token 0 is refused");

    // ---- the response, six elements: cash has no open interest -----------
    const char* cash =
        R"({"status":"success","data":{"candles":[)"
        R"(["2026-08-31T09:15:00+0530",24500.1,24530.5,24495.0,24520.2,125000],)"
        R"(["2026-08-31T09:16:00+0530",24520.2,24525.0,24510.1,24512.0,98000]]}})";
    const auto cc = altair::parse_candles(cash);
    check(cc.has_value(), "a cash response parses");
    if (cc) {
        check(cc->size() == 2, "two candles");
        check((*cc)[0].open == 24500.1 && (*cc)[0].close == 24520.2,
              "OHLC land in the right fields");
        check((*cc)[0].volume == 125000.0, "volume parsed");
        check(!(*cc)[0].oi_known,
              "SIX elements -> oi is ABSENT, not zero (an index reports none)");
        check((*cc)[1].ts_ns > (*cc)[0].ts_ns, "candles are ascending");
    }

    // ---- seven elements: a derivative carries open interest ---------------
    const char* fut =
        R"({"status":"success","data":{"candles":[)"
        R"(["2026-08-31T09:15:00+0530",24500,24530,24495,24520,125000,4821375]]}})";
    const auto fc = altair::parse_candles(fut);
    check(fc.has_value() && fc->size() == 1, "a derivative response parses");
    if (fc && fc->size() == 1) {
        check((*fc)[0].oi_known && (*fc)[0].oi == 4821375.0,
              "SEVEN elements -> oi is present and read");
    }

    // Volume as a JSON float. market.go:216 reads it as float64 before
    // narrowing, so `12345.0` is legal for the same value and a parser
    // demanding an integer token would reject a valid response.
    const char* vf =
        R"({"status":"success","data":{"candles":[)"
        R"(["2026-08-31T09:15:00+0530",1,2,0.5,1.5,12345.0]]}})";
    const auto vc = altair::parse_candles(vf);
    check(vc && vc->size() == 1 && (*vc)[0].volume == 12345.0,
          "volume arriving as a JSON float is accepted");

    // ---- an error body is not an empty day --------------------------------
    //
    // Kite answers a failed call with HTTP 200 and status "error". Read as
    // data that is an empty candle list, which is indistinguishable from a
    // market holiday -- and a holiday is a legitimate gap that gets skipped.
    const char* err =
        R"({"status":"error","message":"Invalid token","error_type":)"
        R"("TokenException"})";
    const auto ec = altair::parse_candles(err);
    check(!ec && ec.error() == altair::HistError::ApiError,
          "status \"error\" is an ApiError, NOT an empty trading day");

    const char* empty = R"({"status":"success","data":{"candles":[]}})";
    const auto emc = altair::parse_candles(empty);
    check(emc && emc->empty(),
          "an empty candle list from a SUCCESS is a holiday, and parses");

    check(!altair::parse_candles(R"({"status":"success","data":{)"),
          "a truncated document is refused");
    check(!altair::parse_candles(
              R"({"status":"success","data":{"candles":[["2026-08-31T09:15:00+0530",1,2]]}})"),
          "a short candle is refused rather than filled with zeros");

    // ---- coverage: the reason this card is more than a parser -------------
    //
    // Kite TRUNCATES an over-long request instead of refusing it, so a wrong
    // day-cap yields a short list and no error. Coverage is checked against
    // what was ASKED for.
    if (cc) {
        const auto cov = altair::verify_coverage(*cc, "2026-08-31",
                                                 "2026-08-31", 4);
        check(cov.has_value() && *cov == 0,
              "a full single day verifies with a zero gap");

        const auto trunc = altair::verify_coverage(*cc, "2026-07-01",
                                                   "2026-08-31", 4);
        check(!trunc && trunc.error() == altair::HistError::IncompleteCoverage,
              "two candles offered as two MONTHS is caught as incomplete "
              "coverage");
    }
    const std::vector<altair::RawCandle> none;
    check(!altair::verify_coverage(none, "2026-07-01", "2026-08-31", 4),
          "an empty result for a two-month window is incomplete, not a "
          "holiday");

    std::printf("\n  NOTE: no network and no credentials are involved in this "
                "test.\n  Every fixture is built from gokiteconnect's "
                "market.go. The parser has\n  never seen a live Kite response "
                "and that is stated rather than assumed.\n");

    std::printf("\n%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
