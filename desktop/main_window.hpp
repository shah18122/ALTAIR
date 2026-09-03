// desktop/main_window.hpp -- the window, and the clock it does not read.
//
// P11Q-01.
//
// THE CLOCK ON SCREEN IS THE TICK'S, NOT THE WALL'S.
//
// Rule 7. The status bar shows the EXCHANGE timestamp carried by the last tick
// applied, and a `QTimer` decides only how often the pump runs -- never what
// time it is. A dashboard that reads `QDateTime::currentDateTime()` shows a
// replay of last Tuesday at today's time and looks live, which is precisely
// the failure P11-13 measured in the web client.
//
// Both clocks are displayed side by side on purpose. In live trading they
// track; in a replay they diverge by however long ago the session was, and
// that divergence is what makes a replay visibly a replay rather than a very
// calm afternoon.
//
// THE PUMP IS RATE-LIMITED, THE DATA IS NOT.
//
// Sixty frames a second cannot carry every tick, so the pump drains a batch
// per timer fire and the view repaints once. That is P11-01's coalescing
// argument in miniature: coalescing is right for a QUOTE, because the next
// frame restates it. It would be wrong for a fill, and this window shows no
// fills -- when it does, they will not come down this path.
//
// AND THE REPLAYER CANNOT BE ASKED FOR THE FUTURE.
//
// `feed/replay.hpp` says it outright: "THERE IS DELIBERATELY NO peek(), at(),
// operator[], begin(), OR ANY OTHER WAY TO SEE A TICK THE CURSOR HAS NOT
// REACHED." So the no-look-ahead property this window depends on is enforced
// one layer down, by an absent function, rather than by this file behaving.

#pragma once

#include <core/time/timestamp.hpp>
#include <feed/replay.hpp>

#include "tick_model.hpp"

#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QStatusBar>
#include <QTableView>
#include <QTimeZone>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <vector>

namespace altair::ui {

/// How many ticks to drain per timer fire. With a 16 ms timer this is a
/// deliberate throughput, not a guess: enough to look alive, bounded so a
/// long session cannot stall the event loop.
inline constexpr int kTicksPerFrame = 40;
inline constexpr int kFrameIntervalMs = 16;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    /// Borrows `ticks`; the caller owns the storage and must outlive the
    /// window. Same contract as `Replayer`, for the same reason.
    MainWindow(const ReplayTick* ticks, std::size_t count, QWidget* parent = nullptr)
        : QMainWindow(parent), replayer_(ticks, count), total_(count) {
        setWindowTitle(QStringLiteral("Altair — replay"));
        resize(880, 420);

        model_ = new TickModel(this);
        view_ = new QTableView(this);
        view_->setModel(model_);
        view_->setSelectionBehavior(QAbstractItemView::SelectRows);
        view_->setAlternatingRowColors(true);
        view_->verticalHeader()->setVisible(false);
        // Stretch the SYMBOL column, not the last one. Qt's default stretches
        // the last section, which here pushes a right-aligned sequence number
        // to the far edge of the window with a field of empty space in front
        // of it -- and empty space in a data grid reads as a missing value.
        view_->horizontalHeader()->setStretchLastSection(false);
        view_->horizontalHeader()->setSectionResizeMode(
            TickModel::ColSymbol, QHeaderView::Stretch);
        for (int c = TickModel::ColLast; c < TickModel::ColumnCount; ++c) {
            view_->horizontalHeader()->setSectionResizeMode(
                c, QHeaderView::ResizeToContents);
        }
        view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        view_->setSortingEnabled(false);   // sorting lands in the next card

        play_ = new QPushButton(QStringLiteral("Pause"), this);
        connect(play_, &QPushButton::clicked, this, &MainWindow::toggle);

        auto* controls = new QHBoxLayout;
        controls->addWidget(play_);
        controls->addStretch();

        auto* root = new QWidget(this);
        auto* layout = new QVBoxLayout(root);
        layout->addWidget(view_);
        layout->addLayout(controls);
        setCentralWidget(root);

        engine_clock_ = new QLabel(this);
        wall_clock_ = new QLabel(this);
        progress_ = new QLabel(this);
        statusBar()->addWidget(progress_);
        statusBar()->addPermanentWidget(engine_clock_);
        statusBar()->addPermanentWidget(wall_clock_);

        timer_ = new QTimer(this);
        timer_->setInterval(kFrameIntervalMs);
        connect(timer_, &QTimer::timeout, this, &MainWindow::pump);
        timer_->start();

        refresh_status();
    }

    TickModel* model() const noexcept { return model_; }

    void add_instrument(std::uint32_t token, const QString& symbol) {
        model_->add_instrument(token, symbol);
        view_->resizeColumnsToContents();
    }

private Q_SLOTS:
    void toggle() {
        if (timer_->isActive()) {
            timer_->stop();
            play_->setText(QStringLiteral("Play"));
        } else {
            timer_->start();
            play_->setText(QStringLiteral("Pause"));
        }
    }

    void pump() {
        int drained = 0;
        while (drained < kTicksPerFrame) {
            const auto t = replayer_.next();
            if (!t.has_value()) {
                timer_->stop();
                play_->setText(QStringLiteral("Play"));
                play_->setEnabled(false);
                break;
            }
            model_->apply_tick(*t);
            ++applied_;
            last_ts_ns_ = t->ts.ns_since_epoch();
            ++drained;
        }
        refresh_status();
    }

private:
    void refresh_status() {
        progress_->setText(
            QStringLiteral("tick %1 / %2   ·   unknown-instrument ticks: %3")
                .arg(applied_)
                .arg(total_)
                .arg(model_->unknown_ticks()));

        // THE ENGINE CLOCK: read off the tick. Never QDateTime::currentDateTime().
        if (applied_ > 0) {
            const auto engine = QDateTime::fromMSecsSinceEpoch(
                last_ts_ns_ / 1'000'000, QTimeZone::utc());
            engine_clock_->setText(
                QStringLiteral("engine %1Z ")
                    .arg(engine.toString(QStringLiteral("HH:mm:ss.zzz"))));
        } else {
            // No tick yet, so there is no engine time. Blank, not "00:00:00" --
            // absence is not zero here either.
            engine_clock_->setText(QStringLiteral("engine — "));
        }

        // The wall clock, labelled as such and never used for a decision.
        wall_clock_->setText(
            QStringLiteral(" wall %1 ")
                .arg(QDateTime::currentDateTime().toString(
                    QStringLiteral("HH:mm:ss"))));
    }

    Replayer replayer_;
    TickModel* model_ = nullptr;
    QTableView* view_ = nullptr;
    QPushButton* play_ = nullptr;
    QLabel* engine_clock_ = nullptr;
    QLabel* wall_clock_ = nullptr;
    QLabel* progress_ = nullptr;
    QTimer* timer_ = nullptr;

    std::size_t total_ = 0;
    std::size_t applied_ = 0;
    std::int64_t last_ts_ns_ = 0;
};

} // namespace altair::ui
