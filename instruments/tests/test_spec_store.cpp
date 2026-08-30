// P1-01 acceptance tests for instruments/contract_spec.hpp.
// Plain main() (Catch2 blocked on vcpkg — see LEDGER blocker #7).

#include <instruments/contract_spec.hpp>

#include <chrono>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

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

// The P0-02 anchor: 2026-08-28 09:15:00 IST, NSE market open.
constexpr Timestamp kOpen{1787888700000000000LL};

/// Build a spec. Everything a test does not care about gets a sane default,
/// so each test reads as the one thing it is checking.
ContractSpec mk(const char* symbol, std::uint32_t kite_tok, std::uint32_t xts_tok,
                std::int64_t lot = 75, std::uint64_t src_hash = 0xABCD)
{
    ContractSpec s{};
    s.id = InstrumentId::Invalid;          // the store must overwrite this
    s.token[static_cast<std::size_t>(FeedSource::Kite)] = kite_tok;
    s.token[static_cast<std::size_t>(FeedSource::Xts)]  = xts_tok;
    s.lot_size = LotSize{lot};
    s.tick_size = Price{5};
    s.strike = Price{0};
    s.freeze_qty = Qty{1800};
    s.band_lower = Price{0};
    s.band_upper = Price{0};
    s.price_scale = 100;                   // equity / F&O: the wire value IS paise
    s.expiry = kOpen + duration::days(30);
    s.valid_from = kOpen - duration::days(365);
    s.valid_to = Timestamp::max();
    s.source_hash = src_hash;
    s.snapshot_at = kOpen;
    s.exchange = Exchange::NSE;
    s.segment = Segment::Fut;
    s.opt_type = OptionType::None;
    s.source = SpecSource::NseMaster;
    s.stale = false;
    if (symbol != nullptr) {
        const std::size_t n = std::strlen(symbol);
        const std::size_t c = n < kMaxSymbolLen ? n : kMaxSymbolLen;
        std::memcpy(s.symbol, symbol, c);
        s.symbol[c] = '\0';
    }
    std::memcpy(s.underlying, "NIFTY", 6);
    return s;
}

} // namespace

void test_spec_traits_and_scale()
{
    check(std::is_trivially_copyable_v<ContractSpec>, "ContractSpec trivially copyable");
    check(default_price_scale(Segment::Fut) == 100, "F&O scale is 100 (paise)");
    check(default_price_scale(Segment::Cash) == 100, "cash scale is 100 (paise)");
    check(default_price_scale(Segment::Currency) == 10'000'000,
          "currency-derivative scale is 10^7 — finer than a paisa");

    check(price_scale_is_representable(100), "scale 100 is representable");
    check(!price_scale_is_representable(10'000'000),
          "10^7 is NOT representable in integer paise");
    check(!price_scale_is_representable(10'000), "10^4 is NOT representable either");

    check(kFeedSourceCount == 2, "two feed sources");
    check(sizeof(ContractSpec) > 0 && sizeof(SpecStore) > 0, "types have size");

    std::printf("        sizeof(ContractSpec) = %zu, sizeof(SpecStore) = %zu (%.1f MB)\n",
                sizeof(ContractSpec), sizeof(SpecStore),
                static_cast<double>(sizeof(SpecStore)) / (1024.0 * 1024.0));
}

void test_store_add_and_identity()
{
    static SpecStore st;
    check(st.size() == 0, "a fresh store is empty");

    const auto a = st.add(mk("NIFTY26SEPFUT", 256265, 35001));
    check(a.has_value(), "first add succeeds");
    check(a.value() == InstrumentId{0}, "ids are dense from 0");

    const auto b = st.add(mk("BANKNIFTY26SEPFUT", 260105, 35002, 30));
    check(b.has_value() && b.value() == InstrumentId{1}, "second id is 1");
    check(st.size() == 2, "size == 2");

    check(st.current(a.value()).value()->lot_size == LotSize{75}, "lot size round-trips");
    check(st.current(a.value()).value()->id == a.value(),
          "the STORE set the id, not the caller");
    check(st.current(InstrumentId{99}).error() == SpecError::NotFound,
          "an unknown id is NotFound");
}

void test_store_token_map_is_bidirectional()
{
    static SpecStore st;
    const auto a = st.add(mk("NIFTY26SEPFUT", 256265, 35001)).value();

    check(st.id_of(FeedSource::Kite, 256265).value() == a, "Kite token resolves");
    check(st.id_of(FeedSource::Xts, 35001).value() == a, "XTS token resolves");
    check(st.token_of(a, FeedSource::Kite).value() == 256265, "id -> Kite token");
    check(st.token_of(a, FeedSource::Xts).value() == 35001, "id -> XTS token");

    // THE REASON THIS CARD EXISTS: the same number in two feeds is two
    // different instruments. 35001 is this contract's XTS token and must not
    // resolve as a Kite token.
    check(st.id_of(FeedSource::Kite, 35001).error() == SpecError::NotFound,
          "an XTS token does NOT resolve in the Kite space");

    check(st.id_of(FeedSource::Kite, 999999).error() == SpecError::NotFound,
          "an unknown token is NotFound");

    // A feed that does not carry the contract uses token 0.
    const auto c = st.add(mk("SENSEX26SEPFUT", 274441, 0, 10)).value();
    check(st.token_of(c, FeedSource::Xts).error() == SpecError::NotFound,
          "token 0 means the feed does not carry it");
    check(st.id_of(FeedSource::Xts, 0).error() == SpecError::NotFound,
          "0 was never registered as a token");
}

void test_store_rejects_bad_input()
{
    static SpecStore st;

    check(st.add(mk(nullptr, 1, 1)).error() == SpecError::BadSymbol, "empty symbol rejected");
    check(st.add(mk("", 1, 1)).error() == SpecError::BadSymbol, "blank symbol rejected");

    char at_limit[kMaxSymbolLen + 1];
    std::memset(at_limit, 'S', kMaxSymbolLen);
    at_limit[kMaxSymbolLen] = '\0';
    check(st.add(mk(at_limit, 11, 12)).has_value(), "a symbol exactly at the limit fits");

    // THE CURRENCY-DERIVATIVE GUARD. Refused, not truncated.
    auto cds = mk("USDINR26SEPFUT", 21, 22);
    cds.segment = Segment::Currency;
    cds.price_scale = 10'000'000;
    check(st.add(cds).error() == SpecError::BadPriceScale,
          "a scale finer than a paisa is REFUSED, never truncated");

    const auto before = st.size();
    check(st.add(mk("NIFTY26SEPFUT", 256265, 35001)).has_value(), "add for duplicate test");
    check(st.add(mk("NIFTY26SEPFUT_DUP", 256265, 35001)).error()
              == SpecError::DuplicateToken,
          "a duplicate token is refused");
    check(st.size() == before + 1, "and the rejection changed nothing");
    check(st.id_of(FeedSource::Kite, 256265).has_value(),
          "the first instrument still resolves");
}

void test_store_point_in_time()
{
    static SpecStore st;

    // Two windows for the same contract, different lot sizes. Distinct tokens
    // so both can live in the store at once.
    auto old_s = mk("NIFTYFUT_OLD", 900001, 800001, 50);
    old_s.valid_from = kOpen - duration::days(60);
    old_s.valid_to   = kOpen - duration::days(30);
    const auto old_id = st.add(old_s).value();

    auto new_s = mk("NIFTYFUT_NEW", 900002, 800002, 75);
    new_s.valid_from = kOpen - duration::days(30);
    new_s.valid_to   = Timestamp::max();
    const auto new_id = st.add(new_s).value();

    check(st.at(old_id, kOpen - duration::days(45)).value()->lot_size == LotSize{50},
          "a backtest 45 days ago sees the OLD lot size");
    check(st.at(new_id, kOpen).value()->lot_size == LotSize{75},
          "today sees the new one");

    // Half-open: the boundary belongs to the NEW window only, so no instant
    // ever matches two specs.
    check(st.at(old_id, kOpen - duration::days(30)).error() == SpecError::NotValidAt,
          "the boundary is NOT in the old window");
    check(st.at(new_id, kOpen - duration::days(30)).has_value(),
          "the boundary IS in the new window");

    check(st.at(old_id, kOpen - duration::days(90)).error() == SpecError::NotValidAt,
          "before either window is NotValidAt");

    check(st.current(old_id).has_value(),
          "current() ignores validity — a backtest must use at()");
}

void test_store_blocking()
{
    static SpecStore st;
    const auto a = st.add(mk("NIFTY26SEPFUT", 256265, 35001)).value();
    const auto b = st.add(mk("BANKNIFTY26SEPFUT", 260105, 35002, 30)).value();

    check(st.block(a).has_value(), "block succeeds");
    check(st.is_blocked(a), "and is observable");
    check(st.blocked_count() == 1, "blocked_count == 1");

    // Blocked wins over everything, at any date.
    check(st.at(a, kOpen).error() == SpecError::Blocked, "at() reports Blocked");
    check(st.current(a).error() == SpecError::Blocked, "current() reports Blocked");

    check(st.block(a).has_value(), "block is idempotent");
    check(st.blocked_count() == 1, "and does not double-count");

    // Rule 9 blocks the symbol, not the engine.
    check(st.current(b).has_value(), "other instruments are unaffected");

    check(st.block(InstrumentId{999}).error() == SpecError::NotFound,
          "blocking an unknown id is NotFound");
}

void test_store_spec_version_order_independent()
{
    static SpecStore st1;
    static SpecStore st2;
    static SpecStore st3;

    (void)st1.add(mk("A", 1, 101, 75, 0x1111));
    (void)st1.add(mk("B", 2, 102, 50, 0x2222));
    (void)st1.add(mk("C", 3, 103, 25, 0x3333));

    // Same three specs, different order.
    (void)st2.add(mk("C", 3, 103, 25, 0x3333));
    (void)st2.add(mk("A", 1, 101, 75, 0x1111));
    (void)st2.add(mk("B", 2, 102, 50, 0x2222));

    check(st1.spec_version() == st2.spec_version(),
          "SAME SPECS, DIFFERENT ORDER -> same spec_version (rule 10)");

    (void)st3.add(mk("A", 1, 101, 75, 0x1111));
    (void)st3.add(mk("B", 2, 102, 50, 0x2222));
    (void)st3.add(mk("C", 3, 103, 25, 0x9999));   // one source_hash altered
    check(st3.spec_version() != st1.spec_version(),
          "any change to a source hash moves spec_version");

    static SpecStore e1;
    static SpecStore e2;
    check(e1.spec_version() == e2.spec_version(), "empty stores agree");
    check(e1.spec_version() != st1.spec_version(), "and differ from a populated one");
}

void test_store_capacity()
{
    static SpecStore st;
    char sym[16];
    bool all_ok = true;
    for (std::size_t i = 0; i < kMaxInstruments; ++i) {
        std::snprintf(sym, sizeof(sym), "SYM%zu", i);
        const auto r = st.add(mk(sym, static_cast<std::uint32_t>(i + 1),
                                 static_cast<std::uint32_t>(i + 1 + kMaxInstruments)));
        if (!r.has_value()) { all_ok = false; break; }
    }
    check(all_ok, "8192 distinct instruments all fit");
    check(st.size() == kMaxInstruments, "size == kMaxInstruments");

    check(st.add(mk("ONE_MORE", 999999, 999998)).error() == SpecError::Full,
          "the next add is Full");
    check(st.size() == kMaxInstruments, "and the rejection changed nothing");
    check(st.id_of(FeedSource::Kite, 1).value() == InstrumentId{0},
          "the first instrument still resolves");
}

namespace {

void report_throughput()
{
    std::printf("\nthroughput — batch-timed, single thread\n");
    static SpecStore st;
    char sym[16];
    for (std::size_t i = 0; i < 2000; ++i) {
        std::snprintf(sym, sizeof(sym), "S%zu", i);
        (void)st.add(mk(sym, static_cast<std::uint32_t>(i + 1),
                        static_cast<std::uint32_t>(i + 100001)));
    }

    constexpr int kOps = 200'000;
    std::uint64_t sink = 0;

    // Worst case: the LAST token, so the linear scan runs the full 2000.
    const std::uint32_t last = 2000;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kOps; ++i) {
        const auto r = st.id_of(FeedSource::Kite, last);
        sink += r.has_value() ? static_cast<std::uint32_t>(r.value()) : 0u;
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double ns0 = std::chrono::duration<double, std::nano>(t1 - t0).count()
                     / static_cast<double>(kOps);
    std::printf("  id_of() worst case, 2000 specs %6.2f ns/call (budget < 20)  %s\n",
                ns0, ns0 < 20.0 ? "OK" : "OVER");
    if (ns0 >= 20.0) {
        std::printf("    linear scan — P2-04 should add an index if this bites\n");
    }

    const auto id = st.id_of(FeedSource::Kite, 1).value();
    const auto t2 = std::chrono::steady_clock::now();
    for (int i = 0; i < kOps; ++i) {
        const auto r = st.at(id, kOpen);
        sink += r.has_value() ? 1u : 0u;
    }
    const auto t3 = std::chrono::steady_clock::now();
    const double ns1 = std::chrono::duration<double, std::nano>(t3 - t2).count()
                     / static_cast<double>(kOps);
    std::printf("  at(id, when)                   %6.2f ns/call (budget < 20)  %s\n",
                ns1, ns1 < 20.0 ? "OK" : "OVER");

    if (sink == 0xFFFFFFFFFFFFFFFFull) { std::printf("  (unreachable)\n"); }
}

} // namespace

int main()
{
    std::printf("altair instruments spec store tests\n");
    test_spec_traits_and_scale();
    test_store_add_and_identity();
    test_store_token_map_is_bidirectional();
    test_store_rejects_bad_input();
    test_store_point_in_time();
    test_store_blocking();
    test_store_spec_version_order_independent();
    test_store_capacity();

    report_throughput();

    if (failures == 0) {
        std::printf("\nPASS\n");
    } else {
        std::printf("\nFAILED (%d)\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
