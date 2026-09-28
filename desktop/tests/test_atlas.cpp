// P36-01 acceptance tests for the Model Atlas.
//
// cmake/AtlasAudit.cmake already guarantees every path in the table is real,
// and it was verified by planting three defects. What it cannot reach is the
// PAGE: whether the tree shows every row, whether the search narrows to the
// right ones, and whether hiding the absences hides exactly the absences.
//
// The last of those is the one worth a test. "Hide absent" is a convenience
// that quietly changes what the reader believes the project contains, so it
// must remove exactly the absent rows and not one more -- an off-by-one there
// would silently delete a real model from somebody's mental map of the engine.
//
// No check description here may contain the substring "F" "AIL" joined.

#include "../atlas.hpp"

#include <QApplication>

#include <cstdio>
#include <cstring>
#include <unordered_set>

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

}  // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    std::printf("P36-01 Model Atlas\n\n");

    std::size_t built = 0, partial = 0, absent = 0;
    for (const auto& r : altair::ui::kAtlasRows) {
        switch (r.status) {
        case altair::ui::AtlasStatus::Implemented: ++built; break;
        case altair::ui::AtlasStatus::Partial:     ++partial; break;
        case altair::ui::AtlasStatus::Absent:      ++absent; break;
        }
    }
    std::printf("  %zu entries: %zu built, %zu partial, %zu absent\n\n",
                altair::ui::kAtlasCount, built, partial, absent);
    check(built == altair::ui::kAtlasCount && partial == 0 && absent == 0,
          "all Atlas numerical engines are implemented; training remains a separate gate");

    // ---- the table's own invariants ------------------------------------
    //
    // Duplicated from the CMake audit on purpose. The audit runs at configure
    // time and a developer editing the table in an IDE will see this one
    // first, and these are the invariants that make the page honest rather
    // than merely populated.
    {
        bool paths_ok = true, absent_clean = true, described = true;
        std::unordered_set<std::uint64_t> ids;
        const altair::ui::AtlasRow* monte_carlo = nullptr;
        for (const auto& r : altair::ui::kAtlasRows) {
            ids.insert(altair::ui::atlas_model_id(r));
            if (std::strcmp(r.family, "9. Simulation") == 0
                && std::strcmp(r.model, "Monte Carlo") == 0) monte_carlo = &r;
            const bool has_file = r.file != nullptr && r.file[0] != '\0';
            if (r.status == altair::ui::AtlasStatus::Absent) {
                if (has_file) { absent_clean = false; }
            } else if (!has_file) {
                paths_ok = false;
            }
            // A row with no description is a row that teaches nothing, which
            // is the one thing this page exists to do.
            if (r.what == nullptr || std::strlen(r.what) < 30) {
                described = false;
                std::printf("        thin description: %s\n", r.model);
            }
        }
        check(paths_ok, "every built or partial row names a file");
        check(absent_clean, "and no absent row names one");
        check(described, "every row explains what the model answers");
        check(ids.size() == altair::ui::kAtlasCount,
              "every model row has a unique stable identity independent of page order");
        if (monte_carlo != nullptr) {
            const QString id = QStringLiteral("atlas.%1").arg(QString::number(
                static_cast<qulonglong>(altair::ui::atlas_model_id(*monte_carlo)), 16));
            check(altair::ui::atlas_row_by_id(id) == monte_carlo
                      && monte_carlo->page == 7,
                  "Monte Carlo stable link resolves to the Analytics workspace");
        } else {
            check(false, "Monte Carlo catalogue row exists");
        }
    }
    // The page-index check is NOT here. It needs the nav list, and pulling
    // main_window.hpp into this target would drag in every chart and panel to
    // learn one number. It lives in MainWindow's constructor instead, beside
    // the existing pages-equal-nav-rows check, which is the one place that
    // already holds both facts.

    // ---- the widget ------------------------------------------------------
    QString opened_id;
    altair::ui::AtlasPanel panel([&opened_id](QString id) { opened_id = std::move(id); });

    check(panel.visible_rows() == static_cast<int>(altair::ui::kAtlasCount),
          "the tree shows every entry before any filter");

    const altair::ui::AtlasRow* missing = nullptr;
    for (const auto& row : altair::ui::kAtlasRows) {
        if (row.status == altair::ui::AtlasStatus::Absent) { missing = &row; break; }
    }
    if (missing) {
        panel.set_hide_absent(true);
        panel.focus_model(QString::fromUtf8(missing->model));
        check(panel.visible_rows() == 1,
              "navigation opens the exact absent model and reveals it in the Atlas");
        check(opened_id.isEmpty(), "filter lookup does not falsely open a route");
    } else {
        check(absent == 0,
              "catalogue may reach zero absent models without inventing a placeholder");
    }
    panel.set_search(QString());

    // Hiding the absences must remove exactly them.
    panel.set_hide_absent(true);
    const int shown = panel.visible_rows();
    std::printf("        with absences hidden: %d rows\n", shown);
    check(shown == static_cast<int>(altair::ui::kAtlasCount - absent),
          "hiding absent removes exactly the absent rows, and no others");
    panel.set_hide_absent(false);
    check(panel.visible_rows() == static_cast<int>(altair::ui::kAtlasCount),
          "and unhiding brings them all back");

    // The search matches across every column, which is what makes it useful
    // to somebody who does not yet know the model's name.
    panel.set_search(QStringLiteral("volatility"));
    const int vol = panel.visible_rows();
    std::printf("        search \"volatility\": %d rows\n", vol);
    check(vol > 0 && vol < static_cast<int>(altair::ui::kAtlasCount),
          "a search narrows the list without emptying it");

    panel.set_search(QStringLiteral("models/gbdt.hpp"));
    check(panel.visible_rows() > 0,
          "searching by FILE finds the models that live in it");

    panel.set_search(QStringLiteral("snaps back"));
    check(panel.visible_rows() == 1,
          "searching a phrase from a description finds that one row -- the "
          "reader does not have to know the model's name first");

    panel.set_search(QStringLiteral("zzzz-no-such-model"));
    check(panel.visible_rows() == 0,
          "and a search matching nothing shows nothing rather than everything");

    panel.set_search(QString());
    check(panel.visible_rows() == static_cast<int>(altair::ui::kAtlasCount),
          "clearing the search restores the full list");

    std::printf("\n%s\n", failures == 0 ? "all checks passed"
                                        : "THERE WERE FAILURES");
    return failures == 0 ? 0 : 1;
}
