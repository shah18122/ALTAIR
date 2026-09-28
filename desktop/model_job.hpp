// desktop/model_job.hpp -- cold-path model work off the GUI thread.
//
// P6-03. The controller owns no model and knows no widgets beyond its QObject
// context. It provides the lifecycle contract shared by every model page:
// generation-safe publication, cooperative cancellation, progress, errors and
// provenance. A superseded job may finish its current numerical primitive, but
// it can never publish over the newer selection.
#pragma once

#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QThread>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace altair::ui {

struct ModelJobContext {
    std::function<bool()> cancelled;
    std::function<void(int, QString)> progress;
};

struct ModelResultField {
    QString name;
    QString value;
    QString unit_or_source;
};

struct ModelResultRow {
    QString section;
    QString value;
};

/// Typed cold-path result contract. Existing report functions can still return
/// QString: the converting constructor preserves their full report while a
/// family is migrated to explicit fields/rows. The UI must label unavailable
/// fields rather than infer numerical values from prose.
struct ModelJobPayload {
    QString report;
    std::vector<ModelResultField> fields;
    std::vector<ModelResultRow> rows;

    ModelJobPayload() = default;
    ModelJobPayload(QString text) : report(std::move(text)) {}
};

struct ModelJobResult {
    std::uint64_t generation = 0;
    QString provenance;
    QString output;
    ModelJobPayload payload;
    QString error;
    bool cancelled = false;
};

class ModelJobController final {
public:
    using Work = std::function<ModelJobPayload(const ModelJobContext&)>;
    using Progress = std::function<void(int, const QString&)>;
    using Completion = std::function<void(const ModelJobResult&)>;

    explicit ModelJobController(QObject* owner) noexcept : owner_(owner) {}

    ModelJobController(const ModelJobController&) = delete;
    ModelJobController& operator=(const ModelJobController&) = delete;

    ~ModelJobController() {
        invalidate();
        for (const auto& worker : workers_) {
            if (worker != nullptr) worker->wait();
        }
    }

    /// Start a new job and invalidate every older publication for this owner.
    /// Work receives no widget pointer and therefore cannot accidentally touch
    /// the GUI from its worker thread.
    [[nodiscard]] std::uint64_t submit(QString provenance, Work work,
                                       Progress progress, Completion complete) {
        invalidate();
        const std::uint64_t generation = generation_;
        auto result = std::make_shared<ModelJobResult>();
        result->generation = generation;
        result->provenance = std::move(provenance);

        auto* thread = QThread::create(
            [this, generation, result, work = std::move(work),
             progress = std::move(progress)]() mutable {
                const ModelJobContext context{
                    [] { return QThread::currentThread()->isInterruptionRequested(); },
                    [this, generation, progress](int value, QString text) {
                        if (!progress || owner_ == nullptr) return;
                        value = std::clamp(value, 0, 100);
                        QMetaObject::invokeMethod(
                            owner_,
                            [this, generation, progress, value,
                             text = std::move(text)] {
                                if (generation == generation_) {
                                    progress(value, text);
                                }
                            },
                            Qt::QueuedConnection);
                    }};

                try {
                    context.progress(0, QStringLiteral("Queued"));
                    if (context.cancelled()) {
                        result->cancelled = true;
                    } else {
                        result->payload = work(context);
                        result->output = result->payload.report;
                        result->cancelled = context.cancelled();
                    }
                    if (!result->cancelled) {
                        context.progress(100, QStringLiteral("Complete"));
                    }
                } catch (const std::exception& e) {
                    result->error = QString::fromUtf8(e.what());
                } catch (...) {
                    result->error = QStringLiteral("Unknown model-job error");
                }
            });
        workers_.push_back(thread);

        QObject::connect(
            thread, &QThread::finished, owner_,
            [this, thread, generation, result,
             complete = std::move(complete)] {
                workers_.erase(
                    std::remove_if(workers_.begin(), workers_.end(),
                                   [thread](const QPointer<QThread>& worker) {
                                       return worker == nullptr || worker == thread;
                                   }),
                    workers_.end());
                if (generation != generation_) return;
                if (thread->isInterruptionRequested()) result->cancelled = true;
                if (complete) complete(*result);
            });
        QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
        thread->start();
        return generation;
    }

    /// Request cooperative cancellation of the current generation. Its
    /// completion is still delivered, explicitly marked cancelled.
    void cancel_current() noexcept {
        for (const auto& worker : workers_) {
            if (worker != nullptr) worker->requestInterruption();
        }
    }

    /// Invalidate current results immediately. Used for page/instrument changes
    /// where an old completion must disappear rather than report "cancelled".
    void invalidate() noexcept {
        ++generation_;
        cancel_current();
    }

    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }

private:
    QObject* owner_ = nullptr;
    std::vector<QPointer<QThread>> workers_;
    std::uint64_t generation_ = 0;
};

} // namespace altair::ui