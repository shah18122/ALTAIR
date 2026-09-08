// P23-05 -- every compute page renders, and none of them quietly gave up.
//
// WHAT THIS CATCHES THAT NOTHING ELSE DOES.
//
// Each page here has a fallback for missing data: "No dataset", "NO DATA",
// "Missing dataset/...". Those exist because absence is not zero and a page
// must say when it has nothing rather than draw a plausible number. But that
// same fallback is invisible in a screenshot nobody takes, and a path typo, a
// renamed directory or a partition that moved would leave a page permanently
// apologising while every other test stayed green.
//
// So this asserts the opposite of what a fallback test usually asserts: that
// the pages which SHOULD have data are NOT showing their fallback. A page
// legitimately without data -- Options has no chain, and says so -- is listed
// separately with the reason, so the list itself has to be edited when the
// situation changes.
//
// It also prints each page's first lines, because a page nobody has read is a
// page whose columns do not line up.
//
// No check description here may contain the substring FAIL.

#include "../quant_pages.hpp"

#include <QCoreApplication>
#include <QString>
#include <QStringList>

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

/// The strings a page prints when it has nothing. Kept in one place so a new
/// fallback added to a page without adding it here is the only way this test
/// can be fooled -- and that is a visible edit in the same file.
const char* const kGaveUp[] = {
    "No dataset", "NO DATA", "Missing dataset", "Too few aligned",
    "fit failed", "The run failed", "The decomposition failed",
};

bool gave_up(const QString& page)
{
    for (const char* m : kGaveUp) {
        if (page.contains(QString::fromUtf8(m))) { return true; }
    }
    return false;
}

void head(const char* name, const QString& page, int lines)
{
    std::printf("\n--- %s (%d chars) ---\n", name, static_cast<int>(page.size()));
    const QStringList ls = page.split(QChar('\n'));
    for (int i = 0; i < lines && i < ls.size(); ++i) {
        std::printf("  %s\n", ls[i].toUtf8().constData());
    }
    if (ls.size() > lines) {
        std::printf("  ... %d more lines\n",
                    static_cast<int>(ls.size()) - lines);
    }
}

} // namespace

using namespace altair;
using namespace altair::ui;

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    std::printf("P23-05 -- every compute page renders\n");
    const QString ds = QString::fromUtf8(ALTAIR_DATASET_DIR);

    // -----------------------------------------------------------------------
    // 1. Pages that need no data at all. They must always render.
    // -----------------------------------------------------------------------
    std::printf("\n[1] pages that compute from arithmetic alone\n");
    struct Free { const char* name; QString page; };
    const Free free_pages[] = {
        {"Execution",  execution_report()},
        {"Portfolio",  portfolio_report()},
        {"ML — Trees", ml_report()},
        {"Options",    options_report()},
        {"Microstructure", microstructure_report()},
        {"Value - DCF",  dcf_report()},
        {"Sizing and Limits", sizing_report()},
    };
    for (const Free& f : free_pages) {
        check(f.page.size() > 200, f.name);
    }

    // -----------------------------------------------------------------------
    // 2. Pages backed by dataset/. These must NOT be showing a fallback.
    // -----------------------------------------------------------------------
    std::printf("\n[2] pages backed by dataset/ — none may be apologising\n");
    struct Backed { const char* name; QString page; };
    const Backed backed[] = {
        {"Volatility",   volatility_report(ds)},
        {"Risk — VaR",   risk_report(ds)},
        {"Regimes",      regime_report(ds)},
        {"Strategies",   strategies_report(ds)},
        {"Overnight",    overnight_report(ds)},
        {"Basis",        basis_report(ds)},
        {"Flagging",     flagging_report(ds)},
        {"Cointegration", cointegration_report(ds)},
        {"Memory",       memory_report(ds)},
    };
    for (const Backed& b : backed) {
        const bool ok = b.page.size() > 200 && !gave_up(b.page);
        if (!ok) {
            std::printf("        %s produced %d chars%s\n", b.name,
                        static_cast<int>(b.page.size()),
                        gave_up(b.page) ? " and hit its no-data fallback" : "");
        }
        check(ok, b.name);
    }

    // -----------------------------------------------------------------------
    // 3. Options is ALLOWED to say it has no data, and must say it.
    // -----------------------------------------------------------------------
    std::printf("\n[3] the one page that legitimately has no data\n");
    const QString opt = options_report();
    check(opt.contains(QStringLiteral("There is no option chain")),
          "Options states plainly that no chain is subscribed");
    check(opt.contains(QStringLiteral("dataset/opt/")),
          "and names the directory that would change that");

    // -----------------------------------------------------------------------
    // 4. The Regimes page computes its clustering rather than describing it.
    // -----------------------------------------------------------------------
    std::printf("\n[4] Regimes no longer reports P18 as prose alone\n");
    const QString reg = regime_report(ds);
    check(reg.contains(QStringLiteral("k-MEANS REGIMES")),
          "the k-means section is present");
    check(reg.contains(QStringLiteral("separation"))
              && reg.contains(QStringLiteral("persistence")),
          "with separation and persistence computed on press");

    // -----------------------------------------------------------------------
    // 4b. The cointegration page's two verdicts must DISAGREE.
    //
    // Spot against its own future is tied by arbitrage and must come back
    // cointegrated; spot against an independent random walk must not. A test
    // that returns the same answer for both is not a test, and it would be
    // invisible in an excerpt that only prints the first fourteen lines.
    // -----------------------------------------------------------------------
    std::printf("\n[4b] cointegration: a known pair and a known non-pair\n");
    const QString ci = cointegration_report(ds);
    const bool real_ok =
        ci.contains(QStringLiteral("COINTEGRATED, as arbitrage requires"));
    const bool noise_ok =
        ci.contains(QStringLiteral("NOT cointegrated, correctly"));
    std::printf("        futures/spot cointegrated : %s\n",
                real_ok ? "yes" : "NO");
    std::printf("        spot/noise   cointegrated : %s\n",
                noise_ok ? "no" : "YES");
    check(real_ok,
          "the future and its own spot come back cointegrated");
    check(noise_ok,
          "and an independent random walk does not, so the test discriminates");

    // -----------------------------------------------------------------------
    // 4c. The DCF page must lead with what it is not.
    //
    // It was a blocked_page until P25-02 because no filings are ingested.
    // That is still true, and a valuation page that stopped saying so would be
    // the most misleading thing in this UI -- the numbers look exactly like
    // fetched ones.
    // -----------------------------------------------------------------------
    std::printf("\n[4c] the DCF page states its inputs are typed, not fetched\n");
    const QString dc = dcf_report();
    check(dc.contains(QStringLiteral("STATED, NOT FETCHED")),
          "the page says the inputs were typed before showing a valuation");
    check(dc.contains(QStringLiteral("TERMINAL SHARE")),
          "and reports how much of the value is a perpetuity assumption");
    check(dc.contains(QStringLiteral("fundamentals.hpp")),
          "and names what would make it real");
    // The refused cells must be shown as refused, never as zero.
    check(dc.contains(QStringLiteral("cells refused")),
          "the sensitivity grid reports refused cells rather than zero-filling");

    // -----------------------------------------------------------------------
    // 4d. NO PAGE MAY CONTAIN "%%".
    //
    // QStringLiteral is not printf. `%%` is not an escape there -- `arg()`
    // substitutes %1..%99 and leaves a bare % alone -- so a printf habit
    // renders literally as "13.2%%" on screen. This has now been introduced
    // and swept four separate times in this UI, which is three times too many
    // for a fix that only ever addressed the instance.
    //
    // Asserting on the CLASS is the fix. A page that reintroduces it fails
    // here rather than shipping a stutter nobody reports.
    // -----------------------------------------------------------------------
    std::printf("\n[4d] no page renders a printf-style double percent\n");
    {
        std::size_t offenders = 0;
        for (const Free& f : free_pages) {
            if (f.page.contains(QStringLiteral("%%"))) {
                ++offenders;
                std::printf("        %s contains %%%%\n", f.name);
            }
        }
        for (const Backed& b : backed) {
            if (b.page.contains(QStringLiteral("%%"))) {
                ++offenders;
                std::printf("        %s contains %%%%\n", b.name);
            }
        }
        check(offenders == 0,
              "no rendered page contains a literal double percent");
    }

    // -----------------------------------------------------------------------
    // 5. Print them.
    // -----------------------------------------------------------------------
    std::printf("\n[5] excerpts\n");
    for (const Backed& b : backed) { head(b.name, b.page, 14); }

    std::printf("\n%s -- %d failing check(s)\n",
                failures == 0 ? "PASS" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
