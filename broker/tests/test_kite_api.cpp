// P2-11 acceptance tests for broker/kite_api.hpp.
//
// The important one is round_trips_against_the_parser: P1-04 turns the
// exchange's decimal rupees into integer paise on the way IN, and this turns
// paise back into decimal rupees on the way OUT. If they are not exact
// inverses, a price drifts somewhere between the tick that suggested a trade
// and the order that expresses it -- and nothing downstream would notice.
//
// No check description here may contain the substring FAIL.

#include <broker/kite_api.hpp>
#include <instruments/kite_dump.hpp>

#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    if (ok) {
        std::printf("  ok  : %s\n", what);
    } else {
        ++failures;
        std::printf("  FAIL: %s\n", what);
    }
}

} // namespace

using namespace altair;

namespace {

const char* fmt(std::int64_t paise, char* buf, std::size_t cap)
{
    const auto r = kite::kite_paise_to_rupees(Price{paise}, buf, cap);
    return r.has_value() ? buf : nullptr;
}

// ── 1 ────────────────────────────────────────────────────────────────────
void formats_paise_as_rupees()
{
    std::printf("\n1 formats_paise_as_rupees\n");
    char b[32];

    check(fmt(5, b, sizeof(b)) && std::strcmp(b, "0.05") == 0,
          "5 paise -> \"0.05\", the tick size P1-04 exists to protect");
    check(fmt(250005, b, sizeof(b)) && std::strcmp(b, "2500.05") == 0,
          "250005 -> \"2500.05\"");
    check(fmt(0, b, sizeof(b)) && std::strcmp(b, "0.00") == 0,
          "0 -> \"0.00\", not \"0\" -- always two decimals");
    check(fmt(100, b, sizeof(b)) && std::strcmp(b, "1.00") == 0,
          "100 -> \"1.00\"");
    check(fmt(50, b, sizeof(b)) && std::strcmp(b, "0.50") == 0,
          "50 -> \"0.50\", not \"0.5\"");
    check(fmt(1, b, sizeof(b)) && std::strcmp(b, "0.01") == 0,
          "one paisa -> \"0.01\"");
    check(fmt(2500000, b, sizeof(b)) && std::strcmp(b, "25000.00") == 0,
          "a NIFTY strike -> \"25000.00\"");
    check(fmt(-150, b, sizeof(b)) && std::strcmp(b, "-1.50") == 0,
          "negatives keep their sign");
    check(fmt(-5, b, sizeof(b)) && std::strcmp(b, "-0.05") == 0,
          "including small ones, where the rupee part is zero");
}

// ── 2 ────────────────────────────────────────────────────────────────────
// The property that matters: parse and format are exact inverses.
void round_trips_against_the_parser()
{
    std::printf("\n2 round_trips_against_the_parser\n");
    char b[32];

    // Every hundredth from 0.01 to 1.00 -- the same battery P1-04 uses, run
    // the other way.
    int mismatches = 0;
    int tried = 0;
    for (std::int64_t p = 1; p <= 100; ++p) {
        if (!fmt(p, b, sizeof(b))) {
            ++mismatches;
            continue;
        }
        const auto back = parse_rupees_to_paise(b, std::strlen(b));
        ++tried;
        if (!back.has_value() || back->raw() != p) {
            ++mismatches;
        }
    }
    check(tried == 100, "all 100 hundredths were attempted");
    check(mismatches == 0,
          "every hundredth 0.01..1.00 formats and parses back EXACTLY");

    // A spread of realistic and awkward magnitudes.
    const std::int64_t cases[] = {
        0, 1, 5, 9, 10, 99, 100, 101, 999, 1000,
        250005, 2500000, 123456789, 999999999999LL,
        -1, -5, -99, -100, -250005,
    };
    int bad = 0;
    int n = 0;
    for (std::int64_t p : cases) {
        if (!fmt(p, b, sizeof(b))) {
            ++bad;
            continue;
        }
        const auto back = parse_rupees_to_paise(b, std::strlen(b));
        ++n;
        if (!back.has_value() || back->raw() != p) {
            ++bad;
            std::printf("       mismatch: %lld -> \"%s\"\n",
                        static_cast<long long>(p), b);
        }
    }
    check(n == static_cast<int>(sizeof(cases) / sizeof(cases[0])),
          "every magnitude case was attempted");
    check(bad == 0,
          "and all of them round-trip exactly -- format and parse are true "
          "inverses, so a price cannot drift between the tick that suggested "
          "a trade and the order that expresses it");
}

// ── 3 ────────────────────────────────────────────────────────────────────
void handles_the_int64_min_edge()
{
    std::printf("\n3 handles_the_int64_min_edge\n");
    char b[32];

    // -INT64_MIN is not representable. Negating it in signed space is UB, not
    // a large number, so the implementation negates in unsigned space.
    const std::int64_t lo = INT64_MIN;
    const bool ok = fmt(lo, b, sizeof(b)) != nullptr;
    check(ok, "INT64_MIN formats without undefined behaviour");
    check(ok && b[0] == '-', "and keeps its sign");
    check(ok && std::strcmp(b, "-92233720368547758.08") == 0,
          "producing the exact value, digit for digit");

    check(fmt(INT64_MAX, b, sizeof(b))
              && std::strcmp(b, "92233720368547758.07") == 0,
          "and INT64_MAX likewise");
}

// ── 4 ────────────────────────────────────────────────────────────────────
void refuses_a_short_buffer()
{
    std::printf("\n4 refuses_a_short_buffer\n");
    char small[8];
    const auto r = kite::kite_paise_to_rupees(Price{250005}, small, sizeof(small));
    check(!r.has_value()
              && r.error() == kite::RupeeFormatError::BufferTooSmall,
          "a buffer under 24 bytes is refused rather than overflowed -- a "
          "truncated price string would parse as a different, valid price");

    char exact[24];
    check(kite::kite_paise_to_rupees(Price{INT64_MIN}, exact, sizeof(exact))
              .has_value(),
          "24 bytes is enough for the widest possible value");
}

// ── 5 ────────────────────────────────────────────────────────────────────
// Pinned so a later edit breaks the build rather than an order.
void constants_are_pinned()
{
    std::printf("\n5 constants_are_pinned\n");

    check(std::strcmp(kite::kOrderTypeSlm, "SL-M") == 0,
          "SL-M carries a HYPHEN -- \"SLM\" is silently a different string");
    check(std::strcmp(kite::kProductMis, "MIS") == 0
              && std::strcmp(kite::kProductCnc, "CNC") == 0,
          "MIS is intraday, CNC is delivery -- confusing them changes the "
          "margin, the STT treatment and the holding period at once");
    check(std::strcmp(kite::kVarietyRegular, "regular") == 0,
          "varieties are LOWER case, unlike products and order types");
    check(std::strcmp(kite::kTransactionBuy, "BUY") == 0
              && std::strcmp(kite::kTransactionSell, "SELL") == 0,
          "transaction types are upper case");
    check(std::strcmp(kite::kExchangeNfo, "NFO") == 0,
          "NSE derivatives are NFO, not NSE");
    check(std::strcmp(kite::kParamTradingsymbol, "tradingsymbol") == 0,
          "tradingsymbol is one word -- a misspelling is dropped by omitempty "
          "on the far side, so the order goes out missing the field");
    check(std::strcmp(kite::kErrToken, "TokenException") == 0,
          "TokenException is the session-expired signal P2-05 must act on");
    check(std::strcmp(kite::kUriPlaceOrder, "/orders/%s") == 0,
          "placing an order substitutes the VARIETY into the path");
    check(std::strcmp(kite::kUriModifyOrder, "/orders/%s/%s") == 0,
          "modifying substitutes variety AND order id");
    check(kite::kMarketProtectionAuto == -1,
          "market protection -1 asks Kite to choose the cap");
    check(std::strcmp(kite::kVersion, "3") == 0
              && std::strcmp(kite::kBaseUri, "https://api.kite.trade") == 0,
          "base URI and version match gokiteconnect v4.4.2");
}

} // namespace

int main()
{
    std::printf("altair broker kite_api tests\n");
    formats_paise_as_rupees();
    round_trips_against_the_parser();
    handles_the_int64_min_edge();
    refuses_a_short_buffer();
    constants_are_pinned();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
