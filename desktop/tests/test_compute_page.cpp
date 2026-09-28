#include "../quant_pages.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QTableWidget>
#include <QThread>

#include <cstdio>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

QString summary_value(QTableWidget* table, const QString& field) {
    if (table == nullptr) return {};
    for (int row = 0; row < table->rowCount(); ++row) {
        if (table->item(row, 0) != nullptr
            && table->item(row, 0)->text() == field
            && table->item(row, 1) != nullptr) {
            return table->item(row, 1)->text();
        }
    }
    return {};
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    altair::ui::ComputePage page(QStringLiteral("TEST MODEL"),
                                 QStringLiteral("Run"), nullptr);
    page.run_async(
        QStringLiteral("family=risk; model=var; symbol=nifty; interval=1d"),
        [](const altair::ui::ModelJobContext& context) {
            context.progress(25, QStringLiteral("Fitting"));
            altair::ui::ModelJobPayload result{QStringLiteral(
                "RISK RESULT\n"
                "Sample: 2,873 sessions / daily bars\n"
                "95% confidence interval: [-1.2%, -0.8%]\n"
                "Net of cost: -0.9% after round-trip cost\n"
                "VaR: 1.0%\n")};
            result.fields = {
                {QStringLiteral("Sample window"), QStringLiteral("2,873 sessions"), QStringLiteral("daily bars")},
                {QStringLiteral("Uncertainty"), QStringLiteral("[-1.2%, -0.8%]"), QStringLiteral("95% confidence interval")},
                {QStringLiteral("Cost-adjusted validation"), QStringLiteral("-0.9%"), QStringLiteral("after round-trip cost")},
                {QStringLiteral("Missing data"), QStringLiteral("None"), QStringLiteral("validated input")},
            };
            result.rows = {
                {QStringLiteral("Risk"), QStringLiteral("VaR = 1.0%")},
                {QStringLiteral("Risk"), QStringLiteral("Net result = -0.9%")},
                {QStringLiteral("Validation"), QStringLiteral("95% interval = [-1.2%, -0.8%]")},
                {QStringLiteral("Validation"), QStringLiteral("2,873 sessions")},
            };
            return result;
        });

    QElapsedTimer timer;
    timer.start();
    auto* summary = page.findChild<QTableWidget*>(QStringLiteral("modelResultSummary"));
    auto* results = page.findChild<QTableWidget*>(QStringLiteral("modelResultTable"));
    while (timer.elapsed() < 3000
           && summary_value(summary, QStringLiteral("Input: symbol")).isEmpty()) {
        app.processEvents();
        QThread::msleep(1);
    }

    check(summary != nullptr && results != nullptr,
          "the compute page exposes typed contract and result tables");
    check(summary_value(summary, QStringLiteral("Input: symbol")) == QStringLiteral("nifty"),
          "run inputs come from the exact worker provenance");
    check(summary_value(summary, QStringLiteral("Sample window")).contains(QStringLiteral("2,873")),
          "the sample window is explicit");
    check(summary_value(summary, QStringLiteral("Uncertainty")).contains(QStringLiteral("-1.2%")),
          "uncertainty is explicit");
    check(summary_value(summary, QStringLiteral("Cost-adjusted validation")) == QStringLiteral("-0.9%"),
          "cost-adjusted validation is explicit");
    check(summary_value(summary, QStringLiteral("Missing data")) == QStringLiteral("None"),
          "missing-data state is carried by the typed result contract");
    check(results != nullptr && results->rowCount() >= 4,
          "real report output is presented as structured result rows");

    return failures == 0 ? 0 : 1;
}