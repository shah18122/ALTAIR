// P1-02c acceptance tests for instruments/nse_equity_master.hpp.
//
// The header and rows are copied from the real EQUITY_L.csv downloaded
// 2026-08-31 -- leading spaces in the header included, because they are the
// first thing that breaks a parser written from memory.
//
// No check description here may contain the substring FAIL.

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

constexpr Timestamp kSnap{1787888700000000000LL};

// The REAL header. Every name after the first carries a leading space.
constexpr const char* kHdr =
    "SYMBOL,NAME OF COMPANY, SERIES, DATE OF LISTING, PAID UP VALUE,"
    " MARKET LOT, ISIN NUMBER, FACE VALUE";

constexpr const char* kEq =
    "20MICRONS,20 Microns Limited,EQ,06-OCT-2008,5,1,INE144J01027,5";
constexpr const char* kRel =
    "RELIANCE,Reliance Industries Limited,EQ,29-NOV-1995,10,1,INE002A01018,10";
constexpr const char* kBe =
    "SOMESCRIP,Some Company Limited,BE,01-JAN-2010,10,1,INE999Z01011,10";
constexpr const char* kBz =
    "OTHERSCRIP,Other Company Limited,BZ,01-JAN-2011,10,1,INE888Z01012,10";

Reconciler g_rec;

std::expected<ContractSpec, EquityMasterError> one(const char* r)
{
    const auto c = parse_equity_header(kHdr, std::strlen(kHdr));
    if (!c) {
        return std::unexpected(c.error());
    }
    return parse_equity_row(r, std::strlen(r), *c, Exchange::NSE, kSnap);
}

// ── 1 ────────────────────────────────────────────────────────────────────
// The first thing that breaks a parser written from memory.
void header_names_carry_leading_spaces()
{
    std::printf("\n1 header_names_carry_leading_spaces\n");

    check(std::strstr(kHdr, ", SERIES") != nullptr,
          "the real header genuinely reads \", SERIES\" with a leading space");

    const auto c = parse_equity_header(kHdr, std::strlen(kHdr));
    check(c.has_value(), "and it parses, because matching TRIMS");
    check(c.has_value() && c->symbol == 0, "SYMBOL is column 0");
    check(c.has_value() && c->series == 2,
          "SERIES is column 2 -- a parser comparing raw text would have found "
          "only SYMBOL and reported every other column missing");
    check(c.has_value() && c->market_lot == 5, "MARKET LOT is column 5");
    check(c.has_value() && c->isin == 6, "ISIN NUMBER is column 6");

    constexpr const char* no_lot = "SYMBOL,NAME OF COMPANY, SERIES";
    check(!parse_equity_header(no_lot, std::strlen(no_lot)).has_value(),
          "a header without MARKET LOT is BadHeader");
}

// ── 2 ────────────────────────────────────────────────────────────────────
// The join question, settled by comparing the two real files.
void the_underlying_of_cash_is_its_trading_symbol()
{
    std::printf("\n2 the_underlying_of_cash_is_its_trading_symbol\n");

    const auto s = one(kRel);
    check(s.has_value(), "the real RELIANCE row parses");
    if (!s) {
        return;
    }
    check(std::strcmp(s->symbol, "RELIANCE") == 0, "symbol RELIANCE");
    check(std::strcmp(s->underlying, "RELIANCE") == 0,
          "and underlying RELIANCE too -- the trading symbol, NOT \"Reliance "
          "Industries Limited\". Kite's cash row carries tradingsymbol "
          "RELIANCE and name \"RELIANCE INDUSTRIES\"; only the symbol agrees, "
          "so keying on the company name would join nothing");
    check(s->segment == Segment::Cash, "segment Cash");
    check(s->opt_type == OptionType::None && s->strike == Price{0},
          "no option type, no strike");
    check(s->lot_size == LotSize{1},
          "MARKET LOT is parsed, not assumed -- rule 1 has no carve-out for "
          "\"obviously 1\"");
    check(s->tick_size == Price{0},
          "and tick size is zero: EQUITY_L carries no tick column, so P1-06 "
          "fills it from a broker rather than a literal 5 being invented here");
}

// ── 3 ────────────────────────────────────────────────────────────────────
void cash_expiry_is_epoch()
{
    std::printf("\n3 cash_expiry_is_epoch\n");

    const auto s = one(kEq);
    check(s.has_value() && s->expiry.is_epoch(),
          "a cash contract's expiry is Timestamp::epoch() -- the SAME "
          "convention P1-04 uses");

    // The consequence: it joins with Kite's cash row for the same scrip.
    g_rec.clear();
    check(g_rec.add(*s).has_value(), "the NSE row is added");

    const char* kite =
        "instrument_token,exchange_token,tradingsymbol,name,last_price,expiry,"
        "strike,tick_size,lot_size,instrument_type,segment,exchange\n"
        "424961,1660,20MICRONS,\"20 MICRONS\",0,,0,0.05,1,EQ,NSE,NSE\n";
    check(load_kite_dump(kite, std::strlen(kite), g_rec, kSnap).has_value(),
          "and Kite's row for the same scrip loads beside it");

    const ReconcileReport r = g_rec.reconcile();
    check(r.contracts == 1,
          "they are ONE contract -- two files, two very different company-name "
          "spellings, joined because both key on the trading symbol and both "
          "agree that 'no expiry' means the epoch");
    check(r.agreed == 1, "and they agree");
    const ContractVerdict* v = *g_rec.verdict_at(0);
    check(v->merged.lot_size == LotSize{1}, "merged lot 1");
    check(v->merged.tick_size == Price{5},
          "and the tick comes from Kite, because the exchange master has none");
}

// ── 4 ────────────────────────────────────────────────────────────────────
// EQ 2301, BE 235, BZ 38 in the real file -- the filter is not theoretical.
void series_filter_refuses_trade_to_trade()
{
    std::printf("\n4 series_filter_refuses_trade_to_trade\n");

    char doc[1024];
    std::snprintf(doc, sizeof(doc), "%s\n%s\n%s\n%s\n%s\n",
                  kHdr, kEq, kBe, kBz, kRel);

    g_rec.clear();
    const auto r = load_equity_master(doc, std::strlen(doc),
                                      kDefaultSeriesFilter, Exchange::NSE,
                                      g_rec, kSnap);
    check(r.has_value(), "the file loads");
    check(r.has_value() && r->added == 2, "the two EQ scrips are admitted");
    check(r.has_value() && r->rejected_series == 2,
          "BE and BZ are refused -- BE is trade-to-trade and does NOT net "
          "intraday, so a strategy that buys and sells in a session takes "
          "delivery on both legs");
    check(r.has_value() && r->unparseable == 0,
          "and they are NOT counted as unparseable: deliberate refusal and "
          "breakage must not share a bucket");

    // The allow-list is honoured as given, not hardcoded.
    static const char* both[] = {"EQ", "BE"};
    const SeriesFilter wide{both, 2};
    g_rec.clear();
    const auto w = load_equity_master(doc, std::strlen(doc), wide,
                                      Exchange::NSE, g_rec, kSnap);
    check(w.has_value() && w->added == 3 && w->rejected_series == 1,
          "widening the allow-list admits BE and still refuses BZ");
}

// ── 5 ────────────────────────────────────────────────────────────────────
// No quotes in the real file today -- but a renamed company would need them.
void quoted_fields_do_not_shift_columns()
{
    std::printf("\n5 quoted_fields_do_not_shift_columns\n");

    constexpr const char* quoted =
        "ACME,\"Acme Industries, The\",EQ,01-JAN-2000,10,7,INE777Z01013,10";
    const auto s = one(quoted);
    check(s.has_value(), "a company name containing a comma parses");
    check(s.has_value() && std::strcmp(s->symbol, "ACME") == 0, "symbol ACME");
    check(s.has_value() && s->lot_size == LotSize{7},
          "and MARKET LOT is 7 -- the column AFTER the quoted field is still "
          "right. A splitter ignoring quotes would have put ISIN here, and it "
          "would still have parsed as a number");

    constexpr const char* unterm =
        "ACME,\"Acme Industries,EQ,01-JAN-2000,10,1,INE777Z01013,10";
    const auto u = one(unterm);
    check(!u.has_value() && u.error() == EquityMasterError::UnquotedComma,
          "an unterminated quote is refused rather than best-effort recovered");
}

// ── 6 ────────────────────────────────────────────────────────────────────
void malformed_rows_are_counted_not_fatal()
{
    std::printf("\n6 malformed_rows_are_counted_not_fatal\n");

    char doc[1024];
    std::snprintf(doc, sizeof(doc), "%s\n%s\nSHORT,ROW\n%s\n", kHdr, kEq, kRel);

    g_rec.clear();
    const auto r = load_equity_master(doc, std::strlen(doc),
                                      kDefaultSeriesFilter, Exchange::NSE,
                                      g_rec, kSnap);
    check(r.has_value(), "the load succeeds");
    check(r.has_value() && r->added == 2, "both good rows are added");
    check(r.has_value() && r->unparseable == 1, "the short row is counted");
    check(r.has_value() && r->first_error_row == 2,
          "and its row number is reported");

    const auto empty_sym = one(",Some Company,EQ,01-JAN-2000,10,1,INE1,10");
    check(!empty_sym.has_value()
              && empty_sym.error() == EquityMasterError::BadSymbol,
          "an empty SYMBOL is BadSymbol");
    const auto bad_lot = one("ACME,Acme,EQ,01-JAN-2000,10,abc,INE1,10");
    check(!bad_lot.has_value() && bad_lot.error() == EquityMasterError::BadNumber,
          "a non-numeric MARKET LOT is BadNumber, never defaulted to 1");
    check(!load_equity_master("", 0, kDefaultSeriesFilter, Exchange::NSE,
                              g_rec, kSnap).has_value(),
          "an empty buffer is EmptyInput");
}

// ── 7 ────────────────────────────────────────────────────────────────────
void exchange_is_a_parameter()
{
    std::printf("\n7 exchange_is_a_parameter\n");

    const auto c = parse_equity_header(kHdr, std::strlen(kHdr));
    const auto nse = parse_equity_row(kRel, std::strlen(kRel), *c,
                                      Exchange::NSE, kSnap);
    const auto bse = parse_equity_row(kRel, std::strlen(kRel), *c,
                                      Exchange::BSE, kSnap);
    check(nse.has_value() && nse->exchange == Exchange::NSE
              && nse->source == SpecSource::NseMaster,
          "stamped NSE, the source is NseMaster");
    check(bse.has_value() && bse->exchange == Exchange::BSE
              && bse->source == SpecSource::BseMaster,
          "stamped BSE, the source is BseMaster -- so the same parser serves "
          "the BSE cash master, and the caller must pass the right exchange "
          "because that field picks the charge schedule");

    // Different exchanges are different contracts, and must not merge.
    g_rec.clear();
    (void)g_rec.add(*nse);
    (void)g_rec.add(*bse);
    const ReconcileReport r = g_rec.reconcile();
    check(r.contracts == 2,
          "RELIANCE on NSE and RELIANCE on BSE are TWO contracts -- exchange "
          "is in the D1 key, and they trade at different prices");
}

// ── 8 ────────────────────────────────────────────────────────────────────
void a_zero_lot_is_refused()
{
    std::printf("\n8 a_zero_lot_is_refused\n");

    const auto z = one("ACME,Acme,EQ,01-JAN-2000,10,0,INE1,10");
    check(!z.has_value() && z.error() == EquityMasterError::BadNumber,
          "a MARKET LOT of 0 is refused outright -- a zero lot size is rule "
          "1's failure mode with the sign flipped, and an order sized from it "
          "is an order for nothing or a division by zero in the sizing rule");

    const auto one_lot = one("ACME,Acme,EQ,01-JAN-2000,10,1,INE1,10");
    check(one_lot.has_value() && one_lot->lot_size == LotSize{1},
          "while 1 is fine");
}

} // namespace

int main()
{
    std::printf("altair instruments nse_equity_master tests\n");
    header_names_carry_leading_spaces();
    the_underlying_of_cash_is_its_trading_symbol();
    cash_expiry_is_epoch();
    series_filter_refuses_trade_to_trade();
    quoted_fields_do_not_shift_columns();
    malformed_rows_are_counted_not_fatal();
    exchange_is_a_parameter();
    a_zero_lot_is_refused();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
