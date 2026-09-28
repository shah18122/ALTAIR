// P3-01 (parser) acceptance tests for instruments/fyers_master.hpp.
// The fixtures are REAL rows fetched from https://public.fyers.in on
// 2026-09-24 -- a cash equity, an index option and an index future -- so the
// column mapping is checked against live data, not a restatement of the parser.

#include <instruments/fyers_master.hpp>

#include <cstdio>
#include <string>
#include <string_view>

namespace {

using namespace altair;
using namespace altair::instruments;

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::printf("FAIL: %s\n", what); }
}

// 1790676600 seconds -> nanoseconds. The BANKNIFTY 29-Sep-2026 expiry.
constexpr std::int64_t kExpiryNs = 1790676600LL * 1'000'000'000LL;

// Verbatim rows from the FYERS NSE_CM / NSE_FO CSV masters.
constexpr const char* kCash =
    "101000000016921,20 MICRONS LTD,0,1,0.01,INE144J01027,0915-1530|1815-1915:,"
    "2026-09-24,,NSE:20MICRONS-EQ,10,10,16921,20MICRONS,16921,-1.0,XX,"
    "101000000016921,None,1,2.0";
constexpr const char* kOption =
    "101126092935000,BANKNIFTY 29 Sep 26 72600 CE,14,30,0.05,,0915-1540|1815-1915:,"
    "2026-09-24,1790676600,NSE:BANKNIFTY26SEP72600CE,10,11,35000,BANKNIFTY,26009,"
    "72600.0,CE,101000000026009,None,0,0.0";
constexpr const char* kFuture =
    "101126092968390,BANKNIFTY 29 Sep 26 FUT,11,30,0.2,,0915-1540|1815-1915:,"
    "2026-09-24,1790676600,NSE:BANKNIFTY26SEPFUT,10,11,68390,BANKNIFTY,26009,"
    "-1.0,XX,101000000026009,None,0,0.0";

void test_cash_row_parses_verified_fields() {
    const auto r = parse_fyers_master_row(kCash);
    check(r.has_value(), "cash row parses");
    if (!r) { return; }
    check(r->fy_token == 101000000016921ULL, "fyToken is the 15-digit uint64");
    check(r->ex_token == 16921u, "exToken");
    check(r->exchange == Exchange::NSE, "exchange NSE");
    check(r->segment == Segment::Cash, "segment Cash (10)");
    check(r->opt_type == OptionType::None, "no option type");
    check(r->lot_size == LotSize{1}, "cash lot size 1");
    check(r->tick_size == Price{1}, "tick 0.01 rupee -> 1 paise");
    check(r->strike == Price{0}, "no strike for cash");
    check(r->expiry == Timestamp::epoch(), "no expiry for cash");
    check(std::string_view(r->isin) == "INE144J01027", "isin");
    check(std::string_view(r->underlying) == "20MICRONS", "underlying");
    check(std::string_view(r->symbol) == "20MICRONS-EQ", "ticker prefix removed for identity");
    check(std::string_view(r->ticker) == "NSE:20MICRONS-EQ", "ticker");

    FyersIdentityMap identities;
    check(identities.bind(fyers_identity_record(*r), InstrumentId{17}).has_value(),
          "the parsed 64-bit token binds directly into the FYERS identity map");
    check(identities.id_of(r->fy_token).value_or(InstrumentId::Invalid) == InstrumentId{17},
          "the parser-to-map adapter preserves all token bits");

    const auto leg = cross_venue_leg(*r, InstrumentId{17});
    check(std::string_view(leg.isin) == "INE144J01027"
              && leg.segment == Segment::Cash,
          "the same verified row feeds the separate ISIN cross-venue identity table");
}

void test_option_row_parses_verified_fields() {
    const auto r = parse_fyers_master_row(kOption);
    check(r.has_value(), "option row parses");
    if (!r) { return; }
    check(r->fy_token == 101126092935000ULL, "option fyToken (needs 64 bits)");
    check(r->ex_token == 35000u, "option exToken");
    check(r->segment == Segment::Opt, "segment Opt (11 + CE)");
    check(r->opt_type == OptionType::CE, "CE");
    check(r->lot_size == LotSize{30}, "BANKNIFTY lot 30");
    check(r->tick_size == Price{5}, "tick 0.05 rupee -> 5 paise");
    check(r->strike == Price{7'260'000}, "strike 72600 rupee -> 7,260,000 paise");
    check(r->expiry == Timestamp{kExpiryNs}, "expiry epoch seconds -> ns");
    check(std::string_view(r->isin) == "", "option isin empty");
    check(std::string_view(r->underlying) == "BANKNIFTY", "option underlying");
}

void test_future_row_is_fut_not_cash_or_opt() {
    const auto r = parse_fyers_master_row(kFuture);
    check(r.has_value(), "future row parses");
    if (!r) { return; }
    check(r->segment == Segment::Fut, "segment 11 + XX resolves to Fut, not Opt");
    check(r->opt_type == OptionType::None, "future has no option type");
    check(r->strike == Price{0}, "future has no strike");
    check(r->tick_size == Price{20}, "tick 0.2 rupee -> 20 paise");
    check(r->lot_size == LotSize{30}, "future lot 30");
    check(r->expiry == Timestamp{kExpiryNs}, "future carries the expiry");
}

void test_short_row_is_refused() {
    const auto few = parse_fyers_master_row("101000000016921,20 MICRONS LTD,0,1");
    check(!few.has_value() && few.error() == FyersMasterError::ShortRow,
          "a row with too few columns is refused");
    // A field containing a comma would over-split; that is refused too, never
    // split wrongly. Simulate by adding a 22nd column.
    const auto many = parse_fyers_master_row(std::string(kCash) + ",extra");
    check(!many.has_value() && many.error() == FyersMasterError::ShortRow,
          "a row with too many columns is refused");
}

void test_unknown_exchange_is_refused() {
    // col10 changed from 10 (NSE) to 99: this reader maps only NSE, and refuses
    // rather than guessing. BSE support needs the BSE master fetched first.
    std::string bad = kCash;
    // find ",10,10,16921," (exchange,segment) and corrupt the exchange.
    const auto pos = bad.find(",10,10,16921,");
    check(pos != std::string::npos, "found the exchange/segment columns");
    if (pos != std::string::npos) { bad.replace(pos, 4, ",99,"); }
    const auto r = parse_fyers_master_row(bad);
    check(!r.has_value() && r.error() == FyersMasterError::BadExchange,
          "an unmapped exchange code is refused");
}

void test_sub_paisa_tick_is_refused_not_truncated() {
    // A tick finer than a paisa must refuse, not silently round to a paisa.
    std::string bad = kCash;
    const auto pos = bad.find(",0.01,");
    if (pos != std::string::npos) { bad.replace(pos, 6, ",0.005,"); }
    const auto r = parse_fyers_master_row(bad);
    check(!r.has_value() && r.error() == FyersMasterError::BadNumber,
          "a sub-paisa tick is refused, not truncated to a paisa");
}

} // namespace

int main() {
    test_cash_row_parses_verified_fields();
    test_option_row_parses_verified_fields();
    test_future_row_is_fut_not_cash_or_opt();
    test_short_row_is_refused();
    test_unknown_exchange_is_refused();
    test_sub_paisa_tick_is_refused_not_truncated();
    std::printf("FYERS master: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
