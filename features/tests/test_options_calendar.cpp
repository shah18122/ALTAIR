// P5-04 acceptance tests for features/options_calendar.hpp.
//
// Test 1 is the card, and it runs in two halves because the standard advice
// ("time is cyclical, use sin/cos") is right for one of them and destructive
// for the other:
//
//   1a  the SESSION does not wrap, and encoding it on a circle collapses the
//       open and the close -- the two least alike moments of the day -- onto
//       identical coordinates.
//   1b  the EXPIRY CYCLE does wrap, and encoding it linearly puts two
//       consecutive sessions at the opposite ends of the range.
//
// Both are measured, not asserted from theory.
//
// No check description here may contain the substring FAIL.

#include <features/options_calendar.hpp>

#include <cmath>
#include <cstddef>
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

/// A tiny deterministic generator, so these numbers are the same on every box.
struct Lcg {
    std::uint64_t s;
    double next()
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>((s >> 11) & ((1ULL << 53) - 1))
               / static_cast<double>(1ULL << 53);
    }
    /// Box-Muller, one at a time. Adequate: this is a fixture, not a model.
    double normal()
    {
        const double u1 = next() > 1e-12 ? next() : 0.5;
        const double u2 = next();
        return std::sqrt(-2.0 * std::log(u1 + 1e-300))
               * std::cos(altair::kTau * u2);
    }
};

} // namespace

using namespace altair;

namespace {

// NSE: 09:15 to 15:30, in nanoseconds from an arbitrary session start.
constexpr std::int64_t kNs = 1'000'000'000LL;
constexpr std::int64_t kSessionNs = 22'500LL * kNs;      // 6h15m

Session nse_session()
{
    return Session{Timestamp{0}, Timestamp{kSessionNs}};
}

// ── 1a ───────────────────────────────────────────────────────────────────
void the_session_clock_does_not_wrap()
{
    std::printf("\n1a the_session_clock_does_not_wrap\n");

    // What a circular encoding of session progress would produce.
    const auto at_open  = cyclic_encode(CyclePosition{0.0, 1.0});
    const auto at_close = cyclic_encode(CyclePosition{1.0, 1.0});
    check(at_open.has_value() && at_close.has_value(),
          "both ends of the session encode");
    const double circle_gap = pair_distance(*at_open, *at_close);
    const double linear_gap = std::fabs(1.0 - 0.0);
    std::printf("    open vs close  ->  circular %.6f   linear %.6f\n",
                circle_gap, linear_gap);
    check(circle_gap < 1e-9,
          "a circular encoding puts the open and the close at a distance of"
          " ZERO -- identical coordinates for 09:15 and 15:30");
    std::printf("    -> those are the two LEAST alike moments of the day:"
                " overnight news being\n       priced in, against square-off"
                " and settlement pressure. The circle is\n       not a"
                " refinement here, it is a much larger error than the linear"
                " encoding.\n");
    check(linear_gap > circle_gap,
          "so the LINEAR encoding is the correct one for session progress, and"
          " that is what build_calendar writes");

    // And that is what it writes.
    CalendarSlots s{};
    s.session_progress = 0; s.since_open_s = 1; s.until_close_s = 2;
    CalendarPosition p{};
    p.session = nse_session();

    FeatureVector open_v{1, 8, Timestamp{0}};
    FeatureVector noon_v{1, 8, Timestamp{kSessionNs / 2}};
    FeatureVector shut_v{1, 8, Timestamp{kSessionNs}};
    (void)build_calendar(Timestamp{0}, p, s, open_v);
    (void)build_calendar(Timestamp{kSessionNs / 2}, p, s, noon_v);
    (void)build_calendar(Timestamp{kSessionNs}, p, s, shut_v);
    std::printf("    progress  open %.3f  mid %.3f  close %.3f\n",
                open_v.value(0).value(), noon_v.value(0).value(),
                shut_v.value(0).value());
    check(open_v.value(0).value() < noon_v.value(0).value()
          && noon_v.value(0).value() < shut_v.value(0).value(),
          "session progress is strictly monotone across the session");
    check(near(shut_v.value(2).value(), 0.0, 1e-9)
          && near(open_v.value(1).value(), 0.0, 1e-9),
          "and seconds-until-close reaches zero exactly at the close");

    // Out of session is refused rather than clamped. A pre-open tick with
    // progress clamped to 0.0 is indistinguishable from the open itself.
    FeatureVector early{1, 8, Timestamp{-kNs}};
    const auto r = build_calendar(Timestamp{-kNs}, p, s, early);
    check(!r && r.error() == CalendarError::OutOfSession,
          "a tick before the open is refused, not clamped -- a clamped 0.0"
          " would be indistinguishable from the opening print");
    check(early.present_count() == 0, "and nothing is written for it");
}

// ── 1b ───────────────────────────────────────────────────────────────────
void the_expiry_cycle_does_wrap()
{
    std::printf("\n1b the_expiry_cycle_does_wrap\n");

    // A weekly expiry cycle: five sessions, expiry on the fifth.
    constexpr double kCycle = 5.0;
    const auto expiry_day = cyclic_encode(CyclePosition{4.0, kCycle});
    const auto next_day   = cyclic_encode(CyclePosition{0.0, kCycle});
    check(expiry_day.has_value() && next_day.has_value(),
          "both sides of the rollover encode");

    // The truth: they are ONE session apart, which is 1/5 of the cycle.
    const double truth = 1.0 / kCycle;
    const double lin = std::fabs(4.0 / kCycle - 0.0 / kCycle);
    const double cyc = cyclic_distance(4.0 / kCycle, 0.0 / kCycle);
    std::printf("    expiry day vs the next session (ONE session apart, %.3f"
                " of the cycle)\n      linear   %.3f  -> %.1fx too far\n"
                "      cyclic   %.3f  -> exact\n",
                truth, lin, lin / truth, cyc);
    check(near(cyc, truth, 1e-12),
          "the cyclic distance is exactly one session, which is the truth");
    check(lin > 3.0 * truth,
          "while the linear encoding puts two CONSECUTIVE sessions at opposite"
          " ends of the range -- the wrap is real here, so the circle is the"
          " right shape");

    // Sanity: the pair is monotone in the circular distance, so a
    // distance-based model sees the circular metric and not an artefact.
    double prev = -1.0;
    bool monotone = true;
    for (int i = 0; i <= 10; ++i) {
        const double f = static_cast<double>(i) / 20.0;   // 0 .. 0.5
        const auto a = cyclic_encode(CyclePosition{0.0, 1.0});
        const auto b = cyclic_encode(CyclePosition{f, 1.0});
        const double d = pair_distance(*a, *b);
        if (d < prev - 1e-12) { monotone = false; }
        prev = d;
    }
    check(monotone,
          "and the sin/cos pair's euclidean distance rises monotonically with"
          " the circular distance, so a kernel reads the circle and not an"
          " artefact of the encoding");

    // A cycle nobody described stays absent.
    CalendarSlots s{};
    s.session_progress = 0;
    s.week_sin = 1; s.week_cos = 2;
    s.expiry_cycle_sin = 3; s.expiry_cycle_cos = 4;
    CalendarPosition p{};
    p.session = nse_session();
    p.week = CyclePosition{2.0, 5.0};       // Wednesday of a normal week
    // p.expiry_cycle left at {0, 0} -- NOT SUPPLIED
    FeatureVector v{1, 8, Timestamp{kNs}};
    const auto n = build_calendar(Timestamp{kNs}, p, s, v);
    check(n.has_value() && *n == 3, "the described cycle writes its pair");
    check(v.has(1) && v.has(2), "the week pair is present");
    check(!v.has(3) && !v.has(4),
          "and the expiry cycle nobody described stays ABSENT -- a defaulted"
          " zero would place every such tick at day one of the cycle, which is"
          " a specific and wrong claim rather than a missing one");
    check(cyclic_encode(CyclePosition{1.0, 0.0}).error()
          == CalendarError::BadCycle,
          "a cycle of zero length is refused outright");
}

// ── 2 ────────────────────────────────────────────────────────────────────
void the_iv_carries_the_width_of_its_own_error_bar()
{
    std::printf("\n2 the_iv_carries_the_width_of_its_own_error_bar\n");
    // A 30-day NIFTY chain, forward 24,000.00.
    const Price F{2'400'000};
    const Years T{30.0 / 365.0};
    const double tick = 5.0;            // half a 10-paise spread

    OptionSlots s{};
    s.iv = 0; s.iv_uncertainty = 1; s.log_moneyness = 2;
    s.time_to_expiry = 3; s.sqrt_time = 4;
    s.delta = 5; s.gamma = 6; s.vega = 7; s.theta = 8;

    struct Case { const char* label; std::int64_t strike; double vol; };
    const Case cases[] = {
        {"24000 (ATM)", 2'400'000, 0.14},
        {"26000 (OTM)", 2'600'000, 0.17},
        {"28000 (wing)", 2'800'000, 0.21},
    };

    double atm_band = 0.0, wing_band = 0.0;
    for (std::size_t i = 0; i < 3; ++i) {
        const Price K{cases[i].strike};
        const auto priced = black76(OptionRight::Call, F, K, T,
                                    Vol{cases[i].vol}, 0.0);
        check(priced.has_value(), "the fixture option prices");
        if (!priced) { continue; }

        OptionPoint o{};
        o.right = OptionRight::Call;
        o.forward = F; o.strike = K; o.t = T;
        o.mid = Price{static_cast<std::int64_t>(priced->price + 0.5)};
        o.price_uncertainty = tick;

        FeatureVector v{1, 9, Timestamp{0}};
        const int w = build_options(o, s, v);
        check(w == 9 && v.complete(),
              "every registered options slot fills for a live quote");
        const double band = v.value(1).value();
        std::printf("    %-13s  iv %.4f  +- %.5f   delta %+.3f  vega %.1f\n",
                    cases[i].label, v.value(0).value(), band,
                    v.value(5).value(), v.value(7).value());
        if (i == 0) { atm_band = band; }
        if (i == 2) { wing_band = band; }
    }

    std::printf("    -> one %g-paise tick resolves the ATM vol to %.6f and the"
                " wing to %.6f:\n       the SAME price uncertainty is %.0fx"
                " wider in vol terms out there.\n",
                tick, atm_band, wing_band, wing_band / atm_band);
    check(wing_band > 10.0 * atm_band,
          "the wing's implied vol is resolved far less precisely than the money"
          " strike's by the same tick -- which is why the band travels as its"
          " own feature rather than being dropped");
    check(atm_band > 0.0 && wing_band > 0.0,
          "and both bands are finite and positive where vega is alive");

    // The skew is doing some of the work, and it is worth separating out.
    // P3-04 measured 433x across this same chain, but priced it FLAT. Quoting
    // the wing at 21 vol instead of 14 raises its vega, which narrows the band
    // -- so a real skewed chain is better conditioned out there than a flat
    // one, and the ratio quoted in the header belongs to the flat case.
    const auto flat = black76(OptionRight::Call, F, Price{2'800'000}, T,
                              Vol{0.14}, 0.0);
    check(flat.has_value(), "the same wing prices at the money strike's vol");
    if (flat) {
        OptionPoint o{};
        o.right = OptionRight::Call;
        o.forward = F; o.strike = Price{2'800'000}; o.t = T;
        o.mid = Price{static_cast<std::int64_t>(flat->price + 0.5)};
        o.price_uncertainty = tick;
        FeatureVector v{1, 9, Timestamp{0}};
        (void)build_options(o, s, v);
        const double flat_band = v.value(1).value();
        std::printf("    the same wing quoted FLAT at 14 vol: +- %.6f -> %.0fx"
                    " the ATM band.\n       So of P3-04's 433x, the skew"
                    " itself claws back a factor of %.0f -- the wing is\n     "
                    "  better conditioned than a flat surface suggests, and"
                    " still far worse than\n       the money strike.\n",
                    flat_band, flat_band / atm_band, flat_band / wing_band);
        check(flat_band > wing_band,
              "a wing quoted at the ATM vol is resolved WORSE than one quoted"
              " at its own higher vol, because the lower vol means less vega"
              " for the tick to divide by");
    }
}

// ── 3 ────────────────────────────────────────────────────────────────────
void expiry_day_fills_no_greeks()
{
    std::printf("\n3 expiry_day_fills_no_greeks\n");
    OptionSlots s{};
    s.iv = 0; s.iv_uncertainty = 1; s.log_moneyness = 2;
    s.time_to_expiry = 3; s.sqrt_time = 4;
    s.delta = 5; s.gamma = 6; s.vega = 7; s.theta = 8;

    OptionPoint o{};
    o.right = OptionRight::Call;
    o.forward = Price{2'400'000};
    o.strike = Price{2'400'000};
    o.t = Years{0.0};                   // expiry
    o.mid = Price{1'000};
    o.price_uncertainty = 5.0;

    FeatureVector v{1, 9, Timestamp{0}};
    const int w = build_options(o, s, v);
    check(w == 1 && v.has(2),
          "at T = 0 only log-moneyness is written -- it is still a fact about"
          " the strike");
    check(!v.has(3) && !v.has(4) && !v.has(5) && !v.has(6) && !v.has(7)
          && !v.has(8),
          "and nothing time-dependent is: at the money on expiry day gamma is"
          " a delta function, not a large number, and P3-01 refuses it rather"
          " than handing back something a model would divide by");
    check(!v.complete(), "so the vector reports itself incomplete");

    // A strike with no quote gets time but no vol.
    OptionPoint q = o;
    q.t = Years{1.0 / 365.0};
    q.mid = Price{0};                   // no quote at all
    FeatureVector u{1, 9, Timestamp{0}};
    const int wq = build_options(q, s, u);
    check(wq == 3 && u.has(3) && u.has(4) && !u.has(0),
          "an unquoted strike still has a time to expiry but NO implied vol --"
          " the two are separate facts and only one of them is missing");
}

// ── 4 ────────────────────────────────────────────────────────────────────
void correlation_is_computed_on_returns_not_levels()
{
    std::printf("\n4 correlation_is_computed_on_returns_not_levels\n");
    constexpr std::size_t kN = 512;
    double a_ret[kN], b_ret[kN], a_lvl[kN], b_lvl[kN];

    double sum_level = 0.0, sum_return = 0.0;
    constexpr int kTrials = 40;
    for (int t = 0; t < kTrials; ++t) {
        Lcg g{0x5EED0000ULL + static_cast<std::uint64_t>(t)};
        double pa = 0.0, pb = 0.0;
        for (std::size_t i = 0; i < kN; ++i) {
            a_ret[i] = g.normal();
            b_ret[i] = g.normal();      // INDEPENDENT of a
            pa += a_ret[i]; pb += b_ret[i];
            a_lvl[i] = pa; b_lvl[i] = pb;
        }
        CrossAssetSlots s{};
        s.correlation = 0; s.beta = 1; s.relative_return = 2; s.pair_count = 3;

        FeatureVector lv{1, 4, Timestamp{0}};
        FeatureVector rv{1, 4, Timestamp{0}};
        (void)build_cross_asset(a_lvl, b_lvl, kN, s, lv);
        (void)build_cross_asset(a_ret, b_ret, kN, s, rv);
        sum_level += std::fabs(lv.value(0).value());
        sum_return += std::fabs(rv.value(0).value());
    }
    const double mean_level = sum_level / kTrials;
    const double mean_return = sum_return / kTrials;
    std::printf("    %d pairs of INDEPENDENT random walks, %zu steps each:\n"
                "      mean |correlation| on LEVELS  %.3f\n"
                "      mean |correlation| on RETURNS %.3f\n",
                kTrials, kN, mean_level, mean_return);
    check(mean_level > 4.0 * mean_return,
          "two series with NOTHING in common correlate strongly in levels and"
          " not at all in returns -- the levels number is spurious regression,"
          " and it is the one that gets a pairs strategy funded");
    check(mean_return < 0.10,
          "the returns correlation of independent walks sits near zero, as it"
          " should");

    // Beta, and the reference that did not move.
    CrossAssetSlots s{};
    s.correlation = 0; s.beta = 1; s.relative_return = 2; s.pair_count = 3;
    double ref[8], asset[8];
    for (std::size_t i = 0; i < 8; ++i) {
        ref[i] = static_cast<double>(i) - 3.5;
        asset[i] = 1.5 * ref[i];        // beta exactly 1.5
    }
    FeatureVector bv{1, 4, Timestamp{0}};
    (void)build_cross_asset(asset, ref, 8, s, bv);
    std::printf("    a series moving 1.5x its reference -> beta %.4f,"
                " correlation %.4f\n",
                bv.value(1).value(), bv.value(0).value());
    check(near(bv.value(1).value(), 1.5, 1e-12), "beta is recovered exactly");
    check(near(bv.value(0).value(), 1.0, 1e-12),
          "and the correlation is 1.0, because it is the same series scaled");

    double flat[8] = {};
    FeatureVector fv{1, 4, Timestamp{0}};
    (void)build_cross_asset(asset, flat, 8, s, fv);
    check(!fv.has(1) && !fv.has(0),
          "a reference that did not move yields NO beta and NO correlation --"
          " absent, not zero, because a beta of zero is the claim 'moves"
          " independently of the index' and this is the absence of a claim");
    check(fv.has(3) && fv.value(3).value() == 8.0,
          "while the sample count is still reported, so a model can see how"
          " much data the missing estimate would have rested on");

    FeatureVector tiny{1, 4, Timestamp{0}};
    (void)build_cross_asset(asset, ref, 2, s, tiny);
    check(!tiny.has(0) && !tiny.has(1) && tiny.has(3),
          "and two observations produce a count but no estimates -- a beta"
          " from two points is a line through two points");
}

} // namespace

int main()
{
    std::printf("altair features options/calendar/cross-asset tests\n");
    the_session_clock_does_not_wrap();
    the_expiry_cycle_does_wrap();
    the_iv_carries_the_width_of_its_own_error_bar();
    expiry_day_fills_no_greeks();
    correlation_is_computed_on_returns_not_levels();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
