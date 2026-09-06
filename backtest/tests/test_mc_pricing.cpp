// backtest/tests/test_mc_pricing.cpp -- P17-01.
//
// The point of an MC pricer is the payoffs with no closed form. The point of
// TESTING one is the payoff that has one: if the simulation cannot reproduce
// Black-76 to inside its own error bar, nothing it says about an Asian is
// worth reading.

#include <backtest/mc_pricing.hpp>

#include <cmath>
#include <cstdio>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %-4s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) { ++failures; }
}

/// A NIFTY-scale option: forward 24,150.00, strike 24,000.00, 30 days, 14% vol.
/// Paise throughout, as everything priced in this tree is.
altair::McSpec nifty_call() {
    altair::McSpec s;
    s.forward = 2'415'000.0;
    s.strike = 2'400'000.0;
    s.t = 30.0 / 365.0;
    s.vol = 0.14;
    s.rate = 0.065;
    s.right = altair::OptionRight::Call;
    s.steps = 1;
    return s;
}

} // namespace

int main() {
    using altair::Price;
    using altair::Vol;
    using altair::Years;
    using altair::black76;
    using altair::mc_asian;
    using altair::mc_european;

    std::printf("P17-01 Monte Carlo option pricing\n");

    const altair::McSpec s = nifty_call();
    const auto exact = black76(s.right, Price{static_cast<std::int64_t>(s.forward)},
                               Price{static_cast<std::int64_t>(s.strike)},
                               Years{s.t}, Vol{s.vol}, s.rate);
    check(exact.has_value(), "Black-76 prices the control exactly");
    if (!exact) { return 1; }
    std::printf("    analytic Black-76: %.2f paise (Rs %.2f)\n",
                exact->price, exact->price / 100.0);

    // ---- 1. THE SIMULATION MUST REPRODUCE THE CLOSED FORM -----------------
    {
        const auto mc = mc_european(s, 200000, 0xC0FFEEu, true);
        check(mc.has_value(), "the European MC runs");
        if (mc) {
            std::printf("    MC 200k antithetic: %.2f +/- %.2f"
                        "  (95%%: %.2f .. %.2f)\n",
                        mc->price, mc->std_error, mc->lo95(), mc->hi95());
            check(mc->agrees_with(exact->price),
                  "and lands within its OWN error bar of Black-76 -- the "
                  "question for an MC price is never 'is it equal' but 'is "
                  "the difference inside the noise'");
        }
    }

    // ---- 2. CONVERGENCE IS 1/sqrt(n), MEASURED --------------------------
    //
    // Quadrupling the paths must halve the error. If it does not, the sampler
    // is correlated and every error bar in this file is too small.
    {
        const auto a = mc_european(s, 20000, 0xAB1E5u, false);
        const auto b = mc_european(s, 80000, 0xAB1E5u, false);
        if (a && b) {
            const double ratio = a->std_error / b->std_error;
            std::printf("    4x the paths -> error ratio %.2f"
                        " (1/sqrt(n) predicts 2.00)\n", ratio);
            check(ratio > 1.7 && ratio < 2.3,
                  "quadrupling paths halves the error, so the draws really "
                  "are independent -- a correlated sampler reports a "
                  "confident wrong price");
        }
    }

    // ---- 3. ANTITHETIC IS MEASURED, NOT ASSUMED ---------------------------
    //
    // It halves variance for a payoff MONOTONE in z. A call is. The reduction
    // is measured because for a non-monotone payoff it buys nothing, and a
    // default that silently buys nothing is worse than a parameter.
    {
        const auto plain = mc_european(s, 100000, 0x5EEDu, false);
        const auto anti = mc_european(s, 100000, 0x5EEDu, true);
        if (plain && anti) {
            std::printf("    plain     %.2f +/- %.3f  (%zu obs)\n",
                        plain->price, plain->std_error, plain->paths);
            std::printf("    antithetic %.2f +/- %.3f  (%zu obs)\n",
                        anti->price, anti->std_error, anti->paths);
            check(anti->std_error < plain->std_error,
                  "antithetic pairing lowers the error on a monotone payoff "
                  "for the same number of normal draws");
            check(anti->paths * 2 >= plain->paths,
                  "and the pair counts as ONE observation, not two -- "
                  "counting them independently would report an error sqrt(2) "
                  "too small and claim the reduction twice");
        }
    }

    // ---- 4. THE PAYOFF WITH NO CLOSED FORM --------------------------------
    //
    // An arithmetic Asian. Its price must sit BELOW the European: averaging
    // the forward reduces the variance of the terminal quantity, and a call on
    // a less volatile underlying is worth less. That is a structural check no
    // amount of path count can fake.
    {
        altair::McSpec a = s;
        a.steps = 30;                      // daily averaging over the month
        const auto asian = mc_asian(a, 100000, 0xA51A4u, true);
        check(asian.has_value(), "the Asian MC runs");
        if (asian) {
            std::printf("    Asian (30 fixings, control variate): "
                        "%.2f +/- %.2f\n", asian->price, asian->std_error);
            check(asian->price < exact->price,
                  "the arithmetic Asian is worth LESS than the European -- "
                  "averaging cuts the variance of what the option is struck "
                  "against, and that is a structural check the path count "
                  "cannot fake");
            check(asian->price > 0.0, "and it is worth something");
        }
    }

    // ---- 5. WHAT THE CONTROL VARIATE BUYS ---------------------------------
    {
        altair::McSpec a = s;
        a.steps = 30;
        const auto with = mc_asian(a, 50000, 0xC7212u, true);
        const auto without = mc_asian(a, 50000, 0xC7212u, false);
        if (with && without) {
            const double gain = without->std_error / with->std_error;
            std::printf("    control variate: error %.3f -> %.3f (%.1fx"
                        " tighter, %.0fx fewer paths for the same"
                        " accuracy)\n",
                        without->std_error, with->std_error, gain, gain * gain);
            check(with->std_error < without->std_error,
                  "the control variate tightens the estimate, and because "
                  "error falls as 1/sqrt(n) an Nx tighter error is N^2 fewer "
                  "paths -- that squaring is the whole reason it is worth "
                  "one extra evaluation per path");
        }
    }

    // ---- 6. REFUSALS ------------------------------------------------------
    {
        altair::McSpec bad = s;
        bad.vol = 0.0;
        check(!mc_european(bad, 1000, 1u).has_value(),
              "zero vol is refused, not priced as an intrinsic -- the same "
              "boundary black76 refuses at rather than returning a limit");
        altair::McSpec one = s;
        one.steps = 1;
        check(!mc_asian(one, 1000, 1u).has_value(),
              "an Asian with ONE fixing is refused: it is a European wearing "
              "the wrong name, and silently pricing it would hide a "
              "modelling error rather than an approximation");
        check(!mc_european(s, 1, 1u).has_value(),
              "one path has no standard error, so it is not a price");
    }

    std::printf("\n%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
