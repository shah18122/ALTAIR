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
#include "analytics_panel.hpp"
#include "atlas.hpp"
#include "audit_panel.hpp"
#include "cost_panel.hpp"
#include "depth_ladder.hpp"
#include "chain_panel.hpp"
#include "kite_link.hpp"
#include "kill_switch.hpp"
#include "order_ticket.hpp"
#include "data/bar_csv.hpp"
#include "data/real_tape.hpp"
#include "broker_status.hpp"
#include <QDir>
#include "kite_panel.hpp"
#include "live_forecast.hpp"
#include "stream_forecast_page.hpp"
#include "terminal.hpp"
#include "quant_pages.hpp"
#include "workspace_navigation.hpp"
#include "workspace_pages.hpp"
#include "workspace_layout.hpp"

#include <QApplication>
#include <QMessageBox>

#include <cstdlib>
#include "feed_status.hpp"
#include "fyers_link.hpp"
#include "broker_page.hpp"
#include "arbitrage_workspace.hpp"
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
#include <QMenuBar>
#include <QSettings>
#include <QScrollArea>
#include <QScrollBar>
#include <QTabWidget>
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

/// IST, +05:30 with no DST -- the market's clock, and the one this window
/// displays. ONE definition: there were briefly two, and a second local
/// shadowed the first at /W4. A timezone constant is exactly the thing that
/// drifts when it is written twice.
[[nodiscard]] inline const QTimeZone& ist_tz() {
    static const QTimeZone tz =
        QTimeZone::fromSecondsAheadOfUtc(5 * 3600 + 30 * 60);
    return tz;
}

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
    /// `sessions` is the trading-day window the tape was loaded with, kept
    /// so a RELOAD asks for the same span the window opened with rather than
    /// silently changing how much tape is on screen.
    MainWindow(const ReplayTick* ticks, std::size_t count, Role role,
               const QString& user, std::size_t sessions,
               QWidget* parent = nullptr)
        : QMainWindow(parent),
          ticks_(ticks),
          count_(count),
          sessions_(sessions),
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
        build_workspace_controls();

        timer_ = new QTimer(this);
        timer_->setInterval(kFrameIntervalMs);
        connect(timer_, &QTimer::timeout, this, &MainWindow::pump);

        auto* full = new QAction(this);
        // Escape belongs to focused widgets/dialogs and the Terminal chain.
        full->setShortcut(QKeySequence(Qt::Key_F11));
        connect(full, &QAction::triggered, this, &MainWindow::toggle_fullscreen);
        addAction(full);

        refresh_status();
    }

    TickModel* model() const noexcept { return model_; }

    /// Open on a given nav page. For screenshots and for launching straight
    /// into the panel you are working on; the nav is still the normal way in.
    void show_page(int index) {
        const int requested = index;
        index = nav_destination(index);
        if (index >= 0 && index < pages_->count()) {
            const bool changed = pages_->currentIndex() != index;
            pages_->setCurrentIndex(index);
            nav_->select_page(index);
            if (changed && index == 1) rebuild_chart();
            refresh_workspace_controls();
            if (requested != index) statusBar()->showMessage(
                QStringLiteral("%1 moved to %2.")
                    .arg(QString::fromUtf8(kNavigationPages[static_cast<std::size_t>(requested)].label),
                         QString::fromUtf8(kNavigationPages[static_cast<std::size_t>(index)].label)), 10000);
        }
    }

    /// Say whether the tape is real instruments or the synthetic fallback.
    ///
    /// THE STATUS BAR MUST NAME THIS. A grid of real prices and a grid of
    /// fabricated ones look identical, and P11Q-01's first version put real
    /// tickers on a random walk -- so the one thing that cannot be left
    /// implicit is which of the two is on screen.
    void set_tape_is_real(bool real, std::size_t sessions,
                          const QString& why_not,
                          std::int64_t first_ns = 0,
                          std::int64_t last_ns = 0) {
        tape_real_ = real;
        tape_sessions_ = sessions;
        tape_first_ns_ = first_ns;
        tape_last_ns_ = last_ns;
        tape_error_ = why_not;
        refresh_status();
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
        if (i < 0 || i >= pages_->count()) {
            return false;
        }
        show_page(i);
        return true;
    }

    /// Drain `n` ticks immediately, so a screenshot or a test starts with a
    /// populated chart instead of an empty one.
    void prime(std::size_t n) {
        seek(static_cast<int>(n));
    }

    /// Press the current page's compute button, if it has one.
    ///
    /// P31-02. `--page` opens on a compute page and the page says "Not run
    /// yet. Press the button above." -- which is correct behaviour and makes
    /// a screenshot of it useless: every one of these pages computes on
    /// demand, deliberately, so that nothing on screen is a cached number
    /// nobody can reproduce.
    ///
    /// This is the same escape `--train` already is for the Models panel, and
    /// it exists for the reason the comment beside `--page` in main.cpp gives:
    /// a capture cannot click, and synthesising a keystroke sends it to
    /// whatever window has focus. It calls click() on the page's own button,
    /// so it runs EXACTLY the path a person's click runs -- it does not reach
    /// past the widget to call the report function itself, which would make
    /// the capture prove something the button does not.
    ///
    /// Returns false when the current page has no compute button, which is
    /// most of them: the grid, the chart and the watchlist are live and have
    /// nothing to press.
    /// Connect the Terminal's price stream, as its button would.
    ///
    /// For --stream, which exists for the reason --compute and --train do: a
    /// capture script must not synthesise a click, because a synthesised click
    /// goes to whatever window has focus.
    void connect_stream() {
        if (terminal_ != nullptr && terminal_->stream() != nullptr) {
            terminal_->stream()->start(QStringLiteral("127.0.0.1"), 7421);
        }
    }

    /// Start replay after the first paint so startup remains responsive.
    void start_replay() {
        if (!timer_->isActive() && !replayer_.exhausted()) {
            timer_->start();
            play_->setText(QStringLiteral("❚❚  Pause"));
        }
    }

    bool compute_current() {
        // dynamic_cast, not qobject_cast: ComputePage is a plain QWidget
        // subclass with no Q_OBJECT macro, and adding one would put it in
        // AUTOMOC's path for every target that includes quant_pages.hpp --
        // five test binaries, each of which would then need the header in its
        // own source list or link against a moc generated for a different
        // target. RTTI is on and this is a UI action, not a hot path.
        auto* page = dynamic_cast<ComputePage*>(pages_->currentWidget());
        if (page == nullptr) { return false; }
        page->button()->click();
        return true;
    }

    /// Route a stable Atlas identity to the workspace declared by its row.
    [[nodiscard]] bool open_model(const QString& id) {
        const AtlasRow* row = atlas_row_by_id(id);
        if (row == nullptr) return false;
        int page = row->page >= 0 ? nav_destination(row->page)
                                  : nav_page_index(QStringLiteral("models.overview"));
        if (page < 0 || page >= pages_->count()) return false;
        if (page == nav_page_index(QStringLiteral("models.overview"))) {
            if (models_panel_ == nullptr || !models_panel_->focus_atlas_model(id)) {
                return false;
            }
        }
        show_page(page);
        return true;
    }

    /// Select a chart data source by index. Same reason as `show_page`:
    /// starting where the work is, without simulating a click.
    void show_source(int index) {
        if (source_ != nullptr && index >= 0 && index < source_->count()) {
            source_->setCurrentIndex(index);
        }
    }

    /// Re-read dataset/ into the grid's tape. Returns what happened, in
    /// one line, for the caller to show where the user is already looking.
    ///
    /// P34-01. The tape was read once, in main(), before the window existed.
    /// So altair_kite_update could bring every series current and the grid
    /// would go on replaying the bars it loaded at startup -- the status line
    /// would even keep reporting the OLD last-bar date, which is worse than
    /// showing nothing because it is a specific wrong answer.
    ///
    /// IT REFUSES ON THE SYNTHETIC TAPE, and that is not laziness. When
    /// dataset/ was unreadable at startup, main() registered the FABRICATED
    /// instruments -- different tokens, names suffixed "(syn)". Swapping the
    /// tape underneath them would feed real ticks to rows that cannot match
    /// them: every tick unknown, every row frozen at its fake price, and the
    /// window looking like it had updated. Rule 9 -- say so and change
    /// nothing.
    [[nodiscard]] QString reload_tape() {
        if (!tape_real_) {
            return QStringLiteral(
                "· grid NOT reloaded -- this window opened on the SYNTHETIC "
                "tape, so its rows are fabricated instruments and real ticks "
                "would not match them. Restart to pick up the real tape.");
        }
        RealTape fresh =
            load_real_tape(QStringLiteral(ALTAIR_DATASET_DIR), sessions_);
        if (!fresh.ok()) {
            // The tape ON SCREEN is left exactly as it was. A failed reload
            // must not empty a grid that was working.
            return QStringLiteral("· grid NOT reloaded, and the old tape is "
                                  "untouched: %1").arg(fresh.error);
        }
        const std::size_t was = count_;
        owned_tape_ = std::move(fresh.ticks);
        ticks_ = owned_tape_.data();
        count_ = owned_tape_.size();
        set_tape_is_real(true, fresh.sessions, QString(), fresh.first_ns,
                         fresh.last_ns);
        restart();
        return QStringLiteral("· grid reloaded: %1 bars over %2 session(s), "
                              "last %3  (was %4 bars)")
            .arg(count_)
            .arg(fresh.sessions)
            .arg(QDateTime::fromMSecsSinceEpoch(fresh.last_ns / 1'000'000,
                                                ist_tz())
                     .toString(QStringLiteral("yyyy-MM-dd HH:mm")))
            .arg(was);
    }

    void add_instrument(std::uint32_t token, const QString& symbol,
                        bool reports_volume = true) {
        model_->add_instrument(token, symbol, reports_volume);
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
    TerminalPage* terminal_ = nullptr;
    AuditPanel* audit_panel_ = nullptr;
    bool tape_real_ = false;
    std::size_t tape_sessions_ = 0;
    std::int64_t tape_first_ns_ = 0;
    std::int64_t tape_last_ns_ = 0;
    QString tape_error_;
    DepthLadder* ladder_ = nullptr;

    void build_nav() {
        nav_ = new WorkspaceNavigation(this);
        pages_ = new WorkspacePages;
        nav_->activate = [this](int index) { show_page(index); };
        auto* viewport = new QScrollArea;
        viewport->setObjectName(QStringLiteral("workspaceViewport"));
        viewport->setAccessibleName(QStringLiteral("Scrollable workspace"));
        viewport->setWidgetResizable(true);
        viewport->setFrameShape(QFrame::NoFrame);
        viewport->setWidget(pages_);
        connect(pages_, &QStackedWidget::currentChanged, viewport, [viewport] {
            viewport->horizontalScrollBar()->setValue(0);
            viewport->verticalScrollBar()->setValue(0);
        });

        auto* split = new QWidget;
        auto* h = new QHBoxLayout(split);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(0);
        h->addWidget(nav_);
        h->addWidget(viewport, 1);
        setCentralWidget(split);
    }

    void refresh_workspace_controls() {
        if (!nav_toggle_) return;
        const auto label = nav_->mode() == NavigationMode::Hidden
            ? QStringLiteral("Show navigation") : QStringLiteral("Hide navigation");
        nav_toggle_->setText(label);
        nav_toggle_->setToolTip(label + QStringLiteral("  Ctrl+Shift+B"));
        nav_favourite_->setText(nav_->is_favourite()
            ? QStringLiteral("★ Saved") : QStringLiteral("☆ Save page"));
        const auto& page = kNavigationPages[static_cast<std::size_t>(nav_->current_page())];
        nav_location_->setText(QStringLiteral("  %1  /  %2  ")
            .arg(QString::fromUtf8(kNavigationGroups[static_cast<std::size_t>(page.group)].label),
                 QString::fromUtf8(page.label)));
    }

    void build_workspace_controls() {
        // Per-user presentation state. Hex encoding prevents settings path injection.
        nav_settings_ = new QSettings(QSettings::defaultFormat(), QSettings::UserScope,
                                     QStringLiteral("Altair"), QStringLiteral("Desktop"), this);
        nav_settings_->beginGroup(QStringLiteral("workspace/v1/") +
                                  QString::fromLatin1(user_.toUtf8().toHex()));
        nav_->restore_state(*nav_settings_);
        auto* replay_bar = findChild<QToolBar*>();
        auto* bar = new QToolBar(QStringLiteral("Workspaces"), this);
        bar->setObjectName(QStringLiteral("workspaceToolbar"));
        bar->setMovable(false);
        bar->setFloatable(false);
        // Recovery controls must not be hidden through the toolbar context menu.
        bar->toggleViewAction()->setEnabled(false);
        if (replay_bar) {
            insertToolBar(replay_bar, bar);
            insertToolBarBreak(replay_bar);
        } else addToolBar(bar);
        nav_toggle_ = bar->addAction(QStringLiteral("Hide navigation"));
        nav_toggle_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+B")));
        connect(nav_toggle_, &QAction::triggered, this, [this] { nav_->toggle_visibility(); });
        // Resizing is on the sidebar edge. Keep presentation actions in the
        // existing Workspaces menu so they remain reachable with navigation hidden.
        auto* layout = new QMenu(QStringLiteral("Navigation appearance"), this);
        for (const auto& entry : {std::pair{QStringLiteral("Expanded"), NavigationMode::Expanded},
                                 std::pair{QStringLiteral("Compact"), NavigationMode::Compact},
                                 std::pair{QStringLiteral("Hidden"), NavigationMode::Hidden}}) {
            connect(layout->addAction(entry.first), &QAction::triggered, this,
                    [this, value = entry.second] { nav_->set_mode(value); });
        }
        auto* terminal_split = terminal_->findChild<QSplitter*>(QString{}, Qt::FindDirectChildrenOnly);
        if (terminal_split) {
            terminal_split->setChildrenCollapsible(false);
            if (!restore_terminal_layout(terminal_split, *nav_settings_))
                statusBar()->showMessage(QStringLiteral("Saved terminal layout is invalid; defaults retained."), 15000);
            connect(terminal_split, &QSplitter::splitterMoved, this, [this, terminal_split] {
                if (!save_terminal_layout(terminal_split, *nav_settings_))
                    statusBar()->showMessage(QStringLiteral("Terminal layout could not be saved."), 15000);
            });
        }
        layout->addSeparator();
        connect(layout->addAction(QStringLiteral("Reset layout only")), &QAction::triggered, this,
                [this, terminal_split] {
            nav_->set_expanded_width(258);
            nav_->set_mode(NavigationMode::Expanded);
            nav_settings_->remove(QStringLiteral("terminalLayout"));
            if (terminal_split) terminal_split->setSizes({240, 500, 260});
        });
        // Keep the safety control next to the navigation toggle. The
        // breadcrumb is allowed to elide on narrow laptop widths; placing
        // Halt after it made the action fall into the toolbar overflow menu.
        auto* halt = bar->addAction(QStringLiteral("Halt controls"));
        connect(halt, &QAction::triggered, this, [this] {
            show_page(2);
            auto* panel = terminal_->halt();
            terminal_->show_halt_controls();
            panel->setFocus(Qt::ShortcutFocusReason);
            QTimer::singleShot(0, this, [this, panel] {
                if (pages_->currentIndex() != 2) return;
                if (auto* viewport = findChild<QScrollArea*>(QStringLiteral("workspaceViewport")))
                    viewport->ensureWidgetVisible(panel, 8, 8);
            });
        });
        // Search has one visible home in the sidebar. Its shortcut remains
        // global so hiding navigation never makes search unreachable.
        auto* search = new QAction(QStringLiteral("Find workspace"), this);
        search->setShortcut(QKeySequence(QStringLiteral("Ctrl+K")));
        connect(search, &QAction::triggered, this, [this] { nav_->show_search(); });
        addAction(search);
        nav_favourite_ = bar->addAction(QStringLiteral("☆ Save page"));
        connect(nav_favourite_, &QAction::triggered, this, [this] { nav_->toggle_favourite(); });
        auto* menu = menuBar()->addMenu(QStringLiteral("Workspaces"));
        connect(menu, &QMenu::aboutToShow, this, [this, menu, layout] {
            nav_->populate_menu(menu);
            menu->addSeparator();
            menu->addMenu(layout);
        });
        nav_location_ = new QLabel(bar);
        nav_location_->setMinimumWidth(0);
        nav_location_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        nav_location_->setMaximumWidth(360);
        nav_location_->setStyleSheet(QStringLiteral("color:#E3A34A;font-weight:600;padding:0 6px;"));
        bar->addWidget(nav_location_);
        auto* toolbar_spacer = new QWidget(bar);
        toolbar_spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        bar->addWidget(toolbar_spacer);
        mode_badge_ = new QLabel(QStringLiteral("  PAPER  "), bar);
        mode_badge_->setObjectName(QStringLiteral("tradingModeBadge"));
        mode_badge_->setAccessibleName(QStringLiteral("Trading mode: paper"));
        mode_badge_->setToolTip(QStringLiteral(
            "Paper mode. This desktop has no broker order transport linked."));
        mode_badge_->setStyleSheet(QStringLiteral(
            "color:#D9EAF2;background:#1C2B36;border:1px solid #2F4A5A;"
            "border-radius:11px;font-weight:700;padding:3px 10px;"));
        bar->addWidget(mode_badge_);
        connection_badge_ = new QLabel(QStringLiteral("  FYERS —  ·  KITE —  "), bar);
        connection_badge_->setObjectName(QStringLiteral("brokerConnectionBadge"));
        connection_badge_->setAccessibleName(QStringLiteral("Verified broker connections"));
        connection_badge_->setToolTip(QStringLiteral(
            "Only fresh, service-verified read-only evidence is shown as connected."));
        connection_badge_->setStyleSheet(QStringLiteral(
            "color:#AFC0C9;background:#161B22;border:1px solid #30363D;"
            "border-radius:11px;padding:3px 10px;"));
        bar->addWidget(connection_badge_);
        nav_->on_state_changed = [this] {
            refresh_workspace_controls();
            nav_->save_state(*nav_settings_);
        };
        show_page(nav_->current_page());
        nav_->save_state(*nav_settings_);
        nav_settings_->sync();
        if (!nav_->recovery_notice().isEmpty())
            statusBar()->showMessage(nav_->recovery_notice(), 15000);
        if (nav_settings_->status() != QSettings::NoError)
            statusBar()->showMessage(QStringLiteral("Navigation preferences could not be saved."), 15000);
        // Scope chrome colours here; do not restyle Atlas or trading-widget internals.
        const auto chrome = QStringLiteral(
            "QToolBar,QMenuBar{background:#0D1117;color:#E6EDF3;border:0;border-bottom:1px solid #21262D;}"
            "QMenu{background:#161B22;color:#E6EDF3;border:1px solid #30363D;border-radius:8px;padding:6px;}"
            "QToolBar{spacing:6px;padding:6px 10px;}"
            "QToolButton{color:#C9D1D9;background:transparent;border:1px solid transparent;border-radius:6px;padding:5px 10px;}"
            "QToolButton:hover,QMenuBar::item:selected,QMenu::item:selected{background:#21262D;color:#F0B765;}"
            "QToolButton:checked{background:#2D333B;color:#F0B765;}"
            "QToolButton:focus{border:1px solid #E3A34A;}"
            "QToolButton:disabled{color:#6E7681;}"
            "QMenuBar::item{padding:6px 10px;border-radius:6px;}"
            "QMenu::item{padding:7px 24px 7px 12px;border-radius:6px;}"
            "QComboBox,QPushButton{color:#E6EDF3;background:#161B22;border:1px solid #30363D;border-radius:6px;padding:5px 10px;}"
            "QComboBox:hover,QPushButton:hover{border-color:#484F58;}"
            "QLabel{color:#E6EDF3;}");
        bar->setStyleSheet(chrome);
        if (replay_bar) replay_bar->setStyleSheet(chrome);
        menuBar()->setStyleSheet(chrome);
        menu->setStyleSheet(chrome);
        layout->setStyleSheet(chrome);
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

        // P11Q-03. The ladder sits BESIDE the grid, in a splitter, because
        // the card puts it "on the tick grid" and depth is read against the
        // trade that just printed. It is given nullptr today: `ReplayTick`
        // carries a last price and a quantity and no depth at all, and the
        // ladder says exactly that rather than drawing an empty book — which
        // would be indistinguishable from an instrument with nothing resting.
        auto* split = new QSplitter(Qt::Horizontal, page);
        split->addWidget(view_);
        ladder_ = new DepthLadder;
        split->addWidget(ladder_);
        split->setStretchFactor(0, 3);
        split->setStretchFactor(1, 1);
        v->addWidget(split, 1);
        ladder_->show_book(nullptr);

        pages_->addWidget(page);
    }

    void build_chart() {
        chart_ = new ChartWidget;

        // WHAT THE CHART IS LOOKING AT. The synthetic replay and the real
        // series on disk are DIFFERENT KINDS OF THING -- one is a tape being
        // bucketed live, the other is bars that were already aggregated by
        // whoever produced the file -- so they are separate entries rather
        // than one list with a hidden mode.
        // P32-05. FIVE ENTRIES FOR A TREE THAT HOLDS TWENTY SERIES.
        //
        // BANKNIFTY arrived at P25 and India VIX got its full intraday set at
        // P30, and this list still offered NIFTY at three intervals plus one
        // VIX daily. The Data Flow page's inventory listed everything on disk
        // while the chart could draw a quarter of it.
        //
        // BUILT FROM THE SAME PARTITION TABLE THE INVENTORY WALKS, so the two
        // cannot drift: a series that appears in one appears in the other, and
        // an entry here for a directory that does not exist is impossible
        // rather than merely unlikely.
        source_ = new QComboBox;
        rebuild_sources();
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

        // P32-05. ZOOM, AS BUTTONS AS WELL AS A WHEEL.
        //
        // The wheel and the drag are discoverable only by trying them. A
        // chart of 8,755 daily candles in 1,500 pixels is five candles a
        // pixel, so every wick, body and colour on it is a lie -- and a
        // reader who does not know the wheel does anything has no way to see
        // an individual bar at all.
        auto* zoom_out = new QPushButton(QStringLiteral("\u2212"));
        auto* zoom_in = new QPushButton(QStringLiteral("+"));
        auto* zoom_all = new QPushButton(QStringLiteral("Fit"));
        for (QPushButton* b : {zoom_out, zoom_in}) {
            b->setFixedWidth(34);
            b->setStyleSheet(QStringLiteral(
                "QPushButton{background:#21262D;color:#C9D1D9;"
                "border:1px solid #30363D;padding:4px;font-size:15px;"
                "font-weight:600;}"
                "QPushButton:hover{background:#30363D;}"));
        }
        zoom_all->setStyleSheet(QStringLiteral(
            "QPushButton{background:#21262D;color:#C9D1D9;"
            "border:1px solid #30363D;padding:4px 10px;}"
            "QPushButton:hover{background:#30363D;}"));
        zoom_out->setToolTip(QStringLiteral("Zoom out  (wheel down)"));
        zoom_in->setToolTip(QStringLiteral("Zoom in  (wheel up)"));
        zoom_all->setToolTip(QStringLiteral("Show the whole series"));
        controls->addWidget(zoom_out);
        controls->addWidget(zoom_in);
        controls->addWidget(zoom_all);
        connect(zoom_in, &QPushButton::clicked, this,
                [this] { chart_->zoom_in(); });
        connect(zoom_out, &QPushButton::clicked, this,
                [this] { chart_->zoom_out(); });
        connect(zoom_all, &QPushButton::clicked, this,
                [this] { chart_->zoom_reset(); });

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

    /// Rebuild the chart's source list from what is ON DISK.
    ///
    /// P34-01. It was built once, in the constructor. altair_kite_update can
    /// create a partition that did not exist when the window opened -- and
    /// the list only offers directories it can see, deliberately, because an
    /// entry that selects to an empty chart reads as a broken chart rather
    /// than as absent data. So a series that arrives mid-session was
    /// invisible until a restart.
    ///
    /// The selection is preserved across the rebuild by its DATA, not its
    /// index: the whole point is that entries may have been inserted, so an
    /// index means something different afterwards.
    void rebuild_sources() {
        const QString keep = source_->currentData().toString();
        source_->clear();
        source_->addItem(QStringLiteral("Synthetic replay"), QString());
        {
            struct Iv { const char* dir; const char* label; long long secs; };
            const Iv ivs[] = {{"1m", "1-minute", 60},
                              {"5m", "5-minute", 300},
                              {"15m", "15-minute", 900},
                              {"60m", "60-minute", 3600},
                              {"1d", "daily", 86400}};
            std::size_t nsym = 0;
            const QuantSymbol* syms = quant_symbols(nsym);
            for (std::size_t i = 0; i < nsym; ++i) {
                for (const Iv& iv : ivs) {
                    const QString rel = QStringLiteral("spot/%1/%2")
                                            .arg(QLatin1String(syms[i].dir),
                                                 QLatin1String(iv.dir));
                    // ONLY WHAT IS ACTUALLY THERE. An entry that selects to an
                    // empty chart is worse than a shorter list: it reads as a
                    // broken chart rather than as absent data.
                    if (!QDir(QStringLiteral(ALTAIR_DATASET_DIR "/") + rel)
                             .exists()) {
                        continue;
                    }
                    source_->addItem(
                        QStringLiteral("%1 %2")
                            .arg(QLatin1String(syms[i].label),
                                 QLatin1String(iv.label)),
                        QStringLiteral("%1|%2").arg(rel).arg(iv.secs));
                }
            }
            // The near future, which is neither spot nor selectable above.
            if (QDir(QStringLiteral(ALTAIR_DATASET_DIR "/fut/nifty/1d"))
                    .exists()) {
                source_->addItem(QStringLiteral("NIFTY future daily"),
                                 QStringLiteral("fut/nifty/1d|86400"));
            }
        }
        const int at = source_->findData(keep);
        if (at >= 0) { source_->setCurrentIndex(at); }
    }

    void build_pages() {
        // P32-01. The terminal: the watchlist, the ticket and the halt
        // control, composed rather than rewritten. See desktop/terminal.hpp
        // for why it is a composition and what the one wire between them is.
        terminal_ = new TerminalPage(role_, user_);
        pages_->addWidget(terminal_);
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

            // P30-03. THE DIAGRAM SHOWS THE PIPE AND SAID NOTHING ABOUT WHAT
            // IS IN IT.
            //
            // A reader could see every stage light up and still not know
            // whether the tree holds a week of data or eleven years, or which
            // instruments. The inventory is COUNTED FROM DISK on each build
            // of this page rather than typed, because a hand-maintained list
            // of what has been ingested is the first thing to go stale --
            // NIFTY BANK arrived and this page would not have mentioned it.
            v->addWidget(new QLabel(QStringLiteral(
                "<h3>What is actually in dataset/</h3>")));
            auto* inv = new QLabel(page);
            inv->setStyleSheet(QStringLiteral(
                "font-family:Consolas,monospace;color:#D6DBDF;"
                "background:#11171C;padding:8px;"));
            inv->setText(dataset_inventory());
            v->addWidget(inv);

            pages_->addWidget(page);
        }

        // P20-02. The wiring table is still here, at the bottom of the
        // Kite panel's API-surface tab -- a number is only worth as much as
        // the pipe it came through -- but the page now leads with the
        // account, which is an operator's question rather than a developer's.
        pages_->addWidget(new KitePanel(
            QStringLiteral(ALTAIR_ACCOUNT_FILE),
            QStringLiteral(ALTAIR_SESSION_FILE)));
        pages_->addWidget(new CostPanel);
        pages_->addWidget(new AnalyticsPanel);

        pages_->addWidget(new ChainPanel);

        // P25-02. Was a blocked_page. Still true that no filings are
        // ingested, and the page says so first — but leaving the arithmetic
        // unexercised because the INPUTS are typed was the wrong call: a DCF
        // is built from typed assumptions in any case, and the sensitivity
        // grid is the actual deliverable.
        auto* dcf_page = new ComputePage(
            QStringLiteral("VALUE — DCF — P11Q-05"),
            QStringLiteral("Value it, then show how little that means"),
            this);
        connect(dcf_page->button(), &QPushButton::clicked, this,
                [dcf_page] { dcf_page->set_text(dcf_report()); });
        pages_->addWidget(dcf_page);

        // P32-03. WAS A BLOCKED PAGE, AND HAD STOPPED BEING TRUE.
        //
        // Its text said the aggregator "has no members to combine until the
        // Models page above has something in it". That was right when it was
        // written and wrong from P16 onward: the gradient-boosted spot
        // forecast fits on real closes at several lag counts under two growth
        // strategies, and every one of those produces a net edge and a
        // standard error. They were simply never combined.
        //
        // A blocked page that is no longer blocked is worse than a wrong
        // number: it says the machinery is untested while the machinery sits
        // there, and it removes the one page that would show what P8-11 and
        // P8-12 exist to show.
        auto* agg_page = new ComputePage(
            QStringLiteral("AGGREGATOR — P8-11 / P8-12"),
            QStringLiteral("Combine six real members, and price the interval"),
            this);
        (void)agg_page->add_symbols();
        connect(agg_page->button(), &QPushButton::clicked, this,
                [agg_page] {
                    const QString sym = agg_page->symbol();
                    const QString root = QStringLiteral(ALTAIR_DATASET_DIR);
                    const QString sub = QStringLiteral("/spot/%1/1d/").arg(sym);
                    const QByteArray label = QStringLiteral("%1 daily")
                        .arg(quant_symbol_by_dir(sym).label).toUtf8();
                    agg_page->run_async(
                        QStringLiteral("family=ensemble; model=aggregator; symbol=%1; interval=1d")
                            .arg(sym),
                        [root, sub, label](const ModelJobContext& context) {
                            context.progress(10, QStringLiteral("Loading members"));
                            if (context.cancelled()) return QString{};
                            context.progress(35, QStringLiteral("Fitting member forecasts"));
                            return aggregator_report(root, sub, label.constData());
                        });
                });
        pages_->addWidget(agg_page);

        audit_panel_ = new AuditPanel;
        pages_->addWidget(audit_panel_);

        // ---- P19-01..06 ------------------------------------------------
        //
        // Six pages for Phases 13-18. Each one computes ON DEMAND, behind a
        // button, and each calls the same function its acceptance test calls.
        //
        // On demand because P11Q learned it the expensive way: a model fit run
        // at construction froze the window for seven seconds before anything
        // was drawn, and the freeze arrived late enough to read as a different
        // bug. And the SAME function because a number that exists in two
        // places has already contradicted itself twice in this UI.
        const QString ds = QStringLiteral(ALTAIR_DATASET_DIR);

        auto* exec_page = new ComputePage(
            QStringLiteral("EXECUTION — P13"),
            QStringLiteral("Show schedules and the Almgren-Chriss limits"),
            this);
        connect(exec_page->button(), &QPushButton::clicked, this,
                [exec_page] { exec_page->set_text(execution_report()); });
        pages_->addWidget(exec_page);

        auto* vol_page = new ComputePage(
            QStringLiteral("VOLATILITY — P14"),
            QStringLiteral("Fit GARCH and race it against EWMA "
                           "(a few seconds)"),
            this);
        connect(vol_page->button(), &QPushButton::clicked, this,
                [vol_page, ds] {
                    const QString sym = vol_page->symbol();
                    vol_page->run_async(
                        QStringLiteral("family=volatility; model=garch-vs-ewma; symbol=%1; interval=1d")
                            .arg(sym),
                        [ds, sym](const ModelJobContext& context) {
                            context.progress(20, QStringLiteral("Loading returns"));
                            return volatility_report(ds, sym);
                        });
                });
        (void)vol_page->add_symbols();
        pages_->addWidget(vol_page);

        auto* var_page = new ComputePage(
            QStringLiteral("RISK — P15"),
            QStringLiteral("Compute VaR and Expected SlippageShortfall three ways"),
            this);
        connect(var_page->button(), &QPushButton::clicked, this,
                [var_page, ds] {
                    const QString sym = var_page->symbol();
                    var_page->run_async(
                        QStringLiteral("family=risk; model=var-es; symbol=%1; interval=1d")
                            .arg(sym),
                        [ds, sym](const ModelJobContext& context) {
                            context.progress(25, QStringLiteral("Computing loss distribution"));
                            return risk_report(ds, sym);
                        });
                });
        (void)var_page->add_symbols();
        pages_->addWidget(var_page);

        auto* port_page = new ComputePage(
            QStringLiteral("PORTFOLIO — P15"),
            QStringLiteral("Run the optimisers against the 1/N control"),
            this);
        connect(port_page->button(), &QPushButton::clicked, this,
                [port_page] {
                    port_page->run_async(
                        QStringLiteral("family=risk; model=portfolio-optimisers; fixture=typed-demo"),
                        [](const ModelJobContext& context) {
                            context.progress(30, QStringLiteral("Solving constrained weights"));
                            return portfolio_report();
                        });
                });
        pages_->addWidget(port_page);

        auto* ml_page = new ComputePage(
            QStringLiteral("ML — P16"),
            QStringLiteral("Fit gradient-boosted trees "
                           "(a few seconds)"),
            this);
        connect(ml_page->button(), &QPushButton::clicked, this,
                [ml_page] {
                    ml_page->run_async(
                        QStringLiteral("family=machine-learning; model=gbdt; fixture=typed-demo"),
                        [](const ModelJobContext& context) {
                            context.progress(25, QStringLiteral("Boosting trees"));
                            return ml_report();
                        });
                });
        pages_->addWidget(ml_page);

        auto* reg_page = new ComputePage(
            QStringLiteral("REGIMES — P14-06 / P18"),
            QStringLiteral("Fit the HMM, against its noise baseline "
                           "(a few seconds)"),
            this);
        connect(reg_page->button(), &QPushButton::clicked, this,
                [reg_page, ds] {
                    const QString sym = reg_page->symbol();
                    reg_page->run_async(
                        QStringLiteral("family=regime; model=hmm-kmeans; symbol=%1; interval=1d")
                            .arg(sym),
                        [ds, sym](const ModelJobContext& context) {
                            context.progress(20, QStringLiteral("Fitting regime models"));
                            return regime_report(ds, sym);
                        });
                });
        (void)reg_page->add_symbols();
        pages_->addWidget(reg_page);

        // P19-07. ONE horizon per press, not all four. The 5-minute fit is
        // 214,000 rows across five folds and two growth strategies, which is
        // tens of seconds -- a page that ran every horizon on open would
        // freeze the window exactly the way P11Q's startup fit did, and the
        // freeze would arrive late enough to read as a different bug.
        // Fit off the UI thread. The retained terminal PriceClient provides
        // read-only trade updates; each admitted trade runs cached inference.
        pages_->addWidget(new StreamForecastPage(
            terminal_->stream(), QStringLiteral(ALTAIR_DATASET_DIR), this));


        // P21-03. Momentum and mean reversion. The button label says what
        // will be computed rather than "Run", because the order of this page
        // IS the argument: the cost hurdle is shown before any return.
        auto* strat_page = new ComputePage(
            QStringLiteral("STRATEGIES — P21-01/02"),
            QStringLiteral("Cost hurdle, noise control, then real NIFTY"),
            this);
        connect(strat_page->button(), &QPushButton::clicked, this,
                [strat_page, ds] {
                    const QString sym = strat_page->symbol();
                    strat_page->run_async(
                        QStringLiteral("family=statistical-alpha; model=momentum-mean-reversion; symbol=%1; interval=1d")
                            .arg(sym),
                        [ds, sym](const ModelJobContext& context) {
                            context.progress(10, QStringLiteral("Running noise controls"));
                            return strategies_report(ds, sym);
                        });
                });
        (void)strat_page->add_symbols();
        pages_->addWidget(strat_page);

        // P22-03. Reads 140 monthly files to rebuild 2,873 sessions, so it
        // stays behind a button like every other page here.
        auto* on_page = new ComputePage(
            QStringLiteral("OVERNIGHT GAP — P22-01/02"),
            QStringLiteral("Decompose the day, then price the gap"),
            this);
        connect(on_page->button(), &QPushButton::clicked, this,
                [on_page, ds] {
                    const QString sym = on_page->symbol();
                    on_page->run_async(
                        QStringLiteral("family=statistical-alpha; model=overnight-gap; symbol=%1; interval=5m")
                            .arg(sym),
                        [ds, sym](const ModelJobContext& context) {
                            context.progress(15, QStringLiteral("Rebuilding sessions"));
                            return overnight_report(ds, sym);
                        });
                });
        (void)on_page->add_symbols();
        pages_->addWidget(on_page);

        // P23-01. Cheap enough to run on press without a warning.
        auto* opt_page = new ComputePage(
            QStringLiteral("OPTIONS — P23-01"),
            QStringLiteral("Round-trip the pricer, fit a surface, "
                           "find the arbitrage"),
            this);
        connect(opt_page->button(), &QPushButton::clicked, this,
                [opt_page] { opt_page->set_text(options_report()); });
        pages_->addWidget(opt_page);

        auto* basis_page = new ComputePage(
            QStringLiteral("BASIS — P23-03"),
            QStringLiteral("Futures against spot, 11 years"),
            this);
        connect(basis_page->button(), &QPushButton::clicked, this,
                [basis_page, ds] {
                    basis_page->run_async(
                        QStringLiteral("family=statistical-alpha; model=cash-futures-basis; dataset=nifty"),
                        [ds](const ModelJobContext& context) {
                            context.progress(20, QStringLiteral("Aligning cash and futures"));
                            return basis_report(ds);
                        });
                });
        pages_->addWidget(basis_page);

        auto* flag_page = new ComputePage(
            QStringLiteral("FLAGGING — P23-06"),
            QStringLiteral("Has the distribution moved? PSI, KS, "
                           "Page-Hinkley, ADWIN"),
            this);
        connect(flag_page->button(), &QPushButton::clicked, this,
                [flag_page, ds] {
                    const QString sym = flag_page->symbol();
                    flag_page->run_async(
                        QStringLiteral("family=model-governance; model=drift-detectors; symbol=%1; interval=1d")
                            .arg(sym),
                        [ds, sym](const ModelJobContext& context) {
                            context.progress(20, QStringLiteral("Comparing distributions"));
                            return flagging_report(ds, sym);
                        });
                });
        (void)flag_page->add_symbols();
        pages_->addWidget(flag_page);

        auto* micro_page = new ComputePage(
            QStringLiteral("MICROSTRUCTURE — P23-07"),
            QStringLiteral("Imbalance, microprice, VPIN, Kyle's lambda"),
            this);
        connect(micro_page->button(), &QPushButton::clicked, this,
                [micro_page] {
                    micro_page->set_text(microstructure_report());
                });
        pages_->addWidget(micro_page);

        auto* size_page = new ComputePage(
            QStringLiteral("SIZING AND LIMITS — P24-01"),
            QStringLiteral("Size on the lower bound, and what caps it"),
            this);
        connect(size_page->button(), &QPushButton::clicked, this,
                [size_page] { size_page->set_text(sizing_report()); });
        pages_->addWidget(size_page);

        auto* coint_page = new ComputePage(
            QStringLiteral("COINTEGRATION — P24-02"),
            QStringLiteral("Spot against futures, and against noise"),
            this);
        connect(coint_page->button(), &QPushButton::clicked, this,
                [coint_page, ds] {
                    const QString sym = coint_page->symbol();
                    coint_page->run_async(
                        QStringLiteral("family=statistical-alpha; model=cointegration; symbol=%1; interval=1d")
                            .arg(sym),
                        [ds, sym](const ModelJobContext& context) {
                            context.progress(20, QStringLiteral("Estimating hedge ratio"));
                            return cointegration_report(ds, sym);
                        });
                });
        (void)coint_page->add_symbols();
        pages_->addWidget(coint_page);

        auto* mem_page = new ComputePage(
            QStringLiteral("MEMORY — P24-03"),
            QStringLiteral("Hurst and EWMA, with their error bars"),
            this);
        connect(mem_page->button(), &QPushButton::clicked, this,
                [mem_page, ds] {
                    const QString sym = mem_page->symbol();
                    mem_page->run_async(
                        QStringLiteral("family=time-series; model=hurst-ewma; symbol=%1; interval=1d")
                            .arg(sym),
                        [ds, sym](const ModelJobContext& context) {
                            context.progress(20, QStringLiteral("Estimating memory"));
                            return memory_report(ds, sym);
                        });
                });
        (void)mem_page->add_symbols();
        pages_->addWidget(mem_page);

        // The callback repaints the pill the instant a session is written,
        // rather than leaving it stale until the next five-second poll. A
        // person who just linked and sees a stale pill concludes it failed and
        // retries with a token that is now spent.
        // P34-01. The callback now does two things: repaint the pill, and
        // rebuild the chart's source list. The updater may have created a
        // partition that did not exist when this window opened -- the list is
        // built from what is ON DISK, so a series that arrives mid-session is
        // invisible until it is rebuilt.
        pages_->addWidget(new KiteLinkPanel(role_, [this] {
            refresh_broker_pill();
            if (broker_page_ != nullptr) broker_page_->refresh();
            rebuild_sources();
            // LAST, because it is the only one that can fail, and it reports
            // in the panel's own log rather than a box over it.
            return reload_tape();
        }));

        auto* neural_page = new ComputePage(
            QStringLiteral("NEURAL TIER — P8-16"),
            QStringLiteral("Run the walk-forward (about 6 s)"),
            this);
        connect(neural_page->button(), &QPushButton::clicked, this,
                [neural_page] {
                    neural_page->run_async(
                        QStringLiteral("family=machine-learning; model=neural-tier; interval=5m; folds=5"),
                        [](const ModelJobContext& context) {
                            context.progress(5, QStringLiteral("Starting neural evaluation process"));
                            return neural_report();
                        });
                });
        pages_->addWidget(neural_page);

        auto* feat_page = new ComputePage(
            QStringLiteral("FEATURES AND KINEMATICS — P26-04"),
            QStringLiteral("Seal a registry, then measure a velocity"),
            this);
        connect(feat_page->button(), &QPushButton::clicked, this,
                [feat_page, ds] {
                    const QString sym = feat_page->symbol();
                    feat_page->run_async(
                        QStringLiteral("family=features; model=registry-kinematics; symbol=%1; interval=1d")
                            .arg(sym),
                        [ds, sym](const ModelJobContext& context) {
                            context.progress(25, QStringLiteral("Building feature measurements"));
                            return features_report(ds, sym);
                        });
                });
        (void)feat_page->add_symbols();
        pages_->addWidget(feat_page);

        auto* arb_page = new QTabWidget(this);
        auto* live_arb = new ArbitrageWorkspace(arb_page);
        live_arb->open_broker_diagnostics = [this] { show_page(34); };
        arb_page->addTab(live_arb, QStringLiteral("Live opportunities"));
        auto* parity_page = new ComputePage(
            QStringLiteral("PARITY AND CALENDAR — P5-06 / P5-07"),
            QStringLiteral("Butterfly, parity, box, and the two alignments"),
            arb_page);
        connect(parity_page->button(), &QPushButton::clicked, parity_page,
                [parity_page] { parity_page->set_text(arbitrage_scans_report()); });
        arb_page->addTab(parity_page, QStringLiteral("Parity & calendar"));
        pages_->addWidget(arb_page);

        // P36-01. THE MODEL ATLAS.
        //
        // It takes a callback that opens a nav page, so a row can be
        // double-clicked to run the thing it describes. The panel does not
        // know this window exists -- same contract as KiteLinkPanel.
        auto* atlas = new AtlasPanel([this](const QString& id) { (void)open_model(id); });
        pages_->addWidget(atlas);
        nav_->open_atlas_model = [this](const QString& model) { (void)open_model(model); };

        // FYERS is the configured primary broker. Keep this page separate
        // from Kite's legacy account/link panels so the operator can see the
        // actual route priority immediately after launch.
        pages_->addWidget(new FyersLinkPanel(role_, [this] {
            refresh_broker_pill();
            if (broker_page_ != nullptr) broker_page_->refresh();
        }));

        // One operator-facing broker page.  The legacy Kite Account, Link
        // Kite and FYERS Primary routes remain stable for scripts and saved
        // workspaces; this page is the consolidated entry point that shows
        // both providers and selects the linked data source transparently.
        broker_page_ = new BrokerPage(role_, [this] {
            refresh_broker_pill();
            if (broker_page_ != nullptr) broker_page_->refresh();
        });
        pages_->addWidget(broker_page_);

        // NAV ROWS AND PAGES MUST BE THE SAME NUMBER, and this is checked
        // rather than trusted.
        //
        // Row N opens page N. If a name is added without a widget -- or a
        // widget without a name -- every page after the gap shifts by one and
        // the window opens the wrong panel with no error anywhere. `--page
        // models` opening the Live Grid was that failure once already, from a
        // different cause, and it cost an afternoon because nothing looked
        // broken.
        //
        // A hard failure at startup rather than a log line: a UI whose nav is
        // off by one is not a UI to trade from, and it should not be possible
        // to run it.
        if (pages_->count() != nav_page_names().size()) {
            QMessageBox::critical(
                nullptr, QStringLiteral("Altair — nav mismatch"),
                QStringLiteral(
                    "%1 nav entries but %2 pages.\n\n"
                    "Row N opens page N, so every page after the gap is "
                    "showing the wrong panel. Fix nav_page_names() or the "
                    "page construction below it.")
                    .arg(nav_page_names().size()).arg(pages_->count()));
            std::abort();
        }

        // P36-01. AND EVERY ATLAS ROW MUST POINT AT A REAL PAGE.
        //
        // The atlas carries a nav index per model so a row can be
        // double-clicked to run the thing it describes. cmake/AtlasAudit.cmake
        // checks the FILE paths but cannot know how many nav pages exist, and
        // this is the one place that holds both facts.
        //
        // A stale index is worse than a missing one: -1 politely does nothing,
        // while an index left behind by a nav change opens the WRONG page and
        // looks like it worked. Same failure the check above exists for.
        for (const auto& r : kAtlasRows) {
            if (r.page < -1 || r.page >= nav_page_names().size()) {
                QMessageBox::critical(
                    nullptr, QStringLiteral("Altair — atlas page out of range"),
                    QStringLiteral(
                        "Model Atlas row \"%1\" names page %2, but there are "
                        "%3 pages.\n\nFix the `page` field in "
                        "desktop/atlas_data.hpp.")
                        .arg(QString::fromUtf8(r.model))
                        .arg(r.page)
                        .arg(nav_page_names().size()));
                std::abort();
            }
        }
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
        fyers_pill_ = new QLabel;
        broker_pill_ = new QLabel;
        who_ = new QLabel;
        phase_ = new QLabel;
        engine_clock_ = new QLabel;
        wall_clock_ = new QLabel;
        progress_ = new QLabel;
        filters_ = new QLabel;

        statusBar()->addWidget(pill_);
        statusBar()->addWidget(fyers_pill_);
        // Broker priority and broker session are separate pills, because they
        // answer different questions. FYERS is the configured primary; Kite
        // remains the secondary session indicator.
        //
        // The first is "is data arriving" and the second is "can we talk to
        // the broker". They are independent: a replay shows LIVE with the
        // broker untouched, and a good Kite session shows NO DATA until
        // something subscribes. One pill covering both would have to pick a
        // colour for a state that is half green.
        statusBar()->addWidget(broker_pill_);
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

    /// Re-read broker metadata and repaint the FYERS-primary/Kite-secondary pills.
    ///
    /// `verified` is 0 here and stays 0 until something actually calls the
    /// API, so this build can reach `Unverified` and never `Authenticated`.
    /// That is correct rather than a limitation: nothing in this process has
    /// made a call, so nothing in this process has grounds to claim the token
    /// works.
    void refresh_broker_pill() {
        const QDateTime service_now = QDateTime::currentDateTimeUtc();
#ifdef ALTAIR_FYERS_ACCOUNT_FILE
        const auto fyers_service = probe_service_snapshot(
            QStringLiteral(ALTAIR_FYERS_ACCOUNT_FILE), broker_view::BrokerId::Fyers,
            service_now);
#else
        const auto fyers_service = probe_service_snapshot(
            QStringLiteral("data/fyers_account.json"), broker_view::BrokerId::Fyers,
            service_now);
#endif
        const FyersState fyers = probe_fyers();
        fyers_pill_->setText(QStringLiteral("  %1  ").arg(
            fyers_service ? service_broker_label(*fyers_service) : fyers_label(fyers.link)));
        fyers_pill_->setStyleSheet(
            QStringLiteral("color:#FFFFFF;background:%1;font-weight:bold;")
                .arg((fyers_service ? broker_colour(fyers_service->authentication)
                                    : fyers_colour(fyers.link)).name()));
        fyers_pill_->setToolTip(fyers_service
            ? QStringLiteral("Fresh service evidence from a verified read-only call.")
            : fyers.detail);

#ifdef ALTAIR_SESSION_FILE
        const QString path = QStringLiteral(ALTAIR_SESSION_FILE);
#else
        const QString path = QStringLiteral("data/kite_session.json");
#endif
        const BrokerState b = probe_broker(path, true);
#ifdef ALTAIR_ACCOUNT_FILE
        const auto kite_service = probe_service_snapshot(
            QStringLiteral(ALTAIR_ACCOUNT_FILE), broker_view::BrokerId::ZerodhaKite,
            service_now);
#else
        const auto kite_service = probe_service_snapshot(
            QStringLiteral("data/kite_account.json"), broker_view::BrokerId::ZerodhaKite,
            service_now);
#endif
        QString text = QStringLiteral("  %1").arg(
            kite_service ? service_broker_label(*kite_service) : broker_label(b.link));
        if (!b.user_id.isEmpty()) {
            text += QStringLiteral(" · %1").arg(b.user_id);
        }
        text += QStringLiteral("  ");
        broker_pill_->setText(text);
        broker_pill_->setStyleSheet(
            QStringLiteral("color:#FFFFFF;background:%1;font-weight:bold;")
                .arg(broker_colour(kite_service ? kite_service->authentication : b.link).name()));
        // The label is the state; the tooltip is what to DO about it. A pill
        // that only shows a colour makes the operator go looking for the
        // reason, which is the moment they stop trusting the pill.
        broker_pill_->setToolTip(kite_service
            ? QStringLiteral("Fresh service evidence from a verified read-only call.")
            : b.detail);
        if (connection_badge_ != nullptr) {
            const QString fyers_text = fyers_service
                ? service_broker_label(*fyers_service) : fyers_label(fyers.link);
            const QString kite_text = kite_service
                ? service_broker_label(*kite_service) : broker_label(b.link);
            connection_badge_->setText(QStringLiteral("  %1  ·  %2  ")
                .arg(fyers_text, kite_text));
            connection_badge_->setToolTip(QStringLiteral(
                "FYERS: %1\nKite: %2\nOnly fresh service verification is treated as connected.")
                .arg(fyers_pill_->toolTip(), broker_pill_->toolTip()));
        }
    }

    void refresh_status() {
        // Rule 10's one live field. Pushed IN rather than the panel reaching
        // for the replayer: the audit page reports what the engine did, and a
        // page that could pull its own position could report a different one
        // from the grid beside it.
        if (audit_panel_ != nullptr) {
            audit_panel_->set_tick_seqno(static_cast<std::uint64_t>(applied_));
        }
        // The broker probe opens a file, so it runs every ~5 s rather than
        // every frame. A session's state changes on the scale of a login, not
        // a repaint, and re-reading it 60 times a second would be a syscall
        // storm to answer a question whose answer is hours old.
        if (since_broker_-- <= 0) {
            since_broker_ = 300;
            refresh_broker_pill();
        }
        const std::int64_t now_ns =
            QDateTime::currentMSecsSinceEpoch() * 1'000'000LL;
        const Liveness live = feed_.liveness(now_ns);
        const std::int64_t age = feed_.age_ns(now_ns);
        // pill_text, not liveness_label: the pill said "LIVE · Replay" in
        // green. See feed_status.hpp.
        pill_->setText(
            QStringLiteral("  %1%2  ")
                .arg(pill_text(live, feed_.source()),
                     age < 0 ? QString()
                             : QStringLiteral(" · last tick %1 ms ago")
                                   .arg(age / 1'000'000)));
        pill_->setStyleSheet(
            QStringLiteral("color:#FFFFFF;background:%1;font-weight:bold;")
                .arg(pill_colour(live, feed_.source()).name()));
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

        // WHICH TAPE, named before anything else on the line. A grid of real
        // prices and one of fabricated prices are indistinguishable, and
        // P11Q-01's first version put real tickers on a random walk.
        const QString src =
            tape_real_
                // THE DATES, NOT JUST THE COUNT. "last 20 sessions" is
                // equally true of a tape ending today and one ending three
                // weeks ago, and the dataset is filled by a manual fetch
                // behind a daily Kite login -- so a stale tape is the normal
                // state, not an edge case, and only the end date shows it.
                ? QStringLiteral(" REAL · 1-min closes · %1 sessions, "
                                 "%2 → %3 ")
                      .arg(tape_sessions_)
                      // IST as a real QTimeZone, which is the idiom already
                      // used in panels.hpp -- not a UTC instant with 19800
                      // seconds bolted on. Qt::UTC as a time-spec is
                      // deprecated in 6.8 and the offset trick reads as a
                      // magic number wherever it appears.
                      .arg(QDateTime::fromMSecsSinceEpoch(
                               tape_first_ns_ / 1'000'000, ist_tz())
                               .toString(QStringLiteral("dd MMM")))
                      .arg(QDateTime::fromMSecsSinceEpoch(
                               tape_last_ns_ / 1'000'000, ist_tz())
                               .toString(QStringLiteral("dd MMM yyyy")))
                : QStringLiteral(" SYNTHETIC · random walk, not market data ");
        progress_->setText(
            src + QStringLiteral("·  tick %1 / %2   ·   unknown-instrument "
                                 "ticks: %3 ")
                      .arg(applied_).arg(count_).arg(model_->unknown_ticks()));
        progress_->setStyleSheet(
            tape_real_ ? QStringLiteral("color:#3FB950;")
                       : QStringLiteral("color:#B9770B;font-weight:bold;"));
        if (!tape_real_ && !tape_error_.isEmpty()) {
            progress_->setToolTip(
                QStringLiteral("No real tape: %1").arg(tape_error_));
        }

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
            const auto engine = QDateTime::fromMSecsSinceEpoch(
                last_ts_ns_ / 1'000'000, ist_tz());
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

    // BORROWED until the first reload, OWNED after it. `ticks_` starts
    // pointing at main()'s vector (the contract on the constructor) and, once
    // reload_tape() succeeds, points into `owned_tape_` instead. The borrowed
    // storage outlives the window either way, so neither pointer can dangle.
    const ReplayTick* ticks_ = nullptr;
    std::size_t count_ = 0;
    std::vector<ReplayTick> owned_tape_;
    std::size_t sessions_ = 0;
    Replayer replayer_;
    MarketClock clock_;

    TickModel* model_ = nullptr;
    FilterProxy* proxy_ = nullptr;
    QTableView* view_ = nullptr;
    WorkspaceNavigation* nav_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    BrokerPage* broker_page_ = nullptr;
    QSettings* nav_settings_ = nullptr;
    QAction* nav_toggle_ = nullptr;
    QAction* nav_favourite_ = nullptr;
    QLabel* nav_location_ = nullptr;

    ChartWidget* chart_ = nullptr;
    QComboBox* source_ = nullptr;
    QComboBox* instrument_ = nullptr;
    QComboBox* bucket_ = nullptr;
    int since_chart_ = 0;

    QPushButton* play_ = nullptr;
    QComboBox* speed_ = nullptr;
    QSlider* scrub_ = nullptr;

    QLabel* pill_ = nullptr;
    QLabel* fyers_pill_ = nullptr;
    QLabel* broker_pill_ = nullptr;
    QLabel* mode_badge_ = nullptr;
    QLabel* connection_badge_ = nullptr;
    int since_broker_ = 0;
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
