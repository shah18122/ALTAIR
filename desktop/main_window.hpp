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

#include "chart/chart_widget.hpp"
#include "data/bar_csv.hpp"
#include "feed_status.hpp"
#include "panels.hpp"
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

/// The nav labels, in order. THE ONE PLACE THEY ARE WRITTEN.
///
/// `build_nav()` fills the list widget from here and `--page` validates
/// against here, so the two cannot drift into disagreeing about what a page is
/// called -- which is the failure that makes a validated flag worse than an
/// unvalidated one, because it refuses a name that is genuinely on screen.
[[nodiscard]] inline QStringList nav_page_names() {
    return {QStringLiteral("Live Grid"),   QStringLiteral("Chart"),
            QStringLiteral("Watchlist"),   QStringLiteral("Models"),
            QStringLiteral("Data Flow"),   QStringLiteral("Broker Wiring"),
            QStringLiteral("Ratio Spread"), QStringLiteral("Value — DCF"),
            QStringLiteral("Aggregator"),  QStringLiteral("Trade Handler"),
            QStringLiteral("Audit Trail")};
}

/// Index of a nav page by name, case- and space-insensitively; -1 if no match.
///
/// Callable BEFORE the window exists, which is the point: `--page` is checked
/// before the login dialog opens, so a typo costs a message rather than a
/// password entry that is then thrown away.
[[nodiscard]] inline int nav_page_index(const QString& name) {
    const QString want = name.simplified().toLower();
    const QStringList names = nav_page_names();
    for (int i = 0; i < names.size(); ++i) {
        if (names[i].simplified().toLower() == want) {
            return i;
        }
    }
    return -1;
}

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
    MainWindow(const ReplayTick* ticks, std::size_t count, Role role,
               const QString& user, QWidget* parent = nullptr)
        : QMainWindow(parent),
          ticks_(ticks),
          count_(count),
          replayer_(ticks, count),
          clock_(DemoSessionTimes::trading(), DemoSessionTimes::pre_open()),
          role_(role),
          user_(user) {
        // The UI is attached to a replay. Saying so in the source of truth
        // means the pill cannot claim otherwise.
        feed_.set_source(FeedSource::Replay);
        feed_.set_transport_up(true);
        setWindowTitle(QStringLiteral("Altair"));

        build_nav();
        build_watchlist();
        build_chart();
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

    /// Open on a given nav page. For screenshots and for launching straight
    /// into the panel you are working on; the nav is still the normal way in.
    void show_page(int index) {
        if (index >= 0 && index < nav_->count()) {
            nav_->setCurrentRow(index);
        }
    }

    /// Run the Models page's walk-forward evaluation. For `--train`; the
    /// button is the normal way in.
    void train_selected() {
        if (models_panel_ != nullptr) {
            models_panel_->run_walk_forward();
        }
    }

    /// The same, by name. Returns false if nothing matched.
    ///
    /// THE INDEX FORM FAILS SILENTLY, WHICH IS WHY THIS EXISTS. `--page models`
    /// went through `QString::toInt()`, which returns 0 for anything
    /// unparseable, so a mistyped page name opened page 0 and looked like it
    /// had worked. A capture taken that way shows the wrong panel and says
    /// nothing about it -- and one was, in this session. Rule 9 (an ambiguous
    /// input blocks and raises rather than guessing) applies to the tooling as
    /// much as to the engine, because tooling that guesses is how a wrong
    /// screenshot reaches a review.
    bool show_page(const QString& name) {
        const int i = nav_page_index(name);
        if (i < 0 || i >= nav_->count()) {
            return false;
        }
        nav_->setCurrentRow(i);
        return true;
    }

    /// Drain `n` ticks immediately, so a screenshot or a test starts with a
    /// populated chart instead of an empty one.
    void prime(std::size_t n) {
        seek(static_cast<int>(n));
    }

    /// Select a chart data source by index. Same reason as `show_page`:
    /// starting where the work is, without simulating a click.
    void show_source(int index) {
        if (source_ != nullptr && index >= 0 && index < source_->count()) {
            source_->setCurrentIndex(index);
        }
    }

    void add_instrument(std::uint32_t token, const QString& symbol) {
        model_->add_instrument(token, symbol);
        instrument_->addItem(symbol, token);
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
            // THE RECEIVE CLOCK, not the tick's exchange timestamp. Staleness
            // is a question about us, and a replay of an old session emits old
            // timestamps forever -- it would look permanently fresh by its own
            // clock. feed/tick.hpp carries recv_ts for exactly this.
            feed_.observe_arrival(QDateTime::currentMSecsSinceEpoch()
                                  * 1'000'000LL);
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
    ModelPanel* models_panel_ = nullptr;

    void build_nav() {
        nav_ = new QListWidget;
        nav_->setFixedWidth(190);
        nav_->setFrameShape(QFrame::NoFrame);
        nav_->setStyleSheet(QStringLiteral(
            "QListWidget{background:#20262B;color:#D6DBDF;border:none;}"
            "QListWidget::item{padding:11px 14px;}"
            "QListWidget::item:selected{background:#2C3E50;color:#FFFFFF;}"));
        // Indented for the left gutter; the names themselves come from
        // nav_page_names() so --page and the nav agree by construction.
        for (const QString& name : nav_page_names()) {
            nav_->addItem(QStringLiteral("  ") + name);
        }
        nav_->setCurrentRow(0);

        pages_ = new QStackedWidget;
        connect(nav_, &QListWidget::currentRowChanged, this, [this](int row) {
            pages_->setCurrentIndex(row);
            if (row == 1) {
                rebuild_chart();   // draw at once rather than on the next tick
            }
        });

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

    void build_chart() {
        chart_ = new ChartWidget;

        // WHAT THE CHART IS LOOKING AT. The synthetic replay and the real
        // series on disk are DIFFERENT KINDS OF THING -- one is a tape being
        // bucketed live, the other is bars that were already aggregated by
        // whoever produced the file -- so they are separate entries rather
        // than one list with a hidden mode.
        source_ = new QComboBox;
        source_->addItem(QStringLiteral("Synthetic replay"), QString());
        source_->addItem(QStringLiteral("NIFTY 1-minute (real)"),
                         QStringLiteral("spot/nifty/1m|60"));
        source_->addItem(QStringLiteral("NIFTY 60-minute (real)"),
                         QStringLiteral("spot/nifty/60m|3600"));
        source_->addItem(QStringLiteral("NIFTY daily (real, 35 years)"),
                         QStringLiteral("spot/nifty/1d|86400"));
        source_->addItem(QStringLiteral("India VIX daily (real)"),
                         QStringLiteral("spot/indiavix/1d|86400"));
        connect(source_, &QComboBox::currentIndexChanged, this,
                [this](int) { rebuild_chart(); });

        instrument_ = new QComboBox;
        connect(instrument_, &QComboBox::currentIndexChanged, this,
                [this](int) { rebuild_chart(); });

        bucket_ = new QComboBox;
        bucket_->addItem(QStringLiteral("1 second"),  1'000'000'000LL);
        bucket_->addItem(QStringLiteral("5 seconds"), 5'000'000'000LL);
        bucket_->addItem(QStringLiteral("15 seconds"), 15'000'000'000LL);
        bucket_->addItem(QStringLiteral("1 minute"),  60'000'000'000LL);
        bucket_->setCurrentIndex(3);   // 1 minute
        connect(bucket_, &QComboBox::currentIndexChanged, this,
                [this](int) { rebuild_chart(); });

        auto* controls = new QHBoxLayout;
        controls->addWidget(new QLabel(QStringLiteral("Source")));
        controls->addWidget(source_);
        controls->addSpacing(16);
        controls->addWidget(new QLabel(QStringLiteral("Instrument")));
        controls->addWidget(instrument_);
        controls->addSpacing(16);
        controls->addWidget(new QLabel(QStringLiteral("Bucket")));
        controls->addWidget(bucket_);
        controls->addStretch();

        auto* page = new QWidget;
        auto* v = new QVBoxLayout(page);
        v->addLayout(controls);
        v->addWidget(chart_, 1);
        pages_->addWidget(page);
    }

    /// Rebuild from the ticks DELIVERED SO FAR -- never the whole array.
    ///
    /// `applied_` is the count the replayer has handed over. Passing
    /// `count_` here instead would let the chart draw the afternoon while the
    /// scrubber sits at 10:15, which is precisely the look-ahead P11-13
    /// measured. The bound is the argument, not a filter applied afterwards.
    /// A bar width in words. Used when the file's native span is what is
    /// actually drawn, so the header names it rather than repeating whatever
    /// the bucket box happens to say.
    [[nodiscard]] static QString describe_span(std::int64_t ns) {
        const std::int64_t s = ns / 1'000'000'000LL;
        if (s % 86400 == 0) {
            return QStringLiteral("%1 day").arg(s / 86400)
                 + (s / 86400 == 1 ? QString() : QStringLiteral("s"));
        }
        if (s % 3600 == 0) {
            return QStringLiteral("%1 hour").arg(s / 3600)
                 + (s / 3600 == 1 ? QString() : QStringLiteral("s"));
        }
        if (s % 60 == 0) {
            return QStringLiteral("%1 minute").arg(s / 60)
                 + (s / 60 == 1 ? QString() : QStringLiteral("s"));
        }
        return QStringLiteral("%1 s").arg(s);
    }

    void rebuild_chart() {
        if (chart_ == nullptr || source_ == nullptr) {
            return;
        }
        const QString spec = source_->currentData().toString();
        if (spec.isEmpty()) {
            rebuild_from_replay();
        } else {
            rebuild_from_disk(spec);
        }
    }

    void rebuild_from_replay() {
        if (instrument_ == nullptr || applied_ == 0) {
            return;
        }
        instrument_->setEnabled(true);
        const auto token = instrument_->currentData().toUInt();
        const auto bucket = bucket_->currentData().toLongLong();
        auto built = build_candles(ticks_, applied_, token, bucket,
                                   last_ts_ns_);
        const auto k = check_conservation(ticks_, applied_, token, built);
        chart_->set_note(QString());   // the reconciliation shows instead
        chart_->set_title(QStringLiteral("%1 - %2  (synthetic replay)")
                              .arg(instrument_->currentText(),
                                   bucket_->currentText()));
        chart_->set_candles(std::move(built), k);
    }

    /// Load a real series off disk.
    ///
    /// Bars load DIRECTLY into candles. They are NOT pushed through the tick
    /// pipeline: a bar's close is not a tick, and re-bucketing closes throws
    /// away the high and the low -- measured at 37.8% of the range on the real
    /// 2026-08-27 session.
    void rebuild_from_disk(const QString& spec) {
        instrument_->setEnabled(false);   // the path names the instrument
        const QStringList parts = spec.split(QLatin1Char('|'));
        if (parts.size() != 2) {
            return;
        }
        const QString rel = parts[0];
        const std::int64_t native_span =
            parts[1].toLongLong() * 1'000'000'000LL;

        const QString dir = QStringLiteral("%1/%2")
                                .arg(QStringLiteral(ALTAIR_DATASET_DIR), rel);

        LoadResult r = load_bars_dir(dir, native_span,
                                     DailyStamp::SessionClose,
                                     /*zero_volume_is_absent=*/true);
        if (!r.ok()) {
            chart_->set_note(r.error);
            chart_->set_candles({}, Conservation{});
            chart_->set_title(rel);
            return;
        }

        // Aggregate UP if the chosen bucket is coarser than the file's native
        // one. Never DOWN: a coarser file cannot be split into finer bars, and
        // interpolating one would invent prices that never traded.
        // Aggregate UP only. A request for a bucket FINER than the file's
        // native one is not an error -- it is a request the data cannot
        // satisfy -- so the native bars are drawn and the title says which
        // bucket is actually on screen. Silently labelling daily bars
        // "1 minute" is how a chart stops meaning what its header says.
        const std::int64_t want = bucket_->currentData().toLongLong();
        const bool aggregated = want > native_span;
        std::vector<Candle> bars =
            aggregated ? aggregate_bars(r.bars, want) : r.bars;
        const QString drawn_bucket =
            aggregated ? bucket_->currentText() : describe_span(native_span);

        const double rounded_pct =
            r.bars.empty() ? 0.0
                           : 100.0 * static_cast<double>(r.rounded_fields)
                                 / static_cast<double>(r.bars.size() * 4);
        chart_->set_note(
            QStringLiteral("%1 bars - %2 price fields rounded to the paisa"
                           " (%3%) - %4 zero volume, %5 no volume field%6")
                .arg(r.bars.size())
                .arg(r.rounded_fields)
                .arg(rounded_pct, 0, 'f', 1)
                .arg(r.zero_volume_rows)
                .arg(r.absent_volume_rows)
                // A skipped row is DATA LOSS and says so in the header rather
                // than only in a struct field nobody reads.
                .arg(r.skipped_rows > 0
                         ? QStringLiteral(" - %1 ROWS SKIPPED")
                               .arg(r.skipped_rows)
                         : QString()));
        chart_->set_title(
            QStringLiteral("%1 - %2%3")
                .arg(rel, drawn_bucket,
                     aggregated ? QStringLiteral(" (aggregated)")
                                : QStringLiteral(" (file's native bars)")));
        chart_->set_candles(std::move(bars), Conservation{});
    }

    void build_pages() {
        pages_->addWidget(new WatchlistPanel(role_));
        models_panel_ = new ModelPanel(role_);
        pages_->addWidget(models_panel_);

        {
            auto* page = new QWidget;
            auto* v = new QVBoxLayout(page);
            v->addWidget(new QLabel(QStringLiteral(
                "<h3>Data flow — feed to broker</h3>")));
            auto* note = new QLabel(QStringLiteral(
                "Drawn from the same table the Broker Wiring page reads, so "
                "the picture and the facts cannot drift apart. A stage past "
                "the wall is hollow: a tick cannot reach it today."));
            note->setWordWrap(true);
            note->setStyleSheet(QStringLiteral("color:#7F8C8D;"));
            v->addWidget(note);
            v->addWidget(new DataflowWidget(feed_.source()), 1);
            pages_->addWidget(page);
        }

        pages_->addWidget(new WiringPanel);

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
        pill_ = new QLabel;
        who_ = new QLabel;
        phase_ = new QLabel;
        engine_clock_ = new QLabel;
        wall_clock_ = new QLabel;
        progress_ = new QLabel;
        filters_ = new QLabel;

        statusBar()->addWidget(pill_);
        statusBar()->addWidget(progress_);
        statusBar()->addWidget(filters_);
        statusBar()->addPermanentWidget(who_);
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
        const std::int64_t now_ns =
            QDateTime::currentMSecsSinceEpoch() * 1'000'000LL;
        const Liveness live = feed_.liveness(now_ns);
        const std::int64_t age = feed_.age_ns(now_ns);
        pill_->setText(
            QStringLiteral("  %1 · %2%3  ")
                .arg(liveness_label(live), feed_name(feed_.source()),
                     age < 0 ? QString()
                             : QStringLiteral(" · last tick %1 ms ago")
                                   .arg(age / 1'000'000)));
        pill_->setStyleSheet(
            QStringLiteral("color:#FFFFFF;background:%1;font-weight:bold;")
                .arg(liveness_colour(live).name()));
        who_->setText(QStringLiteral(" %1 (%2) ")
                          .arg(user_, role_name(role_)));

        if (++since_chart_ >= 15) {
            since_chart_ = 0;
            // Only the live replay needs redrawing. A series loaded from
            // disk does not change under us, and reloading it four times a
            // second would re-read 8,756 rows to draw the same picture.
            if (pages_->currentIndex() == 1
                && source_ != nullptr
                && source_->currentData().toString().isEmpty()) {
                rebuild_chart();
            }
        }
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
        //
        // Displayed in IST, because that is the market's clock and the session
        // window is expressed in it. Showing UTC next to a phase derived from
        // IST is how a screen reads "CLOSED" at what looks like mid-session --
        // which is exactly what the first version of this window did.
        if (clock_.has_engine_time()) {
            static const QTimeZone kIst =
                QTimeZone::fromSecondsAheadOfUtc(5 * 3600 + 30 * 60);
            const auto engine =
                QDateTime::fromMSecsSinceEpoch(last_ts_ns_ / 1'000'000, kIst);
            engine_clock_->setText(
                QStringLiteral(" engine %1 IST ")
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

    ChartWidget* chart_ = nullptr;
    QComboBox* source_ = nullptr;
    QComboBox* instrument_ = nullptr;
    QComboBox* bucket_ = nullptr;
    int since_chart_ = 0;

    QPushButton* play_ = nullptr;
    QComboBox* speed_ = nullptr;
    QSlider* scrub_ = nullptr;

    QLabel* pill_ = nullptr;
    QLabel* who_ = nullptr;
    QLabel* phase_ = nullptr;
    QLabel* engine_clock_ = nullptr;
    QLabel* wall_clock_ = nullptr;
    QLabel* progress_ = nullptr;
    QLabel* filters_ = nullptr;

    QTimer* timer_ = nullptr;
    std::size_t applied_ = 0;
    std::int64_t last_ts_ns_ = 0;

    FeedStatus feed_;
    Role role_ = Role::None;
    QString user_;
};

} // namespace altair::ui
