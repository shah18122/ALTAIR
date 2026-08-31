// Skeleton tests for the three NSE master parsers (P1-02a/b/c).
//
// Two jobs. First, pin the failure mode: every entry point must return
// SchemaNotConfigured rather than a plausible answer, so a caller that wires
// these up before the schemas are known finds out immediately instead of
// trading on invented lot sizes. Second, test the parts that ARE implemented —
// ExpiryMonth and ist_expiry_month are this card's own logic, not the CSV's
// schema, and the month-boundary hazard they exist for is real today.
//
// No check description here may contain the substring FAIL.

#include <instruments/nse_fo_master.hpp>
#include <instruments/nse_mktlots.hpp>
#include <instruments/nse_equity_master.hpp>

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

Reconciler g_rec;

// ── 1 ────────────────────────────────────────────────────────────────────
void every_entry_point_refuses_to_guess()
{
    std::printf("\n1 every_entry_point_refuses_to_guess\n");

    // A row that LOOKS parseable. The danger these stubs guard against is not
    // a crash on garbage -- it is a confident wrong answer on plausible input.
    const char* hdr = "INSTRUMENT,SYMBOL,EXPIRY_DT,STRIKE_PR,OPTION_TYP";
    const char* row = "OPTIDX,NIFTY,25-SEP-2026,25000,CE";
    const Timestamp snap = ist_date(2026, 8, 31);

    const auto h = parse_nse_fo_header(hdr, std::strlen(hdr));
    check(!h.has_value() && h.error() == NseParseError::SchemaNotConfigured,
          "F&O header parser refuses -- SchemaNotConfigured");

    NseFoColumns cols{};
    const auto r = parse_nse_fo_row(row, std::strlen(row), cols, snap);
    check(!r.has_value() && r.error() == NseParseError::SchemaNotConfigured,
          "F&O row parser refuses on a row that looks entirely valid");

    const auto px = parse_nse_rupees_to_paise("0.05", 4);
    check(!px.has_value() && px.error() == NseParseError::SchemaNotConfigured,
          "the price parser refuses rather than returning a plausible 5");

    const auto ex = parse_nse_expiry("25-SEP-2026", 11);
    check(!ex.has_value() && ex.error() == NseParseError::SchemaNotConfigured,
          "the expiry parser refuses -- the format is not yet known, and "
          "guessing fails only where day <= 12");

    g_rec.clear();
    const auto ld = load_nse_fo_master(row, std::strlen(row), g_rec, snap);
    check(!ld.has_value() && ld.error() == NseParseError::SchemaNotConfigured,
          "the loader refuses");
    check(g_rec.size() == 0, "and nothing reached the reconciler");

    MktLotsTable tbl;
    const auto ml = load_mktlots("UNDERLYING,SYMBOL,MAR-26", 24, tbl);
    check(!ml.has_value() && ml.error() == MktLotsError::SchemaNotConfigured,
          "the mktlots loader refuses");
    const auto lot = tbl.lot_for("NIFTY", ExpiryMonth{2026, 9});
    check(!lot.has_value() && lot.error() == MktLotsError::SchemaNotConfigured,
          "and lot_for refuses rather than returning LotSize{0}");

    const char* eq_allowed[] = {"EQ"};
    const SeriesFilter filt{eq_allowed, 1};
    const auto eh = parse_equity_header("SYMBOL,SERIES,MARKET LOT", 24);
    check(!eh.has_value() && eh.error() == EquityMasterError::SchemaNotConfigured,
          "the equity header parser refuses");
    const auto er = parse_equity_row("RELIANCE,EQ,1", 13, EquityColumns{},
                                     filt, snap);
    check(!er.has_value() && er.error() == EquityMasterError::SchemaNotConfigured,
          "the equity row parser refuses");
}

// ── 2 ────────────────────────────────────────────────────────────────────
// ist_expiry_month IS implemented: it is P1-02b's own logic, not the CSV's
// schema. The hazard is that an expiry is an IST midnight, which is 18:30 UTC
// the PREVIOUS day -- so a UTC reading moves every 1st-of-month expiry into
// the previous month and looks up the wrong lot size.
void expiry_month_is_read_in_ist()
{
    std::printf("\n2 expiry_month_is_read_in_ist\n");

    const Timestamp first = ist_date(2026, 9, 1);

    // Assert the trap EXISTS before asserting it is handled.
    const std::int64_t utc_day = first.ns_since_epoch() / 86'400'000'000'000LL;
    const detail::CivilDate utc_civil = detail::civil_from_days(utc_day);
    check(utc_civil.month == 8 && utc_civil.day == 31,
          "an IST midnight on 1 Sep is 31 Aug in UTC -- the trap is real");

    const ExpiryMonth m = ist_expiry_month(first);
    check(m.year == 2026 && m.month == 9,
          "but ist_expiry_month reads September, not August");

    check(ist_expiry_month(ist_date(2026, 9, 24)) == (ExpiryMonth{2026, 9}),
          "a mid-month expiry reads its own month");
    check(ist_expiry_month(ist_date(2026, 12, 31)) == (ExpiryMonth{2026, 12}),
          "the last day of a year reads December");
    check(ist_expiry_month(ist_date(2027, 1, 1)) == (ExpiryMonth{2027, 1}),
          "and 1 January reads the NEW year, not the old one");

    // A leap day, because civil_from_days is where that goes wrong.
    check(ist_expiry_month(ist_date(2028, 2, 29)) == (ExpiryMonth{2028, 2}),
          "29 February 2028 reads February 2028");

    check((ExpiryMonth{2026, 9}) != (ExpiryMonth{2026, 10}),
          "ExpiryMonth compares by value");
    check((ExpiryMonth{2026, 9}) < (ExpiryMonth{2026, 10}),
          "and orders chronologically");
    check((ExpiryMonth{2026, 12}) < (ExpiryMonth{2027, 1}),
          "across a year boundary too");
}

} // namespace

int main()
{
    std::printf("altair instruments NSE master skeleton tests\n");
    every_entry_point_refuses_to_guess();
    expiry_month_is_read_in_ist();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
