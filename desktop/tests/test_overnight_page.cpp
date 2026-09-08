// P22-04 acceptance tests for the Overnight Gap page.
//
// WHY A PAGE NEEDS A TEST OF ITS OWN.
//
// `quant_pages.hpp` claims every figure it shows is produced by the same
// function the acceptance test calls. For most pages that claim is structural:
// the page hands the model layer a series and prints what comes back. This one
// is different, because it has its OWN READER -- `ui_load_sessions` -- and a
// reader is where a page can silently start answering a different question.
//
// Specifically: `SessionBar::open` must be the first bar's OPEN, the pre-open
// auction print, and `first_bar_close` must be that bar's CLOSE. Those two are
// five minutes apart and on this history they differ by 4.07 bps a day, which
// is 38% of the whole strategy. A reader that took the close for both would
// still produce a plausible page, a positive number, and a wrong answer.
//
// So test 2 compares the UI reader against `strategies/`' own reader, column by
// column, on the same files. Two independent paths to the same number is what
// makes it a property of the data rather than of one parser.
//
// No check description here may contain the substring FAIL.

#include "../quant_pages.hpp"

#include <strategies/overnight.hpp>

#include <QCoreApplication>
#include <QString>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

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

/// An INDEPENDENT reader, written against the same files but not sharing a
/// line of code with `ui_load_sessions`. That independence is the point; a
/// shared helper would make test 2 compare a function with itself.
std::vector<altair::SessionBar> reference_sessions()
{
    const std::filesystem::path dir =
        std::filesystem::path(ALTAIR_DATASET_DIR) / "spot" / "nifty" / "5m";
    std::vector<altair::SessionBar> out;
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) { return out; }
    std::vector<std::filesystem::path> files;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (e.path().extension() == ".csv") { files.push_back(e.path()); }
    }
    std::sort(files.begin(), files.end());

    std::string current;
    for (const auto& f : files) {
        std::ifstream in(f);
        if (!in) { continue; }
        std::string line;
        std::getline(in, line);
        while (std::getline(in, line)) {
            std::vector<std::string> cols;
            std::istringstream ss(line);
            std::string cell;
            while (std::getline(ss, cell, ',')) { cols.push_back(cell); }
            if (cols.size() < 5) { continue; }
            if (cols[0].size() < 10 || cols[1].empty() || cols[4].empty()) {
                continue;
            }
            const std::string day = cols[0].substr(0, 10);
            const double o = std::stod(cols[1]);
            const double c = std::stod(cols[4]);
            if (!(o > 0.0) || !(c > 0.0)) { continue; }
            if (day != current) {
                current = day;
                out.push_back({o, c, c});
            } else {
                out.back().close = c;
            }
        }
    }
    return out;
}

} // namespace

using namespace altair;

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    std::printf("P22-04 -- the Overnight Gap page\n");

    const QString root = QString::fromUtf8(ALTAIR_DATASET_DIR);

    // -----------------------------------------------------------------------
    // 1. The page's own reader loads something.
    // -----------------------------------------------------------------------
    std::printf("\n[1] the page reader\n");
    const auto ui = ui::ui_load_sessions(root + QStringLiteral("/spot/nifty/5m"));
    std::printf("        ui_load_sessions: %zu sessions\n", ui.size());
    check(ui.size() > 2000, "the page reader found the five-minute partition");
    if (ui.size() <= 2000) {
        std::printf("\nFAILED -- dataset incomplete\n");
        return 1;
    }

    // -----------------------------------------------------------------------
    // 2. It agrees with an independent reader, COLUMN BY COLUMN.
    // -----------------------------------------------------------------------
    std::printf("\n[2] against an independent reader of the same files\n");
    const auto ref = reference_sessions();
    std::printf("        reference reader: %zu sessions\n", ref.size());
    check(ui.size() == ref.size(), "both readers found the same sessions");
    if (ui.size() != ref.size()) { return 1; }

    std::size_t open_diff = 0, close_diff = 0, first_diff = 0;
    for (std::size_t i = 0; i < ui.size(); ++i) {
        if (ui[i].open != ref[i].open) { ++open_diff; }
        if (ui[i].close != ref[i].close) { ++close_diff; }
        if (ui[i].first_bar_close != ref[i].first_bar_close) { ++first_diff; }
    }
    std::printf("        differing: open %zu, close %zu, first_bar %zu\n",
                open_diff, close_diff, first_diff);
    check(open_diff == 0 && close_diff == 0 && first_diff == 0,
          "every price in every session matches, in all three columns");

    // THE BUG THIS FILE EXISTS FOR. If `open` were read from the close column,
    // the two would be equal on every session and the page would quietly
    // report the 09:20 number under a heading that says 09:15.
    std::size_t same = 0;
    for (const SessionBar& b : ui) {
        if (b.open == b.first_bar_close) { ++same; }
    }
    std::printf("        sessions where open == first_bar_close: %zu of %zu\n",
                same, ui.size());
    check(same < ui.size() / 10,
          "the open is the auction print, not a copy of the first bar's close");

    // -----------------------------------------------------------------------
    // 3. The decomposition the page prints satisfies the identity.
    // -----------------------------------------------------------------------
    std::printf("\n[3] the identity, through the page's data\n");
    const auto r = session_returns(ui);
    check(r.has_value(), "the decomposition was produced");
    if (!r) { return 1; }
    double worst = 0.0;
    for (std::size_t i = 0; i < r->overnight.size(); ++i) {
        worst = std::max(worst, std::fabs(r->overnight[i] + r->session[i]
                                          - r->close_to_close[i]));
    }
    std::printf("        worst residual %.3e bps\n", worst);
    check(worst < 1e-9, "overnight + session == close-to-close, exactly");

    // -----------------------------------------------------------------------
    // 4. The rendered page contains the numbers, not a placeholder.
    // -----------------------------------------------------------------------
    std::printf("\n[4] the rendered report\n");
    const QString page = ui::overnight_report(root);
    check(!page.contains(QStringLiteral("NO DATA")),
          "the page found its data rather than reporting absence");
    check(page.contains(QStringLiteral("09:15 auction"))
              && page.contains(QStringLiteral("09:20 first bar")),
          "both execution assumptions are shown, never just the flattering one");
    check(page.contains(QStringLiteral("READ THIS FIRST")),
          "the execution caveat is on the page, above the numbers");

    // The page's headline number must be the strategy layer's, to the digit
    // the page prints. This is the claim quant_pages.hpp makes about every
    // page, checked here for the one that has its own reader.
    const auto res = run_overnight(ui, OvernightSpec{});
    check(res.has_value(), "the strategy layer ran on the page's data");
    if (!res) { return 1; }
    const QString printed =
        QStringLiteral("%1").arg(res->mean_taken_bps, 8, 'f', 4);
    std::printf("        strategy layer mean: %s\n",
                printed.trimmed().toUtf8().constData());
    check(page.contains(printed.trimmed()),
          "and that exact figure appears in the rendered page");

    // A page that reported a positive edge without the break-even beside it
    // would be the most misleading thing in this UI.
    check(page.contains(QStringLiteral("b/e vs hold")),
          "the break-even against buy-and-hold is shown beside the return");

    std::printf("\n%s -- %d failing check(s)\n",
                failures == 0 ? "PASS" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
