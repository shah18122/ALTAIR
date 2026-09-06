// models/tests/test_dcf_sensitivity.cpp -- P10-02b.
//
// The Models page has listed DCF as "blocked on data — no filings ingested"
// since it shipped, and that is true of the FORWARD direction: without
// point-in-time fundamentals there is no cash flow to discount, and inventing
// one to make the row go green would be exactly the kind of plausible wrong
// number this project keeps catching.
//
// What was NOT blocked, and what this card finishes, is the part that makes a
// DCF falsifiable at all:
//
//   1. A two-way sensitivity grid. A DCF quoted as one number is dishonest,
//      because the two inputs that move it most are the two nobody can
//      observe.
//   2. The REVERSE DCF. Instead of "what is it worth" -- unanswerable without
//      filings -- ask "what would have to be true for today's price to be
//      right". That needs a market price, which we have for every instrument
//      in the master, and it turns an unfalsifiable forecast into a claim
//      about growth that can be checked against a company's history and
//      against the size of the economy.
//
// The cash-flow figures below are ILLUSTRATIVE and labelled as such. Nothing
// here claims to value a real company; the findings are about the SHAPE of a
// DCF's uncertainty, which is a property of the arithmetic and not of any
// particular firm.

#include <models/dcf.hpp>

#include <cstdio>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %-4s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) { ++failures; }
}

/// A mid-cap shape: Rs 100 crore of FCFF, 12% for five years, fading over ten
/// to 5% forever, discounted at 12%. Round numbers on purpose -- they are a
/// worked example, not a company.
altair::GrowthProfile profile() {
    altair::GrowthProfile g;
    g.high_years = 5;
    g.high_growth = 0.12;
    g.fade_years = 10;
    g.terminal_growth = 0.05;
    return g;
}

constexpr double kFcff = 1'000'000'000.0;   // Rs 100 crore, in rupees
constexpr double kWacc = 0.12;

} // namespace

int main() {
    using altair::DcfError;
    using altair::GrowthProfile;
    using altair::dcf_fcff;
    using altair::implied_terminal_growth;
    using altair::sensitivity_grid;

    std::printf("P10-02b DCF sensitivity and the reverse DCF\n");

    const GrowthProfile g = profile();
    const auto base = dcf_fcff(kFcff, g, kWacc);
    check(base.has_value(), "the base valuation computes");
    if (!base) { return 1; }

    std::printf("\n  BASE: Rs %.0f crore enterprise value\n",
                base->value() / 10'000'000.0);
    std::printf("    terminal share %.1f%% -- the fraction of this number that "
                "is an\n    assumption about forever rather than a forecast of "
                "anything\n", 100.0 * base->dcf.terminal_share);

    // 43%, not the 60-80% a five-year DCF shows -- because this profile
    // projects FIFTEEN years explicitly before the perpetuity starts. The
    // first version of this check demanded over half and failed, and the
    // reason it failed is the more interesting fact:
    //
    // EXTENDING THE PROJECTION MOVES THE ASSUMPTION, IT DOES NOT REMOVE IT.
    //
    // An analyst who dislikes a 70% terminal share can halve it by forecasting
    // ten more years. Nothing has been learned about the company; the guess
    // about forever has been relabelled as fifteen individual guesses about
    // years 6 through 20, each of which is also unobservable. The terminal
    // share is therefore a number to read alongside the projection length,
    // never on its own -- and it is still the single largest component here.
    std::printf("    (43%% not 70%%, because this projects 15 years before the "
                "perpetuity;\n     a shorter projection moves value INTO the "
                "terminal, it does not create it)\n");
    check(base->dcf.terminal_share > 0.3 && base->dcf.terminal_share < 1.0,
          "a substantial minority of the value is the perpetuity, and the "
          "share is reported next to the projection length because extending "
          "the projection relabels the assumption rather than removing it");

    // ---- 1. THE GRID ------------------------------------------------------
    {
        const auto grid = sensitivity_grid(kFcff, g, kWacc, 0.005, 0.005, 2);
        check(grid.has_value(), "the sensitivity grid computes");
        if (grid) {
            std::printf("\n  SENSITIVITY, +/- 1%% on each axis in 50bp steps\n");
            std::printf("    %8s", "WACC\\g");
            for (std::size_t j = 0; j < grid->n_growth; ++j) {
                std::printf(" %9.2f%%", 100.0 * grid->growth[j]);
            }
            std::printf("\n");
            for (std::size_t i = 0; i < grid->n_rate; ++i) {
                std::printf("    %7.2f%%", 100.0 * grid->rate[i]);
                for (std::size_t j = 0; j < grid->n_growth; ++j) {
                    const std::size_t k = i * grid->n_growth + j;
                    if (grid->priced[k]) {
                        std::printf(" %10.0f", grid->value[k] / 10'000'000.0);
                    } else {
                        // NOT a zero. g >= r is undefined, not cheap.
                        std::printf(" %10s", "--");
                    }
                }
                std::printf("\n");
            }
            std::printf("    (Rs crore; -- is g >= WACC, undefined rather "
                        "than low)\n");

            std::printf("\n    spread %.2fx across the grid; %zu cells "
                        "refused\n", grid->spread(), grid->refused());
            check(grid->spread() > 1.5,
                  "one percent of uncertainty on each unobservable input "
                  "moves the valuation by more than 50% -- so a DCF reported "
                  "as a single number has hidden its own error bar, and any "
                  "'20% undervalued' conclusion is inside the noise");
            // A WELL-CENTRED GRID PRICES EVERY CELL, and asserting otherwise
            // was this test's own error: the first version demanded a refusal
            // here, got none, and failed. It should not have expected one --
            // WACC 11-13% against terminal growth 4-6% never approaches the
            // singularity, and a grid whose corners are undefined is a badly
            // centred grid, not a demonstration of anything.
            check(grid->refused() == 0,
                  "every cell of a sanely centred grid prices, so the spread "
                  "above is a real range and not an artefact of missing "
                  "corners");
        }
    }

    // ---- 1b. AND THE SINGULARITY IS REFUSED WHERE IT IS REAL --------------
    //
    // Checked on a grid that actually straddles the discount rate, because
    // that is the only place the question arises.
    {
        GrowthProfile near = g;
        near.terminal_growth = kWacc - 0.005;      // 50bp under the discount
        const auto grid = sensitivity_grid(kFcff, near, kWacc, 0.005, 0.005, 2);
        if (grid) {
            std::printf("\n    straddling the discount rate: %zu of %zu cells "
                        "refused\n",
                        grid->refused(), grid->n_rate * grid->n_growth);
            check(grid->refused() > 0,
                  "where terminal growth reaches the discount rate the cell is "
                  "REFUSED, not priced -- a perpetuity growing at its own "
                  "discount rate is infinite, and a zero there would drag a "
                  "range down while looking like arithmetic");
            check(grid->spread() > 0.0,
                  "and the surviving cells still produce a usable range, so "
                  "one undefined corner does not destroy the table");
        }
    }

    // ---- 2. THE REVERSE DCF -----------------------------------------------
    //
    // The direction that survives having no filings, and the one that can be
    // argued with.
    {
        // Suppose the market caps this at 1.6x the base valuation.
        const double market = base->value() * 1.6;
        const auto implied = implied_terminal_growth(kFcff, g, kWacc, market);
        check(implied.has_value(), "the reverse DCF solves");
        if (implied) {
            std::printf("\n  REVERSE DCF\n");
            std::printf("    at 1.60x the base valuation, the price implies "
                        "%.2f%% perpetual growth\n"
                        "    against the %.2f%% assumed.\n",
                        100.0 * *implied, 100.0 * g.terminal_growth);
            check(*implied > g.terminal_growth,
                  "a higher price implies higher perpetual growth, which is "
                  "the direction that makes the inversion a check rather than "
                  "a curiosity");
            check(*implied < kWacc,
                  "and it stays strictly below the discount rate -- at g == r "
                  "the perpetuity is infinite and ANY price is reachable, so "
                  "a solver that clamped there would report a bound as a "
                  "measurement");

            // Round-trip: the implied rate must reprice to the target.
            GrowthProfile back = g;
            back.terminal_growth = *implied;
            const auto reprice = dcf_fcff(kFcff, back, kWacc);
            const double err = reprice
                ? std::fabs(reprice->value() - market) / market : 1.0;
            std::printf("    round trip reprices to within %.2e of the "
                        "target\n", err);
            check(err < 1e-6,
                  "and it round-trips: feeding the implied growth back "
                  "reproduces the price it was solved from");
        }
    }

    // ---- 3. WHAT THE INVERSION IS FOR -------------------------------------
    //
    // The number is only useful if there is something to compare it to. A
    // perpetual growth rate above nominal GDP means the firm eventually
    // BECOMES the economy, which is the arithmetic bound that makes an
    // implied growth rate falsifiable rather than merely large.
    {
        const double nominal_gdp = 0.105;   // ~10.5%, India, illustrative
        const double rich = base->value() * 3.0;
        const auto implied = implied_terminal_growth(kFcff, g, kWacc, rich);
        if (implied) {
            std::printf("\n    at 3.00x, the implied perpetual rate is "
                        "%.2f%%.\n", 100.0 * *implied);
            std::printf("    Against nominal GDP near %.1f%%, a perpetual rate "
                        "above that is not\n    optimistic -- it is a firm "
                        "that eventually becomes the whole economy.\n"
                        "    That bound is what makes the number arguable, "
                        "and it is the entire\n    reason to run the DCF "
                        "backwards.\n", 100.0 * nominal_gdp);
            check(*implied > g.terminal_growth,
                  "a 3x price demands materially more growth than the base "
                  "case assumed");
        }
    }

    // ---- 4. REFUSALS ------------------------------------------------------
    {
        check(!sensitivity_grid(kFcff, g, kWacc, 0.0, 0.005).has_value(),
              "a zero step is refused rather than producing one column "
              "repeated five times");
        const auto impossible =
            implied_terminal_growth(kFcff, g, kWacc, base->value() * 0.01);
        check(!impossible.has_value(),
              "a target the model cannot reach even at -10% perpetual decline "
              "is REFUSED, not clamped -- returning the bound would present "
              "a limit as a measurement");
    }

    std::printf("\n  WHAT IS STILL BLOCKED\n");
    std::printf("    The forward direction. Every number above starts from a "
                "cash flow, and\n    there are no point-in-time filings in "
                "this tree. That is a data drop,\n    not a code gap: the "
                "machinery here prices whatever it is handed, and\n    the "
                "reverse DCF works today because it needs a PRICE, which we "
                "have.\n");

    std::printf("\n%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
