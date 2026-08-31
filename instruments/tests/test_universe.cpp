// P1-09 acceptance tests for instruments/universe.hpp.
//
// No check description here may contain the substring FAIL.

#include <instruments/universe.hpp>
#include <instruments/reconcile.hpp>

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

constexpr Timestamp kToday = ist_date(2026, 9, 1);

// The real reference: NIFTY around 24'080 (from the bar data), BANKNIFTY
// around 54'000. In paise.
constexpr UniverseEntry kEntries[] = {
    {"NIFTY",     Price{2'408'000}},
    {"BANKNIFTY", Price{5'400'000}},
    {"RELIANCE",  Price{}},            // futures/cash only: no strike filter
};

UniverseConfig cfg()
{
    UniverseConfig c{};
    c.entries = kEntries;
    c.entry_count = 3;
    c.segments = static_cast<std::uint8_t>(seg_bit(Segment::Cash)
                                           | seg_bit(Segment::Fut)
                                           | seg_bit(Segment::Opt));
    c.exchanges = ex_bit(Exchange::NSE);
    c.max_expiry_days = 60;
    c.strike_band = 200'000;           // Rs 2000 either side
    return c;
}

ContractSpec mk(const char* underlying, Segment seg, Timestamp expiry,
                std::int64_t strike = 0, Exchange ex = Exchange::NSE)
{
    ContractSpec s{};
    s.id = InstrumentId::Invalid;
    s.lot_size = LotSize{65};
    s.tick_size = Price{5};
    s.strike = Price{strike};
    s.price_scale = 100;
    s.expiry = expiry;
    s.valid_from = kToday;
    s.valid_to = Timestamp::max();
    s.exchange = ex;
    s.segment = seg;
    s.opt_type = (seg == Segment::Opt) ? OptionType::CE : OptionType::None;
    s.source = SpecSource::KiteDump;
    s.source_hash = static_cast<std::uint64_t>(strike) + 1u;
    std::snprintf(s.symbol, sizeof(s.symbol), "%s-%lld", underlying,
                  static_cast<long long>(strike));
    std::snprintf(s.underlying, sizeof(s.underlying), "%s", underlying);
    return s;
}

Reconciler g_rec;

// ── 1 ────────────────────────────────────────────────────────────────────
void underlying_allow_list()
{
    std::printf("\n1 underlying_allow_list\n");
    UniverseFilter f{cfg()};
    const Timestamp exp = ist_date(2026, 9, 24);

    check(f.judge(mk("NIFTY", Segment::Fut, exp), kToday)
              == UniverseReject::Admitted, "NIFTY is in the universe");
    check(f.judge(mk("BANKNIFTY", Segment::Fut, exp), kToday)
              == UniverseReject::Admitted, "so is BANKNIFTY");
    check(f.judge(mk("ABCAPITAL", Segment::Fut, exp), kToday)
              == UniverseReject::Underlying,
          "ABCAPITAL is not -- and the real bhavcopy carries 216 October "
          "futures, of which a configured universe wants a handful");
    check(f.judge(mk("", Segment::Fut, exp), kToday)
              == UniverseReject::Underlying, "an empty underlying is refused");
}

// ── 2 ────────────────────────────────────────────────────────────────────
// The rule that makes an options universe fit at all.
void strike_band_is_what_makes_options_fit()
{
    std::printf("\n2 strike_band_is_what_makes_options_fit\n");
    UniverseFilter f{cfg()};
    const Timestamp exp = ist_date(2026, 9, 24);

    // Reference 24'080.00; band Rs 2000.
    check(f.judge(mk("NIFTY", Segment::Opt, exp, 2'408'000), kToday)
              == UniverseReject::Admitted, "the at-the-money strike is admitted");
    check(f.judge(mk("NIFTY", Segment::Opt, exp, 2'608'000), kToday)
              == UniverseReject::Admitted, "exactly Rs 2000 above is admitted");
    check(f.judge(mk("NIFTY", Segment::Opt, exp, 2'208'000), kToday)
              == UniverseReject::Admitted, "and Rs 2000 below");
    check(f.judge(mk("NIFTY", Segment::Opt, exp, 2'608'100), kToday)
              == UniverseReject::StrikeBand, "a paisa beyond the band is out");
    check(f.judge(mk("NIFTY", Segment::Opt, exp, 5'000'000), kToday)
              == UniverseReject::StrikeBand,
          "and a far-out-of-the-money 50'000 strike is out -- the real Kite "
          "dump carries 40'913 calls and 40'891 puts, and all but a few "
          "hundred are strikes nobody will trade today");

    // A future has no strike, so the band must not touch it.
    check(f.judge(mk("NIFTY", Segment::Fut, exp, 0), kToday)
              == UniverseReject::Admitted,
          "a FUTURE has no strike, and a zero strike must not be measured "
          "against the reference as though it were one");

    // An underlying with no reference has no strike filter.
    check(f.judge(mk("RELIANCE", Segment::Opt, exp, 9'999'999), kToday)
              == UniverseReject::Admitted,
          "an underlying with reference 0 has NO strike filter -- 'we did not "
          "supply a reference' must not silently mean 'reject everything'");
}

// ── 3 ────────────────────────────────────────────────────────────────────
void expiry_horizon_and_expired()
{
    std::printf("\n3 expiry_horizon_and_expired\n");
    UniverseFilter f{cfg()};

    check(f.judge(mk("NIFTY", Segment::Fut, ist_date(2026, 9, 24)), kToday)
              == UniverseReject::Admitted, "a near-month future is admitted");
    check(f.judge(mk("NIFTY", Segment::Fut, ist_date(2026, 10, 27)), kToday)
              == UniverseReject::Admitted, "so is October, inside 60 days");
    check(f.judge(mk("NIFTY", Segment::Fut, ist_date(2027, 6, 24)), kToday)
              == UniverseReject::TooFarOut,
          "but a June 2027 contract is beyond the horizon -- fo_mktlots really "
          "does list expiries out to JUN-31");

    check(f.judge(mk("NIFTY", Segment::Fut, ist_date(2026, 8, 27)), kToday)
              == UniverseReject::Expired,
          "and an ALREADY EXPIRED contract is refused: a bhavcopy is a "
          "snapshot of a trading day and carries the contracts that expired "
          "on it");
}

// ── 4 ────────────────────────────────────────────────────────────────────
// The exemption that would otherwise drop the entire cash universe.
void cash_is_exempt_from_the_expiry_rules()
{
    std::printf("\n4 cash_is_exempt_from_the_expiry_rules\n");
    UniverseFilter f{cfg()};

    ContractSpec cash = mk("RELIANCE", Segment::Cash, Timestamp::epoch());
    check(cash.expiry.is_epoch(), "cash carries Timestamp::epoch() as 'no expiry'");
    check(f.judge(cash, kToday) == UniverseReject::Admitted,
          "and is ADMITTED -- the epoch is 1970, so an expiry rule applied "
          "blindly would call every cash instrument expired and drop the whole "
          "cash universe in silence");

    // The distinction is is_epoch(), not "old".
    check(f.judge(mk("NIFTY", Segment::Fut, ist_date(1999, 1, 1)), kToday)
              == UniverseReject::Expired,
          "while a DERIVATIVE with a genuinely ancient expiry is still Expired");
}

// ── 5 ────────────────────────────────────────────────────────────────────
void segment_and_exchange_switches()
{
    std::printf("\n5 segment_and_exchange_switches\n");

    UniverseConfig c = cfg();
    c.segments = seg_bit(Segment::Fut);          // futures only
    UniverseFilter f{c};
    const Timestamp exp = ist_date(2026, 9, 24);

    check(f.judge(mk("NIFTY", Segment::Fut, exp), kToday)
              == UniverseReject::Admitted, "futures are on");
    check(f.judge(mk("NIFTY", Segment::Opt, exp, 2'408'000), kToday)
              == UniverseReject::Segment, "options are off");
    check(f.judge(mk("RELIANCE", Segment::Cash, Timestamp::epoch()), kToday)
              == UniverseReject::Segment, "cash is off");

    // Exchange: NSE only, so a BSE contract is refused even for a listed name.
    check(f.judge(mk("NIFTY", Segment::Fut, exp, 0, Exchange::BSE), kToday)
              == UniverseReject::ExchangeOff,
          "and a BSE contract is refused even though NIFTY is a listed "
          "underlying -- exchange is checked first, because a BSE NIFTY is a "
          "different contract at a different price");
}

// ── 6 ────────────────────────────────────────────────────────────────────
void rejections_are_counted_by_reason()
{
    std::printf("\n6 rejections_are_counted_by_reason\n");
    UniverseFilter f{cfg()};
    const Timestamp exp = ist_date(2026, 9, 24);

    (void)f.admit(mk("NIFTY", Segment::Fut, exp), kToday);
    (void)f.admit(mk("ABCAPITAL", Segment::Fut, exp), kToday);
    (void)f.admit(mk("NIFTY", Segment::Opt, exp, 9'000'000), kToday);
    (void)f.admit(mk("NIFTY", Segment::Fut, ist_date(2027, 6, 24)), kToday);
    (void)f.admit(mk("NIFTY", Segment::Fut, ist_date(2026, 8, 1)), kToday);

    const auto& s = f.stats();
    check(s.admitted == 1, "one admitted");
    check(s.underlying == 1, "one refused for its underlying");
    check(s.strike_band == 1, "one for the strike band");
    check(s.too_far_out == 1, "one for the horizon");
    check(s.expired == 1, "one already expired");
    check(s.rejected() == 4,
          "and the reasons are counted SEPARATELY -- 'the universe came out "
          "empty' is undiagnosable without knowing which rule emptied it");
}

// ── 7 ────────────────────────────────────────────────────────────────────
// The point of the card: the filter runs at the gate.
void the_reconciler_applies_it_at_add()
{
    std::printf("\n7 the_reconciler_applies_it_at_add\n");

    static UniverseFilter f{cfg()};
    f.reset_stats();
    g_rec.clear();
    g_rec.set_universe(&f, kToday);

    const Timestamp exp = ist_date(2026, 9, 24);
    const auto good = g_rec.add(mk("NIFTY", Segment::Fut, exp));
    check(good.has_value(), "an in-universe contract is added");

    const auto out = g_rec.add(mk("ABCAPITAL", Segment::Fut, exp));
    check(!out.has_value() && out.error() == ReconcileError::OutOfUniverse,
          "an out-of-universe one is refused with OutOfUniverse -- NOT an "
          "error to alarm on, it is the filter working");
    check(g_rec.size() == 1,
          "and it never reached storage: the filter runs BEFORE anything is "
          "kept, so the store cannot fill with contracts nobody will trade");

    // Turning the filter off admits everything again.
    g_rec.set_universe(nullptr, kToday);
    check(g_rec.add(mk("ABCAPITAL", Segment::Fut, exp)).has_value(),
          "with no filter installed, everything is admitted");
    check(g_rec.size() == 2, "and stored");
}

// ── 8 ────────────────────────────────────────────────────────────────────
// The number that motivated the whole card.
void a_realistic_universe_fits()
{
    std::printf("\n8 a_realistic_universe_fits\n");

    static UniverseFilter f{cfg()};
    f.reset_stats();
    g_rec.clear();
    g_rec.set_universe(&f, kToday);

    // Two indices, four expiries, every 50-rupee strike from 0 to 50'000,
    // both call and put. That is the shape of the real dump.
    const Timestamp exps[4] = {
        ist_date(2026, 9, 24), ist_date(2026, 10, 27),
        ist_date(2026, 11, 24), ist_date(2027, 6, 24),
    };
    std::size_t offered = 0;
    for (const char* u : {"NIFTY", "BANKNIFTY", "ABCAPITAL"}) {
        for (const Timestamp& e : exps) {
            for (std::int64_t k = 0; k <= 5'000'000; k += 5'000) {
                (void)g_rec.add(mk(u, Segment::Opt, e, k));
                ++offered;
            }
        }
    }

    const auto& s = f.stats();
    std::printf("       offered %zu, admitted %llu, rejected %llu\n",
                offered, static_cast<unsigned long long>(s.admitted),
                static_cast<unsigned long long>(s.rejected()));
    check(offered > 3000, "a realistically sized dump was offered");
    check(s.admitted > 0, "some contracts were admitted");
    check(s.admitted < kMaxInstruments,
          "and the admitted set FITS the 8'192-contract store -- which the raw "
          "files do not: 106'150 Kite rows and 30'488 exchange rows would have "
          "filled it from whichever loaded first, and WHICH 8'192 survived "
          "would have depended on CSV row order");
    check(g_rec.size() == s.admitted,
          "the reconciler holds exactly the admitted set");
    check(s.too_far_out > 0, "the June 2027 expiry was excluded by horizon");
    check(s.underlying > 0, "and ABCAPITAL by the allow-list");
}

} // namespace

int main()
{
    std::printf("altair instruments universe tests\n");
    underlying_allow_list();
    strike_band_is_what_makes_options_fit();
    expiry_horizon_and_expired();
    cash_is_exempt_from_the_expiry_rules();
    segment_and_exchange_switches();
    rejections_are_counted_by_reason();
    the_reconciler_applies_it_at_add();
    a_realistic_universe_fits();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
