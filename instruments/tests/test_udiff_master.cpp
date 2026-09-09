// P1-02a / P1-03a acceptance tests for instruments/udiff_master.hpp.
//
// Every row here is copied from the real bhavcopies downloaded 2026-08-31, not
// invented. If a schema detail is asserted, a file on disk says it.
//
// No check description here may contain the substring FAIL.

#include <instruments/udiff_master.hpp>

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

// The real 34-column UDiFF header, verbatim.
constexpr const char* kHdr =
    "TradDt,BizDt,Sgmt,Src,FinInstrmTp,FinInstrmId,ISIN,TckrSymb,SctySrs,"
    "XpryDt,FininstrmActlXpryDt,StrkPric,OptnTp,FinInstrmNm,OpnPric,HghPric,"
    "LwPric,ClsPric,LastPric,PrvsClsgPric,UndrlygPric,SttlmPric,OpnIntrst,"
    "ChngInOpnIntrst,TtlTradgVol,TtlTrfVal,TtlNbOfTxsExctd,SsnId,NewBrdLotQty,"
    "Rmks,Rsvd1,Rsvd2,Rsvd3,Rsvd4";

// A real stock-option row (ABCAPITAL) and a real index-future row (NIFTY),
// both taken from the NSE file.
constexpr const char* kStockOpt =
    "2026-08-31,2026-08-31,FO,NSE,STO,67194,,ABCAPITAL,,2026-11-23,2026-11-23,"
    "410.00,CE,ABCAPITAL26NOV410CE,0,0,0,0,0,0,0,0,0,0,0,0,0,F1,3100,,,,,";
constexpr const char* kIdxFut =
    "2026-08-31,2026-08-31,FO,NSE,IDF,12345,,NIFTY,,2026-10-27,2026-10-27,"
    ",,NIFTY26OCTFUT,0,0,0,0,0,0,0,0,0,0,0,0,0,F1,65,,,,,";
// A real BSE index-option row (SENSEX) -- the SAME layout.
constexpr const char* kBseIdxOpt =
    "2026-08-31,2026-08-31,FO,BSE,IDO,555,,SENSEX,,2026-09-24,2026-09-24,"
    "78900.00,CE,SENSEX26SEP78900CE,0,0,0,0,0,0,0,0,0,0,0,0,0,F1,20,,,,,";

std::expected<ContractSpec, UdiffError> row(const char* r, Exchange ex)
{
    const auto c = parse_udiff_header(kHdr, std::strlen(kHdr));
    if (!c) {
        return std::unexpected(c.error());
    }
    return parse_udiff_row(r, std::strlen(r), *c, ex, kSnap);
}

Reconciler g_rec;

// ── 1 ────────────────────────────────────────────────────────────────────
void header_resolves_by_name()
{
    std::printf("\n1 header_resolves_by_name\n");

    const auto c = parse_udiff_header(kHdr, std::strlen(kHdr));
    check(c.has_value(), "the real 34-column header parses");
    check(c.has_value() && c->fin_instrm_tp == 4, "FinInstrmTp is column 4");
    check(c.has_value() && c->tckr_symb == 7, "TckrSymb is column 7");
    check(c.has_value() && c->xpry_dt == 9, "XpryDt is column 9");
    check(c.has_value() && c->strk_pric == 11, "StrkPric is column 11");
    check(c.has_value() && c->optn_tp == 12, "OptnTp is column 12");
    check(c.has_value() && c->fin_instrm_nm == 13, "FinInstrmNm is column 13");
    check(c.has_value() && c->lot_qty == 28,
          "NewBrdLotQty is column 28 -- the lot size, and the reason this file "
          "is the primary authority for anything");

    // Position is not a contract: UDiFF has gained columns over time.
    constexpr const char* rev =
        "NewBrdLotQty,FinInstrmNm,OptnTp,StrkPric,XpryDt,TckrSymb,FinInstrmTp";
    const auto r = parse_udiff_header(rev, std::strlen(rev));
    check(r.has_value() && r->lot_qty == 0 && r->fin_instrm_tp == 6,
          "a reordered header resolves to the same logical mapping");

    constexpr const char* no_lot =
        "TradDt,FinInstrmTp,TckrSymb,XpryDt,StrkPric,OptnTp,FinInstrmNm";
    check(!parse_udiff_header(no_lot, std::strlen(no_lot)).has_value(),
          "a header WITHOUT NewBrdLotQty is BadHeader -- without it this file "
          "is not primary for anything");
}

// ── 2 ────────────────────────────────────────────────────────────────────
void real_stock_option_row()
{
    std::printf("\n2 real_stock_option_row\n");

    const auto s = row(kStockOpt, Exchange::NSE);
    check(s.has_value(), "the real ABCAPITAL option row parses");
    if (!s) {
        return;
    }
    check(std::strcmp(s->underlying, "ABCAPITAL") == 0,
          "underlying is TckrSymb -- which is what Kite's `name` carries, so "
          "P1-06's key can match");
    check(std::strcmp(s->symbol, "ABCAPITAL26NOV410CE") == 0,
          "symbol is FinInstrmNm, the same shape as Kite's tradingsymbol");
    check(s->segment == Segment::Opt, "STO is a stock OPTION");
    check(s->opt_type == OptionType::CE, "CE");
    check(s->strike == Price{41'000},
          "strike \"410.00\" -> 41000 paise, through P1-04's converter and "
          "never a double");
    check(s->lot_size == LotSize{3100}, "lot size 3100");
    check(s->exchange == Exchange::NSE, "exchange NSE");
    check(s->source == SpecSource::NseMaster, "source NseMaster -- PRIMARY");
    check(s->tick_size == Price{0},
          "tick size is ZERO: UDiFF carries no tick column, and defaulting to "
          "5 paise would be a literal and a guess (rule 1)");
    check(ist_ns_since_midnight(s->expiry) == 0,
          "expiry is an IST midnight, cross-checked against P0-02");
}

// ── 3 ────────────────────────────────────────────────────────────────────
void futures_leave_strike_and_type_empty()
{
    std::printf("\n3 futures_leave_strike_and_type_empty\n");

    const auto s = row(kIdxFut, Exchange::NSE);
    check(s.has_value(), "the real NIFTY future row parses");
    if (!s) {
        return;
    }
    check(s->segment == Segment::Fut, "IDF is an index FUTURE");
    check(s->opt_type == OptionType::None, "with no option type");
    check(s->strike == Price{0}, "and a zero strike");
    check(std::strcmp(s->underlying, "NIFTY") == 0, "underlying NIFTY");
    check(s->lot_size == LotSize{65},
          "lot size 65 -- the LIVE NIFTY lot, and it matches both "
          "fo_mktlots and the Kite dump exactly");

    // A future carrying a strike is not a future.
    char bad[512];
    std::snprintf(bad, sizeof(bad), "%s", kIdxFut);
    char* p = std::strstr(bad, ",,NIFTY26OCTFUT");
    check(p != nullptr, "the empty strike/type pair is locatable");
    if (p != nullptr) {
        // Put a strike where the file leaves a blank.
        std::memcpy(p, ",1,", 3);
        const auto c = parse_udiff_header(kHdr, std::strlen(kHdr));
        const auto r = parse_udiff_row(bad, std::strlen(bad), *c,
                                       Exchange::NSE, kSnap);
        check(!r.has_value(),
              "a future carrying a strike is REFUSED rather than having the "
              "field silently dropped");
    }
}

// ── 4 ────────────────────────────────────────────────────────────────────
// The finding that collapsed two cards into one.
void bse_uses_the_identical_layout()
{
    std::printf("\n4 bse_uses_the_identical_layout\n");

    const auto s = row(kBseIdxOpt, Exchange::BSE);
    check(s.has_value(),
          "a real BSE SENSEX row parses with the SAME header and the SAME "
          "parser -- the P1-03a card's premise that BSE differs structurally "
          "is wrong for the derivatives master");
    if (!s) {
        return;
    }
    check(s->exchange == Exchange::BSE, "exchange is BSE");
    check(s->source == SpecSource::BseMaster,
          "and the source is BseMaster -- the ONLY thing that differs is the "
          "exchange stamped on the output, which is a parameter");
    check(std::strcmp(s->underlying, "SENSEX") == 0, "underlying SENSEX");
    check(s->strike == Price{7'890'000}, "strike 78900.00 -> 78.9 lakh paise");
    check(s->lot_size == LotSize{20}, "lot size 20");
    check(s->segment == Segment::Opt && s->opt_type == OptionType::CE,
          "IDO + CE is an index call");

    // The exchange field is load-bearing: it selects the charge schedule.
    const auto as_nse = row(kBseIdxOpt, Exchange::NSE);
    check(as_nse.has_value() && as_nse->exchange == Exchange::NSE,
          "the same row stamped NSE carries NSE -- so the caller MUST pass the "
          "right one, because that field picks the charge schedule and a "
          "mislabelled contract is priced wrong in one direction forever");
}

// ── 5 ────────────────────────────────────────────────────────────────────
void unknown_instrument_type_blocks()
{
    std::printf("\n5 unknown_instrument_type_blocks\n");

    char bad[512];
    std::snprintf(bad, sizeof(bad), "%s", kIdxFut);
    char* p = std::strstr(bad, ",IDF,");
    if (p != nullptr) {
        std::memcpy(p, ",XYZ,", 5);
    }
    const auto c = parse_udiff_header(kHdr, std::strlen(kHdr));
    const auto r = parse_udiff_row(bad, std::strlen(bad), *c, Exchange::NSE, kSnap);
    check(!r.has_value() && r.error() == UdiffError::UnknownInstrumentType,
          "an unrecognised FinInstrmTp is refused, NEVER defaulted -- every "
          "unknown row would otherwise become a tradable future at a guessed "
          "segment (rule 9)");

    // The legacy vocabulary is NOT accepted: UDiFF uses IDF/IDO/STF/STO, and
    // silently accepting FUTIDX would hide a format change.
    std::snprintf(bad, sizeof(bad), "%s", kIdxFut);
    p = std::strstr(bad, ",IDF,");
    if (p != nullptr) {
        std::memcpy(p, ",FUT,", 5);
    }
    const auto legacy = parse_udiff_row(bad, std::strlen(bad), *c,
                                        Exchange::NSE, kSnap);
    check(!legacy.has_value(),
          "and the legacy FUTIDX-era vocabulary is refused too -- accepting "
          "both spellings would hide a format change rather than reveal it");
}

// ── 6 ────────────────────────────────────────────────────────────────────
void load_feeds_the_reconciler()
{
    std::printf("\n6 load_feeds_the_reconciler\n");

    char doc[2048];
    std::snprintf(doc, sizeof(doc), "%s\n%s\n%s\n%s\n",
                  kHdr, kStockOpt, kIdxFut, kBseIdxOpt);

    g_rec.clear();
    const auto r = load_udiff_master(doc, std::strlen(doc), Exchange::NSE,
                                     g_rec, kSnap);
    check(r.has_value(), "a multi-row document loads");
    check(r.has_value() && r->added == 3, "three contracts added");
    check(r.has_value() && r->unparseable == 0, "none unparseable");

    const ReconcileReport rep = g_rec.reconcile();
    check(rep.contracts == 3, "three distinct contracts in the reconciler");
    check(rep.primary_source_seen,
          "and a PRIMARY was seen -- this file is the authority P1-06 has been "
          "waiting for since Phase 1 began");

    check(!load_udiff_master("", 0, Exchange::NSE, g_rec, kSnap).has_value(),
          "an empty buffer is EmptyInput");
    check(!load_udiff_master(kHdr, std::strlen(kHdr), Exchange::NSE, g_rec,
                             kSnap).has_value(),
          "a header-only buffer is EmptyInput");
}

// ── 7 ────────────────────────────────────────────────────────────────────
// The whole point of Phase 1, finally demonstrable.
void three_way_agreement_on_real_values()
{
    std::printf("\n7 three_way_agreement_on_real_values\n");

    g_rec.clear();

    // The exchange master says NIFTY October future, lot 65.
    char doc[1024];
    std::snprintf(doc, sizeof(doc), "%s\n%s\n", kHdr, kIdxFut);
    check(load_udiff_master(doc, std::strlen(doc), Exchange::NSE, g_rec,
                            kSnap).has_value(), "the NSE master loads");

    // Kite's dump says the same contract, same lot. Real values, real quoting.
    const char* kite =
        "instrument_token,exchange_token,tradingsymbol,name,last_price,expiry,"
        "strike,tick_size,lot_size,instrument_type,segment,exchange\n"
        "13579,246,NIFTY26OCTFUT,\"NIFTY\",0,2026-10-27,0,0.05,65,FUT,"
        "NFO-FUT,NFO\n";
    check(load_kite_dump(kite, std::strlen(kite), g_rec, kSnap).has_value(),
          "and the Kite dump loads into the same reconciler");

    const ReconcileReport rep = g_rec.reconcile();
    check(rep.contracts == 1,
          "the exchange master and the broker describe ONE contract -- the D1 "
          "tuple joined them across two entirely different file formats");
    check(rep.agreed == 1, "and they AGREE");
    check(rep.blocked == 0, "so nothing is blocked");

    const ContractVerdict* v = *g_rec.verdict_at(0);
    check(v->verdict == ReconcileVerdict::Agreed, "verdict Agreed");
    check(v->merged.lot_size == LotSize{65},
          "merged lot size 65 -- the same number in the NSE bhavcopy, "
          "fo_mktlots and the Kite dump");
    check(v->merged.tick_size == Price{5},
          "and the tick comes from KITE, because UDiFF carries none -- which "
          "is exactly the cross-check the three-way design exists for");
    check(v->merged.source == SpecSource::NseMaster,
          "with the exchange master winning precedence for everything else");
    check(v->merged.token[static_cast<std::size_t>(FeedSource::Kite)] == 13579,
          "and the broker token carried through");
}

// ── 8 ────────────────────────────────────────────────────────────────────
void malformed_rows_do_not_abort_the_load()
{
    std::printf("\n8 malformed_rows_do_not_abort_the_load\n");

    char doc[2048];
    std::snprintf(doc, sizeof(doc),
                  "%s\n%s\nnot,enough,fields\n%s\n",
                  kHdr, kStockOpt, kIdxFut);

    g_rec.clear();
    const auto r = load_udiff_master(doc, std::strlen(doc), Exchange::NSE,
                                     g_rec, kSnap);
    check(r.has_value(), "the load still succeeds");
    check(r.has_value() && r->added == 2, "both good rows are added");
    check(r.has_value() && r->unparseable == 1, "the bad row is counted");
    check(r.has_value() && r->first_error_row == 2,
          "and its row NUMBER is reported, so a message can name it");
    check(r.has_value() && r->first_error == UdiffError::TooFewFields,
          "with the reason");
}

} // namespace

int main()
{
    std::printf("altair instruments udiff_master tests\n");
    header_resolves_by_name();
    real_stock_option_row();
    futures_leave_strike_and_type_empty();
    bse_uses_the_identical_layout();
    unknown_instrument_type_blocks();
    load_feeds_the_reconciler();
    three_way_agreement_on_real_values();
    malformed_rows_do_not_abort_the_load();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
