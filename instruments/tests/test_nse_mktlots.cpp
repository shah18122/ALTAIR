// P1-02b acceptance tests for instruments/nse_mktlots.hpp.
//
// The header and rows here are copied from the real fo_mktlots.csv downloaded
// 2026-08-31, padding included. If a schema detail is asserted, a file on disk
// says it.
//
// No check description here may contain the substring FAIL.

#include <instruments/nse_mktlots.hpp>

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

// The real header and three real rows, padding preserved exactly.
constexpr const char* kDoc =
    "UNDERLYING                          ,SYMBOL    ,SEP-26     ,OCT-26     ,NOV-26     ,DEC-26     \n"
    "NIFTY 50                            ,NIFTY     ,65         ,65         ,65         ,65         \n"
    "NIFTY BANK                          ,BANKNIFTY ,30         ,30         ,30         ,           \n"
    "ABB INDIA LIMITED                   ,ABB       ,125        ,125        ,           ,           \n";

// ~100 KB: file scope, not the stack.
MktLotsTable g_tbl;

constexpr std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) noexcept
{
    y -= (m <= 2);
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned shifted = (m > 2) ? (m - 3u) : (m + 9u);
    const unsigned doy = (153u * shifted + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

constexpr Timestamp ist_date(std::int64_t y, unsigned m, unsigned d) noexcept
{
    return Timestamp{days_from_civil(y, m, d) * 86'400'000'000'000LL
                     - kIstOffset.raw()};
}

// ── 1 ────────────────────────────────────────────────────────────────────
void the_real_padded_file_parses()
{
    std::printf("\n1 the_real_padded_file_parses\n");

    check(load_mktlots(kDoc, std::strlen(kDoc), g_tbl).has_value(),
          "the real fixed-width file parses, padding and all");
    check(g_tbl.underlyings() == 3, "three underlyings");
    check(g_tbl.months() == 4, "four expiry months resolved FROM THE HEADER");
    check(g_tbl.month_at(0) == (ExpiryMonth{2026, 9}),
          "SEP-26 -> September 2026");
    check(g_tbl.month_at(3) == (ExpiryMonth{2026, 12}), "DEC-26 -> December 2026");

    const auto n = g_tbl.lot_for("NIFTY", ExpiryMonth{2026, 9});
    check(n.has_value() && *n == LotSize{65},
          "NIFTY September lot is 65 -- the live value, matching both the NSE "
          "bhavcopy and the Kite dump");
    check(g_tbl.lot_for("ABB", ExpiryMonth{2026, 9}).value() == LotSize{125},
          "ABB is 125");
}

// ── 2 ────────────────────────────────────────────────────────────────────
// The join question the card flagged as blocking, now settled by real files.
void symbol_is_the_join_key_not_underlying()
{
    std::printf("\n2 symbol_is_the_join_key_not_underlying\n");

    check(g_tbl.lot_for("NIFTY", ExpiryMonth{2026, 9}).has_value(),
          "\"NIFTY\" -- the SYMBOL column -- resolves");
    check(!g_tbl.lot_for("NIFTY 50", ExpiryMonth{2026, 9}).has_value(),
          "\"NIFTY 50\" -- the UNDERLYING column -- does NOT, and must not: "
          "Kite's `name` and UDiFF's TckrSymb both carry NIFTY, so SYMBOL is "
          "the only column that joins to anything");
    check(g_tbl.lot_for("BANKNIFTY", ExpiryMonth{2026, 9}).value() == LotSize{30},
          "BANKNIFTY likewise, at 30");
}

// ── 3 ────────────────────────────────────────────────────────────────────
// The columns roll every month, so they are resolved by NAME.
void rollover_does_not_shift_the_lookup()
{
    std::printf("\n3 rollover_does_not_shift_the_lookup\n");

    // Next month's file: SEP-26 is gone and JAN-27 has appeared at the end.
    constexpr const char* kNext =
        "UNDERLYING                          ,SYMBOL    ,OCT-26     ,NOV-26     ,DEC-26     ,JAN-27     \n"
        "NIFTY 50                            ,NIFTY     ,65         ,65         ,65         ,70         \n";

    static MktLotsTable next;
    check(load_mktlots(kNext, std::strlen(kNext), next).has_value(),
          "next month's file parses");

    const auto a = g_tbl.lot_for("NIFTY", ExpiryMonth{2026, 10});
    const auto b = next.lot_for("NIFTY", ExpiryMonth{2026, 10});
    check(a.has_value() && b.has_value() && *a == *b,
          "the SAME (underlying, month) query gives the same lot from both "
          "files, even though October moved from column 3 to column 2 -- a "
          "positional parser is correct for exactly one month");

    check(!g_tbl.lot_for("NIFTY", ExpiryMonth{2027, 1}).has_value(),
          "and this month's file simply does not know about JAN-27");
    check(next.lot_for("NIFTY", ExpiryMonth{2027, 1}).value() == LotSize{70},
          "while next month's does, at 70");
}

// ── 4 ────────────────────────────────────────────────────────────────────
void a_blank_cell_is_not_a_zero_lot()
{
    std::printf("\n4 a_blank_cell_is_not_a_zero_lot\n");

    const auto r = g_tbl.lot_for("BANKNIFTY", ExpiryMonth{2026, 12});
    check(!r.has_value() && r.error() == MktLotsError::NotFound,
          "BANKNIFTY has a BLANK December cell, so the answer is NotFound");
    check(g_tbl.lot_for("BANKNIFTY", ExpiryMonth{2026, 11}).value()
              == LotSize{30},
          "while November, which is populated, gives 30");
    check(!g_tbl.lot_for("ABB", ExpiryMonth{2026, 11}).has_value(),
          "ABB stops after October, and November is NotFound");

    // NotFound is "does not trade that month". LotSize{0} would be a lot size,
    // and rule 1's failure mode with the sign flipped -- an order sized from it
    // is an order for nothing, or a division by zero in the sizing rule.
    check(!g_tbl.lot_for("NOSUCHSYMBOL", ExpiryMonth{2026, 9}).has_value(),
          "an unknown underlying is NotFound too, never a zero");
}

// ── 5 ────────────────────────────────────────────────────────────────────
void expiry_month_is_read_in_ist()
{
    std::printf("\n5 expiry_month_is_read_in_ist\n");

    const Timestamp first = ist_date(2026, 10, 1);
    const std::int64_t utc_day = first.ns_since_epoch() / 86'400'000'000'000LL;
    const detail::CivilDate utc = detail::civil_from_days(utc_day);
    check(utc.month == 9 && utc.day == 30,
          "an IST midnight on 1 October is 30 September in UTC -- the trap");

    check(ist_expiry_month(first) == (ExpiryMonth{2026, 10}),
          "but ist_expiry_month reads October");
    check(g_tbl.lot_for("NIFTY", ist_expiry_month(first)).value() == LotSize{65},
          "so a 1st-of-month expiry looks up OCTOBER's lot, not September's");

    check(ist_expiry_month(ist_date(2026, 9, 29)) == (ExpiryMonth{2026, 9}),
          "a mid-month expiry reads its own month");
    check(ist_expiry_month(ist_date(2027, 1, 1)) == (ExpiryMonth{2027, 1}),
          "and 1 January reads the NEW year");
}

// ── 6 ────────────────────────────────────────────────────────────────────
void footers_and_malformed_headers()
{
    std::printf("\n6 footers_and_malformed_headers\n");

    constexpr const char* kWithFooter =
        "UNDERLYING                          ,SYMBOL    ,SEP-26     \n"
        "NIFTY 50                            ,NIFTY     ,65         \n"
        "Note: lot sizes revised w.e.f 26-SEP-2026\n";
    static MktLotsTable t;
    check(load_mktlots(kWithFooter, std::strlen(kWithFooter), t).has_value(),
          "a file with a trailing note parses");
    check(t.underlyings() == 1,
          "and the note is SKIPPED, not parsed into a phantom underlying");

    constexpr const char* kBadMonth =
        "UNDERLYING,SYMBOL,SEPT-2026\nNIFTY 50,NIFTY,65\n";
    check(!load_mktlots(kBadMonth, std::strlen(kBadMonth), t).has_value(),
          "an unparseable month header is BadMonth, not a silently dropped "
          "column");

    constexpr const char* kWrongCols = "FOO,BAR,SEP-26\nx,y,1\n";
    const auto w = load_mktlots(kWrongCols, std::strlen(kWrongCols), t);
    check(!w.has_value() && w.error() == MktLotsError::BadHeader,
          "a header whose first two columns are not UNDERLYING and SYMBOL is "
          "BadHeader -- the shape is the contract");

    check(!load_mktlots("", 0, t).has_value(), "an empty buffer is EmptyInput");
}

// ── 7 ────────────────────────────────────────────────────────────────────
void month_abbreviations_are_exact()
{
    std::printf("\n7 month_abbreviations_are_exact\n");

    static MktLotsTable t;
    constexpr const char* kAll =
        "UNDERLYING,SYMBOL,JAN-27,FEB-27,MAR-27,APR-27,MAY-27,JUN-27,"
        "JUL-27,AUG-27,SEP-27,OCT-27,NOV-27,DEC-27\n"
        "X,X,1,2,3,4,5,6,7,8,9,10,11,12\n";
    check(load_mktlots(kAll, std::strlen(kAll), t).has_value(),
          "all twelve abbreviations parse");
    bool all_right = true;
    for (std::uint8_t m = 1; m <= 12; ++m) {
        const auto v = t.lot_for("X", ExpiryMonth{2027, m});
        if (!v || v->raw() != m) {
            all_right = false;
        }
    }
    check(all_right, "and each maps to the right month number, in order");

    constexpr const char* kLower = "UNDERLYING,SYMBOL,Sep-26\nX,X,1\n";
    check(!load_mktlots(kLower, std::strlen(kLower), t).has_value(),
          "mixed-case month names are refused -- accepting both spellings "
          "would hide a format change instead of revealing it");
}

// ── 8 ────────────────────────────────────────────────────────────────────
void completes_a_spec_lot_size()
{
    std::printf("\n8 completes_a_spec_lot_size\n");

    // A contract expiring 24 September 2026 -- the real NIFTY monthly expiry.
    const Timestamp expiry = ist_date(2026, 9, 24);
    const ExpiryMonth m = ist_expiry_month(expiry);
    check(m == (ExpiryMonth{2026, 9}), "its expiry month is September 2026");

    const auto lot = g_tbl.lot_for("NIFTY", m);
    check(lot.has_value() && *lot == LotSize{65},
          "and fo_mktlots gives lot 65 for it");
    check(lot.has_value() && *lot == LotSize{65},
          "which is the SAME 65 the UDiFF bhavcopy's NewBrdLotQty carries and "
          "the same 65 in the Kite dump -- three files, three formats, one "
          "lot size, and rule 1 satisfied without a literal anywhere");
}

} // namespace

int main()
{
    std::printf("altair instruments nse_mktlots tests\n");
    the_real_padded_file_parses();
    symbol_is_the_join_key_not_underlying();
    rollover_does_not_shift_the_lookup();
    a_blank_cell_is_not_a_zero_lot();
    expiry_month_is_read_in_ist();
    footers_and_malformed_headers();
    month_abbreviations_are_exact();
    completes_a_spec_lot_size();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
