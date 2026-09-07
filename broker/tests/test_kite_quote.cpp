// broker/tests/test_kite_quote.cpp -- P20-03.
//
// Parsed against a body byte-for-byte in the shape Kite sends, taken from the
// mock responses in Zerodha's own Go client rather than invented. A parser
// tested only against a body I wrote is a parser tested against my
// assumptions.
//
// The findings this file exists to protect:
//
//   1. A ZEROED DEPTH SLOT IS NOT LIQUIDITY. Kite always sends five levels;
//      a thin book has fewer real ones.
//   2. ABSENT IS NOT EMPTY. A symbol Kite did not return must not come back as
//      a Quote with a bid of zero.
//   3. ROUNDING, NOT TRUNCATION. Truncating rupees to paise biases the SPREAD.

#include <broker/kite_quote.hpp>

#include <cstdio>
#include <string>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %-4s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) { ++failures; }
}

/// A two-instrument response. NIFTY 50 is an INDEX -- no depth, no OI. The
/// future has a full book with a zero-filled tail, which is the case that
/// matters.
const char* kBody = R"({"status":"success","data":{
"NSE:NIFTY 50":{"instrument_token":256265,"timestamp":"2026-09-07 15:29:59",
 "last_price":24150.25,"last_quantity":0,"average_price":0,"volume":0,
 "buy_quantity":0,"sell_quantity":0,
 "ohlc":{"open":24080.10,"high":24199.95,"low":24051.55,"close":24102.40},
 "net_change":47.85,"lower_circuit_limit":0,"upper_circuit_limit":0,
 "depth":{"buy":[{"price":0,"quantity":0,"orders":0},
                 {"price":0,"quantity":0,"orders":0},
                 {"price":0,"quantity":0,"orders":0},
                 {"price":0,"quantity":0,"orders":0},
                 {"price":0,"quantity":0,"orders":0}],
          "sell":[{"price":0,"quantity":0,"orders":0},
                  {"price":0,"quantity":0,"orders":0},
                  {"price":0,"quantity":0,"orders":0},
                  {"price":0,"quantity":0,"orders":0},
                  {"price":0,"quantity":0,"orders":0}]}},
"NFO:NIFTY26SEPFUT":{"instrument_token":17505794,
 "timestamp":"2026-09-07 15:29:58","last_price":24188.75,"last_quantity":50,
 "average_price":24160.35,"volume":9284150,"buy_quantity":118300,
 "sell_quantity":96450,
 "ohlc":{"open":24120.00,"high":24240.00,"low":24090.05,"close":24140.60},
 "net_change":48.15,"oi":13284500,"oi_day_high":13400000,"oi_day_low":13100000,
 "lower_circuit_limit":21725.55,"upper_circuit_limit":26555.65,
 "depth":{"buy":[{"price":24188.55,"quantity":300,"orders":4},
                 {"price":24188.40,"quantity":150,"orders":2},
                 {"price":24188.25,"quantity":75,"orders":1},
                 {"price":0,"quantity":0,"orders":0},
                 {"price":0,"quantity":0,"orders":0}],
          "sell":[{"price":24188.95,"quantity":225,"orders":3},
                  {"price":24189.10,"quantity":100,"orders":2},
                  {"price":0,"quantity":0,"orders":0},
                  {"price":0,"quantity":0,"orders":0},
                  {"price":0,"quantity":0,"orders":0}]}}}})";

} // namespace

int main() {
    using altair::kite::QuoteError;
    using altair::kite::parse_quote;
    using altair::kite::quote_target;

    std::printf("P20-03 the /quote parser\n");

    // ---- 1. THE FUTURE: A REAL BOOK ---------------------------------------
    {
        const auto q = parse_quote(kBody, "NFO:NIFTY26SEPFUT");
        check(q.has_value(), "the futures quote parses");
        if (!q) { return 1; }

        std::printf("    token %u  last %lld paise  volume %lld\n",
                    q->instrument_token,
                    static_cast<long long>(q->last_price.raw()),
                    static_cast<long long>(q->volume));
        check(q->instrument_token == 17505794u,
              "the instrument token comes off the right instrument -- the "
              "scan is bounded to this symbol's object, so a later symbol's "
              "fields cannot be read as this one's");
        check(q->last_price.raw() == 2418875,
              "24188.75 rupees becomes 2,418,875 PAISE at the boundary, once, "
              "because rule 3 says money is integer paise and the wire format "
              "is a float");

        // THE TOUCH. What P11Q-11's empty columns were waiting for.
        check(q->has_touch(), "it has a bid AND an ask");
        std::printf("    bid %lld x %lld   ask %lld x %lld   spread %lld p\n",
                    static_cast<long long>(q->buy[0].price.raw()),
                    static_cast<long long>(q->buy[0].quantity),
                    static_cast<long long>(q->sell[0].price.raw()),
                    static_cast<long long>(q->sell[0].quantity),
                    static_cast<long long>(q->spread_paise()));
        check(q->spread_paise() == 40,
              "and the spread is 40 paise -- a real number, which no amount "
              "of historical closes could ever have produced: a candle "
              "carries one trade on whichever side lifted");

        // ---- THE ZEROED TAIL ------------------------------------------
        int real_buy = 0, real_sell = 0;
        for (int i = 0; i < 5; ++i) {
            if (q->buy[i].populated) { ++real_buy; }
            if (q->sell[i].populated) { ++real_sell; }
        }
        std::printf("    depth levels actually populated: %d buy, %d sell "
                    "(of 5 sent)\n", real_buy, real_sell);
        check(real_buy == 3 && real_sell == 2,
              "Kite sent five levels a side and only 3 and 2 are REAL -- the "
              "rest are zero-filled, and drawing a zeroed slot as a price is "
              "infinite liquidity at the best possible price (P11Q-03)");
        check(!q->buy[3].populated && q->buy[3].price.raw() == 0,
              "the tail slots carry zero AND say they are unpopulated, so a "
              "caller cannot lose the distinction by reading price alone");

        check(q->has_oi && q->oi == 13284500,
              "a derivative carries open interest, and `has_oi` records that "
              "it was PRESENT rather than leaving 0 to mean two things");
        check(q->upper_circuit.raw() == 2655565
                  && q->lower_circuit.raw() == 2172555,
              "circuit limits parse -- the band an order is rejected outside "
              "of, which a pre-trade check needs and cannot compute");

        const double micro = q->microprice();
        std::printf("    microprice %.2f paise (mid would be %.2f)\n",
                    micro,
                    0.5 * static_cast<double>(q->buy[0].price.raw()
                                              + q->sell[0].price.raw()));
        check(micro > static_cast<double>(q->buy[0].price.raw())
                  && micro < static_cast<double>(q->sell[0].price.raw()),
              "the microprice sits inside the touch, and away from the mid "
              "when the book is lopsided -- which is most of the time");
    }

    // ---- 2. THE INDEX: NO BOOK, AND THAT IS NOT AN EMPTY BOOK -------------
    {
        const auto q = parse_quote(kBody, "NSE:NIFTY 50");
        check(q.has_value(), "the index quote parses");
        if (q) {
            check(!q->has_touch(),
                  "NIFTY 50 has NO touch -- an index does not trade, it is "
                  "computed from things that do, so there is nothing to bid "
                  "for and `has_touch()` says so rather than returning zeros");
            check(!q->has_oi,
                  "and no open interest, which is different from a future "
                  "with none outstanding");
            check(q->last_price.raw() == 2415025 && q->volume == 0,
                  "it still has a level and a volume of zero, and P11Q-09 "
                  "already settled that the zero volume is ABSENT rather than "
                  "measured");
        }
    }

    // ---- 3. ABSENT IS NOT EMPTY -------------------------------------------
    {
        const auto q = parse_quote(kBody, "NSE:RELIANCE");
        check(!q && q.error() == QuoteError::NotPresent,
              "a symbol Kite did not return is NotPresent, not a Quote full "
              "of zeros -- otherwise a typo in a watchlist row shows a bid of "
              "0.00, which is a price");
    }

    // ---- 4. ROUNDING, NOT TRUNCATION --------------------------------------
    //
    // Truncating biases every price DOWN by up to a paisa. On a bid-ask pair
    // that biases the spread itself, and a cost model fed understated spreads
    // reports edges that are not there.
    {
        const char* body = R"({"status":"success","data":{
"NSE:X":{"instrument_token":1,"last_price":100.005,
 "depth":{"buy":[{"price":100.004,"quantity":1,"orders":1}],
          "sell":[{"price":100.006,"quantity":1,"orders":1}]}}}})";
        const auto q = parse_quote(body, "NSE:X");
        check(q.has_value(), "a sub-paisa price parses");
        if (q) {
            std::printf("    100.005 -> %lld paise; bid 100.004 -> %lld; "
                        "ask 100.006 -> %lld\n",
                        static_cast<long long>(q->last_price.raw()),
                        static_cast<long long>(q->buy[0].price.raw()),
                        static_cast<long long>(q->sell[0].price.raw()));
            check(q->buy[0].price.raw() == 10000
                      && q->sell[0].price.raw() == 10001,
                  "the bid rounds to 10000 and the ask to 10001, so the "
                  "spread is 1 paisa -- truncation would give 10000 and 10000 "
                  "and report a spread of ZERO, which is a crossed-tight book "
                  "that does not exist");
        }
    }

    // ---- 5. THE ERROR ENVELOPE --------------------------------------------
    {
        const char* err = R"({"status":"error","message":"Invalid token",
"error_type":"TokenException"})";
        const auto q = parse_quote(err, "NSE:X");
        check(!q && q.error() == QuoteError::ApiError,
              "an error envelope is an ApiError, not a Malformed body -- the "
              "response parsed fine, Kite simply said no, and a caller "
              "retrying a parse failure would retry forever");
        check(!parse_quote("not json at all", "NSE:X").has_value(),
              "and something that is not a response at all is refused");
    }

    // ---- 6. THE REQUEST -----------------------------------------------------
    {
        const std::string t = quote_target({"NSE:NIFTY 50", "NFO:NIFTY26SEPFUT"});
        std::printf("    target: %s\n", t.c_str());
        check(t.find("NIFTY%2050") != std::string::npos,
              "the space in 'NIFTY 50' is percent-encoded -- an NSE index "
              "symbol contains one, and an unencoded space truncates the "
              "query at the first instrument");
        check(t.find("?i=") != std::string::npos
                  && t.find("&i=") != std::string::npos,
              "and multiple instruments are one request rather than N, "
              "because Kite rate-limits per call and a watchlist is a batch");
    }

    std::printf("\n%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
