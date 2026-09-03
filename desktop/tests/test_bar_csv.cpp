// P11Q-06 acceptance tests -- against the REAL NIFTY series in dataset/.
//
// Test 1 is the card: a bar is not a tick. Re-bucketing 1m CLOSES into 5m
// candles is measured against aggregating the bars properly, on a real
// session, and the range it loses is the number a stop is placed against.
//
// Test 2: decimal rupees to exact paise, and how much of this dataset is not
// exact to begin with.
//
// Test 3: a volume of zero on an index bar is not zero volume.
//
// Test 4: a daily bar has no time of day, and the choice decides whether the
// market clock says OPEN or CLOSED for thirty-five years of history.
//
// No check description here may contain the substring FAIL.

#include "../data/bar_csv.hpp"
#include "../format.hpp"
#include "../market_clock.hpp"

#include <QCoreApplication>
#include <QString>

#include <cstdio>

#ifndef ALTAIR_DATASET_DIR
#  define ALTAIR_DATASET_DIR "dataset"
#endif

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

constexpr std::int64_t kMinute = 60'000'000'000LL;
constexpr std::int64_t kDay = 24LL * 3600 * 1'000'000'000LL;

QString dataset(const char* rel)
{
    return QStringLiteral("%1/%2").arg(QStringLiteral(ALTAIR_DATASET_DIR),
                                       QString::fromUtf8(rel));
}

} // namespace

// ---------------------------------------------------------------------------

static void test_a_bar_is_not_a_tick()
{
    std::printf("\n[1] a bar is not a tick\n");

    // A real, full NIFTY session.
    const LoadResult r = load_bars_csv(dataset("spot/nifty/1m/2026-08-27.csv"),
                                       kMinute, DailyStamp::SessionClose,
                                       /*zero_volume_is_absent=*/true);
    check(r.ok(), "the real 1-minute session loads");
    if (!r.ok()) {
        std::printf("    %s\n", r.error.toUtf8().constData());
        return;
    }
    std::printf("    %zu one-minute bars, %zu skipped\n", r.bars.size(),
                r.skipped_rows);
    check(r.bars.size() > 300,
          "and it is a real session rather than a handful of rows");

    constexpr std::int64_t kFive = 5 * kMinute;
    const auto proper = aggregate_bars(r.bars, kFive);
    const auto naive = bars_as_ticks_naive(r.bars, kFive);

    const double proper_range = mean_range_paise(proper);
    const double naive_range = mean_range_paise(naive);
    const double lost = 100.0 * (1.0 - naive_range / proper_range);

    std::printf("    aggregated properly : %zu five-minute candles, mean range"
                " %.0f paise\n", proper.size(), proper_range);
    std::printf("    closes as ticks     : %zu candles, mean range %.0f paise"
                "  (%.1f%% of the range gone)\n",
                naive.size(), naive_range, lost);

    check(proper.size() == naive.size(),
          "both produce the same number of candles, so the difference is not"
          " in the bucketing -- which is what makes it hard to spot");
    check(naive_range < proper_range,
          "treating each bar's CLOSE as a tick discards the high and the low,"
          " so the reconstructed range is the spread of the closes rather than"
          " the true extent");
    check(lost > 10.0,
          "and the loss is large: every high, every low, every range-based stop"
          " and every ATR computed downstream is understated by it");

    // The extremes are what actually went missing.
    std::int64_t proper_hi = proper.front().high;
    std::int64_t naive_hi = naive.front().high;
    for (const Candle& c : proper) proper_hi = std::max(proper_hi, c.high);
    for (const Candle& c : naive) naive_hi = std::max(naive_hi, c.high);
    std::printf("    session high: %s properly, %s from closes\n",
                format_paise(proper_hi).toUtf8().constData(),
                format_paise(naive_hi).toUtf8().constData());
    check(proper_hi >= naive_hi,
          "the session high from the bars is at or above the high of the"
          " closes, and it is the one that actually traded");
}

static void test_decimal_rupees_to_paise()
{
    std::printf("\n[2] decimal rupees to exact paise\n");

    std::int64_t v = 0;
    bool rounded = false;

    check(parse_price_paise(QStringLiteral("24266.5"), v, rounded) && v == 2426650
              && !rounded,
          "a two-decimal price parses exactly, with no rounding");
    check(parse_price_paise(QStringLiteral("24262.85"), v, rounded) && v == 2426285,
          "and so does a full paisa value");
    check(parse_price_paise(QStringLiteral("279.01999"), v, rounded)
              && v == 27902 && rounded,
          "while 279.01999 -- a float round-trip of 279.02 from upstream --"
          " rounds to 279.02 and SAYS it was rounded");
    check(parse_price_paise(QStringLiteral("100"), v, rounded) && v == 10000,
          "a bare integer is rupees, not paise");
    check(!parse_price_paise(QStringLiteral("abc"), v, rounded),
          "and text is refused rather than parsed as zero, which would put a"
          " price of nothing into the series");

    // How much of the real dataset is not exact.
    const LoadResult daily = load_bars_csv(dataset("spot/nifty/1d/all.csv"),
                                           kDay, DailyStamp::SessionClose,
                                           /*zero_volume_is_absent=*/true);
    check(daily.ok(), "the 35-year daily series loads");
    if (!daily.ok()) {
        return;
    }
    const std::size_t fields = daily.bars.size() * 4;
    std::printf("    daily series: %zu bars, %zu of %zu price fields carried"
                " more than two decimals (%.1f%%)\n",
                daily.bars.size(), daily.rounded_fields, fields,
                100.0 * static_cast<double>(daily.rounded_fields)
                    / static_cast<double>(fields));
    check(daily.rounded_fields > 0,
          "a real fraction of this dataset is not exact to a paisa, and the"
          " count is REPORTED rather than absorbed by the parser -- resolving"
          " it silently is how a series acquires a bias nobody can see");
    check(daily.skipped_rows == 0,
          "and nothing was skipped, so the count is over the whole file");
}

static void test_zero_volume_is_not_zero()
{
    std::printf("\n[3] a volume of zero on an index bar is not zero volume\n");

    const LoadResult absent = load_bars_csv(dataset("spot/nifty/1d/all.csv"),
                                            kDay, DailyStamp::SessionClose,
                                            /*zero_volume_is_absent=*/true);
    const LoadResult literal = load_bars_csv(dataset("spot/nifty/1d/all.csv"),
                                             kDay, DailyStamp::SessionClose,
                                             /*zero_volume_is_absent=*/false);
    check(absent.ok() && literal.ok(), "both readings of the same file load");
    if (!absent.ok() || !literal.ok()) {
        return;
    }

    std::size_t unknown = 0;
    for (const Candle& c : absent.bars) {
        if (!c.volume_known) ++unknown;
    }
    std::printf("    %zu of %zu daily bars report volume 0 -- the index"
                " published no turnover in the early years\n",
                absent.zero_volume_rows, absent.bars.size());

    check(unknown == absent.zero_volume_rows,
          "read as ABSENT, every zero-volume row is marked unknown, so a chart"
          " can leave the bar blank rather than drawing a zero-height one that"
          " says nothing traded on a day the market was open");

    std::size_t literal_unknown = 0;
    for (const Candle& c : literal.bars) {
        if (!c.volume_known) ++literal_unknown;
    }
    check(literal_unknown == 0,
          "read as LITERAL, none is -- and the difference is a property of the"
          " SOURCE, which is why the argument is required and has no default");

    // An unknown component makes an aggregate unknown, rather than being
    // silently treated as zero.
    std::vector<Candle> mixed;
    Candle a{};
    a.start_ns = 0; a.end_ns = kDay; a.volume = 100; a.volume_known = true;
    Candle b{};
    b.start_ns = kDay; b.end_ns = 2 * kDay; b.volume = 0; b.volume_known = false;
    mixed.push_back(a);
    mixed.push_back(b);
    const auto week = aggregate_bars(mixed, 7 * kDay);
    check(week.size() == 1 && !week.front().volume_known,
          "and summing a known volume with an unknown one yields an UNKNOWN"
          " total, not a total that quietly counted the unknown as zero");
}

static void test_daily_stamp_decides_the_session()
{
    std::printf("\n[4] a daily bar has no time of day\n");

    const LoadResult close_stamped =
        load_bars_csv(dataset("spot/nifty/1d/all.csv"), kDay,
                      DailyStamp::SessionClose, true);
    check(close_stamped.ok(), "the daily series loads");
    if (!close_stamped.ok() || close_stamped.bars.empty()) {
        return;
    }

    MarketClock clock(DemoSessionTimes::trading(), DemoSessionTimes::pre_open());
    clock.observe(Timestamp{close_stamped.bars.back().start_ns});
    const SessionPhase at_close = clock.phase();

    // What stamping midnight would have done -- which is what happens if a
    // bare date is parsed without a decision.
    MarketClock midnight(DemoSessionTimes::trading(),
                         DemoSessionTimes::pre_open());
    const std::int64_t day_start =
        (close_stamped.bars.back().start_ns / kDay) * kDay;
    midnight.observe(Timestamp{day_start});

    std::printf("    stamped at the session close -> %s;  stamped at midnight"
                " -> %s\n",
                phase_label(at_close).toUtf8().constData(),
                phase_label(midnight.phase()).toUtf8().constData());

    check(at_close == SessionPhase::Open,
          "stamping the session close puts a daily bar inside the trading"
          " window, which is when its OHLC actually became final");
    check(midnight.phase() != SessionPhase::Open,
          "while stamping midnight -- what a bare date parses to if nobody"
          " decides -- puts thirty-five years of history outside the session"
          " and the market clock reads CLOSED for all of it");

    // The stamp is required, not defaulted.
    const LoadResult unspecified =
        load_bars_csv(dataset("spot/nifty/1d/all.csv"), kDay,
                      DailyStamp::Unspecified, true);
    check(unspecified.ok() && unspecified.bars.empty()
              && unspecified.skipped_rows > 0,
          "and DailyStamp::Unspecified parses no daily row at all rather than"
          " picking an hour, so the choice cannot be made by omission");
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    std::printf("P11Q-06 -- the real NIFTY series\n");
    test_a_bar_is_not_a_tick();
    test_decimal_rupees_to_paise();
    test_zero_volume_is_not_zero();
    test_daily_stamp_decides_the_session();

    std::printf("\n%s\n", failures == 0 ? "all checks passed"
                                        : "checks did not pass");
    return failures == 0 ? 0 : 1;
}
