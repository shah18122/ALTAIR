#include "../fyers_panel.hpp"

#include <QApplication>
#include <QDateTime>
#include <QFile>
#include <QPlainTextEdit>
#include <QTemporaryDir>

#include <cstdio>

namespace {
int failures = 0;

void check(bool ok, const char* description) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", description);
    if (!ok) ++failures;
}

bool write_fixture(const QString& path, bool complete) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const QByteArray body = QStringLiteral(R"({
  "schema_version": 1,
  "broker": "FYERS",
  "fetched_at_unix": %1,
  "account_id": "FY-TEST",
  "snapshot_complete": %2,
  "profile_status": 200,
  "profile_state": "present",
  "profile": {"s":"ok","fy_id":"FY-TEST"},
  "funds_status": 200,
  "funds_state": "present",
  "funds": {"s":"ok","fund_limit":[]},
  "positions_status": 200,
  "positions_state": "present",
  "positions": {"s":"ok","data":[]},
  "holdings_status": 200,
  "holdings_state": "present",
  "holdings": {"s":"ok","data":[]},
  "orders_status": 200,
  "orders_state": "%3",
  "orders": %4
})")
        .arg(now)
        .arg(complete ? QStringLiteral("true") : QStringLiteral("false"))
        .arg(complete ? QStringLiteral("present") : QStringLiteral("invalid"))
        .arg(complete ? QStringLiteral("{\"s\":\"ok\",\"data\":[]}")
                      : QStringLiteral("null"))
        .toUtf8();
    return file.write(body) == body.size();
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("fyers.json"));
    check(write_fixture(path, true), "write complete FYERS fixture");

    altair::ui::FyersAccountPanel panel(path);
    check(panel.snapshot_summary().contains(QStringLiteral("COMPLETE"))
              && panel.snapshot_summary().contains(QStringLiteral("FRESH")),
          "complete fresh FYERS snapshot is labelled explicitly");

    check(write_fixture(path, false), "write partial FYERS fixture");
    panel.reload_snapshot();
    check(panel.snapshot_summary().contains(QStringLiteral("PARTIAL")),
          "partial FYERS snapshot cannot appear complete");
    bool invalid_order_visible = false;
    for (auto* page : panel.findChildren<QPlainTextEdit*>()) {
        invalid_order_visible = invalid_order_visible
            || page->toPlainText().contains(QStringLiteral("invalid"));
    }
    check(invalid_order_visible,
          "semantic section failure remains visible even with HTTP 200");

    std::printf("FYERS panel: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}