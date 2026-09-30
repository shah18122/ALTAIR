// Tests for instruments/fyers_symbol.hpp.
//
// The BANKNIFTY ticker is the real one from the FYERS NSE_FO master row used
// in test_fyers_master.cpp. That row's expiry, 1790676600, is 29 Sep 2026
// 15:40 IST -- the end of FYERS' F&O session. Altair times expiry at the
// 15:30 IST exchange close, ten minutes earlier: 1790676000.

#include <instruments/fyers_symbol.hpp>

#include <cstdio>
#include <string_view>

namespace {

using namespace altair;
using namespace altair::instruments;

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::printf("FAIL: %s\n", what); }
    else { std::printf("  ok  : %s\n", what); }
}

bool is(const char* a, std::string_view b) { return std::string_view{a} == b; }

constexpr int kYear = 2026;

void test_equity_and_index() {
    const auto sbin = parse_fyers_symbol("NSE:SBIN-EQ", kYear);
    check(sbin && sbin->kind == FyersSymbolKind::Equity && is(sbin->exchange.data(), "NSE")
              && is(sbin->underlying.data(), "SBIN") && is(sbin->series.data(), "EQ"),
          "NSE:SBIN-EQ is an equity with series EQ");
    const auto bajaj = parse_fyers_symbol("NSE:BAJAJ-AUTO-EQ", kYear);
    check(bajaj && is(bajaj->underlying.data(), "BAJAJ-AUTO"),
          "a dash inside the name stays in the underlying");
    const auto nifty = parse_fyers_symbol("NSE:NIFTY50-INDEX", kYear);
    check(nifty && nifty->kind == FyersSymbolKind::Index && is(nifty->underlying.data(), "NIFTY50"),
          "NSE:NIFTY50-INDEX is an index");
    check(fyers_spot_symbol(*nifty) == "NSE:NIFTY50-INDEX", "an index is its own spot");
    check(fyers_spot_symbol(*sbin) == "NSE:SBIN-EQ", "an equity is its own spot");
}

void test_monthly_option_and_future() {
    const auto opt = parse_fyers_symbol("NSE:BANKNIFTY26SEP72600CE", kYear);
    check(opt && opt->kind == FyersSymbolKind::Option && opt->right == FyersRight::Call
              && is(opt->underlying.data(), "BANKNIFTY") && opt->strike == Price{7'260'000}
              && opt->year == 2026 && opt->month == 9 && opt->day == 0,
          "monthly option: underlying, strike in paise, month, no day");
    check(!fyers_ticker_expiry(*opt).has_value(),
          "a monthly ticker does not claim an expiry date");
    check(fyers_spot_symbol(*opt) == "NSE:NIFTYBANK-INDEX", "BANKNIFTY options price off NIFTYBANK");

    const auto fut = parse_fyers_symbol("NSE:BANKNIFTY26SEPFUT", kYear);
    check(fut && fut->kind == FyersSymbolKind::Future && fut->month == 9 && fut->year == 2026
              && fut->strike == Price{0},
          "monthly future");
    const auto dash_fut = parse_fyers_symbol("NSE:BAJAJ-AUTO26OCTFUT", kYear);
    check(dash_fut && dash_fut->kind == FyersSymbolKind::Future
              && is(dash_fut->underlying.data(), "BAJAJ-AUTO"),
          "future on a dashed underlying");
    check(fyers_spot_symbol(*dash_fut) == "NSE:BAJAJ-AUTO-EQ", "stock derivative prices off the -EQ line");

    const auto nxt = parse_fyers_symbol("NSE:NIFTYNXT5026OCT70000PE", kYear);
    check(nxt && is(nxt->underlying.data(), "NIFTYNXT50") && nxt->right == FyersRight::Put
              && nxt->strike == Price{7'000'000},
          "an underlying ending in digits is read correctly");
    const auto idea = parse_fyers_symbol("NSE:IDEA26SEP7.5CE", kYear);
    check(idea && idea->strike == Price{750}, "a decimal strike is exact paise");
    const auto sensex = parse_fyers_symbol("BSE:SENSEX26SEP82000CE", kYear);
    check(sensex && fyers_spot_symbol(*sensex) == "BSE:SENSEX-INDEX", "SENSEX options price off the index");
}

void test_weekly_option() {
    const auto w = parse_fyers_symbol("NSE:NIFTY2692925000CE", kYear);
    check(w && w->kind == FyersSymbolKind::Option && w->year == 2026 && w->month == 9
              && w->day == 29 && w->strike == Price{2'500'000},
          "weekly option: YY M DD then strike");
    const auto expiry = fyers_ticker_expiry(*w);
    check(expiry && expiry->ns_since_epoch() == 1'790'676'000LL * 1'000'000'000LL,
          "weekly expiry is 15:30 IST on the date");
    const auto oct = parse_fyers_symbol("NSE:NIFTY26O0624500PE", kYear);
    check(oct && oct->month == 10 && oct->day == 6 && oct->right == FyersRight::Put,
          "O is October in a weekly ticker");
}

void test_refusals() {
    check(parse_fyers_symbol("", kYear).error() == FyersSymbolError::Empty, "empty is refused");
    check(parse_fyers_symbol("SBIN-EQ", kYear).error() == FyersSymbolError::NoExchange,
          "no exchange prefix is refused");
    check(!parse_fyers_symbol("NSE:NIFTY26SEP0CE", kYear).has_value(), "a zero strike is refused");
    check(!parse_fyers_symbol("NSE:NIFTY26SEP100.125CE", kYear).has_value(), "a sub-paisa strike is refused");
    check(!parse_fyers_symbol("NSE:NIFTY2623125000CE", kYear).has_value(),
          "an impossible weekly date (Feb 31) is refused, not re-read as NIFTY2/2062");
    check(!parse_fyers_symbol("NSE:NIFTY40SEP25000CE", kYear).has_value(),
          "an expiry year far outside the reference window is refused");
    check(parse_fyers_symbol("NSE:NIFTY", kYear).error() == FyersSymbolError::Unrecognised,
          "a bare name is refused");
    check(parse_fyers_symbol(std::string_view{
              "NSE:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA-EQ"}, kYear)
              .error() == FyersSymbolError::TooLong,
          "an over-long ticker is refused");
    check(fyers_spot_symbol(*parse_fyers_symbol("MCX:CRUDEOIL26OCTFUT", kYear)).empty(),
          "MCX has no spot this reader can name");
}

} // namespace

int main() {
    std::printf("FYERS ticker reader\n");
    test_equity_and_index();
    test_monthly_option_and_future();
    test_weekly_option();
    test_refusals();
    std::printf("FYERS symbol: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
