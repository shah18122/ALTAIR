// Standalone READ-ONLY intraday audit. No broker or engine linkage.
#include "bar_consistency_io.hpp"
#include <QCoreApplication>
#include <QJsonDocument>
#include <cstdio>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    QMap<QString, QString> values;
    bool round_subpaise = false;
    bool zero_volume_absent = false;
    QString error;
    for (qsizetype i = 1; i < args.size(); ++i) {
        const QString option = args[i];
        if (option == "--help") {
            std::puts("Read-only Altair OHLCV consistency audit.\n"
                "--base FILE_OR_DIRECTORY --target FILE_OR_DIRECTORY\n"
                "--base-minutes N --target-minutes M [--round-subpaise] [--zero-volume-absent]\n"
                "Requires M>N, M%N=0, M<=375. Interval-open timestamps +05:30, session 09:15-15:30.\n"
                "Output JSON to stdout. Exit 0 consistent, 1 invalid/incomplete/mismatch, 2 usage error.\n"
                "No source repair; no calendar/provider/contract equivalence certification.");
            return 0;
        }
        if (option == "--round-subpaise" || option == "--zero-volume-absent") {
            bool& flag = option == "--round-subpaise" ? round_subpaise : zero_volume_absent;
            if (flag) { error = "Repeated option: " + option; break; }
            flag = true;
        } else if (option == "--base" || option == "--target" ||
                   option == "--base-minutes" || option == "--target-minutes") {
            if (values.contains(option) || i + 1 >= args.size() || args[i + 1].startsWith("--")) {
                error = "Missing value or repeated option: " + option;
                break;
            }
            values.insert(option, args[++i]);
        } else {
            error = "Unknown option: " + option;
            break;
        }
    }
    bool base_ok = false;
    bool target_ok = false;
    const int base_minutes = values.value("--base-minutes").toInt(&base_ok);
    const int target_minutes = values.value("--target-minutes").toInt(&target_ok);
    if (error.isEmpty() && (!base_ok || !target_ok ||
        !altair::ui::barcheck::detail::durations_ok(base_minutes, target_minutes) ||
        values.value("--base").isEmpty() || values.value("--target").isEmpty()))
        error = "Require --base, --target and durations 0 < base < target <= 375 with target % base == 0";

    altair::ui::barcheck::Report report;
    if (!error.isEmpty()) {
        report.issue("usage", error);
    } else {
        const auto base = altair::ui::barcheck::load(values["--base"], base_minutes,
            round_subpaise, zero_volume_absent, report);
        const auto target = altair::ui::barcheck::load(values["--target"], target_minutes,
            round_subpaise, zero_volume_absent, report);
        altair::ui::barcheck::compare(base, target, base_minutes, target_minutes, report);
    }
    QJsonObject json = report.json();
    json.insert("base", values.value("--base"));
    json.insert("target", values.value("--target"));
    json.insert("base_minutes", base_minutes);
    json.insert("target_minutes", target_minutes);
    json.insert("round_subpaise", round_subpaise);
    json.insert("zero_volume_absent", zero_volume_absent);
    const QByteArray output = QJsonDocument(json).toJson(QJsonDocument::Indented);
    if (std::fwrite(output.constData(), 1, static_cast<std::size_t>(output.size()), stdout) !=
        static_cast<std::size_t>(output.size()) || std::fflush(stdout) != 0) return 2;
    return !error.isEmpty() ? 2 : report.ok() ? 0 : 1;
}
