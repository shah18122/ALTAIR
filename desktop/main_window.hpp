// desktop/main_window.hpp -- the shell: nav, clock, replay, and the pages.
//
// P11Q-03.
//
// THE CLOCK ON SCREEN IS THE TICK'S, NOT THE WALL'S.
//
// Rule 7 at the most-read widget. Both clocks are shown, side by side and
// labelled, because the DIVERGENCE is the signal: live, they track; replaying,
// they are hours apart. One clock cannot tell you which mode you are in, and a
// dashboard showing only the wall clock says the market is open at 21:40 on a
// Thursday.
//
// A BACKWARD SCRUB REPLAYS FORWARD.
//
// `feed/replay.hpp` has no rewind -- deliberately, for the same reason it has
// no `peek()`. So seeking backwards rebuilds the `Replayer` and replays from
// the start through the SAME code path, and the model resets to NO DATA rather
// than to zero on the way. It is slower than a seek and it is the only version
// that cannot render a number the replay has not reached, which is exactly
// what P11-13 measured going wrong in the web client.
//
// THE PAGES THAT ARE NOT WIRED SAY SO.
//
// The nav holds the strategy and model pages Smit asked for. The ones with no
// engine behind them yet render a NAMED BLOCKER -- which card wires them and
// what it needs -- rather than a chart of plausible nothing. A dashboard panel
// showing invented numbers is indistinguishable from one showing real ones,
// and that is the whole reason this project has a rule 9.

#pragma once

#include <core/time/timestamp.hpp>
#include <feed/replay.hpp>

#include "filter.hpp"
#include "market_clock.hpp"
#include "tick_model.hpp"

#include <QAction>
#include <QComboBox>
#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QPushButton>
#include <QSlider>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTableView>
#include <QTimeZone>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWidget>

#include <vector>

namespace altair::ui {

inline constexpr int kFrameIntervalMs = 16;

/// A page with nothing behind it yet, saying which card wires it.
///
/// Deliberately not a chart of placeholder data. A panel of invented numbers
/// looks exactly like a panel of real ones.
[[nodiscard]] inline QWidget* blocked_page(const QString& title,
                                           const QString& card,
                                           const QString& blocker) {
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->addStretch();

    auto* h = new QLabel(QStringLiteral("<h2>%1</h2>").arg(title));
    h->setAlignment(Qt::AlignCenter);
    v->addWidget(h);

    auto* c = new QLabel(QStringLiteral("<b>%1</b>").arg(card));
    c->setAlignment(Qt::AlignCenter);
    c->setStyleSheet(QStringLiteral("color:#B9770B;"));
    v->addWidget(c);

    auto* b = new QLabel(blocker);
    b->setAlignment(Qt::AlignCenter);
    b->setWordWrap(true);
    b->setStyleSheet(QStringLiteral("color:#7F8C8D;"));
    b->setMaximumWidth(560);
    v->addWidget(b, 0, Qt::AlignHCenter);

    v->addStretch();
    return w;
}

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    /// Borrows `ticks`; the caller owns the storage and must outlive the
    /// window. Same contract as `Replayer`, for the same reason.
    MainWindow(const ReplayTick* ticks, std::size_t count,
               QWidget* parent = nullptr)
        : QMainWindow(parent),
          ticks_(ticks),
          count_(count),
          replayer_(ticks, count),
          clock_(DemoSessionTimes::trading(), DemoSessionTimes::pre_open()) {
        setWindowTitle(QStringLiteral("Altair"));

        build_nav();
        build_watchlist();
        build_pages();
        build_toolbar();
        build_status();

        timer_ = new QTimer(this);
        timer_->setInterval(kFrameIntervalMs);
        connect(timer_, &QTimer::timeout, this, &MainWindow::pump);
        timer_->start();

        auto* full = new QAction(this);
        full->setShortcuts({QKeySequence(Qt::Key_F11), QKeySequence(Qt::Key_Escape)});
        connect(full, &QAction::triggered, this, &MainWindow::toggle_fullscreen);
        addAction(full);

        refresh_status();
    }

    TickModel* model() const noexcept { return model_; }

    void add_instrument(std::uint32_t token, const QString& symbol) {
        model_->add_instrument(token, symbol);
    }

private Q_SLOTS:
    void toggle_fullscreen() {
        if (isFullScreen()) {
            showMaximized();
        } else {
            showFullScreen();
        }
    }

    void toggle_play() {
        if (timer_->isActive()) {
            timer_->stop();
            play_->setText(QStringLiteral("▶  Play"));
        } else {
            if (replayer_.exhausted()) {
                return;
            }
            timer_->start();
            play_->setText(QStringLiteral("❚❚  Pause"));
        }
    }

    void restart() {
        replayer_ = Replayer(ticks_, count_);
        model_->reset_values();
        applied_ = 0;
        last_ts_ns_ = 0;
        clock_ = MarketClock(DemoSessionTimes::trading(),
                             DemoSessionTimes::pre_open());
        play_->setEnabled(true);
        if (!timer_->isActive()) {
            timer_->start();
            play_->setText(QStringLiteral("❚❚  Pause"));
        }
        refresh_status();
    }

    /// Seek to a tick index. Backwards means REPLAYING FORWARD from the start,
    /// because `Replayer` has no rewind and should not grow one.
    void seek(int target) {
        const auto want = static_cast<std::size_t>(target);
        if (want < applied_) {
            replayer_ = Replayer(ticks_, count_);
            model_->reset_values();
            applied_ = 0;
            last_ts_ns_ = 0;
        }
        while (applied_ < want) {
            const auto t = replayer_.next();
            if (!t.has_value()) {
                break;
            }
            model_->apply_tick(*t);
            last_ts_ns_ = t->ts.ns_since_epoch();
            clock_.observe(t->ts);
            ++applied_;
        }
        refresh_status();
    }

    void pump() {
        const int budget = speed_->currentData().toInt();
        int drained = 0;
        while (drained < budget) {
            const auto t = replayer_.next();
            if (!t.has_value()) {
                timer_->stop();
                play_->setText(QStringLiteral("▶  Play"));
                play_->setEnabled(false);
                break;
            }
            model_->apply_tick(*t);
            last_ts_ns_ = t->ts.ns_since_epoch();
            clock_.observe(t->ts);
            ++applied_;
            ++drained;
        }
        refresh_status();
    }

    void open_filter(int column) {
        QStringList values;
        int blanks = 0;
        model_->column_values(column, values, blanks);

        const QString label =
            model_->headerData(column, Qt::Horizontal).toString();
        const bool numeric = column != TickModel::ColSymbol;

        FilterDialog dlg(label, column, numeric, values, blanks,
                         proxy_->filter_for(column), this);
        if (dlg.exec() != QDialog::Accepted) {
            return;
        }
        const ColumnFilter f = dlg.result();
        if (f.active()) {
            proxy_->set_filter(column, f);
        } else {
            proxy_->clear_filter(column);
        }
        mark_filtered_headers();
        refresh_status();
    }

    void header_context_menu(const QPoint& pos) {
        const int col = view_->horizontalHeader()->logicalIndexAt(pos);
        if (col < 0) {
            return;
        }
        QMenu menu(this);
        auto* filter = menu.addAction(QStringLiteral("Filter…"));
        auto* clear = menu.addAction(QStringLiteral("Clear this filter"));
        clear->setEnabled(proxy_->has_filter(col));
        menu.addSeparator();
        auto* clear_all = menu.addAction(QStringLiteral("Clear all filters"));
        clear_all->setEnabled(proxy_->active_filter_count() > 0);

        QAction* chosen = menu.exec(
            view_->horizontalHeader()->mapToGlobal(pos));
        if (chosen == filter) {
            open_filter(col);
        } else if (chosen == clear) {
            proxy_->clear_filter(col);
            mark_filtered_headers();
            refresh_status();
        } else if (chosen == clear_all) {
            proxy_->clear_all();
            mark_filtered_headers();
            refresh_status();
        }
    }

private:
    void build_nav() {
        nav_ = new QListWidget;
        nav_->setFixedWidth(190);
        nav_->setFrameShape(QFrame::NoFrame);
        nav_->setStyleSheet(QStringLiteral(
            "QListWidget{background:#20262B;color:#D6DBDF;border:none;}"
            "QListWidget::item{padding:11px 14px;}"
            "QListWidget::item:selected{background:#2C3E50;color:#FFFFFF;}"));
        for (const auto& name : {QStringLiteral("  Watchlist"),
                                 QStringLiteral("  Ratio Spread"),
                                 QStringLiteral("  Value — DCF"),
                                 QStringLiteral("  Models"),
                                 QStringLiteral("  Aggregator"),
                                 QStringLiteral("  Trade Handler"),
                                 QStringLiteral("  Audit Trail")}) {
            nav_->addItem(name);
        }
        nav_->setCurrentRow(0);

        pages_ = new QStackedWidget;
        connect(nav_, &QListWidget::currentRowChanged,
                pages_, &QStackedWidget::setCurrentIndex);

        auto* split = new QWidget;
        auto* h = new QHBoxLayout(split);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(0);
        h->addWidget(nav_);
        h->addWidget(pages_, 1);
        setCentralWidget(split);
    }

    void build_watchlist() {
        model_ = new TickModel(this);
        proxy_ = new FilterProxy(this);
        proxy_->setSourceModel(model_);

        view_ = new QTableView;
        view_->setModel(proxy_);
        view_->setSelectionBehavior(QAbstractItemView::SelectRows);
        view_->setAlternatingRowColors(true);
        view_->verticalHeader()->setVisible(false);
        view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        view_->setSortingEnabled(true);   // sorts on SortRole, not the string
        view_->sortByColumn(TickModel::ColSymbol, Qt::AscendingOrder);

        auto* hh = view_->horizontalHeader();
        hh->setStretchLastSection(false);
        hh->setSectionResizeMode(TickModel::ColSymbol, QHeaderView::Stretch);
        for (int c = TickModel::ColLast; c < TickModel::ColumnCount; ++c) {
            hh->setSectionResizeMode(c, QHeaderView::ResizeToContents);
        }
        hh->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(hh, &QHeaderView::customContextMenuRequested,
                this, &MainWindow::header_context_menu);
        connect(hh, &QHeaderView::sectionDoubleClicked,
                this, &MainWindow::open_filter);

        auto* page = new QWidget;
        auto* v = new QVBoxLayout(page);
        auto* hint = new QLabel(QStringLiteral(
            "Click a header to sort · double-click or right-click a header to "
            "filter · F11 toggles full screen"));
        hint->setStyleSheet(QStringLiteral("color:#7F8C8D;padding:2px 4px;"));
        v->addWidget(hint);
        v->addWidget(view_, 1);
        pages_->addWidget(page);
    }

    void build_pages() {
        pages_->addWidget(blocked_page(
            QStringLiteral("Ratio Spread"), QStringLiteral("P11Q-05"),
            QStringLiteral(
                "strategies/parity.hpp already prices the spacing-weighted "
                "butterfly and refuses an unknown hedge leg. Wiring it needs a "
                "live option chain — an instrument set, a spot, and quotes for "
                "each strike. The demo generator emits four instruments and one "
                "strike, so there is no chain to price yet.")));

        pages_->addWidget(blocked_page(
            QStringLiteral("Value — DCF"), QStringLiteral("P11Q-05"),
            QStringLiteral(
                "models/dcf.hpp computes FCFF at WACC and FCFE at the cost of "
                "equity, with separate EnterpriseValue and EquityValue types so "
                "the two cannot be confused. It needs point-in-time "
                "fundamentals from strategies/fundamentals.hpp, which returns "
                "nothing before a statement's FILING date — and no filings have "
                "been ingested.")));

        pages_->addWidget(blocked_page(
            QStringLiteral("Models"), QStringLiteral("P11Q-05"),
            QStringLiteral(
                "models/registry.hpp and models/serving.hpp exist and are "
                "tested. Nothing is trained: Phase 8's exit needs real tick "
                "data and LibTorch, and neither is present. Showing a scorecard "
                "for a model that has never seen a market would be a panel of "
                "invented numbers.")));

        pages_->addWidget(blocked_page(
            QStringLiteral("Aggregator"), QStringLiteral("P11Q-05"),
            QStringLiteral(
                "models/aggregator.hpp combines member signals and sizes on the "
                "LOWER confidence bound of edge, with n_eff correcting for "
                "correlated members. It has no members to combine until the "
                "Models page above has something in it.")));

        pages_->addWidget(blocked_page(
            QStringLiteral("Trade Handler"), QStringLiteral("P11Q-05 / Phase 12"),
            QStringLiteral(
                "The order path stops at translation, by design. "
                "oms/kite_adapter.hpp builds the form body for POST /orders and "
                "does NOT send it; the transport needs the vcpkg net feature, "
                "which is not built; there is no data/kite_session.json and so "
                "no access token; and P1-07 is blocked on credentials. Four "
                "independent blockers, none of them code I can write here.")));

        pages_->addWidget(blocked_page(
            QStringLiteral("Audit Trail"), QStringLiteral("P11Q-05"),
            QStringLiteral(
                "Every row must carry all five of rule 10's stamps — model "
                "hash, feature version, config hash, spec version, tick seqno — "
                "and a row missing one is rendered AS not reproducible rather "
                "than with a blank column. The engine emits the seqno today; "
                "the other four arrive with a trained model and a loaded "
                "config.")));
    }

    void build_toolbar() {
        auto* bar = addToolBar(QStringLiteral("Replay"));
        bar->setMovable(false);

        play_ = new QPushButton(QStringLiteral("❚❚  Pause"));
        connect(play_, &QPushButton::clicked, this, &MainWindow::toggle_play);
        bar->addWidget(play_);

        auto* restart = new QPushButton(QStringLiteral("⟲  Restart"));
        connect(restart, &QPushButton::clicked, this, &MainWindow::restart);
        bar->addWidget(restart);

        bar->addSeparator();
        bar->addWidget(new QLabel(QStringLiteral(" Speed ")));
        speed_ = new QComboBox;
        // Ticks drained per 16 ms frame. Named in ticks, not in "x", because
        // a multiplier implies a wall-clock relationship the replay does not
        // have -- the tape's own timestamps are what advance.
        speed_->addItem(QStringLiteral("10 ticks/frame"), 10);
        speed_->addItem(QStringLiteral("40 ticks/frame"), 40);
        speed_->addItem(QStringLiteral("200 ticks/frame"), 200);
        speed_->addItem(QStringLiteral("1000 ticks/frame"), 1000);
        speed_->setCurrentIndex(1);
        bar->addWidget(speed_);

        bar->addSeparator();
        scrub_ = new QSlider(Qt::Horizontal);
        scrub_->setMinimum(0);
        scrub_->setMaximum(static_cast<int>(count_));
        scrub_->setMinimumWidth(260);
        connect(scrub_, &QSlider::sliderReleased, this, [this] {
            seek(scrub_->value());
        });
        bar->addWidget(scrub_);
    }

    void build_status() {
        phase_ = new QLabel;
        engine_clock_ = new QLabel;
        wall_clock_ = new QLabel;
        progress_ = new QLabel;
        filters_ = new QLabel;

        statusBar()->addWidget(progress_);
        statusBar()->addWidget(filters_);
        statusBar()->addPermanentWidget(phase_);
        statusBar()->addPermanentWidget(engine_clock_);
        statusBar()->addPermanentWidget(wall_clock_);
    }

    void mark_filtered_headers() {
        // A filtered column says so in its header. Excel draws a funnel; this
        // appends one, which is the same signal and needs no custom painting.
        for (int c = 0; c < TickModel::ColumnCount; ++c) {
            const QString base =
                model_->headerData(c, Qt::Horizontal).toString();
            view_->horizontalHeader()->model()->setHeaderData(
                c, Qt::Horizontal,
                proxy_->has_filter(c) ? base + QStringLiteral("  ▼") : base);
        }
    }

    void refresh_status() {
        scrub_->blockSignals(true);
        scrub_->setValue(static_cast<int>(applied_));
        scrub_->blockSignals(false);

        progress_->setText(
            QStringLiteral(" tick %1 / %2   ·   unknown-instrument ticks: %3 ")
                .arg(applied_).arg(count_).arg(model_->unknown_ticks()));

        const int shown = proxy_->rowCount();
        const int total = model_->rowCount();
        filters_->setText(
            shown == total
                ? QStringLiteral(" %1 rows ").arg(total)
                : QStringLiteral(" %1 of %2 rows — %3 filter(s) active ")
                      .arg(shown).arg(total).arg(proxy_->active_filter_count()));

        const SessionPhase p = clock_.phase();
        phase_->setText(QStringLiteral(" %1 ").arg(phase_label(p)));
        phase_->setStyleSheet(
            QStringLiteral("color:%1;font-weight:bold;")
                .arg(phase_colour(p).name()));

        // THE ENGINE CLOCK: read off the tick, never QDateTime::currentDateTime().
        if (clock_.has_engine_time()) {
            const auto engine = QDateTime::fromMSecsSinceEpoch(
                last_ts_ns_ / 1'000'000, QTimeZone::utc());
            engine_clock_->setText(
                QStringLiteral(" engine %1Z ")
                    .arg(engine.toString(QStringLiteral("HH:mm:ss.zzz"))));
        } else {
            // No tick yet, so there is no engine time. Blank, not 00:00:00 --
            // absence is not zero here either.
            engine_clock_->setText(QStringLiteral(" engine — "));
        }
        wall_clock_->setText(
            QStringLiteral(" wall %1 ")
                .arg(QDateTime::currentDateTime().toString(
                    QStringLiteral("HH:mm:ss"))));
    }

    const ReplayTick* ticks_ = nullptr;
    std::size_t count_ = 0;
    Replayer replayer_;
    MarketClock clock_;

    TickModel* model_ = nullptr;
    FilterProxy* proxy_ = nullptr;
    QTableView* view_ = nullptr;
    QListWidget* nav_ = nullptr;
    QStackedWidget* pages_ = nullptr;

    QPushButton* play_ = nullptr;
    QComboBox* speed_ = nullptr;
    QSlider* scrub_ = nullptr;

    QLabel* phase_ = nullptr;
    QLabel* engine_clock_ = nullptr;
    QLabel* wall_clock_ = nullptr;
    QLabel* progress_ = nullptr;
    QLabel* filters_ = nullptr;

    QTimer* timer_ = nullptr;
    std::size_t applied_ = 0;
    std::int64_t last_ts_ns_ = 0;
};

} // namespace altair::ui
