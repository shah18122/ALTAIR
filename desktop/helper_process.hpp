// desktop/helper_process.hpp -- bounded asynchronous subprocess execution.
//
// Broker OAuth and dataset tools deliberately live outside the desktop
// process.  This adapter preserves that boundary without ever waiting on the
// GUI thread.  A generation is attached to every run so callers can reject a
// late result after their own state has changed.

#pragma once

#include <QByteArray>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace altair::ui {

enum class HelperProcessError : std::uint8_t {
    None,
    StartFailed,
    TimedOut,
    Crashed,
    OutputLimitExceeded,
    Cancelled,
};

struct HelperProcessResult final {
    std::uint64_t generation = 0;
    int exit_code = -1;
    QString output;
    QString detail;
    HelperProcessError error = HelperProcessError::None;

    [[nodiscard]] bool ran_to_completion() const noexcept {
        return error == HelperProcessError::None;
    }
};

class HelperProcess final {
public:
    using Completion = std::function<void(HelperProcessResult)>;
    static constexpr qsizetype kMaxOutputBytes = 256 * 1024;

    HelperProcess() {
        timer_.setSingleShot(true);
        QObject::connect(&timer_, &QTimer::timeout, [&] {
            if (!active_ || !process_) return;
            const std::uint64_t run = generation_;
            forced_error_ = HelperProcessError::TimedOut;
            if (process_->state() == QProcess::NotRunning) {
                finish(run, QStringLiteral("helper process timed out"));
            } else {
                process_->kill();
            }
        });
    }

    HelperProcess(const HelperProcess&) = delete;
    HelperProcess& operator=(const HelperProcess&) = delete;

    ~HelperProcess() {
        // QProcess also terminates a live child in its destructor.  Do it
        // explicitly after suppressing callbacks so a panel being destroyed
        // can never receive a completion through a dangling capture.
        active_ = false;
        completion_ = {};
        wipe_input();
        timer_.stop();
        if (process_) {
            process_->disconnect();
            if (process_->state() != QProcess::NotRunning) process_->kill();
        }
    }

    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] std::uint64_t generation() const noexcept {
        return generation_;
    }

    // Returns nullopt if this runner is already occupied or the invocation is
    // invalid.  No callback is made in that case.
    [[nodiscard]] std::optional<std::uint64_t>
    start(QString program, QStringList arguments, QString working_directory,
          int timeout_ms, Completion completion) {
        return start(std::move(program), std::move(arguments),
                     std::move(working_directory), timeout_ms, {},
                     std::move(completion));
    }

    // The optional standard input is intended for one-time OAuth redirect
    // values.  It is never placed in the process argument list and is wiped
    // from this object immediately after QProcess accepts it.
    [[nodiscard]] std::optional<std::uint64_t>
    start(QString program, QStringList arguments, QString working_directory,
          int timeout_ms, QByteArray standard_input, Completion completion) {
        if (active_ || program.trimmed().isEmpty() || timeout_ms <= 0
            || standard_input.size() > kMaxInputBytes || !completion) {
            return std::nullopt;
        }

        ++generation_;
        active_ = true;
        forced_error_ = HelperProcessError::None;
        output_.clear();
        input_ = std::move(standard_input);
        completion_ = std::move(completion);

        // A killed QProcess can still have queued Windows notifier events.
        // Reusing it lets an old completion race the next invocation. Every
        // generation therefore owns a fresh process object.
        process_ = std::make_unique<QProcess>();
        process_->setProcessChannelMode(QProcess::MergedChannels);
        process_->setProgram(std::move(program));
        process_->setArguments(std::move(arguments));
        process_->setWorkingDirectory(std::move(working_directory));
        const std::uint64_t run = generation_;
        QObject::connect(process_.get(), &QProcess::readyRead,
                         [this, run] {
                             if (active_ && generation_ == run) drain_output();
                         });
        QObject::connect(process_.get(), &QProcess::started, [this, run] {
            if (!active_ || generation_ != run || !process_ || input_.isEmpty())
                return;
            process_->write(input_);
            // Every private helper request is line/length framed. Do not call
            // closeWriteChannel() here: on Windows it can synchronously wait
            // for the pipe while the child exits after consuming its frame,
            // blocking the GUI event loop and its timeout timer.
            // input_ remains alive until finish(); see the Windows overlapped
            // pipe note below.
        });
        QObject::connect(
            process_.get(), &QProcess::errorOccurred,
            [this, run](QProcess::ProcessError process_error) {
                if (!active_ || generation_ != run || !process_) return;
                if (process_error == QProcess::FailedToStart) {
                    forced_error_ = HelperProcessError::StartFailed;
                    finish(run, process_->errorString());
                }
            });
        QObject::connect(
            process_.get(),
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            [this, run](int exit_code, QProcess::ExitStatus status) {
                if (!active_ || generation_ != run) return;
                if (forced_error_ == HelperProcessError::None
                    && status == QProcess::CrashExit) {
                    forced_error_ = HelperProcessError::Crashed;
                }
                finish(run,
                       status == QProcess::CrashExit
                           ? QStringLiteral("helper process crashed")
                           : QString{},
                       exit_code);
            });
        process_->start();
        // Keep input_ alive until completion. On Windows the pipe write may
        // still be overlapped after write() returns.
        timer_.start(timeout_ms);
        return generation_;
    }

    void cancel() {
        if (!active_) return;
        forced_error_ = HelperProcessError::Cancelled;
        if (!process_ || process_->state() == QProcess::NotRunning) {
            finish(generation_, QStringLiteral("helper process cancelled"));
        } else {
            process_->kill();
        }
    }

private:
    void drain_output() {
        if (!active_ || !process_) return;
        const QByteArray next = process_->readAll();
        const qsizetype room = kMaxOutputBytes - output_.size();
        // QByteArray::first(n) requires n <= size(). `room` is the remaining
        // destination capacity, not the size of this read. Passing 256 KiB for
        // a short readyRead chunk made Qt scan beyond the chunk and crash in
        // VCRUNTIME140!memchr. Clamp to both bounds.
        if (room > 0) output_.append(next.first(std::min(next.size(), room)));
        if (next.size() > room
            && forced_error_ == HelperProcessError::None) {
            forced_error_ = HelperProcessError::OutputLimitExceeded;
            timer_.stop();
            process_->kill();
        }
    }

    void finish(std::uint64_t run, QString detail, int exit_code = -1) {
        if (!active_ || generation_ != run) return;
        drain_output();
        timer_.stop();

        HelperProcessResult result;
        result.generation = generation_;
        result.exit_code = exit_code;
        result.output = QString::fromUtf8(output_);
        result.detail = std::move(detail);
        result.error = forced_error_;
        if (result.error == HelperProcessError::OutputLimitExceeded) {
            result.detail = QStringLiteral(
                "helper output exceeded the 256 KiB safety limit");
        } else if (result.error == HelperProcessError::TimedOut) {
            result.detail = QStringLiteral("helper process timed out");
        } else if (result.error == HelperProcessError::Cancelled) {
            result.detail = QStringLiteral("helper process cancelled");
        }

        active_ = false;
        wipe_input();
        forced_error_ = HelperProcessError::None;
        // Do not destroy the signal sender while Qt is still unwinding its
        // finished/error signal. In particular, a killed Windows QProcess can
        // strand its notifier when deleteLater runs before the next helper
        // starts. A panel runs only a handful of cold-path commands, so retain
        // completed process objects until this runner is destroyed.
        if (process_) retired_.push_back(std::move(process_));
        auto completion = std::move(completion_);
        completion_ = {};
        completion(std::move(result));
    }

    std::unique_ptr<QProcess> process_;
    std::vector<std::unique_ptr<QProcess>> retired_;
    QTimer timer_;
    QByteArray output_;
    static constexpr qsizetype kMaxInputBytes = 64 * 1024;
    QByteArray input_;
    Completion completion_;
    std::uint64_t generation_ = 0;
    HelperProcessError forced_error_ = HelperProcessError::None;
    bool active_ = false;

    void wipe_input() noexcept {
        input_.fill('\0');
        input_.clear();
        input_.squeeze();
    }
};

} // namespace altair::ui
