#include "desktop/helper_process.hpp"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>

#include <cstdlib>
#include <iostream>
#include <optional>

namespace {

bool check(bool condition, const char* message) {
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}

std::optional<altair::ui::HelperProcessResult>
run(altair::ui::HelperProcess& helper, const QStringList& args,
    int timeout_ms, QByteArray standard_input = {}) {
    QEventLoop loop;
    std::optional<altair::ui::HelperProcessResult> result;
    const auto generation = helper.start(
        QStringLiteral(ALTAIR_HELPER_PROCESS_CHILD), args, {}, timeout_ms,
        std::move(standard_input),
        [&](altair::ui::HelperProcessResult finished) {
            result = std::move(finished);
            loop.quit();
        });
    if (!generation) return std::nullopt;
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, &loop, &QEventLoop::quit);
    watchdog.start(5000);
    loop.exec();
    return result;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    using altair::ui::HelperProcess;
    using altair::ui::HelperProcessError;
    bool ok = true;
    HelperProcess helper;

    const auto completed = run(helper, {QStringLiteral("--ok")}, 2000);
    ok &= check(completed.has_value(), "normal helper returned no result");
    if (completed) {
        ok &= check(completed->ran_to_completion(),
                    "normal helper reported infrastructure failure");
        ok &= check(completed->exit_code == 7, "exit code was not preserved");
        ok &= check(completed->output.contains(QStringLiteral("child output")),
                    "merged output was not captured");
        ok &= check(completed->generation == 1,
                    "first generation was not one");
    }

    std::optional<altair::ui::HelperProcessResult> timed_out;
    QEventLoop timeout_loop;
    const auto timeout_generation = helper.start(
        QStringLiteral(ALTAIR_HELPER_PROCESS_CHILD),
        {QStringLiteral("--slow")}, {}, 20,
        [&](altair::ui::HelperProcessResult result) {
            timed_out = std::move(result);
            timeout_loop.quit();
        });
    ok &= check(timeout_generation.has_value(), "timeout helper did not start");
    ok &= check(!helper.start(QStringLiteral("unused"), {}, {}, 1000,
                              [](auto) {}).has_value(),
                "a second concurrent helper was accepted");
    QTimer::singleShot(5000, &timeout_loop, &QEventLoop::quit);
    timeout_loop.exec();
    ok &= check(timed_out.has_value(), "timeout produced no completion");
    if (timed_out) {
        ok &= check(timed_out->error == HelperProcessError::TimedOut,
                    "timeout was not classified");
        ok &= check(timed_out->generation == 2,
                    "generation did not advance");
    }
    ok &= check(!helper.active(), "runner remained active after timeout");

    const auto piped = run(
        helper, {QStringLiteral("--stdin")}, 2000,
        QByteArrayLiteral("sensitive-value\n"));
    ok &= check(piped.has_value(), "stdin helper returned no result");
    if (piped) {
        ok &= check(piped->ran_to_completion() && piped->exit_code == 0,
                    "sensitive input appeared in the child argument list");
        ok &= check(piped->output.contains(QStringLiteral("stdin-bytes=15")),
                    "sensitive input was not delivered over stdin");
    }

    const auto oversized = run(helper, {QStringLiteral("--large")}, 2000);
    ok &= check(oversized.has_value(), "oversized helper returned no result");
    if (oversized) {
        ok &= check(oversized->error == HelperProcessError::OutputLimitExceeded,
                    "oversized output was not classified");
        ok &= check(oversized->output.toUtf8().size()
                        == HelperProcess::kMaxOutputBytes,
                    "oversized output was not truncated at exactly 256 KiB");
    }

    std::cout << (ok ? "helper process: ok\n" : "helper process: FAILED\n");
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
