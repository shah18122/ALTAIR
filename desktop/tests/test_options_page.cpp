// P23-02 acceptance tests for the Options page.
//
// The page makes four claims that can be checked without a market: that the
// pricer and the solver are inverses to within the tick, that the wings stop
// being resolvable, that the SVI fit recovers parameters it was given, and
// that the butterfly scan finds an arbitrage boundary when pushed past one.
//
// It also PRINTS the whole report. A page nobody has read is a page whose
// columns do not line up, and the cheapest way to keep that honest is to put
// its output in the test log where a reviewer sees it.
//
// No check description here may contain the substring FAIL.

#include "../quant_pages.hpp"

#include <QCoreApplication>
#include <QString>

#include <cmath>
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

} // namespace

using namespace altair;

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    std::printf("P23-02 -- the Options page\n");

    // -----------------------------------------------------------------------
    // 1. Pricer and solver are inverses, to within the tick.
    // -----------------------------------------------------------------------
    std::printf("\n[1] round trip: price at a known vol, solve it back\n");
    const double F = 24000.0 * 100.0;
    const double T = 30.0 / 365.0;
    const double rate = 0.065;
    const Vol truth{0.15};

    const auto atm = black76(OptionRight::Call,
                             Price{static_cast<std::int64_t>(F)},
                             Price{static_cast<std::int64_t>(F)},
                             Years{T}, truth, rate);
    check(atm.has_value(), "the at-the-money option priced");
    if (!atm) { return 1; }
    const auto back = implied_vol_black76(
        OptionRight::Call, Price{static_cast<std::int64_t>(atm->price + 0.5)},
        Price{static_cast<std::int64_t>(F)},
        Price{static_cast<std::int64_t>(F)}, Years{T}, rate);
    check(back.has_value(), "and the vol solved back out of that price");
    if (!back) { return 1; }

    // The tolerance is DERIVED, not chosen: half a paise pushed through vega
    // is exactly the vol the rounding can move. Asserting a round number here
    // would either pass on a broken solver or fail on a correct one.
    const double tick_band = iv_uncertainty(atm->vega, 0.5);
    const double err = std::fabs(back->vol.raw() - truth.raw());
    std::printf("        recovered %.8f against %.8f, error %.2e\n",
                back->vol.raw(), truth.raw(), err);
    std::printf("        half a paise through vega is %.2e of vol\n",
                tick_band);
    check(err <= tick_band * 2.0,
          "the recovery error is within the tick's own vol width");

    // -----------------------------------------------------------------------
    // 2. The wings stop resolving, and that is the finding.
    // -----------------------------------------------------------------------
    std::printf("\n[2] where vega collapses, the vol is not measurable\n");
    std::printf("        moneyness      vega        +/- vol band\n");
    double atm_band = 0.0, wing_band = 0.0;
    for (const double mult : {1.00, 1.10, 1.20, 1.35, 1.50}) {
        const auto g = black76(OptionRight::Call,
                               Price{static_cast<std::int64_t>(F)},
                               Price{static_cast<std::int64_t>(F * mult)},
                               Years{T}, truth, rate);
        if (!g) {
            std::printf("        %9.2f    refused\n", mult);
            continue;
        }
        const double band = iv_uncertainty(g->vega, 0.5);
        std::printf("        %9.2f  %12.4f    %12.6f\n",
                    mult, g->vega / 100.0, band);
        if (mult == 1.00) { atm_band = band; }
        wing_band = band;
    }
    check(wing_band > atm_band * 10.0,
          "the far wing resolves vol at least ten times more coarsely than "
          "the money");

    // -----------------------------------------------------------------------
    // 3. SVI recovers parameters it was handed.
    // -----------------------------------------------------------------------
    std::printf("\n[3] SVI fit against a slice built from stated parameters\n");
    SviParams given;
    given.a = 0.04 * T;
    given.b = 0.4 * T;
    given.rho = -0.7;
    given.m = 0.0;
    given.sigma = 0.12;
    std::vector<SviFitPoint> pts;
    for (int i = -10; i <= 10; ++i) {
        SviFitPoint pt;
        pt.k = 0.03 * static_cast<double>(i);
        pt.w = svi_total_variance(given, pt.k);
        pt.weight = 1.0;
        pts.push_back(pt);
    }
    const auto fit = svi_fit(pts.data(), pts.size());
    check(fit.has_value(), "the fit converged");
    if (fit) {
        std::printf("        rmse %.3e  worst residual %.3e  iterations %d\n",
                    fit->rmse, fit->worst_residual, fit->iterations);
        // The slice is EXACT, so the residual should be at round-off. A loose
        // tolerance here would pass a fit that had found a different slice
        // with a similar shape.
        check(fit->rmse < 1e-8,
              "on an exact slice the weighted RMSE is at round-off");
        double worst_par = 0.0;
        worst_par = std::max(worst_par, std::fabs(fit->params.rho - given.rho));
        worst_par = std::max(worst_par,
                             std::fabs(fit->params.sigma - given.sigma));
        std::printf("        worst parameter error %.3e\n", worst_par);
        check(worst_par < 1e-4,
              "and the recovered parameters are the ones it was given");
        check(fit->butterfly.clean,
              "the fitted slice carries no butterfly arbitrage");
    }

    // -----------------------------------------------------------------------
    // 4. Pushed far enough, the scan DOES find arbitrage.
    // -----------------------------------------------------------------------
    std::printf("\n[4] the butterfly scan is not vacuous\n");
    double first_bad = -1.0;
    for (const double mul : {1.0, 2.0, 4.0, 8.0, 16.0, 32.0, 64.0}) {
        SviParams q = given;
        q.b = 0.4 * T * mul;
        const auto scan = svi_butterfly_scan(q, -0.6, 0.6, 401);
        if (!scan) { continue; }
        std::printf("        b %10.6f   min g %12.4g   %s\n",
                    q.b, scan->worst, scan->clean ? "clean" : "ARBITRAGE");
        if (!scan->clean && first_bad < 0.0) { first_bad = q.b; }
    }
    check(first_bad > 0.0,
          "a slope steep enough to price a negative density is detected");

    // -----------------------------------------------------------------------
    // 5. The rendered page, printed in full.
    // -----------------------------------------------------------------------
    std::printf("\n[5] the rendered page\n");
    const QString page = ui::options_report();
    check(page.contains(QStringLiteral("WHAT THIS IS NOT")),
          "the page states it is not a market measurement, before any number");
    check(page.contains(QStringLiteral("dataset/opt/")),
          "and names the data that would make it one");
    check(!page.contains(QStringLiteral("The fit was refused")),
          "the SVI section produced a fit rather than an apology");
    check(page.contains(QStringLiteral("ARBITRAGE")),
          "the arbitrage sweep found its boundary on the page too");

    std::printf("\n---------------- rendered page ----------------\n%s"
                "-----------------------------------------------\n",
                page.toUtf8().constData());

    std::printf("\n%s -- %d failing check(s)\n",
                failures == 0 ? "PASS" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
