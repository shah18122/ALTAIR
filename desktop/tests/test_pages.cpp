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
#include <QTemporaryDir>
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
        {"Features",     features_report(ds)},
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
    // 4e. The registry must SEAL, and refuse to grow afterwards.
    //
    // A registry that can accept a feature after a model was fitted against it
    // is not a version, and `feature_version` is one of the five things rule
    // 10 requires beside every live decision.
    // -----------------------------------------------------------------------
    std::printf("\n[4e] the feature registry seals and then refuses\n");
    const QString fe = features_report(ds);
    check(fe.contains(QStringLiteral("SEALED")),
          "the registry seals and reports a feature_version");
    check(fe.contains(QStringLiteral("refused, as it must be")),
          "and refuses an add() after the seal");
    check(!fe.contains(QStringLiteral("ACCEPTED — which would be a bug")),
          "the post-seal add was not accepted");
    check(fe.contains(QStringLiteral("vel/se")),
          "the kinematics section reports velocity over its standard error, "
          "not a bare velocity");

    // -----------------------------------------------------------------------
    // 4f. The scorecard must never show an aggregate without its cells.
    //
    // flagging/scorecard.hpp exists because an IC of +0.30 in trending
    // regimes and -0.30 in ranging ones aggregates to about zero, and read as
    // one number that is a model with no edge rather than a model with a
    // switch. A page that printed the aggregate alone would be the exact
    // failure the header is written against.
    // -----------------------------------------------------------------------
    std::printf("\n[4f] the scorecard reports per regime, with error bars\n");
    const QString fl = flagging_report(ds);
    check(fl.contains(QStringLiteral("THE SCORECARD")),
          "the scorecard section is present");
    check(fl.contains(QStringLiteral("IC se")),
          "every IC is printed beside its own standard error");
    check(fl.contains(QStringLiteral("populated cells")),
          "and the page counts how many clear two standard errors");
    check(fl.contains(QStringLiteral("AND THE AGGREGATE, WITH WHAT IT HID")),
          "the aggregate is shown WITH the cell counts it was built from");
    check(fl.contains(QStringLiteral("thinnest")),
          "including the thinnest cell, because that is the second thing an "
          "aggregate hides");
    check(fl.contains(QStringLiteral("nothing here has traded")),
          "and it says plainly that this is a backtest, not a track record");

    // -----------------------------------------------------------------------
    // 4g. P30-03 -- the dataset inventory, against a directory built HERE.
    //
    // The first version of series_span read the first and last filename and
    // assumed name order was date order. Against dataset/spot/nifty/1m, which
    // holds nothing but YYYY-MM.csv, it was right. Against
    // dataset/spot/banknifty/1d, which holds all.csv AND vendor_pre2015.csv,
    // it reported the series as ending 2015-01-08 when all.csv runs to
    // 2026-09-08 -- because "vendor_pre2015.csv" sorts after "all.csv".
    //
    // That shape is reconstructed here rather than read off this box, for two
    // reasons: dataset/ is gitignored and regenerable, so a test that depends
    // on it passes or fails for reasons that have nothing to do with the code;
    // and the adversarial case has to be guaranteed present, not hoped for.
    // -----------------------------------------------------------------------
    std::printf("\n[4g] dataset inventory\n");
    {
        QTemporaryDir td;
        check(td.isValid(), "a temporary directory for the inventory case");

        const auto put = [&](const char* name, const QStringList& rows) {
            QFile f(td.filePath(QLatin1String(name)));
            const bool ok = f.open(QIODevice::WriteOnly | QIODevice::Text);
            if (!ok) { return; }
            QTextStream ts(&f);
            ts << "time,open,high,low,close,volume\n";
            for (const QString& r : rows) { ts << r << "\n"; }
        };

        // all.csv covers the modern history; the vendor file covers the old
        // history and sorts LAST. This is the exact shape that broke.
        put("all.csv", {QStringLiteral("2000-01-01,1,1,1,1,"),
                        QStringLiteral("2020-06-15,2,2,2,2,"),
                        QStringLiteral("2026-09-08,3,3,3,3,")});
        put("vendor_pre2015.csv", {QStringLiteral("1990-07-03,4,4,4,4,"),
                                   QStringLiteral("2015-01-08,5,5,5,5,")});

        const SeriesSpan sp = series_span(td.path());
        check(sp.files == 2, "both csv files are counted");
        check(sp.first == QStringLiteral("1990-07-03"),
              "first is the OLDEST stamp across every file -- it is in the "
              "file that sorts SECOND");
        check(sp.last == QStringLiteral("2026-09-08"),
              "and last is the NEWEST stamp across every file -- it is in the "
              "file that sorts FIRST, which is what the name-order version "
              "got wrong");

        // A header with no rows under it must contribute NOTHING. Without the
        // shape check, "time" sorts above every date and would win `last`
        // outright, pinning the whole series to a word.
        QTemporaryDir th;
        if (th.isValid()) {
            QFile f(th.filePath(QStringLiteral("empty.csv")));
            if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
                QTextStream ts(&f);
                ts << "time,open,high,low,close,volume\n";
            }
            f.close();
            const SeriesSpan e = series_span(th.path());
            check(e.files == 1, "a header-only file is still a file");
            check(e.first.isEmpty() && e.last.isEmpty(),
                  "but it reports NO span -- a header is not a bar, and the "
                  "word 'time' sorts above every date it would otherwise "
                  "beat outright");
        }

        // Absence is not zero here either: a directory with no csv at all is
        // reported as zero files, and the page skips it rather than drawing a
        // row of blanks that reads like a series with no data in it.
        QTemporaryDir tn;
        if (tn.isValid()) {
            const SeriesSpan n = series_span(tn.path());
            check(n.files == 0, "an empty directory is zero files, not a row");
        }

        const QString inv = dataset_inventory();
        // Printed for the same reason the page excerpts below are: a table
        // nobody reads is a table whose columns do not line up, and this one
        // is the first place a missing backfill shows.
        std::printf("%s", inv.toUtf8().constData());
        check(inv.contains(QStringLiteral("series")),
              "the inventory renders a header");
        check(inv.contains(QStringLiteral("gitignored and regenerable")),
              "and says what dataset/ is, so the page is not read as a "
              "manifest of things that must exist");
    }

    // -----------------------------------------------------------------------
    // 4h. P31-01 -- the parity and calendar scanners.
    //
    // Sections 1 and 2 price nothing and must ALWAYS compute: a convexity
    // margin is a property of three quotes and a calendar margin is a
    // difference of two total variances. Sections 3-5 price a bill and are
    // blocked where config/charges.toml has no loader, which is the `default`
    // preset -- so what is asserted here is that the page says WHICH, rather
    // than that a particular number appears.
    // -----------------------------------------------------------------------
    std::printf("\n[4h] parity and calendar\n");
    {
        const QString ar = arbitrage_scans_report();
        check(ar.contains(QStringLiteral("There is no option chain")),
              "the page says there is no chain before it shows any number");
        check(ar.contains(QStringLiteral("EVEN LADDER")),
              "the butterfly section is present");
        check(ar.contains(QStringLiteral("FREE MONEY")),
              "and it reports the fictional arbitrage the textbook form finds "
              "on an arbitrage-free chain, which is the whole card");
        check(ar.contains(QStringLiteral("DO NOT SHARE A FORWARD")),
              "the calendar section is present");
        check(ar.contains(QStringLiteral("VIOLATIONS HIDDEN")),
              "and it reports violations HIDDEN by the strike-aligned scan -- "
              "concealing, not inventing, which is the worse failure");
        check(!ar.contains(QStringLiteral("WHICH IS A BUG")),
              "no refusal that the page checks has stopped refusing");

        // Exactly one of the two branches, never both and never neither.
        //
        // The marker is a whole phrase that appears nowhere else. The first
        // version looked for the word "BLOCKED", which a later edit then
        // introduced into the short-cash line three paragraphs away, and the
        // check went red for a reason that had nothing to do with it. A
        // sentinel unrelated prose can switch on is not a sentinel.
        const bool costed = ar.contains(QStringLiteral("implied carry"));
        const bool blocked =
            ar.contains(QStringLiteral("COSTED SCANNERS UNAVAILABLE"));
        check(costed != blocked,
              costed ? "charges.toml loaded, so the costed scanners computed"
                     : "no charges.toml loader in this build, and the page "
                       "names that blocker instead of costing against a "
                       "literal -- rule 5 is what everything downstream "
                       "trusts");
        if (costed) {
            // The short-cash asymmetry only means something when a REVERSAL
            // is the chosen side; on a fair chain the conversion wins and the
            // branch is never taken. The page quotes the hedge rich to reach
            // it, and this is the assertion that it did.
            check(ar.contains(QStringLiteral("reversal, BLOCKED")),
                  "a spot-hedged REVERSAL is blocked for want of short cash, "
                  "which is P5-05's asymmetry reaching the option book");
            check(!ar.contains(QStringLiteral("no reversal reached")),
                  "and the quote used to show it really does select a "
                  "reversal -- otherwise the branch under test is never taken "
                  "and the line means nothing");
        }
        std::printf("%s", ar.toUtf8().constData());
    }

    // -----------------------------------------------------------------------
    // 4i. P32-03 -- the aggregator, on members that exist.
    //
    // The page it replaced was a BLOCKED page whose text said there was
    // nothing to combine. What is asserted here is that there IS: that six
    // members were built from real walk-forward runs, that the two malformed
    // ensembles are refused, and that the correlation sweep is present rather
    // than a single flattering rho.
    // -----------------------------------------------------------------------
    std::printf("\n[4i] aggregator\n");
    {
        const QString ag = aggregator_report(
            ds, QStringLiteral("/spot/nifty/1d/"), "NIFTY daily");
        check(!ag.contains(QStringLiteral("NO MEMBERS")),
              "the aggregator has members to combine -- the page it replaced "
              "said it did not, and that stopped being true at P16");
        check(ag.contains(QStringLiteral("level-wise")),
              "and they are named, with their own error bars");
        check(!ag.contains(QStringLiteral("WHICH IS A BUG")),
              "a mismatched horizon and a mismatched feature registry are "
              "both still REFUSED -- combining across horizons estimates "
              "neither quantity");
        check(ag.contains(QStringLiteral("n_eff")),
              "the correlation sweep is shown rather than one rho that "
              "suits the answer");
        std::printf("%s", ag.toUtf8().constData());
    }

    // -----------------------------------------------------------------------
    // 4j. P32-04 -- the instrument selector actually selects.
    //
    // A combo box that changes a label and not the data is the worst outcome
    // here: the page would report NIFTY numbers under a BankNifty heading and
    // there is nothing on screen to catch it. So this asserts the reports
    // DIFFER between instruments, which they cannot do unless a different
    // directory was read.
    // -----------------------------------------------------------------------
    std::printf("\n[4j] the instrument selector\n");
    {
        const QString nif = QStringLiteral("nifty");
        const QString bnf = QStringLiteral("banknifty");
        const QString vix = QStringLiteral("indiavix");

        const QString v_n = volatility_report(ds, nif);
        const QString v_b = volatility_report(ds, bnf);
        const QString v_v = volatility_report(ds, vix);
        check(v_n != v_b,
              "volatility on BankNifty is not the NIFTY page with a different "
              "title -- the numbers move, so a different directory was read");
        check(v_b.contains(QStringLiteral("NIFTY BANK")),
              "and the page names the instrument in its own text, not only in "
              "the combo box -- text gets copied and screenshotted");
        check(v_v.contains(QStringLiteral("NOT A TRADEABLE SERIES")),
              "India VIX carries its caveat: it is an index of implied vol, "
              "so a figure net of trading cost is net of a cost nobody can "
              "pay");

        const QString m_n = memory_report(ds, nif);
        const QString m_b = memory_report(ds, bnf);
        check(m_n != m_b, "the memory page moves with the instrument too");

        const QString r_n = regime_report(ds, nif);
        const QString r_b = regime_report(ds, bnf);
        check(r_n != r_b, "and so does the regime page");

        check(!v_b.contains(QStringLiteral("dataset/spot/nifty")),
              "a BankNifty page that cannot find its data names the BANKNIFTY "
              "path -- pointing the reader at a nifty directory that is fine "
              "is worse than saying nothing");
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
