// app/tests/test_price_text.cpp -- CX02-A1a, finding C14-001.
//
// Both dataset writers used to write prices at six significant digits, so
// NIFTY 24,123.45 went to disk as 24123.5. Nothing downstream could see it:
// the file parsed and the number was plausible.
//
// The sweep in test 1 is the evidence, and test 2 shows that the same sweep
// REJECTS the formatter being replaced. A check that also passed `%g` would
// prove nothing -- the standing lesson from P33, where three checks shipped
// reporting clean while broken.
//
// No check description here may contain the substring FAIL.

#include <app/price_text.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %-4s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) { ++failures; }
}

using altair::dataset::PriceTextError;
using altair::dataset::format_price;
using altair::dataset::kPriceTextMax;
using altair::dataset::round_trips;

/// The text a paise amount should produce, built with integers only: rupees,
/// then up to two decimals with trailing zeros dropped.
std::string paise_text(std::int64_t p) {
    std::string s = std::to_string(p / 100);
    const int frac = static_cast<int>(p % 100);
    if (frac != 0) {
        s += '.';
        s += static_cast<char>('0' + frac / 10);
        if (frac % 10 != 0) { s += static_cast<char>('0' + frac % 10); }
    }
    return s;
}

std::string fmt(double v) {
    char buf[kPriceTextMax];
    const auto n = format_price(v, buf, sizeof buf);
    return n ? std::string(buf, *n) : std::string("<refused>");
}

/// The line CX02-A1b removes from kite_update_main.cpp, reproduced so the
/// property can be shown to reject it.
std::string old_percent_g(double v) {
    char buf[64];
    const int n = std::snprintf(buf, sizeof buf, "%g", v);
    return std::string(buf, n > 0 ? static_cast<std::size_t>(n) : 0u);
}

// ── 1 ────────────────────────────────────────────────────────────────────
void price_text_round_trips_every_paise_value() {
    std::printf("\n1 price_text_round_trips_every_paise_value\n");
    std::size_t tried = 0, wrong_text = 0, not_round_trip = 0;
    const auto one = [&](std::int64_t p) {
        const double v = static_cast<double>(p) / 100.0;
        const std::string t = fmt(v);
        ++tried;
        if (t != paise_text(p)) {
            if (wrong_text == 0) {
                std::printf("    first mismatch: paise %lld -> \"%s\", want \"%s\"\n",
                            static_cast<long long>(p), t.c_str(),
                            paise_text(p).c_str());
            }
            ++wrong_text;
        }
        if (!round_trips(t.data(), t.size(), v)) { ++not_round_trip; }
    };
    // Every 7th paise from 0 to Rs 1,00,000.00, then the edges the stride
    // skips: a whole rupee, a one-paisa price, the BANKNIFTY and NIFTY shapes
    // that six significant digits rounded, and the six-digit boundary.
    for (std::int64_t p = 7; p <= 10'000'000; p += 7) { one(p); }
    for (std::int64_t p : {1LL, 5LL, 99LL, 100LL, 2'412'345LL, 5'123'465LL,
                           9'999'999LL, 10'000'000LL, 99'999'999LL,
                           100'000'000LL, 123'456'789'01LL}) {
        one(p);
    }
    std::printf("    %zu prices: %zu with different text, %zu not round-tripping\n",
                tried, wrong_text, not_round_trip);
    check(wrong_text == 0,
          "every paise amount is written as its exact two-decimal text");
    check(not_round_trip == 0,
          "and every one parses back to the identical double");
}

// ── 2 ────────────────────────────────────────────────────────────────────
void price_text_never_rounds_to_six_significant_digits() {
    std::printf("\n2 price_text_never_rounds_to_six_significant_digits\n");
    check(fmt(24123.45) == "24123.45", "NIFTY 24123.45 is written 24123.45");
    check(fmt(51234.65) == "51234.65", "BANKNIFTY 51234.65 is written 51234.65");

    // THE PLANTED VIOLATION. The old formatter, held to the same property.
    const std::string g = old_percent_g(24123.45);
    std::printf("    old %%g wrote \"%s\"\n", g.c_str());
    check(!round_trips(g.data(), g.size(), 24123.45),
          "the round-trip property REJECTS the %g formatter it replaces");

    std::size_t g_lossy = 0;
    for (std::int64_t p = 1'000'000; p <= 10'000'000; p += 7) {
        const double v = static_cast<double>(p) / 100.0;
        const std::string t = old_percent_g(v);
        if (!round_trips(t.data(), t.size(), v)) { ++g_lossy; }
    }
    std::printf("    over Rs 10,000..1,00,000 at stride 7, %%g lost precision on %zu prices\n",
                g_lossy);
    check(g_lossy > 1'000'000,
          "and the sweep in test 1 would have caught it on most prices in range");
}

// ── 3 ────────────────────────────────────────────────────────────────────
void price_text_is_decimal_agnostic_and_never_exponential() {
    std::printf("\n3 price_text_is_decimal_agnostic_and_never_exponential\n");
    check(fmt(10.68) == "10.68", "India VIX 10.68 survives");
    check(fmt(83.1234) == "83.1234",
          "a four-decimal rate survives -- nothing assumes two decimals");
    check(fmt(100000.0) == "100000",
          "Rs 1,00,000 is 100000, not 1e+05");
    check(fmt(1e14) == "100000000000000",
          "and so is a value far past any traded price");
    check(fmt(0.05) == "0.05", "a one-tick price keeps its leading zero");
}

// ── 4 ────────────────────────────────────────────────────────────────────
void price_text_refuses_what_is_not_a_price() {
    std::printf("\n4 price_text_refuses_what_is_not_a_price\n");
    char buf[kPriceTextMax];
    const auto nan = format_price(std::numeric_limits<double>::quiet_NaN(),
                                  buf, sizeof buf);
    check(!nan && nan.error() == PriceTextError::NotFinite, "NaN is refused");
    const auto inf = format_price(std::numeric_limits<double>::infinity(),
                                  buf, sizeof buf);
    check(!inf && inf.error() == PriceTextError::NotFinite,
          "infinity is refused");
    const auto neg = format_price(-0.01, buf, sizeof buf);
    check(!neg && neg.error() == PriceTextError::Negative,
          "a negative price is refused");
    const auto big = format_price(1e15, buf, sizeof buf);
    check(!big && big.error() == PriceTextError::OutOfRange,
          "a price at the ceiling is refused");
    // CX02-A2c (R-AB-026). Zero is not a price. Kite returns it for a
    // halted or malformed candle, and `,0,0,0,0,` asserts a market that
    // traded at nothing -- the same argument the fetcher already makes for an
    // all-zero VOLUME.
    const auto zero = format_price(0.0, buf, sizeof buf);
    check(!zero && zero.error() == PriceTextError::Zero,
          "zero_price_is_refused");
    const auto negzero = format_price(-0.0, buf, sizeof buf);
    check(!negzero && negzero.error() == PriceTextError::Zero,
          "and negative zero is the same non-price, refused the same way");
}

// ── 5 ────────────────────────────────────────────────────────────────────
void price_text_buffer_bound_refuses() {
    std::printf("\n5 price_text_buffer_bound_refuses\n");
    char small[8];
    const auto r7 = format_price(24123.45, small, 7);
    check(!r7 && r7.error() == PriceTextError::BufferTooSmall,
          "seven bytes for an eight-character price is refused, not truncated");
    const auto r8 = format_price(24123.45, small, 8);
    check(r8 && *r8 == 8 && std::string(small, 8) == "24123.45",
          "exactly eight bytes is enough");
    const auto null = format_price(24123.45, nullptr, 64);
    check(!null && null.error() == PriceTextError::BufferTooSmall,
          "a null buffer is refused");

    // kPriceTextMax's proof, at the edges it names.
    char buf[kPriceTextMax];
    const auto hi = format_price(std::nextafter(1e15, 0.0), buf, sizeof buf);
    check(hi.has_value(),
          "the largest value below the ceiling fits kPriceTextMax");
    const auto lo = format_price(1e-29, buf, sizeof buf);
    check(lo.has_value(), "and so does 1e-29");
}

} // namespace

int main() {
    std::printf("CX02-A1a -- dataset price text\n");
    price_text_round_trips_every_paise_value();
    price_text_never_rounds_to_six_significant_digits();
    price_text_is_decimal_agnostic_and_never_exponential();
    price_text_refuses_what_is_not_a_price();
    price_text_buffer_bound_refuses();
    std::printf("\n%s -- %d failing check(s)\n",
                failures == 0 ? "PASS" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
