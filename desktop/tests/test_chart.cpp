// P11Q-04 acceptance tests.
//
// Test 1 is the card: a nanosecond timestamp in a float32 collapses the x
// axis, and rebasing in int64 first restores it. Measured in distinct
// positions, not asserted.
//
// Test 2: price survives float32 at NSE magnitudes and a NOTIONAL axis does
// not -- the counter-intuitive half, re-measured in C++.
//
// Test 3: a bucket boundary belongs to exactly one candle, and volume
// reconciles against the tape.
//
// Test 4: the forming candle is marked, a dead minute produces no candle, and
// the crosshair reports a candle's own values.
//
// No check description here may contain the substring FAIL.

#include "../chart/candles.hpp"
#include "../chart/chart_widget.hpp"
#include "../format.hpp"

#include <QApplication>

#include <cstdio>
#include <set>
#include <vector>

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

using namespace altair;
using namespace altair::ui;

constexpr std::int64_t kSessionOpen = 1'788'428'100'000'000'000LL;
constexpr std::int64_t kMinute = 60'000'000'000LL;
constexpr std::uint32_t kToken = 256265;

ReplayTick tick(std::int64_t ts, std::uint64_t seq, std::int64_t paise,
                std::int64_t qty, std::uint32_t token = kToken)
{
    ReplayTick t{};
    t.ts = Timestamp{ts};
    t.seqno = seq;
    t.token = token;
    t.last = Price{paise};
    t.qty = Qty{qty};
    return t;
}

} // namespace

// ---------------------------------------------------------------------------

static void test_float32_timestamps()
{
    std::printf("\n[1] a nanosecond timestamp does not survive float32\n");

    const double res = float32_resolution(static_cast<double>(kSessionOpen));
    std::printf("    float32 resolution at 1.79e18 ns: %.0f ns = %.1f"
                " SECONDS\n", res, res / 1e9);
    check(res > 60e9,
          "at today's epoch one float32 step is over a minute wide, so every"
          " tick in that span lands on a single x coordinate");

    // Two hours of ticks 100 ms apart. Nothing exotic.
    constexpr std::size_t kN = 72'000;
    std::vector<std::int64_t> stamps;
    stamps.reserve(kN);
    for (std::size_t i = 0; i < kN; ++i) {
        stamps.push_back(kSessionOpen
                         + static_cast<std::int64_t>(i) * 100'000'000LL);
    }

    std::set<float> naive;
    for (std::int64_t ts : stamps) {
        naive.insert(naive_x(ts));
    }

    Domain d{};
    d.origin_ns = stamps.front();
    d.span_ns = stamps.back() - stamps.front() + 1;
    std::set<float> rebased;
    for (std::int64_t ts : stamps) {
        rebased.insert(to_x(ts, d));
    }

    std::printf("    %zu ticks over 2 hours: raw float32 keeps %zu distinct x"
                " positions; rebased keeps %zu\n",
                kN, naive.size(), rebased.size());

    check(naive.size() < kN / 100,
          "the raw conversion collapses the session into a handful of"
          " positions -- the chart does not error, it draws a staircase, and"
          " the staircase reads as low-frequency structure in the market");
    check(rebased.size() == kN,
          "subtracting the domain origin in INT64 first keeps every tick"
          " distinct; doing the subtraction after the conversion loses the"
          " precision before it can save it, which is the version that looks"
          " correct in a diff");

    const double in_window = float32_resolution(static_cast<double>(d.span_ns));
    std::printf("    within the window float32 resolves to %.3f ms\n",
                in_window / 1e6);
    check(in_window < 1e6,
          "and inside a two-hour window it resolves finer than a millisecond,"
          " which is finer than any pixel at any zoom this chart offers");
}

static void test_price_is_fine_notional_is_not()
{
    std::printf("\n[2] price survives float32; a notional axis does not\n");

    const double at_nifty = float32_resolution(2'400'000.0);
    std::printf("    float32 at 2,400,000 paise (NIFTY 24,000) resolves to"
                " %.2f paise, against a 5-paise tick\n", at_nifty);
    check(at_nifty < 5.0,
          "price in paise is SAFE at Indian equity magnitudes -- about twenty"
          " times the headroom needed, which is exactly why this one gets"
          " missed");

    // Rs 50 crore = 5e10 paise.
    const double at_book = float32_resolution(50'000'000'000.0);
    std::printf("    float32 at Rs 50 crore (5e10 paise) resolves to %.0f"
                " paise = Rs %.0f\n", at_book, at_book / 100.0);
    check(at_book > 1000.0,
          "while a notional axis resolves to hundreds of rupees, so an equity"
          " curve moving in Rs 10 steps -- ordinary intraday granularity on a"
          " book that size -- draws as a staircase");

    // The same rebasing fixes it, which is why the axis is per SERIES.
    PriceAxis a{};
    a.origin_paise = 50'000'000'000LL;
    a.span_paise = 500'000;              // Rs 5,000 of range
    std::set<float> levels;
    for (int i = 0; i < 500; ++i) {
        levels.insert(to_y(a.origin_paise + i * 1000LL, a));  // Rs 10 steps
    }
    std::printf("    a P&L curve with 500 distinct levels keeps %zu after"
                " rebasing\n", levels.size());
    check(levels.size() == 500,
          "rebased to the series' own range every level survives, which is why"
          " the axis is rebased PER SERIES rather than shared across the"
          " chart");
}

static void test_bucket_boundaries_and_conservation()
{
    std::printf("\n[3] a bucket boundary belongs to exactly one candle\n");

    std::vector<ReplayTick> ticks;
    std::uint64_t seq = 1;
    // Sixty minutes, 1000 ticks each, with the first twenty of every minute
    // sharing the boundary timestamp exactly -- exchange timestamps are
    // quantised and a batch of orders is timed to the boundary, so this is
    // the ordinary case rather than a contrived one.
    for (int m = 0; m < 60; ++m) {
        const std::int64_t base = kSessionOpen + m * kMinute;
        for (int j = 0; j < 1000; ++j) {
            const std::int64_t ts =
                j < 20 ? base : base + static_cast<std::int64_t>(j) * 60'000'000LL;
            ticks.push_back(tick(ts, seq++, 2'400'000 + ((m * 7 + j) % 200), 1));
        }
    }

    const auto built = build_candles(ticks.data(), ticks.size(), kToken,
                                     kMinute, kSessionOpen + 61 * kMinute);
    const auto k = check_conservation(ticks.data(), ticks.size(), kToken, built);

    std::printf("    %zu ticks -> %zu candles; tape volume %lld, chart volume"
                " %lld\n", ticks.size(), built.size(),
                static_cast<long long>(k.tape_volume),
                static_cast<long long>(k.chart_volume));

    check(built.size() == 60, "one candle per traded minute");
    check(k.agrees,
          "the candles total the tape exactly, in integers -- the engine's own"
          " per-tick invariant applied to the chart, and far cheaper to assert"
          " than to notice");

    // What the closed-interval version would have reported.
    std::int64_t double_counted = 0;
    for (const ReplayTick& t : ticks) {
        if (t.ts.ns_since_epoch() % kMinute == 0) {
            double_counted += t.qty.raw();
        }
    }
    const double overstated =
        100.0 * static_cast<double>(double_counted)
        / static_cast<double>(k.tape_volume);
    std::printf("    a closed interval would have counted %lld boundary ticks"
                " twice: +%.1f%%\n",
                static_cast<long long>(double_counted), overstated);
    check(double_counted > 0,
          "and the boundary ticks are real -- a closed [start, end] counts each"
          " of them in two candles, in the number a volume-confirmation rule"
          " keys on");
}

static void test_forming_gaps_and_crosshair()
{
    std::printf("\n[4] the forming candle, the gap, and the readout\n");

    std::vector<ReplayTick> ticks;
    std::uint64_t seq = 1;
    // Minute 0 and minute 2 trade. Minute 1 is dead -- a halt, lunch, an
    // illiquid strike.
    for (int m : {0, 2}) {
        for (int j = 0; j < 10; ++j) {
            ticks.push_back(tick(kSessionOpen + m * kMinute
                                     + static_cast<std::int64_t>(j) * 1'000'000'000LL,
                                 seq++, 2'400'000 + j, 1));
        }
    }
    // "Now" is inside minute 2, so minute 2 is still forming.
    const std::int64_t now = kSessionOpen + 2 * kMinute + 30'000'000'000LL;
    const auto built = build_candles(ticks.data(), ticks.size(), kToken,
                                     kMinute, now);

    std::printf("    two traded minutes with a dead minute between them"
                " produced %zu candles\n", built.size());
    check(built.size() == 2,
          "a minute with no trades produces NO candle -- a zero candle spikes"
          " to the axis and one carrying the previous close draws a flat line"
          " saying trading happened, and neither is true");

    check(built[0].complete, "the settled minute is complete");
    check(!built[1].complete,
          "and the minute the tape is still inside is NOT: its close is the"
          " latest trade, not a close, and an overlay computed over it repaints"
          " its own last point");

    check(built[0].open == 2'400'000 && built[0].close == 2'400'009
              && built[0].high == 2'400'009 && built[0].low == 2'400'000
              && built[0].volume == 10,
          "and the OHLCV of a settled candle is exactly what the ticks in its"
          " bucket said");

    // A pointer three-quarters of the way through the first minute.
    const auto h = snap_crosshair(built, kSessionOpen + 45'000'000'000LL);
    check(h.valid && h.index == 0,
          "the crosshair snaps to the candle under the pointer");
    check(!h.extrapolated, "and does not claim to be past the end");

    const auto beyond = snap_crosshair(built, kSessionOpen + 50 * kMinute);
    check(beyond.valid && beyond.extrapolated,
          "while a pointer past the last candle SAYS SO rather than silently"
          " snapping and looking like a hit");

    const auto empty = snap_crosshair({}, kSessionOpen);
    check(!empty.valid, "and an empty chart yields no crosshair at all");

    // A flat series still gets a usable axis.
    std::vector<Candle> flat{Candle{0, 1, 100, 100, 100, 100, 1, 1, true}};
    check(price_axis(flat).span_paise > 0,
          "a flat series gets a non-zero price span rather than dividing by"
          " zero and drawing a line at NaN");

    // A PRICE AXIS MUST NOT GO BELOW ZERO. The real 35-year NIFTY series
    // ranges 279 to 27,939, and six percent of that span is 1,659 paise -- so
    // a symmetric pad puts the origin at MINUS 1,286.63. That is a price that
    // cannot exist, and it was visible on screen the moment the real series
    // loaded.
    std::vector<Candle> wide{
        Candle{0, 1, 27902, 27902, 27902, 27902, 0, 1, true},
        Candle{1, 2, 2793885, 2793885, 2793885, 2793885, 0, 1, true}};
    const PriceAxis a = price_axis(wide);
    std::printf("    a 279..27,939 series pads to an origin of %s\n",
                format_paise(a.origin_paise).toUtf8().constData());
    check(a.origin_paise >= 0,
          "the padded origin is clamped at zero, because a price axis labelled"
          " with a negative rupee value is drawing something that cannot"
          " happen");
    check(a.span_paise > 2793885 - a.origin_paise,
          "while the pad ABOVE is untouched -- there is no upper bound on a"
          " price, so only the low end needs the clamp");
}


// ── 5. P32-05: zoom is a window over the data, and it rescales the price axis
//
// THE FAILURE THIS MUST NOT HAVE is a zoom that narrows the candle count and
// leaves the price axis spanning the whole series. The visible bars would then
// occupy a sliver of the height and the chart would look like a flat line that
// somebody had zoomed into -- which is exactly what a broken zoom looks like
// to a reader, and exactly what a real one must not.
//
// So this asserts BOTH: fewer candles drawn, AND a price axis derived from the
// ones that are.
void test_zoom_windows_the_data()
{
    std::printf("\n5 zoom_windows_the_data\n");

    // A series that RISES steadily, so a window near one end has a genuinely
    // different price range from the whole. A flat series would pass a broken
    // implementation.
    std::vector<Candle> c;
    for (int i = 0; i < 400; ++i) {
        const std::int64_t px = 2'000'000 + static_cast<std::int64_t>(i) * 1000;
        c.push_back(Candle{static_cast<std::int64_t>(i) * 60'000'000'000LL,
                           static_cast<std::int64_t>(i + 1) * 60'000'000'000LL,
                           px, px + 500, px - 500, px, 10, 1, true});
    }

    ChartWidget w;
    w.set_candles(c, Conservation{});
    check(w.visible_count() == c.size(),
          "a fresh series shows every candle -- the zoom starts fitted");
    const PriceAxis full = w.axis();

    w.zoom_in();
    const std::size_t after_one = w.visible_count();
    std::printf("    %zu candles -> %zu after one zoom\n",
                c.size(), after_one);
    check(after_one < c.size(), "zooming in shows FEWER candles");

    for (int i = 0; i < 12; ++i) { w.zoom_in(); }
    const PriceAxis tight = w.axis();
    std::printf("    price span %s -> %s\n",
                format_paise(full.span_paise).toUtf8().constData(),
                format_paise(tight.span_paise).toUtf8().constData());
    check(tight.span_paise < full.span_paise,
          "and the PRICE AXIS narrows with it -- a zoom that keeps the whole "
          "series' range draws the visible bars as a flat line in the middle "
          "of the height, which is what a broken zoom looks like");

    check(w.visible_count() >= 8,
          "there is a floor on the window: below a handful of candles one wick "
          "sets the entire price scale");

    // Pan must stay inside the series. An off-by-one here reads past the end
    // of the vector in a process that is holding positions.
    w.pan(-100.0);
    check(w.visible_first() == 0, "panning back past the start clamps at 0");
    w.pan(100.0);
    check(w.visible_first() + w.visible_count() <= c.size(),
          "and panning forward past the end clamps at the last candle");

    w.zoom_reset();
    check(w.visible_count() == c.size() && w.visible_first() == 0,
          "Fit returns to the whole series");

    // A NEW SERIES RESETS THE WINDOW. Keeping indices 300..320 across an
    // instrument change means a different span of a different thing, and it
    // would look like a chart that simply moved.
    for (int i = 0; i < 6; ++i) { w.zoom_in(); }
    std::vector<Candle> other(c.begin(), c.begin() + 50);
    w.set_candles(other, Conservation{});
    check(w.visible_count() == other.size() && w.visible_first() == 0,
          "and loading a different series resets the window rather than "
          "keeping an index range that means nothing in the new one");
}

int main(int argc, char** argv)
{
    // A QApplication, because test 5 constructs a widget. QCoreApplication is
    // not enough: ChartWidget is a QWidget and needs the GUI application.
    QApplication app(argc, argv);
    std::printf("P11Q-04 -- candles, and the float32 boundary\n");
    test_float32_timestamps();
    test_price_is_fine_notional_is_not();
    test_bucket_boundaries_and_conservation();
    test_forming_gaps_and_crosshair();
    test_zoom_windows_the_data();

    std::printf("\n%s\n", failures == 0 ? "all checks passed"
                                        : "checks did not pass");
    return failures == 0 ? 0 : 1;
}
