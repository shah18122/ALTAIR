// P1-04 acceptance tests for instruments/kite_dump.hpp.
// Plain main() (Catch2 blocked on vcpkg — see LEDGER blocker #7).

#include <instruments/kite_dump.hpp>

#include <chrono>
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

constexpr Timestamp kSnap{1787888700000000000LL};   // 2026-08-28 09:15 IST

std::expected<Price, KiteParseError> rp(const char* s)
{
    return parse_rupees_to_paise(s, std::strlen(s));
}

std::expected<Timestamp, KiteParseError> ex(const char* s)
{
    return parse_kite_expiry(s, std::strlen(s));
}

constexpr const char* kHeader =
    "instrument_token,exchange_token,tradingsymbol,name,last_price,expiry,"
    "strike,tick_size,lot_size,instrument_type,segment,exchange";

std::expected<ContractSpec, KiteParseError> row(const char* r)
{
    const auto c = parse_kite_header(kHeader, std::strlen(kHeader));
    if (!c.has_value()) {
        return std::unexpected(c.error());
    }
    return parse_kite_row(r, std::strlen(r), c.value(), kSnap);
}

} // namespace

void test_kite_rupees_to_paise()
{
    check(rp("0.05").value() == Price{5},
          "\"0.05\" -> 5 paise — the value a double truncates to 4");
    check(rp("0.5").value() == Price{50}, "\"0.5\" -> 50");
    check(rp("0.50").value() == Price{50}, "\"0.50\" -> 50");
    check(rp("1").value() == Price{100}, "\"1\" -> 100");
    check(rp("25000").value() == Price{2'500'000}, "\"25000\" -> 25 lakh paise");
    check(rp("25000.5").value() == Price{2'500'050}, "\"25000.5\" -> 2500050");
    check(rp("1.23").value() == Price{123}, "\"1.23\" -> 123");
    check(rp("0").value() == Price{0}, "\"0\" -> 0");
    check(rp("0.00").value() == Price{0}, "\"0.00\" -> 0");
    check(rp("-1.5").value() == Price{-150}, "\"-1.5\" -> -150");
    check(rp("-0.05").value() == Price{-5}, "\"-0.05\" -> -5");
    check(rp(".5").value() == Price{50}, "a leading dot is legal");
    check(rp("5.").value() == Price{500}, "a trailing dot is legal");

    // Every hundredth from 0.01 to 1.00, exactly. This is the loop a
    // double-based implementation fails somewhere in the middle.
    bool all_exact = true;
    char buf[16];
    for (int i = 1; i <= 100; ++i) {
        std::snprintf(buf, sizeof(buf), "%d.%02d", i / 100, i % 100);
        const auto r = rp(buf);
        if (!r.has_value() || r.value() != Price{i}) { all_exact = false; break; }
    }
    check(all_exact, "every hundredth 0.01..1.00 round-trips EXACTLY");
}

void test_kite_rupees_rejects_bad_input()
{
    check(rp("").error() == KiteParseError::BadNumber, "empty is NOT zero");
    check(rp("-").error() == KiteParseError::BadNumber, "lone minus rejected");
    check(rp(".").error() == KiteParseError::BadNumber, "lone dot rejected");
    check(rp("abc").error() == KiteParseError::BadNumber, "letters rejected");
    check(rp("1.2.3").error() == KiteParseError::BadNumber, "two dots rejected");
    check(rp("1e5").error() == KiteParseError::BadNumber, "no exponent form");
    check(rp("1 2").error() == KiteParseError::BadNumber, "embedded space rejected");
    check(rp("0.001").error() == KiteParseError::BadDecimal,
          "three decimals is finer than a paisa");
    check(rp("1.234").error() == KiteParseError::BadDecimal, "and so is this");
    check(rp("99999999999999999999").error() == KiteParseError::Overflow,
          "past int64 paise is Overflow");
}

void test_kite_expiry_is_ist()
{
    // 2026-08-28 UTC midnight is 1787875200 s. IST midnight is 5h30m EARLIER
    // in UTC terms — 18:30 the previous day.
    const std::int64_t utc_mid = 1787875200000000000LL;
    check(ex("2026-08-28").value() == Timestamp{utc_mid - 19'800'000'000'000LL},
          "IST midnight is 18:30 UTC the previous day");
    check(ist_ns_since_midnight(ex("2026-08-28").value()) == 0,
          "and it IS an IST midnight — cross-checked against P0-02");

    check(ex("1970-01-01").value() == Timestamp{-19'800'000'000'000LL},
          "IST midnight before the epoch is negative");
    check(ex("").value() == Timestamp::epoch(),
          "an empty expiry is cash, NOT an error");

    check(ex("2026-8-28").error() == KiteParseError::BadDate, "no zero padding");
    check(ex("2026/08/28").error() == KiteParseError::BadDate, "wrong separator");
    check(ex("28-08-2026").error() == KiteParseError::BadDate, "wrong order");
    check(ex("2026-13-01").error() == KiteParseError::BadDate, "month 13");
    check(ex("2026-08-32").error() == KiteParseError::BadDate, "day 32");
    check(ex("2026-02-30").error() == KiteParseError::BadDate, "30 February");
    check(ex("garbage").error() == KiteParseError::BadDate, "garbage");
}

void test_kite_header_column_order_independent()
{
    const auto a = parse_kite_header(kHeader, std::strlen(kHeader));
    check(a.has_value(), "the documented header parses");
    check(a.value().instrument_token == 0, "instrument_token is column 0");
    check(a.value().exchange == 11, "exchange is column 11");

    // The same columns, reversed. Order must not matter — Kite has reordered
    // this file before.
    constexpr const char* rev =
        "exchange,segment,instrument_type,lot_size,tick_size,strike,expiry,"
        "last_price,name,tradingsymbol,exchange_token,instrument_token";
    const auto b = parse_kite_header(rev, std::strlen(rev));
    check(b.has_value(), "the reversed header parses too");
    check(b.value().instrument_token == 11, "instrument_token is now column 11");
    check(b.value().exchange == 0, "exchange is now column 0");

    constexpr const char* missing =
        "instrument_token,exchange_token,tradingsymbol,name,last_price,expiry,"
        "strike,tick_size,instrument_type,segment,exchange";   // no lot_size
    check(parse_kite_header(missing, std::strlen(missing)).error()
              == KiteParseError::BadHeader,
          "a missing required column is BadHeader");
    check(parse_kite_header("", 0).error() == KiteParseError::BadHeader,
          "an empty header is BadHeader");
}

void test_kite_row_to_spec()
{
    constexpr const char* opt =
        "12345,678,NIFTY26SEP25000CE,NIFTY,0,2026-09-24,25000,0.05,75,CE,NFO-OPT,NFO";
    const auto s = row(opt);
    check(s.has_value(), "a NIFTY option row parses");
    if (s.has_value()) {
        check(s->token[static_cast<std::size_t>(FeedSource::Kite)] == 12345,
              "Kite token set");
        check(s->token[static_cast<std::size_t>(FeedSource::Xts)] == 0,
              "XTS token is 0 — this source knows nothing about XTS");
        check(std::strcmp(s->symbol, "NIFTY26SEP25000CE") == 0, "symbol");
        check(std::strcmp(s->underlying, "NIFTY") == 0, "underlying");
        check(s->strike == Price{2'500'000}, "strike 25000 -> 25 lakh paise");
        check(s->tick_size == Price{5}, "tick 0.05 -> 5 paise, NOT 4");
        check(s->lot_size == LotSize{75}, "lot size 75");
        check(s->opt_type == OptionType::CE, "CE");
        check(s->exchange == Exchange::NSE, "NFO -> NSE");
        check(s->segment == Segment::Opt, "NFO + CE -> Opt");
        check(s->price_scale == 100, "price scale 100");
        check(s->source == SpecSource::KiteDump, "source is KiteDump");
        check(!s->stale, "not stale");
        check(s->valid_from == kSnap, "valid_from is the snapshot");
        check(s->valid_to == Timestamp::max(), "valid_to is open");
        check(ist_ns_since_midnight(s->expiry) == 0, "expiry is an IST midnight");
    }

    constexpr const char* fut =
        "12346,679,NIFTY26SEPFUT,NIFTY,0,2026-09-24,0,0.05,75,FUT,NFO-FUT,NFO";
    const auto f = row(fut);
    check(f.has_value() && f->segment == Segment::Fut, "NFO + FUT -> Fut");
    check(f.has_value() && f->opt_type == OptionType::None, "no option type");

    constexpr const char* cash =
        "738561,2885,RELIANCE,RELIANCE INDUSTRIES,0,,0,0.05,1,EQ,NSE,NSE";
    const auto c = row(cash);
    check(c.has_value() && c->segment == Segment::Cash, "NSE -> Cash");
    check(c.has_value() && c->expiry == Timestamp::epoch(), "cash has no expiry");

    // The row's own bytes drive source_hash.
    const auto again = row(opt);
    check(again.has_value() && again->source_hash == s->source_hash,
          "the same row hashes the same");
    constexpr const char* edited =
        "12345,678,NIFTY26SEP25000CE,NIFTY,0,2026-09-24,25000,0.05,50,CE,NFO-OPT,NFO";
    const auto e = row(edited);
    check(e.has_value() && e->source_hash != s->source_hash,
          "one edited byte moves source_hash");
}

void test_kite_row_rejects_bad_input()
{
    check(row("1,2,3").error() == KiteParseError::TooFewFields, "short row");
    check(row("1,2,SYM,N,0,2026-09-24,0,0.05,abc,FUT,NFO-FUT,NFO").error()
              == KiteParseError::BadNumber, "lot_size \"abc\"");
    check(row("1,2,SYM,N,0,2026-09-24,0,0.001,75,FUT,NFO-FUT,NFO").error()
              == KiteParseError::BadDecimal, "tick_size 0.001");
    check(row("1,2,SYM,N,0,2026-13-01,0,0.05,75,FUT,NFO-FUT,NFO").error()
              == KiteParseError::BadDate, "expiry month 13");
    check(row("1,2,SYM,N,0,2026-09-24,0,0.05,75,FUT,MCX-FUT,MCX").error()
              == KiteParseError::UnknownExchange, "exchange MCX");
    check(row("1,2,,N,0,2026-09-24,0,0.05,75,FUT,NFO-FUT,NFO").error()
              == KiteParseError::BadSymbol, "empty symbol");

    char long_sym[64];
    std::memset(long_sym, 'S', 32);
    long_sym[32] = '\0';
    char r32[256];
    std::snprintf(r32, sizeof(r32),
                  "1,2,%s,N,0,2026-09-24,0,0.05,75,FUT,NFO-FUT,NFO", long_sym);
    check(row(r32).error() == KiteParseError::BadSymbol, "32-char symbol rejected");

    long_sym[31] = '\0';
    char r31[256];
    std::snprintf(r31, sizeof(r31),
                  "1,2,%s,N,0,2026-09-24,0,0.05,75,FUT,NFO-FUT,NFO", long_sym);
    check(row(r31).has_value(), "31-char symbol accepted");
}

void test_kite_load_dump_skips_bad_rows()
{
    static SpecStore st;
    // Rows 2 and 4 are malformed: a bad number and a bad date.
    static const char dump[] =
        "instrument_token,exchange_token,tradingsymbol,name,last_price,expiry,"
        "strike,tick_size,lot_size,instrument_type,segment,exchange\n"
        "1,11,SYMA,NIFTY,0,2026-09-24,0,0.05,75,FUT,NFO-FUT,NFO\n"
        "2,12,SYMB,NIFTY,0,2026-09-24,0,0.05,abc,FUT,NFO-FUT,NFO\n"
        "3,13,SYMC,NIFTY,0,2026-09-24,0,0.05,75,FUT,NFO-FUT,NFO\n"
        "4,14,SYMD,NIFTY,0,2026-13-99,0,0.05,75,FUT,NFO-FUT,NFO\n"
        "5,15,SYME,NIFTY,0,2026-09-24,0,0.05,75,FUT,NFO-FUT,NFO\n";

    const auto r = load_kite_dump_unreconciled(dump, sizeof(dump) - 1, st, kSnap);
    check(r.has_value(), "a dump with bad rows still loads");
    if (r.has_value()) {
        check(r->added == 3, "three good rows added");
        check(r->unparseable == 2, "two bad rows counted");
        check(r->first_error_row == 2, "the first bad row is row 2");
        check(r->first_error == KiteParseError::BadNumber, "and its error is recorded");
    }
    check(st.id_of(FeedSource::Kite, 1).has_value(), "row 1 is in the store");
    check(st.id_of(FeedSource::Kite, 5).has_value(), "row 5 survived the bad rows");
    check(st.id_of(FeedSource::Kite, 2).error() == SpecError::NotFound,
          "the bad row is absent");

    // Every row bad still yields a report, not an error.
    static SpecStore st2;
    static const char allbad[] =
        "instrument_token,exchange_token,tradingsymbol,name,last_price,expiry,"
        "strike,tick_size,lot_size,instrument_type,segment,exchange\n"
        "1,11,SYMA,NIFTY,0,2026-09-24,0,0.05,abc,FUT,NFO-FUT,NFO\n"
        "2,12,SYMB,NIFTY,0,2026-09-24,0,0.05,abc,FUT,NFO-FUT,NFO\n";
    const auto r2 = load_kite_dump_unreconciled(allbad, sizeof(allbad) - 1, st2, kSnap);
    check(r2.has_value() && r2->added == 0 && r2->unparseable == 2,
          "an all-bad dump reports rather than errors");

    check(load_kite_dump_unreconciled("", 0, st2, kSnap).error() == KiteParseError::EmptyInput,
          "an empty buffer is EmptyInput");
    static const char hdr_only[] =
        "instrument_token,exchange_token,tradingsymbol,name,last_price,expiry,"
        "strike,tick_size,lot_size,instrument_type,segment,exchange\n";
    check(load_kite_dump_unreconciled(hdr_only, sizeof(hdr_only) - 1, st2, kSnap).error()
              == KiteParseError::EmptyInput,
          "a header-only buffer is EmptyInput");
}

void test_kite_load_dump_into_store()
{
    static SpecStore st;
    static const char dump[] =
        "instrument_token,exchange_token,tradingsymbol,name,last_price,expiry,"
        "strike,tick_size,lot_size,instrument_type,segment,exchange\n"
        "12345,678,NIFTY26SEPFUT,NIFTY,0,2026-09-24,0,0.05,75,FUT,NFO-FUT,NFO\n"
        "12346,679,BANKNIFTY26SEPFUT,BANKNIFTY,0,2026-09-24,0,0.05,30,FUT,NFO-FUT,NFO\n";

    const auto r = load_kite_dump_unreconciled(dump, sizeof(dump) - 1, st, kSnap);
    check(r.has_value() && r->added == 2, "two rows loaded");
    check(st.size() == 2, "store size matches the report");

    const auto id = st.id_of(FeedSource::Kite, 12345);
    check(id.has_value(), "resolves by Kite token");
    check(st.current(id.value()).value()->lot_size == LotSize{75}, "lot size 75");
    check(st.token_of(id.value(), FeedSource::Xts).error() == SpecError::NotFound,
          "no XTS token yet — P1-05 supplies it");

    // A second load of the SAME dump: every row is a duplicate token. The
    // store's guard is what makes a re-load idempotent.
    const auto r2 = load_kite_dump_unreconciled(dump, sizeof(dump) - 1, st, kSnap);
    check(r2.has_value(), "a re-load returns a report");
    check(r2->added == 0, "nothing added the second time");
    check(r2->rejected_by_sink == 2, "both rows rejected as duplicates");
    check(st.size() == 2, "and the store is unchanged");
}

namespace {

void report_throughput()
{
    std::printf("\nthroughput — pre-open path, no budget\n");
    constexpr std::size_t kRows = 10'000;
    static char buf[kRows * 80 + 256];
    std::size_t n = 0;
    n += static_cast<std::size_t>(
        std::snprintf(buf + n, sizeof(buf) - n, "%s\n", kHeader));
    for (std::size_t i = 0; i < kRows; ++i) {
        n += static_cast<std::size_t>(std::snprintf(
            buf + n, sizeof(buf) - n,
            "%zu,%zu,SYM%zu,NIFTY,0,2026-09-24,25000,0.05,75,CE,NFO-OPT,NFO\n",
            i + 1, i + 1, i));
    }

    static SpecStore st;
    const auto t0 = std::chrono::steady_clock::now();
    const auto r = load_kite_dump_unreconciled(buf, n, st, kSnap);
    const auto t1 = std::chrono::steady_clock::now();
    const double sec = std::chrono::duration<double>(t1 - t0).count();

    if (r.has_value()) {
        std::printf("  %zu rows parsed, %zu added, %zu rejected in %.4f s\n",
                    kRows, r->added, r->rejected_by_sink, sec);
        std::printf("  = %.2f M rows/s (runs once pre-open, not on a tick)\n",
                    static_cast<double>(kRows) / sec / 1e6);
    } else {
        std::printf("  load failed\n");
    }
}

} // namespace

namespace {

// The Reconciler overload added when P1-06 landed. Kite is a CROSS-CHECK, not
// an authority: loading it into the reconciler is what subjects it to the
// three-way check. The unreconciled overload cannot do this, which is why it
// is now named for what it skips.
Reconciler g_rec;

// Found by running the real 106'150-row dump: Kite quotes the `name` column.
// Left in place, the underlying becomes "NIFTY" WITH the quote characters, and
// P1-06's D1 key can never match an exchange master that writes NIFTY bare --
// so every contract reconciles as SingleSource and the three-way check
// silently does nothing at all.
void quoted_name_is_unquoted()
{
    std::printf("\nquoted name column\n");

    const char* dump =
        "instrument_token,exchange_token,tradingsymbol,name,last_price,expiry,"
        "strike,tick_size,lot_size,instrument_type,segment,exchange\n"
        "77777,43,NIFTY26SEPFUT,\"NIFTY\",0,2026-09-24,0,0.05,65,FUT,NFO-FUT,NFO\n";

    // static, NOT a local: SpecStore is 1.63 MB and MSVC's default stack is
    // 1 MB. A local here segfaults -- which it duly did, in a hazard this
    // repo had already documented in P1-06 and then walked into anyway.
    static SpecStore local;
    const auto r = load_kite_dump_unreconciled(dump, std::strlen(dump), local, kSnap);
    check(r.has_value() && r->added == 1, "the real quoted form parses");

    const auto id = local.id_of(FeedSource::Kite, 77777);
    check(id.has_value(), "and resolves");
    const auto sp = local.current(*id);
    check(sp.has_value() && std::strcmp((*sp)->underlying, "NIFTY") == 0,
          "the underlying is NIFTY, WITHOUT the quote characters -- with them "
          "it could never match an exchange master and every contract would "
          "reconcile as SingleSource");
    check(sp.has_value() && std::strcmp((*sp)->symbol, "NIFTY26SEPFUT") == 0,
          "and an unquoted column is untouched");
    check(sp.has_value() && (*sp)->lot_size.raw() == 65,
          "lot size 65 -- the live NIFTY lot, not the 75 of earlier examples");
}

void loads_into_the_reconciler()
{
    std::printf("\nreconciled load path\n");
    g_rec.clear();

    const char* dump =
        "instrument_token,exchange_token,tradingsymbol,name,last_price,expiry,"
        "strike,tick_size,lot_size,instrument_type,segment,exchange\n"
        "11111,43,NIFTY26SEP25000CE,NIFTY,0,2026-09-24,25000,0.05,75,CE,NFO-OPT,NFO\n";

    const auto r = load_kite_dump(dump, std::strlen(dump), g_rec, kSnap);
    check(r.has_value(), "a dump loads into a Reconciler");
    check(r.has_value() && r->added == 1, "one contract added");

    // Alone, Kite is uncorroborated -- and says so.
    ReconcileReport rep = g_rec.reconcile();
    check(rep.single_source == 1,
          "Kite alone is SingleSource, not silently authoritative");
    check(!rep.primary_source_seen, "and no primary was seen");

    // Now an exchange master that DISAGREES about the lot size. Through the
    // store this would simply have overwritten nothing and gone unnoticed;
    // through the reconciler it blocks.
    ContractSpec nse{};
    nse.id = InstrumentId::Invalid;
    nse.lot_size = LotSize{50};
    nse.tick_size = Price{5};
    nse.strike = Price{2500000};
    nse.price_scale = 100;
    nse.expiry = *parse_kite_expiry("2026-09-24", 10);
    nse.valid_from = kSnap;
    nse.valid_to = Timestamp::max();
    nse.source_hash = 0x9911;
    nse.snapshot_at = kSnap;
    nse.exchange = Exchange::NSE;
    nse.segment = Segment::Opt;
    nse.opt_type = OptionType::CE;
    nse.source = SpecSource::NseMaster;
    std::snprintf(nse.symbol, sizeof(nse.symbol), "NIFTY26SEP25000CE");
    std::snprintf(nse.underlying, sizeof(nse.underlying), "NIFTY");
    check(g_rec.add(nse).has_value(), "an NSE master row is added");

    rep = g_rec.reconcile();
    check(rep.contracts == 1,
          "the Kite row and the NSE row are ONE contract -- the tuple joined "
          "them across two number spaces");
    check(rep.conflicts == 1, "and they disagree on lot size");
    check(rep.blocked == 1, "so the symbol is BLOCKED, not quietly resolved");

    const ContractVerdict* v = *g_rec.verdict_at(0);
    check(v->field == ConflictField::LotSize, "the conflict is the lot size");
    check(v->values[static_cast<std::size_t>(SpecSource::KiteDump)] == 75,
          "Kite said 75");
    check(v->values[static_cast<std::size_t>(SpecSource::NseMaster)] == 50,
          "the exchange said 50 -- and neither is guessed at");
}

} // namespace

int main()
{
    std::printf("altair instruments kite_dump tests\n");
    test_kite_rupees_to_paise();
    test_kite_rupees_rejects_bad_input();
    test_kite_expiry_is_ist();
    test_kite_header_column_order_independent();
    test_kite_row_to_spec();
    test_kite_row_rejects_bad_input();
    test_kite_load_dump_skips_bad_rows();
    test_kite_load_dump_into_store();
    quoted_name_is_unquoted();
    loads_into_the_reconciler();

    report_throughput();

    if (failures == 0) {
        std::printf("\nPASS\n");
    } else {
        std::printf("\nFAILED (%d)\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
