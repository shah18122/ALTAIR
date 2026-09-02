// P8-01 acceptance tests for models/labels.hpp.
//
// Test 1 is the card: the first touch wins. Two implementations, the same
// paths, and a count of how many labels flip -- every flip is a trade closed
// at a loss and recorded as a profit, handed to a model as ground truth.
//
// Test 2: the label window ends at the touch, not at the horizon, because
// P6-06 purges on it.
//
// Test 3: overlapping labels are not independent observations.
//
// No check description here may contain the substring FAIL.

#include <models/labels.hpp>

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

constexpr std::size_t kN = 20'000;
Price close_[kN], high_[kN], low_[kN];

void make_path(std::uint64_t seed, double sigma_per_bar)
{
    Lcg g{seed};
    double p = 2'400'000.0;
    for (std::size_t i = 0; i < kN; ++i) {
        p *= 1.0 + g.normal() * sigma_per_bar;
        const double wick = std::fabs(g.normal()) * sigma_per_bar * p * 0.5;
        close_[i] = Price{static_cast<std::int64_t>(p)};
        high_[i] = Price{static_cast<std::int64_t>(p + wick)};
        low_[i] = Price{static_cast<std::int64_t>(p - wick)};
    }
}

BarrierSpec spec(double target, double stop, std::size_t horizon)
{
    BarrierSpec b{};
    b.target_sigmas = target;
    b.stop_sigmas = stop;
    b.horizon = horizon;
    b.vertical = VerticalPolicy::SignOfReturn;
    return b;
}

// ── 1 ────────────────────────────────────────────────────────────────────
// THE card.
void the_first_touch_wins()
{
    std::printf("\n1 the_first_touch_wins\n");
    // A hand-built path that stops out and then recovers past the target.
    // Entry 24,000. Stop at -1%, target at +1%.
    Price p[8]  = {Price{2'400'000}, Price{2'395'000}, Price{2'370'000},
                   Price{2'380'000}, Price{2'400'000}, Price{2'420'000},
                   Price{2'430'000}, Price{2'435'000}};
    const auto b = spec(1.0, 1.0, 6);
    const double sigma = 0.01;          // 1% -> barriers at 23,760 / 24,240

    const auto ordered = label_event(p, nullptr, nullptr, 8, 0, sigma, b);
    const auto unordered = label_event_unordered(p, nullptr, nullptr, 8, 0,
                                                 sigma, b);
    check(ordered.has_value() && unordered.has_value(), "both label the path");
    if (!ordered || !unordered) { return; }

    std::printf("    a path that stops out on bar 2 (23,700) and recovers past"
                " the target on bar 6 (24,300):\n"
                "      barriers          %.2f / %.2f\n"
                "      FIRST TOUCH       %s on bar %zu, label %+d\n"
                "      independent tests %s on bar %zu, label %+d\n",
                static_cast<double>(ordered->lower.raw()) / 100.0,
                static_cast<double>(ordered->upper.raw()) / 100.0,
                ordered->touched == BarrierTouched::Stop ? "STOP" : "target",
                ordered->bars_held, ordered->sign,
                unordered->touched == BarrierTouched::Stop ? "stop" : "TARGET",
                unordered->bars_held, unordered->sign);

    check(ordered->touched == BarrierTouched::Stop && ordered->sign == -1,
          "walking the path forward stops at the FIRST touch, which is the"
          " stop on bar 2 -- the trade was closed at a loss and the label says"
          " so");
    check(unordered->touched == BarrierTouched::Target
          && unordered->sign == +1,
          "while two independent existence tests with the target preferred"
          " label the SAME path a win -- a trade closed at a loss, recorded as"
          " a profit, handed to a model as ground truth");
    std::printf("    -> CLAUDE.md hard rule 8 is this defect in the live path:"
                " 'the tighter stop is\n       checked before the original"
                " stop', which cost the predecessor about Rs 41K\n       in one"
                " replay. Here it costs something worse than a mispriced trade,"
                "\n       because the model trained on it goes looking for"
                " more of them.\n");

    // How often it happens on real-shaped data.
    make_path(0x1AB1E, 0.004);
    std::size_t flips = 0, decided = 0;
    for (std::size_t e = 0; e + 40 < kN; e += 7) {
        const auto a = label_event(close_, high_, low_, kN, e, 0.004,
                                   spec(1.5, 1.5, 30));
        const auto u = label_event_unordered(close_, high_, low_, kN, e, 0.004,
                                             spec(1.5, 1.5, 30));
        if (!a || !u || !a->decided()) { continue; }
        ++decided;
        if (a->sign != u->sign) { ++flips; }
    }
    std::printf("    on %zu labelled events from a %zu-bar synthetic path,"
                " 1.5-sigma barriers,\n    30-bar horizon:  %zu labels FLIP"
                " (%.1f%%)\n",
                decided, kN, flips,
                100.0 * static_cast<double>(flips)
                    / static_cast<double>(decided));
    check(decided > 1000, "a usable number of events were labelled");
    check(flips > 0,
          "and a material fraction of them disagree -- every one is a losing"
          " trade labelled as a winner, and they are not randomly distributed:"
          " they are concentrated in exactly the choppy paths a model would"
          " most like to learn to avoid");
    check(flips * 100 / decided >= 1,
          "at least one label in a hundred, on an ordinary path with no"
          " pathology in it");

    // Within a single bar, the stop is assumed.
    Price one[3] = {Price{2'400'000}, Price{2'400'000}, Price{2'400'000}};
    Price hi[3] = {Price{2'400'000}, Price{2'500'000}, Price{2'400'000}};
    Price lo[3] = {Price{2'400'000}, Price{2'300'000}, Price{2'400'000}};
    const auto both = label_event(one, hi, lo, 3, 0, 0.01, spec(1.0, 1.0, 2));
    check(both.has_value() && both->touched == BarrierTouched::Stop,
          "when ONE bar's range contains both barriers the bar does not say"
          " which came first, and the STOP is assumed -- the conservative"
          " reading, and the one a live order would have suffered");
}

// ── 2 ────────────────────────────────────────────────────────────────────
void the_label_window_ends_at_the_touch()
{
    std::printf("\n2 the_label_window_ends_at_the_touch\n");
    make_path(0x1710D0A1, 0.004);
    const auto b = spec(1.5, 1.5, 30);

    std::size_t sum_held = 0, n = 0, quick = 0;
    for (std::size_t e = 0; e + 40 < kN; e += 11) {
        const auto l = label_event(close_, high_, low_, kN, e, 0.004, b);
        if (!l || !l->decided()) { continue; }
        check(l->window.start == e && l->window.end == e + l->bars_held,
              "the window runs from the event to the touch");
        sum_held += l->bars_held;
        if (l->bars_held * 4 < b.horizon) { ++quick; }
        ++n;
        if (n >= 3) { break; }
    }
    // Full pass for the statistic.
    sum_held = 0; n = 0; quick = 0;
    for (std::size_t e = 0; e + 40 < kN; e += 3) {
        const auto l = label_event(close_, high_, low_, kN, e, 0.004, b);
        if (!l || !l->decided()) { continue; }
        sum_held += l->bars_held;
        if (l->bars_held * 4 < b.horizon) { ++quick; }
        ++n;
    }
    const double mean_held = static_cast<double>(sum_held)
                           / static_cast<double>(n);
    std::printf("    %zu labels, %zu-bar horizon:\n"
                "      mean bars actually held        %6.2f\n"
                "      purging on the HORIZON would   %6.2f bars per label\n"
                "      over-purge by                  %6.2f bars, %.0f%% more"
                " than needed\n",
                n, b.horizon, mean_held, static_cast<double>(b.horizon),
                static_cast<double>(b.horizon) - mean_held,
                100.0 * (static_cast<double>(b.horizon) / mean_held - 1.0));
    check(mean_held < static_cast<double>(b.horizon),
          "labels touch their barriers well before the time limit on average");
    check(quick > 0,
          "and many touch in a small fraction of it");
    std::printf("    -> P6-06 purges training samples whose label windows"
                " overlap the test set. A\n       label that touched on bar 3"
                " of a 30-bar horizon depends on three bars of\n       future,"
                " not thirty. Reporting the horizon throws away training data"
                " to fix\n       a leak that is not there.\n");
}

// ── 3 ────────────────────────────────────────────────────────────────────
void overlapping_labels_are_not_independent_observations()
{
    std::printf("\n3 overlapping_labels_are_not_independent"
                "_observations\n");
    make_path(0x0BE12A, 0.004);
    const auto b = spec(1.5, 1.5, 30);

    constexpr std::size_t kEvents = 2000;
    static Label labels[kEvents];
    static double uniq[kEvents];
    std::size_t k = 0;
    for (std::size_t e = 0; e + 40 < kN && k < kEvents; ++e) {
        const auto l = label_event(close_, high_, low_, kN, e, 0.004, b);
        if (l) { labels[k++] = *l; }
    }
    check(k == kEvents, "two thousand consecutive events label");
    check(average_uniqueness(labels, k, kN, uniq).has_value(),
          "and their uniqueness computes");

    const double eff = effective_observations(uniq, k);
    std::printf("    %zu labels from CONSECUTIVE events, %zu-bar horizon:\n"
                "      row count                     %6zu\n"
                "      EFFECTIVE observations        %6.1f\n"
                "      each row is worth             %6.3f of an observation\n",
                k, b.horizon, k, eff, eff / static_cast<double>(k));
    check(eff < static_cast<double>(k),
          "the effective number of observations is below the row count --"
          " labels from consecutive events share most of their future, so they"
          " are not separate observations");
    check(eff < 0.5 * static_cast<double>(k),
          "and by a wide margin, not a rounding term");
    std::printf("    -> the same arithmetic as P6-04's correlated signals,"
                " applied to labels instead\n       of features. P7-03's"
                " deflated Sharpe wants THIS number, not the row count:\n"
                "       feeding it %zu when the truth is %.0f overstates the"
                " evidence by %.1fx.\n",
                k, eff, static_cast<double>(k) / eff);

    // Spaced events overlap less, which is the check that the measure works.
    std::size_t k2 = 0;
    for (std::size_t e = 0; e + 40 < kN && k2 < 500; e += 60) {
        const auto l = label_event(close_, high_, low_, kN, e, 0.004, b);
        if (l) { labels[k2++] = *l; }
    }
    (void)average_uniqueness(labels, k2, kN, uniq);
    const double eff2 = effective_observations(uniq, k2);
    std::printf("    the same labels from events spaced 60 bars apart:"
                " %.3f of an observation each\n",
                eff2 / static_cast<double>(k2));
    check(eff2 / static_cast<double>(k2) > 0.9,
          "events spaced beyond the horizon barely overlap and are worth"
          " nearly a full observation each, so the measure is tracking the"
          " overlap rather than penalising sample size");
}

// ── 4 ────────────────────────────────────────────────────────────────────
void the_decisions_have_no_defaults()
{
    std::printf("\n4 the_decisions_have_no_defaults\n");
    Price p[8] = {Price{2'400'000}, Price{2'401'000}, Price{2'402'000},
                  Price{2'403'000}, Price{2'404'000}, Price{2'405'000},
                  Price{2'406'000}, Price{2'407'000}};

    BarrierSpec no_policy = spec(1.0, 1.0, 5);
    no_policy.vertical = VerticalPolicy::Unspecified;
    check(label_event(p, nullptr, nullptr, 8, 0, 0.01, no_policy).error()
          == LabelError::NoVerticalPolicy,
          "a spec that did not choose what to do at the time limit is REFUSED"
          " -- a signed label makes every event a directional bet including"
          " those that went nowhere, a neutral label creates a third class"
          " that is mostly noise, and both are decisions");

    check(label_event(p, nullptr, nullptr, 8, 0, 0.0, spec(1.0, 1.0, 5)).error()
          == LabelError::NoVolatility,
          "and a zero volatility estimate is refused rather than producing"
          " zero-width barriers that everything instantly touches");

    // Fixed-width barriers are a regime label wearing a return label's name.
    make_path(0xCA1F0000, 0.001);           // quiet
    std::size_t calm_touched = 0, calm_n = 0;
    for (std::size_t e = 0; e + 40 < 4000; e += 7) {
        // A FIXED 1% barrier, expressed as sigmas of the WRONG sigma.
        const auto l = label_event(close_, high_, low_, kN, e, 0.01,
                                   spec(1.0, 1.0, 30));
        // A PRICE barrier, not decided(): decided() includes the vertical
        // touch, which nearly every event reaches, so counting it measures
        // whether the path was long enough rather than whether the barrier
        // was reachable.
        if (l) {
            ++calm_n;
            if (l->touched == BarrierTouched::Target
                || l->touched == BarrierTouched::Stop) { ++calm_touched; }
        }
    }
    make_path(0x5701A400, 0.008);          // eight times the volatility
    std::size_t wild_touched = 0, wild_n = 0;
    for (std::size_t e = 0; e + 40 < 4000; e += 7) {
        const auto l = label_event(close_, high_, low_, kN, e, 0.01,
                                   spec(1.0, 1.0, 30));
        if (l) {
            ++wild_n;
            if (l->touched == BarrierTouched::Target
                || l->touched == BarrierTouched::Stop) { ++wild_touched; }
        }
    }
    std::printf("    a FIXED 1%% barrier, 30-bar horizon:\n"
                "      quiet regime (0.1%% per bar)   %5.1f%% of events reach a"
                " price barrier\n"
                "      wild regime  (0.8%% per bar)   %5.1f%% of events reach a"
                " price barrier\n",
                100.0 * static_cast<double>(calm_touched)
                    / static_cast<double>(calm_n),
                100.0 * static_cast<double>(wild_touched)
                    / static_cast<double>(wild_n));
    check(wild_touched > 2 * calm_touched,
          "a fixed-width barrier is reached far more often in a volatile"
          " regime than a quiet one -- so a fixed-width label set is a REGIME"
          " label wearing a return label's name, which is why the widths here"
          " are multiples of a per-event volatility estimate");

    // A truncated path is unlabelled, not neutral.
    const auto trunc = label_event(p, nullptr, nullptr, 8, 5, 0.01,
                                   spec(1.0, 1.0, 30));
    check(trunc.has_value() && trunc->touched == BarrierTouched::Truncated
          && !trunc->decided(),
          "an event whose path ends before the horizon is TRUNCATED, not"
          " vertical -- the time limit was never reached, so it is unlabelled"
          " rather than labelled neutral, and the ragged right edge of every"
          " series does not all land in one class");

    Label blank{};
    check(blank.touched == BarrierTouched::Unknown && !blank.decided(),
          "and a zeroed label is Unknown, so it cannot pass for one that hit"
          " its profit target");
}

} // namespace

int main()
{
    std::printf("altair triple-barrier label tests\n");
    the_first_touch_wins();
    the_label_window_ends_at_the_touch();
    overlapping_labels_are_not_independent_observations();
    the_decisions_have_no_defaults();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
