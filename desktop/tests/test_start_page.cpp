// Tests for desktop/start_page.hpp -- Start here, in plain words.

#include "../start_page.hpp"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cstdio>

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("  %s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

void write(const QString& path, const QByteArray& text) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) f.write(text);
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    using namespace altair::ui;
    std::printf("Start here\n");
    QTemporaryDir root;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    // An unreadable or missing data/ reads as a halt (fail-safe): the app's data/ exists.
    QDir().mkpath(root.filePath(QStringLiteral("data")));
    const QString idle = start_now_html(root.path(), now);
    check(idle.contains(QStringLiteral("Prices: not running")) && idle.contains(QStringLiteral("Paper only"))
              && idle.contains(QStringLiteral("Models: not running")),
          "nothing running: says so, and that orders are paper");

    write(root.filePath(QStringLiteral("data/live/feed_status.json")), R"({"source":"fyers","state":"streaming"})");
    write(root.filePath(QStringLiteral("data/live/engine_state.json")),
          R"({"models":[{"name":"a","state":"in position"},{"name":"b","state":"abstaining"},{"name":"c","state":"flat"}]})");
    write(root.filePath(QStringLiteral("data/live_trading.json")),
          QStringLiteral(R"({"armed":true,"by":"smit","expires_unix":%1})").arg(now + 3600).toUtf8());
    const QString busy = start_now_html(root.path(), now);
    check(busy.contains(QStringLiteral("live from FYERS")), "the live feed is named");
    check(busy.contains(QStringLiteral("REAL ORDERS ARE ON")), "LIVE armed is said loudly");
    check(busy.contains(QStringLiteral("3 running on paper")) && busy.contains(QStringLiteral("1 holding a position"))
              && busy.contains(QStringLiteral("1 sitting out")),
          "the models: how many, holding, sitting out");

    write(root.filePath(QStringLiteral("data/kill_request.json")), R"({"requested_by":"smit","reason":"test"})");
    check(start_now_html(root.path(), now).contains(QStringLiteral("HALTED")), "a halt outranks LIVE");

    const auto v = start_verdicts();
    bool complete = !v.empty();
    for (const auto& x : v) complete = complete && !x.what.isEmpty() && !x.found.isEmpty() && !x.verdict.isEmpty() && !x.source.isEmpty();
    check(complete, "every verdict says what, what it made, what to do, and where the numbers come from");
    const QString table = start_verdicts_html(v);
    check(table.contains(QStringLiteral("t 1.5")) && table.contains(QStringLiteral("no change")),
          "the table carries the measured results in plain words");

    StartPage page(root.path());
    check(page.now_label() != nullptr && page.now_label()->text().contains(QStringLiteral("HALTED")), "the page shows the state");

    std::printf("%s\n", failures == 0 ? "all start page checks passed" : "start page checks did not pass");
    return failures == 0 ? 0 : 1;
}
