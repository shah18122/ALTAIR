#include "../model_job.hpp"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

void wait_until(const std::function<bool()>& done, int timeout_ms = 3000) {
    QEventLoop loop;
    QTimer poll;
    poll.setInterval(2);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (done()) loop.quit();
    });
    QTimer::singleShot(timeout_ms, &loop, &QEventLoop::quit);
    poll.start();
    loop.exec();
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QObject owner;
    altair::ui::ModelJobController jobs(&owner);

    std::atomic<bool> old_started{false};
    int completions = 0;
    QString published;
    QString provenance;
    int last_progress = -1;

    (void)jobs.submit(
        QStringLiteral("old selection"),
        [&](const altair::ui::ModelJobContext& context) {
            old_started = true;
            while (!context.cancelled()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return QStringLiteral("OLD RESULT");
        },
        {},
        [&](const altair::ui::ModelJobResult&) { ++completions; });
    wait_until([&] { return old_started.load(); });

    (void)jobs.submit(
        QStringLiteral("model=gbdt; symbol=nifty; interval=1d"),
        [](const altair::ui::ModelJobContext& context) {
            context.progress(40, QStringLiteral("Fitting"));
            return QStringLiteral("NEW RESULT");
        },
        [&](int value, const QString&) { last_progress = value; },
        [&](const altair::ui::ModelJobResult& result) {
            ++completions;
            published = result.output;
            provenance = result.provenance;
        });
    wait_until([&] { return completions == 1; });
    check(completions == 1 && published == QStringLiteral("NEW RESULT"),
          "a superseded job cannot publish over the newer generation");
    check(provenance.contains(QStringLiteral("symbol=nifty")),
          "the published result keeps exact provenance");
    check(last_progress == 100,
          "progress is delivered on the owner thread through completion");

    bool cancelled = false;
    (void)jobs.submit(
        QStringLiteral("cancel test"),
        [](const altair::ui::ModelJobContext& context) {
            while (!context.cancelled()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return QString{};
        },
        {},
        [&](const altair::ui::ModelJobResult& result) { cancelled = result.cancelled; });
    jobs.cancel_current();
    wait_until([&] { return cancelled; });
    check(cancelled, "cooperative cancellation is reported explicitly");

    QString error;
    (void)jobs.submit(
        QStringLiteral("error test"),
        [](const altair::ui::ModelJobContext&) -> QString {
            throw std::runtime_error("fit exploded");
        },
        {},
        [&](const altair::ui::ModelJobResult& result) { error = result.error; });
    wait_until([&] { return !error.isEmpty(); });
    check(error == QStringLiteral("fit exploded"),
          "worker exceptions become bounded user-visible errors");

    return failures == 0 ? 0 : 1;
}