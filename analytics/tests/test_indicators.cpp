// P6-01 acceptance tests for analytics/indicators.hpp.
//
// Test 1 is the card: Wilder's smoothing uses alpha = 1/N and the exponential
// moving average everyone reaches for uses 2/(N+1). Substituting one for the
// other in RSI or ATR gives a confident, plausible indicator that is wrong by
// roughly a factor of two in responsiveness -- and therefore in signal count.
//
// Test 2 is the second half of the same defect: true range includes the gap,
// and the bars where the two definitions disagree are exactly the bars a stop
// is sized on.
//
// Test 4 is the axis question: the recurrence is serial in time, so SIMD goes
// across instruments. The requirement is agreement, not speed.
//
// No check description here may contain the substring FAIL.

#include <analytics/indicators.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>

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

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

struct Lcg {
    std::uint64_t s;
    double uniform()
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>((s >> 11) & ((1ULL << 53) - 1))
               / static_cast<double>(1ULL << 53);
    }
    double normal()
    {
        const double u1 = uniform();
        const double u2 = uniform();
        return std::sqrt(-2.0 * std::log(u1 + 1e-300))
               * std::cos(6.283185307179586 * u2);
    }
};

} // namespace

using namespace altair;

namespace {

/// A synthetic NIFTY-scale close series: a random walk with a mild drift.
void make_closes(double* out, std::size_t n, std::uint64_t seed)
{
    Lcg g{seed};
    double p = 2'400'000.0;
    for (std::size_t i = 0; i < n; ++i) {
        p += g.normal() * 1500.0 + 30.0;
        out[i] = p;
    }
}

// ── 1 ────────────────────────────────────────────────────────────────────
// THE card.
void wilder_smoothing_is_not_an_ema_of_the_same_period()
{
    std::printf("\n1 wilder_smoothing_is_not_an_ema_of_the_same_period\n");
    const int N = 14;
    Rma w{N};
    Ema e{N};
    Ema equiv{Rma::equivalent_ema_period(N)};

    std::printf("    period %d:  Wilder alpha %.4f    EMA alpha %.4f"
                "    ratio %.2fx\n",
                N, w.alpha(), e.alpha(), e.alpha() / w.alpha());
    check(near(w.alpha(), 1.0 / 14.0, 1e-15),
          "Wilder's alpha is 1/N");
    check(near(e.alpha(), 2.0 / 15.0, 1e-15),
          "and the standard EMA's is 2/(N+1)");
    check(e.alpha() > 1.8 * w.alpha(),
          "so the EMA is more than 1.8x as responsive at the SAME stated"
          " period -- an 87% difference hiding behind one number called '14'");
    check(near(equiv.alpha(), w.alpha(), 1e-15),
          "and Wilder(14) has exactly the alpha of EMA(27), which is the"
          " identity Wilder(N) == EMA(2N - 1)");

    // What that does to RSI, measured on a real-shaped series.
    constexpr std::size_t kN = 4000;
    static double closes[kN];
    make_closes(closes, kN, 0xA11CE);

    // The correct RSI, and the one built by substituting an EMA for the RMA.
    Rsi correct{N};
    struct WrongRsi {                       // RSI(14) with EMA(14) smoothing
        Ema gain{14}, loss{14};
        Price prev{0};
        bool seeded = false;
        double value = 50.0;
        void update(Price c) {
            if (!seeded) { prev = c; seeded = true; return; }
            const double d = static_cast<double>(c.raw() - prev.raw());
            prev = c;
            const double g = gain.update(d > 0.0 ? d : 0.0);
            const double l = loss.update(d < 0.0 ? -d : 0.0);
            value = l <= 0.0 ? 100.0 : 100.0 - 100.0 / (1.0 + g / l);
        }
    } wrong;

    int correct_signals = 0, wrong_signals = 0;
    double max_gap = 0.0, sum_abs_gap = 0.0;
    std::size_t compared = 0;
    bool c_above = false, w_above = false;
    for (std::size_t i = 0; i < kN; ++i) {
        const Price c{static_cast<std::int64_t>(closes[i])};
        (void)correct.update(c);
        wrong.update(c);
        if (!correct.ready() || i < 60) { continue; }
        const double cv = correct.value().value();
        const double wv = wrong.value;
        const double gap = std::fabs(cv - wv);
        if (gap > max_gap) { max_gap = gap; }
        sum_abs_gap += gap;
        ++compared;
        // Count 70/30 crossings, which is what an RSI strategy trades.
        const bool c_hi = cv > 70.0, c_lo = cv < 30.0;
        const bool w_hi = wv > 70.0, w_lo = wv < 30.0;
        if ((c_hi && !c_above) || (c_lo && c_above)) {
            ++correct_signals;
            c_above = c_hi;
        }
        if ((w_hi && !w_above) || (w_lo && w_above)) {
            ++wrong_signals;
            w_above = w_hi;
        }
    }

    std::printf("    on %zu bars of a NIFTY-scale walk:\n"
                "      mean |RSI difference|   %6.2f points\n"
                "      worst difference        %6.2f points\n"
                "      70/30 crossings, Wilder %6d\n"
                "      70/30 crossings, EMA    %6d   (%.2fx as many)\n",
                compared, sum_abs_gap / static_cast<double>(compared), max_gap,
                correct_signals, wrong_signals,
                static_cast<double>(wrong_signals)
                    / static_cast<double>(correct_signals));
    check(max_gap > 5.0,
          "the two 'RSI(14)' series differ by more than five whole RSI points"
          " at their worst, on ordinary data with no pathology in it");
    check(wrong_signals > correct_signals,
          "and the EMA version crosses 70 and 30 MORE often -- so a strategy"
          " built on it takes more trades than the one that was backtested,"
          " and every extra trade is one the design never contemplated");
    std::printf("    -> this is the exact shape review gate 7 exists for: no"
                " warning, no NaN, no\n       crash. Two functions with the same"
                " name and a factor of two between them.\n       `ema` and `rma`"
                " are separate types here, and Rsi and Atr call `rma`.\n");
}

// ── 2 ────────────────────────────────────────────────────────────────────
void true_range_includes_the_gap()
{
    std::printf("\n2 true_range_includes_the_gap\n");
    // Yesterday closed at 2,950.00. Today gaps to 3,068 and trades a quiet
    // Rs 15 range: a 4% move followed by half a percent of activity.
    Bar b{};
    b.open = Price{306'800};
    b.high = Price{306'800};
    b.low  = Price{305'300};
    b.close = Price{306'000};
    check(b.valid(), "the gap bar is internally consistent");

    const double tr = true_range(b, Price{295'000});
    const double hl = static_cast<double>(b.high.raw() - b.low.raw());
    std::printf("    prev close 2950.00, bar 3068.00 H / 3053.00 L:\n"
                "      H - L        Rs %7.2f\n"
                "      TRUE range   Rs %7.2f   (%.1fx)\n",
                hl / 100.0, tr / 100.0, tr / hl);
    check(near(tr, 306'800.0 - 295'000.0, 1e-9),
          "true range is high minus the PREVIOUS CLOSE, Rs 118.00, because"
          " that is the distance the market actually travelled");
    check(tr > 7.0 * hl,
          "which is more than seven times the H-L range on this bar -- and"
          " H-L is what an ATR built from the wrong definition would report");
    std::printf("    -> a stop sized on Rs 15.00 the morning after a Rs 118.00"
                " gap is a stop placed\n       inside the noise of a market that"
                " just moved. The two definitions agree on\n       every quiet"
                " bar and diverge precisely on the ones that matter.\n");

    // On an ordinary inside bar the two agree exactly.
    Bar quiet{};
    quiet.open = Price{295'200}; quiet.high = Price{295'600};
    quiet.low = Price{294'800};  quiet.close = Price{295'100};
    check(near(true_range(quiet, Price{295'000}),
               static_cast<double>(quiet.high.raw() - quiet.low.raw()), 1e-9),
          "and on a bar that contains the previous close the two definitions"
          " give the same number, which is why the bug survives testing on"
          " calm data");

    // The first bar of a series has no previous close and says so.
    check(near(true_range(quiet, Price{0}),
               static_cast<double>(quiet.high.raw() - quiet.low.raw()), 1e-9),
          "the first bar of a series falls back to H-L, which is the only"
          " honest answer when there is no previous close");
}

// ── 3 ────────────────────────────────────────────────────────────────────
void warmup_and_the_supertrend_ratchet()
{
    std::printf("\n3 warmup_and_the_supertrend_ratchet\n");
    Ema e{20};
    for (int i = 0; i < 19; ++i) { (void)e.update(100.0); }
    check(!e.ready() && !e.value().has_value(),
          "an EMA that has seen 19 of its 20 bars is not ready, and reading it"
          " gives WarmingUp rather than a number dominated by its seed");
    (void)e.update(100.0);
    check(e.ready() && e.value().has_value(), "at 20 bars it is");

    Rma r{14};
    for (int i = 0; i < 14; ++i) { (void)r.update(static_cast<double>(i + 1)); }
    check(r.ready() && near(r.value().value(), 7.5, 1e-12),
          "and Wilder's seeds with the SIMPLE average of its first N inputs --"
          " 7.5 for 1..14 -- which is how Wilder defined it and is not the"
          " same as seeding with the first value");

    // Supertrend: the ratchet means a widening raw band does not loosen the
    // stop while the trend holds.
    Supertrend st{10, 3.0};
    Lcg g{0xBEEF};
    double p = 2'400'000.0;
    double prev_line = 0.0;
    int loosenings = 0, bars = 0;
    TrendDirection dir = TrendDirection::Unknown;
    for (int i = 0; i < 600; ++i) {
        p += g.normal() * 1200.0 + 200.0;         // a persistent uptrend
        Bar b{};
        b.open = Price{static_cast<std::int64_t>(p)};
        b.high = Price{static_cast<std::int64_t>(p + 900)};
        b.low  = Price{static_cast<std::int64_t>(p - 900)};
        b.close = Price{static_cast<std::int64_t>(p + 100)};
        const auto v = st.update(b);
        if (!v) { continue; }
        if (v->direction == TrendDirection::Up
            && dir == TrendDirection::Up && prev_line > 0.0) {
            ++bars;
            if (v->line < prev_line - 1e-9) { ++loosenings; }
        }
        dir = v->direction;
        prev_line = v->line;
    }
    std::printf("    supertrend over %d bars of an uptrend: the trailing line"
                " moved AWAY from price\n    on %d of them (%.1f%%)\n",
                bars, loosenings,
                100.0 * static_cast<double>(loosenings)
                    / static_cast<double>(bars));
    check(bars > 100, "the uptrend held for a usable number of bars");
    check(loosenings == 0,
          "and the trailing line NEVER loosened while the trend held -- that"
          " is the ratchet, and without it every widening of the raw band"
          " becomes a chance to whipsaw");
}

// ── 4 ────────────────────────────────────────────────────────────────────
void simd_goes_across_instruments_not_across_bars()
{
    std::printf("\n4 simd_goes_across_instruments_not_across_bars\n");
    constexpr std::size_t L = 8;
    constexpr std::size_t kBars = 50'000;
    const int period = 21;

    static double series[L][kBars];
    for (std::size_t i = 0; i < L; ++i) {
        make_closes(series[i], kBars, 0x5150 + i);
    }

    // Scalar: one Ema per instrument, one full pass each.
    double scalar_out[L];
    for (std::size_t i = 0; i < L; ++i) {
        Ema e{period};
        for (std::size_t t = 0; t < kBars; ++t) { (void)e.update(series[i][t]); }
        scalar_out[i] = e.raw();
    }

    // Lane-parallel: all L instruments advancing one bar at a time.
    EmaLanes<L> lanes{period};
    double row[L];
    for (std::size_t t = 0; t < kBars; ++t) {
        for (std::size_t i = 0; i < L; ++i) { row[i] = series[i][t]; }
        lanes.update(row);
    }

    bool identical = true;
    for (std::size_t i = 0; i < L; ++i) {
        if (lanes.values()[i] != scalar_out[i]) { identical = false; }
    }
    std::printf("    %zu instruments x %zu bars, EMA(%d):\n"
                "      lane 0  scalar %.6f   lanes %.6f\n",
                L, kBars, period, scalar_out[0], lanes.values()[0]);
    check(identical,
          "the lane-parallel form is BIT-IDENTICAL to running the scalar EMA"
          " once per instrument -- not close, identical: the operations are"
          " the same operations in the same order, only interleaved");
    check(lanes.ready(), "and it reports itself ready after its period");

    // The point about the other axis, stated as a fact about the recurrence.
    Ema serial{period};
    (void)serial.update(100.0);
    const double after_one = serial.raw();
    (void)serial.update(200.0);
    const double after_two = serial.raw();
    check(after_two != after_one && near(after_two,
              after_one + serial.alpha() * (200.0 - after_one), 1e-12),
          "bar 2's value is a function of bar 1's value, exactly -- so the"
          " time axis is a serial dependency chain and no rearrangement"
          " computes bar 2 first");
    std::printf("    -> which is why the vector axis is INSTRUMENTS, not bars."
                " L independent\n       recurrences advancing in lockstep is L"
                " independent multiply-adds, which is\n       what a vector unit"
                " does. An implementation that appeared to vectorise\n       along"
                " time would be computing a different quantity, and the"
                " bit-identity\n       check above is what keeps this one"
                " honest.\n");
}

} // namespace

int main()
{
    std::printf("altair indicator kernel tests\n");
    wilder_smoothing_is_not_an_ema_of_the_same_period();
    true_range_includes_the_gap();
    warmup_and_the_supertrend_ratchet();
    simd_goes_across_instruments_not_across_bars();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
