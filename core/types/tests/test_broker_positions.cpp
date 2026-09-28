// P0-02c acceptance tests for core/types/broker_positions.hpp.
// Plain main(), following core/types/tests/test_broker_state.cpp.

#include <types/broker_positions.hpp>

#include <cstdint>
#include <cstdio>

namespace {

using namespace altair;
using namespace altair::broker_view;

int failures{};
constexpr Timestamp now{100};
constexpr EvidenceWindow window{Timestamp{90}, Timestamp{110}};

// An opaque InstrumentId value. NOT InstrumentKey::None, and deliberately not
// any particular spec-store index — core/types must not know what 7 means.
constexpr std::uint32_t kRealInstrument = 7;

void check(bool ok, const char* message) {
    if (!ok) { ++failures; std::printf("FAIL: %s\n", message); }
}

SessionKey bound_session() { return {BrokerId::Fyers, 1, 2}; }

// A priced, identifiable, bound row: the shape a healthy provider produces.
Position row(std::uint32_t instrument, Qty qty, Price average) {
    Position p;
    p.instrument = static_cast<InstrumentKey>(instrument);
    p.product = PositionProduct::Intraday;
    p.net_qty = qty;
    p.average_price = average;
    p.last_mark = Price{10'250};
    p.marked = window;
    return p;
}

// A snapshot that would be usable() if every row inside count were valid.
PositionSnapshot snapshot(std::uint16_t count) {
    PositionSnapshot s;
    s.account_session = bound_session();
    s.observed = window;
    s.count = count;
    return s;
}

// ─────────────────────────────────────────────────────────────────────────
// Compile-time assertions: the payload is constexpr and noexcept, so every
// one of these is settled by the compiler rather than by a test run.
// ─────────────────────────────────────────────────────────────────────────
static_assert(noexcept(checked_count(0)));
static_assert(noexcept(valid(Position{})));
static_assert(noexcept(usable(PositionSnapshot{}, now)));
static_assert(noexcept(unrealised(Position{})));
static_assert(noexcept(unrealised_total(PositionSnapshot{})));
static_assert(noexcept(is_flat(Position{})));

static_assert(checked_count(kMaxPositionsPerAccount).value() == 256);
static_assert(!checked_count(kMaxPositionsPerAccount + 1).has_value());
static_assert(checked_count(kMaxPositionsPerAccount + 1).error()
              == PositionError::TooManyPositions);

// Money stays exact paise: these are the acceptance values, evaluated at
// compile time. A double anywhere in the path would not be constexpr-equal.
static_assert(unrealised(Position{InstrumentKey{7}, PositionProduct::Intraday,
                                   Qty{50}, Price{10'000}, Price{10'250},
                                   window}).value() == Notional{12'500});
static_assert(unrealised(Position{InstrumentKey{7}, PositionProduct::Intraday,
                                   Qty{-50}, Price{10'000}, Price{9'900},
                                   window}).value() == Notional{5'000});
static_assert(unrealised(Position{}).error() == PositionError::NoMark);
static_assert(is_flat(Position{InstrumentKey{7}, PositionProduct::Unknown,
                               Qty{0}, Price{}, std::nullopt, window}));
static_assert(!is_flat(Position{InstrumentKey{7}, PositionProduct::Unknown,
                                Qty{1}, Price{}, std::nullopt, window}));

// One row whose own product leaves int64 paise: (10^9 paise) * (~2^63 units).
static_assert(unrealised(Position{InstrumentKey{7}, PositionProduct::Intraday,
                                  Qty{9'223'372'036'854'775'807}, Price{0},
                                  Price{1'000'000'000}, window}).error()
              == PositionError::Overflow);

// P0-02c1: the SUBTRACTION feeding that product leaves int64 too, on a row
// valid() accepts — it constrains average_price but places no constraint on
// last_mark. `-2 - 9'223'372'036'854'775'807` is INT64_MIN - 1, which has no
// int64 answer. This static_assert is the one that could not previously be
// written: the unchecked `operator-` made it a constant-evaluation failure
// rather than a value, so the defect was unstateable in a test.
static_assert(unrealised(Position{InstrumentKey{7}, PositionProduct::Intraday,
                                  Qty{1}, Price{9'223'372'036'854'775'807},
                                  Price{-2}, window}).error()
              == PositionError::Overflow);

// The RUNNING SUM is guarded too: each row below fits on its own, and the
// total does not. A total that wrapped would print a confidently wrong number.
static_assert([] {
    PositionSnapshot s;
    s.account_session = SessionKey{BrokerId::Fyers, 1, 2};
    s.observed = window;
    s.count = 2;
    for (std::uint16_t i = 0; i < s.count; ++i) {
        s.position[i].instrument = InstrumentKey{7};
        s.position[i].product = PositionProduct::Intraday;
        s.position[i].net_qty = Qty{9'223'372'036};
        s.position[i].average_price = Price{0};
        s.position[i].last_mark = Price{1'000'000'000};
        s.position[i].marked = window;
    }
    const auto total = unrealised_total(s);
    return !total.has_value() && total.error() == PositionError::Overflow;
}());

void test_checked_count_refuses_over_cap() {
    const auto at_cap = checked_count(kMaxPositionsPerAccount);
    check(at_cap.has_value(), "the cap itself is an acceptable count");
    check(at_cap.has_value() && at_cap.value() == 256, "the cap narrows to 256");
    const auto over_cap = checked_count(kMaxPositionsPerAccount + 1);
    check(!over_cap.has_value(), "one past the cap is refused");
    check(!over_cap.has_value() && over_cap.error() == PositionError::TooManyPositions,
          "the refusal is TooManyPositions");
    // Clamping would have produced a successful 256. Refused, not clamped.
    check(!(over_cap.has_value() && over_cap.value() == 256),
          "one past the cap was refused rather than clamped to 256");
    check(!checked_count(9999).has_value(), "a hostile count is refused too");
}

void test_unrealised_long_gain_and_loss() {
    auto p = row(kRealInstrument, Qty{50}, Price{10'000});
    p.last_mark = Price{10'250};
    const auto gain = unrealised(p);
    check(gain.has_value() && *gain == Notional{12'500},
          "50 units up 250 paise is a 12'500 paise gain");

    p.last_mark = Price{9'900};
    const auto loss = unrealised(p);
    check(loss.has_value() && *loss == Notional{-5'000},
          "50 units down 100 paise is a 5'000 paise loss");
}

void test_unrealised_short_sign() {
    auto p = row(kRealInstrument, Qty{-50}, Price{10'000});
    p.last_mark = Price{9'900};
    const auto gain = unrealised(p);
    check(gain.has_value() && *gain == Notional{5'000},
          "a short that fell 100 paise is a POSITIVE 5'000 paise");

    p.last_mark = Price{10'250};
    const auto loss = unrealised(p);
    check(loss.has_value() && *loss == Notional{-12'500},
          "a short that rose 250 paise is a NEGATIVE 12'500 paise");
}

void test_unrealised_absent_mark_is_not_zero() {
    auto p = row(kRealInstrument, Qty{50}, Price{10'000});
    p.last_mark.reset();
    const auto r = unrealised(p);
    check(!r.has_value(), "an absent mark cannot be priced");
    check(!r.has_value() && r.error() == PositionError::NoMark,
          "an absent mark is NoMark");
    check(!(r.has_value() && *r == Notional{0}), "an absent mark is not zero");
    // The other tempting substitute is zero-as-a-mark: (0 - 10'000) * 50.
    check(!(r.has_value() && *r == Notional{-500'000}),
          "an absent mark is not reported as a zero mark");

    // valid() constrains average_price but not last_mark, so this row is
    // structurally valid and its P&L subtraction still leaves int64.
    // Refused, not UB. (P0-02c1)
    auto extreme = row(kRealInstrument, Qty{1}, Price::max());
    extreme.last_mark = Price{-2};
    check(valid(extreme), "the extreme row is structurally valid");
    const auto over = unrealised(extreme);
    check(!over.has_value() && over.error() == PositionError::Overflow,
          "a P&L subtraction that leaves int64 paise is refused");
}

void test_unrealised_total_refuses_unmarked_row() {
    auto s = snapshot(3);
    s.position[0] = row(kRealInstrument, Qty{50}, Price{10'000});
    s.position[1] = row(kRealInstrument + 1, Qty{50}, Price{10'000});
    s.position[1].last_mark.reset();                    // the unmarked middle row
    s.position[2] = row(kRealInstrument + 2, Qty{50}, Price{10'000});

    const auto total = unrealised_total(s);
    check(!total.has_value(), "a snapshot holding an unmarked row cannot be totalled");
    check(!total.has_value() && total.error() == PositionError::NoMark,
          "the total reports NoMark");
    // Rows 0 and 2 are worth 12'500 each. Skipping row 1 would print 25'000.
    check(!(total.has_value() && *total == Notional{25'000}),
          "the total is not the sum of only the marked rows");

    // The RUNNING SUM is guarded, not just each row's product: both rows below
    // price fine on their own (~9.22e18 paise each) and their total does not.
    // A wrapped total would print a confidently wrong number.
    auto huge = snapshot(2);
    for (std::uint16_t i = 0; i < huge.count; ++i) {
        huge.position[i] = row(kRealInstrument, Qty{9'223'372'036}, Price{0});
        huge.position[i].last_mark = Price{1'000'000'000};
    }
    check(unrealised(huge.position[0]).has_value(), "each huge row prices on its own");
    const auto sum = unrealised_total(huge);
    check(!sum.has_value(), "a total that leaves int64 paise is refused");
    check(!sum.has_value() && sum.error() == PositionError::Overflow,
          "the running total reports Overflow");

    // A count the array cannot hold is refused before any row is read. Row 0
    // here is unmarked on purpose: without the cap guard the loop would price
    // that row first and answer NoMark, and a count past the array would have
    // been read off the end. The structural refusal has to come first.
    auto oversize = snapshot(3);
    oversize.position[0] = row(kRealInstrument, Qty{50}, Price{10'000});
    oversize.position[0].last_mark.reset();
    oversize.position[1] = row(kRealInstrument + 1, Qty{50}, Price{10'000});
    oversize.position[2] = row(kRealInstrument + 2, Qty{50}, Price{10'000});
    oversize.count = static_cast<std::uint16_t>(kMaxPositionsPerAccount + 44);
    const auto over = unrealised_total(oversize);
    check(!over.has_value() && over.error() == PositionError::TooManyPositions,
          "a count past the array is refused, not read past the end");
}

void test_usable_rejects_stale_and_unbound() {
    auto stale = snapshot(1);
    stale.position[0] = row(kRealInstrument, Qty{50}, Price{10'000});
    stale.observed.expires_at = now;                    // expiry is exclusive
    check(!usable(stale, now), "an expired observation window is not usable");

    auto never_observed = snapshot(1);
    never_observed.position[0] = row(kRealInstrument, Qty{50}, Price{10'000});
    never_observed.observed.observed_at = Timestamp::epoch();
    check(!usable(never_observed, now), "an unobserved snapshot is not usable");

    auto unbound = snapshot(1);
    unbound.position[0] = row(kRealInstrument, Qty{50}, Price{10'000});
    unbound.account_session.generation = 0;             // unbound account session
    check(!usable(unbound, now), "an unbound account session is not usable");

    auto wrong_schema = snapshot(1);
    wrong_schema.position[0] = row(kRealInstrument, Qty{50}, Price{10'000});
    wrong_schema.schema_version = kPositionSchemaVersion + 1;
    check(!usable(wrong_schema, now), "a future schema is not usable");

    auto good = snapshot(1);
    good.position[0] = row(kRealInstrument, Qty{50}, Price{10'000});
    check(usable(good, now), "matching schema, bound session and fresh window is usable");
}

void test_usable_rejects_invalid_row_within_count() {
    // The other way a row fails valid(): a negative average entry price in
    // paise. A provider that reports one has lost the sign, so the row cannot
    // be priced or reconciled even though it names a real instrument.
    const auto negative_entry = row(kRealInstrument, Qty{50}, Price{-1});
    check(!valid(negative_entry), "a negative average price is not a valid row");
    auto negative_snapshot = snapshot(1);
    negative_snapshot.position[0] = negative_entry;
    check(!usable(negative_snapshot, now),
          "a negative average price inside count refuses the snapshot");

    auto s = snapshot(3);
    s.position[0] = row(kRealInstrument, Qty{50}, Price{10'000});
    s.position[1] = row(kRealInstrument + 1, Qty{50}, Price{10'000});
    s.position[1].instrument = InstrumentKey::None;     // row 1 cannot be identified
    s.position[2] = row(kRealInstrument + 2, Qty{50}, Price{10'000});
    check(valid(s.position[0]) && valid(s.position[2]), "the outer rows are fine");
    check(!usable(s, now), "one invalid row inside count refuses the snapshot");

    // Rows past count are not read, so a defaulted tail cannot refuse a snapshot
    // whose reported rows are all sound.
    auto trailing = snapshot(3);
    trailing.position[0] = row(kRealInstrument, Qty{50}, Price{10'000});
    trailing.position[1] = row(kRealInstrument + 1, Qty{50}, Price{10'000});
    trailing.position[2] = row(kRealInstrument + 2, Qty{50}, Price{10'000});
    check(!valid(trailing.position[3]), "the row past count is left defaulted");
    check(usable(trailing, now), "rows at or past count are not examined");
}

void test_flat_row_is_valid_but_flat() {
    const auto flat = row(kRealInstrument, Qty{0}, Price{10'000});
    check(valid(flat), "a flat row naming a real instrument is structurally valid");
    check(is_flat(flat), "net_qty zero is flat");
    check(!is_flat(row(kRealInstrument, Qty{1}, Price{10'000})), "a long row is not flat");
    check(!is_flat(row(kRealInstrument, Qty{-1}, Price{10'000})), "a short row is not flat");

    auto s = snapshot(1);
    s.position[0] = flat;
    check(usable(s, now), "a snapshot holding a flat row is usable");

    const auto total = unrealised_total(s);
    check(total.has_value() && *total == Notional{0},
          "a flat row prices to exactly zero without a special case");
}

} // namespace

int main() {
    test_checked_count_refuses_over_cap();
    test_unrealised_long_gain_and_loss();
    test_unrealised_short_sign();
    test_unrealised_absent_mark_is_not_zero();
    test_unrealised_total_refuses_unmarked_row();
    test_usable_rejects_stale_and_unbound();
    test_usable_rejects_invalid_row_within_count();
    test_flat_row_is_valid_but_flat();
    std::printf("Broker positions: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}

