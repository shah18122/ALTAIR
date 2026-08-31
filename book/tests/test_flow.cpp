// P2-09b acceptance tests for book/flow.hpp.
//
// No check description here may contain the substring FAIL.

#include <book/flow.hpp>

#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <cmath>

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

bool near(double a, double b, double tol = 1e-9)
{
    return std::fabs(a - b) <= tol;
}

} // namespace

using namespace altair;

namespace {

BookState quoted(std::int64_t bid, std::int64_t ask)
{
    BookState b{};
    b.id = static_cast<InstrumentId>(1);
    b.bid[0] = DepthLevel{Price{bid}, Qty{100}, 1, 0};
    b.ask[0] = DepthLevel{Price{ask}, Qty{100}, 1, 0};
    b.bid_levels = 1;
    b.ask_levels = 1;
    b.crossed = bid >= ask;
    return b;
}

// ── 1 ────────────────────────────────────────────────────────────────────
// D2. Lee-Ready needs the PRIOR book.
void trade_classification_needs_the_prior_book()
{
    std::printf("\n1 trade_classification_needs_the_prior_book\n");

    const BookState prior = quoted(2'500'000, 2'500'100);   // mid 2'500'050

    check(classify_trade(Price{2'500'100}, prior) == TradeSide::Buy,
          "a print at the ask is a BUY -- someone lifted the offer");
    check(classify_trade(Price{2'500'000}, prior) == TradeSide::Sell,
          "a print at the bid is a SELL");
    check(classify_trade(Price{2'500'060}, prior) == TradeSide::Buy,
          "above the mid is a buy");
    check(classify_trade(Price{2'500'040}, prior) == TradeSide::Sell,
          "below the mid is a sell");

    check(classify_trade(Price{2'500'050}, prior) == TradeSide::Unknown,
          "exactly AT the mid is Unknown, not a coin flip -- a tick-rule "
          "fallback would manufacture a side from nothing, and at-mid prints "
          "are common enough that the invention would matter");

    const BookState crossed = quoted(2'500'100, 2'500'000);
    check(classify_trade(Price{2'500'050}, crossed) == TradeSide::Unknown,
          "and a crossed book classifies nothing -- it has no usable mid");

    BookState one_sided = quoted(2'500'000, 2'500'100);
    one_sided.ask_levels = 0;
    check(classify_trade(Price{2'500'000}, one_sided) == TradeSide::Unknown,
          "nor does a one-sided book");
}

// ── 2 ────────────────────────────────────────────────────────────────────
// D7. The rule that governs this whole file.
void estimators_refuse_to_speak_too_early()
{
    std::printf("\n2 estimators_refuse_to_speak_too_early\n");

    Vpin v{Vpin::Config{1000, 10, 10}};
    for (int i = 0; i < 9; ++i) {
        v.on_trade(TradeSide::Buy, Qty{1000});
    }
    check(v.completed_buckets() == 9, "nine buckets have completed");
    check(!v.value().has_value(),
          "but VPIN is EMPTY at nine of a ten-bucket minimum -- an estimate "
          "from too few observations is not a small number with a wide error "
          "bar, it is noise wearing the costume of a signal");

    v.on_trade(TradeSide::Buy, Qty{1000});
    check(v.value().has_value(), "the tenth bucket unlocks it");
    check(v.value()->buckets == 10,
          "and it reports the sample size alongside the value -- ROADMAP 3, "
          "measurements carry their error");

    KyleLambda k{KyleLambda::Config{30}};
    for (int i = 1; i <= 29; ++i) {
        k.on_observation(i, Price{i * 2});
    }
    check(!k.value().has_value(), "Kyle's lambda is empty at 29 of 30 samples");
    k.on_observation(30, Price{60});
    check(k.value().has_value() && k.value()->samples == 30,
          "and reports its sample count when it does speak");
}

// ── 3 ────────────────────────────────────────────────────────────────────
void vpin_is_bucketed_by_volume_not_time()
{
    std::printf("\n3 vpin_is_bucketed_by_volume_not_time\n");

    // Perfectly one-sided flow: every bucket is maximally imbalanced.
    Vpin all_buy{Vpin::Config{100, 10, 10}};
    for (int i = 0; i < 10; ++i) {
        all_buy.on_trade(TradeSide::Buy, Qty{100});
    }
    const auto a = all_buy.value();
    check(a.has_value() && near(a->value, 1.0),
          "flow entirely on one side gives VPIN 1.0 -- maximum imbalance");

    // Perfectly balanced flow.
    Vpin balanced{Vpin::Config{100, 10, 10}};
    for (int i = 0; i < 10; ++i) {
        balanced.on_trade(TradeSide::Buy, Qty{50});
        balanced.on_trade(TradeSide::Sell, Qty{50});
    }
    const auto b = balanced.value();
    check(b.has_value() && near(b->value, 0.0),
          "balanced flow gives 0.0");

    // 75/25 flow.
    Vpin skew{Vpin::Config{100, 10, 10}};
    for (int i = 0; i < 10; ++i) {
        skew.on_trade(TradeSide::Buy, Qty{75});
        skew.on_trade(TradeSide::Sell, Qty{25});
    }
    const auto s = skew.value();
    check(s.has_value() && near(s->value, 0.5),
          "75 against 25 gives 0.5 -- |75-25| / 100");
    check(s.has_value() && s->value >= 0.0 && s->value <= 1.0,
          "and every reading is within [0, 1]");

    // A trade LARGER than one bucket must be split, not dropped or lumped.
    Vpin big{Vpin::Config{100, 10, 10}};
    big.on_trade(TradeSide::Buy, Qty{1000});
    check(big.completed_buckets() == 10,
          "a 1000-unit print across 100-unit buckets fills TEN of them -- a "
          "block is exactly when VPIN should move, and lumping it into one "
          "bucket would spike that one and starve the next");
    check(big.value().has_value() && near(big.value()->value, 1.0),
          "with the imbalance correctly attributed throughout");
}

// ── 4 ────────────────────────────────────────────────────────────────────
void unknown_side_fills_the_bucket_but_takes_no_side()
{
    std::printf("\n4 unknown_side_fills_the_bucket_but_takes_no_side\n");

    Vpin v{Vpin::Config{100, 10, 10}};
    for (int i = 0; i < 10; ++i) {
        v.on_trade(TradeSide::Unknown, Qty{100});
    }
    check(v.completed_buckets() == 10,
          "unknown-side volume still FILLS buckets -- it is volume that "
          "happened, and pretending otherwise would stretch each bucket over a "
          "longer stretch of tape");
    const auto r = v.value();
    check(r.has_value() && near(r->value, 0.0),
          "but contributes to neither side, so the imbalance is zero rather "
          "than invented");

    Vpin mixed{Vpin::Config{100, 10, 10}};
    for (int i = 0; i < 10; ++i) {
        mixed.on_trade(TradeSide::Buy, Qty{50});
        mixed.on_trade(TradeSide::Unknown, Qty{50});
    }
    const auto m = mixed.value();
    check(m.has_value() && near(m->value, 0.5),
          "half buy and half unclassified gives 0.5, not 1.0 -- the unknown "
          "half is not silently credited to the buy side");
}

// ── 5 ────────────────────────────────────────────────────────────────────
void kyle_lambda_recovers_a_known_slope()
{
    std::printf("\n5 kyle_lambda_recovers_a_known_slope\n");

    // Construct exactly dP = 3 * v, so the true slope is 3 paise per unit.
    KyleLambda k{KyleLambda::Config{10}};
    for (std::int64_t v = 1; v <= 50; ++v) {
        k.on_observation(v, Price{v * 3});
    }
    const auto r = k.value();
    check(r.has_value(), "the slope is reported");
    check(r.has_value() && near(r->lambda, 3.0, 1e-9),
          "and equals exactly 3.0 paise per unit on noiseless data");
    check(r.has_value() && near(r->r2, 1.0, 1e-9),
          "with an R-squared of 1.0 -- a perfect fit, as constructed");

    // Signed flow: selling pressure moves price DOWN, and the slope stays
    // positive because both sign together.
    KyleLambda both{KyleLambda::Config{10}};
    for (std::int64_t v = 1; v <= 25; ++v) {
        both.on_observation(v, Price{v * 3});
        both.on_observation(-v, Price{-v * 3});
    }
    check(both.value().has_value() && near(both.value()->lambda, 3.0, 1e-9),
          "buy and sell pressure together still recover +3 -- lambda is impact "
          "PER UNIT of signed flow, and sells move price down");
}

// ── 6 ────────────────────────────────────────────────────────────────────
void kyle_reports_a_poor_fit_honestly()
{
    std::printf("\n6 kyle_reports_a_poor_fit_honestly\n");

    // Price drifting UP regardless of which way the flow went. This is the
    // case D4 exists for: a model fitting an intercept would absorb the drift
    // and report a flattering slope, while a through-the-origin fit correctly
    // says the flow explains nothing.
    KyleLambda k{KyleLambda::Config{10}};
    const std::int64_t drift[10] = {7, 3, 11, 9, 2, 14, 5, 1, 8, 6};
    for (int i = 0; i < 10; ++i) {
        // Flow alternates sign; the price change does NOT follow it.
        k.on_observation(((i % 2) == 0) ? 100 : -100, Price{drift[i]});
    }
    const auto r = k.value();
    check(r.has_value(), "a slope is still produced");
    check(r.has_value() && std::fabs(r->lambda) < 0.05,
          "and it is near zero -- flow direction did not move price here");
    check(r.has_value() && r->r2 < 0.1,
          "with an R-squared near zero, which is the estimator saying the fit "
          "explains nothing -- a bare slope would have hidden that");
    check(r.has_value() && r->r2 >= 0.0 && r->r2 <= 1.0,
          "and R-squared stays within [0, 1]");

    // The contrast, so the low value above is shown to be meaningful rather
    // than something this estimator always reports.
    KyleLambda good{KyleLambda::Config{10}};
    for (std::int64_t v = 1; v <= 10; ++v) {
        good.on_observation(v, Price{v * 4});
    }
    check(good.value().has_value() && good.value()->r2 > 0.99,
          "while genuinely explanatory data reports an R-squared near 1");
}

// ── 7 ────────────────────────────────────────────────────────────────────
void zero_flow_observations_are_ignored()
{
    std::printf("\n7 zero_flow_observations_are_ignored\n");

    KyleLambda k{KyleLambda::Config{5}};
    for (int i = 0; i < 20; ++i) {
        k.on_observation(0, Price{100});     // price moved, no flow
    }
    check(k.samples() == 0,
          "zero-flow observations are not counted -- they contribute nothing "
          "to a through-the-origin fit and would only make the estimate look "
          "better supported than it is");
    check(!k.value().has_value(), "so no value is reported");

    for (std::int64_t v = 1; v <= 5; ++v) {
        k.on_observation(v, Price{v * 2});
    }
    check(k.samples() == 5, "real observations do count");
    check(k.value().has_value() && near(k.value()->lambda, 2.0, 1e-9),
          "and the slope is unaffected by the zero-flow noise");
}

// ── 8 ────────────────────────────────────────────────────────────────────
void windows_roll_and_reset()
{
    std::printf("\n8 windows_roll_and_reset\n");

    // A window of 5 must forget the sixth-oldest bucket.
    Vpin v{Vpin::Config{100, 5, 5}};
    for (int i = 0; i < 5; ++i) {
        v.on_trade(TradeSide::Buy, Qty{100});      // fully imbalanced
    }
    check(near(v.value()->value, 1.0), "five one-sided buckets give 1.0");

    for (int i = 0; i < 5; ++i) {
        v.on_trade(TradeSide::Buy, Qty{50});       // balanced
        v.on_trade(TradeSide::Sell, Qty{50});
    }
    check(near(v.value()->value, 0.0),
          "five balanced buckets later it reads 0.0 -- the window ROLLED and "
          "the old imbalance is gone, not averaged in forever");
    check(v.value()->buckets == 5, "and it still reports a window of 5");

    v.reset();
    check(v.completed_buckets() == 0 && !v.value().has_value(),
          "reset() clears the history and the estimator goes quiet again");

    KyleLambda k{KyleLambda::Config{5}};
    for (std::int64_t i = 1; i <= 10; ++i) {
        k.on_observation(i, Price{i});
    }
    check(k.value().has_value(), "Kyle has a value");
    k.reset();
    check(k.samples() == 0 && !k.value().has_value(),
          "and reset() takes it back to silence");
}

} // namespace

int main()
{
    std::printf("altair book flow tests\n");
    trade_classification_needs_the_prior_book();
    estimators_refuse_to_speak_too_early();
    vpin_is_bucketed_by_volume_not_time();
    unknown_side_fills_the_bucket_but_takes_no_side();
    kyle_lambda_recovers_a_known_slope();
    kyle_reports_a_poor_fit_honestly();
    zero_flow_observations_are_ignored();
    windows_roll_and_reset();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
