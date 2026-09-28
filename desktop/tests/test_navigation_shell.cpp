// Native shell integration; isolated files/settings, empty tape, no network actions.
#include "../auth.hpp"
#include "../main_window.hpp"
#include <QTemporaryDir>
#include <QScreen>
#include <cstdio>

namespace {
int failures = 0;
void check(bool ok, const char* description) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", description);
    if (!ok) ++failures;
}
void forecast_connection_continuity() {
    using altair::ui::ForecastStreamContinuity;
    ForecastStreamContinuity state(true);
    for (std::size_t i = 0; i < ForecastStreamContinuity::kRequiredFreshIntervals; ++i)
        state.completed_interval();
    check(state.ready(), "live forecast can warm after 20 completed intervals");
    check(!state.observe_connection(true), "duplicate connected status is not a new epoch");
    check(state.observe_connection(false) && !state.ready() && state.fresh_intervals() == 0,
          "disconnect invalidates all pre-disconnect warm-up bars");
    check(!state.observe_connection(false), "duplicate disconnected status is not a new epoch");
    check(state.observe_connection(true) && !state.ready(),
          "reconnect starts a new epoch with no inherited warm-up");
    check(state.consume_reanchor() && !state.consume_reanchor(),
          "only the first valid post-reconnect tick consumes the re-anchor");
    for (std::size_t i = 0; i < ForecastStreamContinuity::kRequiredFreshIntervals - 1; ++i)
        state.completed_interval();
    check(!state.ready(), "19 post-reconnect completed intervals cannot emit a forecast");
    state.completed_interval();
    check(state.ready(), "forecast warm-up completes only after 20 new intervals");
}
void terminal_layout_roundtrip() {
    QTemporaryDir directory;
    QSettings settings(directory.filePath("layout.ini"), QSettings::IniFormat);
    QSplitter split(Qt::Horizontal);
    for (int i = 0; i < 3; ++i) split.addWidget(new QWidget);
    split.resize(1000, 200);
    split.show();
    QApplication::processEvents();
    split.setSizes({200, 500, 300});
    check(altair::ui::save_terminal_layout(&split, settings), "terminal layout saves three positive sizes");
    const auto saved = split.sizes();
    split.setSizes({300, 300, 400});
    check(altair::ui::restore_terminal_layout(&split, settings) && split.sizes() == saved,
          "terminal pane sizes roundtrip");
    for (const auto& bad : {QStringList{"0", "200", "300"}, QStringList{"100001", "200", "300"},
                           QStringList{"bad", "200", "300"}, QStringList{"100", "200"}}) {
        settings.setValue("terminalLayout/sizes", bad);
        check(!altair::ui::restore_terminal_layout(&split, settings) && split.sizes() == saved,
              "malformed terminal sizes refuse without changing geometry");
    }
    settings.setValue("terminalLayout/id", "unknown-layout");
    check(!altair::ui::restore_terminal_layout(&split, settings), "unknown terminal layout identity refused");
}
void permanent_controls(QMainWindow& window, QToolBar* bar, QAction* toggle, QAction* halt) {
    const auto visible = [bar](QAction* action) {
        const auto rect = bar->actionGeometry(action);
        return rect.isValid() && bar->rect().contains(rect);
    };
    check(bar->isVisible() && visible(toggle) && visible(halt) && window.statusBar()->isVisible(),
          "recovery, Halt controls and status fit in laptop chrome");
}
void shell_laptop_geometry(altair::ui::MainWindow& window, altair::ui::WorkspaceNavigation* nav,
                           QToolBar* bar, QAction* toggle, QAction* halt) {
    auto* viewport = window.findChild<QScrollArea*>("workspaceViewport");
    check(viewport != nullptr, "workspace has a scrolling viewport");
    const QList<QSize> sizes{{1024, 640}, {1280, 720}, {1366, 768}, {1920, 1080}};
    for (const auto mode : {altair::ui::NavigationMode::Expanded, altair::ui::NavigationMode::Compact,
                           altair::ui::NavigationMode::Hidden}) {
        nav->set_mode(mode);
        for (const auto size : sizes) {
            window.resize(size);
            for (const int page : {2, 32}) {
                window.show_page(page);
                QApplication::processEvents();
                if (window.size() != size)
                    std::printf("Geometry requested %dx%d actual %dx%d min %dx%d\n", size.width(), size.height(),
                        window.width(), window.height(), window.minimumWidth(), window.minimumHeight());
                check(window.size() == size, "active page cannot enlarge requested laptop window");
                permanent_controls(window, bar, toggle, halt);
            }
        }
    }
    nav->set_mode(altair::ui::NavigationMode::Expanded);
    window.resize(1280, 720);
    for (int page = 0; page < 35; ++page) {
        window.show_page(page);
        QApplication::processEvents();
        check(window.size() == QSize(1280, 720), "all35 page layouts stay within laptop viewport");
    }
    window.show_page(2);
    QApplication::processEvents();
    if (viewport) {
        auto* horizontal = viewport->horizontalScrollBar();
        horizontal->setValue(horizontal->maximum());
        check(horizontal->value() == horizontal->maximum(), "wide terminal content remains reachable by scrolling");
        horizontal->setValue(0);
        halt->trigger();
        QApplication::processEvents();
        auto* terminal = window.findChild<altair::ui::TerminalPage*>();
        auto* panel = terminal->halt();
        check(viewport->viewport()->rect().contains(
                  panel->mapTo(viewport->viewport(), panel->rect().center())),
              "Halt navigation brings offscreen controls into the viewport");
    }
}
void scale_variants() {
    // CTest launches the same geometry assertions with100%,125%,200% device scales.
    auto* screen = QApplication::primaryScreen();
    bool ok = false;
    const double scale = qEnvironmentVariable("QT_SCALE_FACTOR", "1").toDouble(&ok);
    check(screen && ok && screen->devicePixelRatio() == scale,
          "geometry tests actually run at the requested device scale");
}
}
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setFont(QFont(QStringLiteral("Segoe UI"), 9));
    QTemporaryDir temporary;
    if (!temporary.isValid()) return 1;
    const auto cwd = QDir::currentPath();
    if (!QDir::setCurrent(temporary.path())) return 1;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    forecast_connection_continuity();
    terminal_layout_roundtrip();
    scale_variants();
    {
        altair::ui::MainWindow window(nullptr, 0, altair::ui::Role::Staff,
                                      QStringLiteral("cx06-test"), 1);
        window.resize(1440, 900);
        window.show();
        QApplication::processEvents();
        auto* nav = static_cast<altair::ui::WorkspaceNavigation*>(
            window.findChild<QWidget*>(QStringLiteral("workspaceNavigation")));
        auto* stack = window.findChild<QStackedWidget*>(QStringLiteral("workspacePages"));
        auto* toolbar = window.findChild<QToolBar*>(QStringLiteral("workspaceToolbar"));
        check(nav && stack && toolbar, "real shell owns navigation, pages and permanent toolbar");
        if (!nav || !stack || !toolbar) return 1;
        check(stack->count() == 35 && stack->currentIndex() == 2, "fresh workspace opens existing Terminal");
        for (int i = 0; i < 35; ++i) {
            const int destination = altair::ui::nav_destination(i);
            check(window.show_page(altair::ui::nav_page_id(i)) && stack->currentIndex() == destination &&
                  nav->current_page() == destination, "legacy routes resolve to canonical page stack destinations");
        }
        check(!window.show_page(QStringLiteral("invalid.page")) && stack->currentIndex() == 34,
              "invalid route does not select another page");
        QAction* toggle = nullptr;
        QAction* halt = nullptr;
        for (auto* action : toolbar->actions()) {
            if (action->text() == QStringLiteral("Hide navigation")) toggle = action;
            if (action->text() == QStringLiteral("Halt controls")) halt = action;
        }
        check(toggle && halt, "navigation recovery and Halt controls actions exist");
        bool layout_button = false;
        for (auto* button : toolbar->findChildren<QToolButton*>())
            layout_button = layout_button || button->text() == QStringLiteral("Layout");
        check(!layout_button, "Layout toolbar button removed");
        if (toggle) {
            toggle->trigger();
            QApplication::processEvents();
            check(nav->isHidden() && toolbar->isVisible() && window.statusBar()->isVisible() &&
                  toggle->text() == QStringLiteral("Show navigation"),
                  "hidden navigation preserves recovery control and market status");
            toggle->trigger();
            check(!nav->isHidden(), "permanent action restores navigation");
        }
        if (halt) {
            halt->trigger();
            auto* terminal = window.findChild<altair::ui::TerminalPage*>();
            bool exposed = false;
            for (auto* tabs : terminal->findChildren<QTabWidget*>())
                if (tabs->currentWidget() == terminal->halt()) exposed = true;
            check(stack->currentIndex() == 2 && exposed, "Halt controls opens existing terminal tab");
            check(!QFile::exists(QStringLiteral("data/kill_request.json")),
                  "navigation to Halt controls does not request a halt");
        }
        bool f11 = false, global_escape = false;
        for (auto* action : window.actions()) {
            f11 = f11 || action->shortcuts().contains(QKeySequence(Qt::Key_F11));
            global_escape = global_escape || action->shortcuts().contains(QKeySequence(Qt::Key_Escape));
        }
        check(f11 && !global_escape, "F11 retained without a conflicting window Escape action");
        check(!toolbar->toggleViewAction()->isEnabled(), "toolbar hide menu cannot strand recovery control");
        window.show_page(QStringLiteral("accounts.brokers"));
        QApplication::processEvents();
        auto* brokers = window.findChild<QWidget*>(QStringLiteral("brokerControlCenter"));
        auto* broker_tabs = window.findChild<QTabWidget*>(QStringLiteral("brokerWorkspaceTabs"));
        check(brokers && broker_tabs && broker_tabs->count() == 6,
              "Brokers control plane exposes overview, accounts, activity and both link flows");
        check(brokers && brokers->findChild<QLabel*>(QStringLiteral("fyersConnectionState"))
                      && brokers->findChild<QLabel*>(QStringLiteral("kiteConnectionState")),
              "web-style broker cards retain independently attributed connection states");
        check(brokers && brokers->findChildren<QPushButton*>(QStringLiteral("brokerPrimaryAction")).size() == 2
                      && brokers->findChildren<QPushButton*>(QStringLiteral("brokerSecondaryAction")).size() == 2,
              "each broker card offers direct account and connection actions");
        window.show_page(32);
        if (!nav->is_favourite()) nav->toggle_favourite();
        nav->set_expanded_width(320);
        nav->set_mode(altair::ui::NavigationMode::Compact);
        QAction* reset = nullptr;
        for (auto* action : window.findChildren<QAction*>())
            if (action->text() == "Reset layout only") reset = action;
        check(reset != nullptr, "layout reset action exists");
        if (reset) reset->trigger();
        check(nav->current_page() == 32 && nav->is_favourite() && nav->expanded_width() == 258 &&
              nav->mode() == altair::ui::NavigationMode::Expanded,
              "layout reset preserves current page and favourites");
        if (toggle && halt) shell_laptop_geometry(window, nav, toolbar, toggle, halt);
        if (argc == 2) {
            QSettings clean(temporary.filePath(QStringLiteral("capture.ini")), QSettings::IniFormat);
            nav->restore_state(clean);
            window.show_page(QStringLiteral("models.atlas"));
            nav->findChild<QTreeWidget*>()->scrollToTop();
            window.resize(1280, 720);
            QApplication::processEvents();
            check(window.grab().save(QString::fromLocal8Bit(argv[1])), "real native shell image saved");
            window.show_page(QStringLiteral("accounts.brokers"));
            QApplication::processEvents();
            check(window.grab().save(QString::fromLocal8Bit(argv[1]) + ".brokers.png"),
                  "broker control plane image saved");
            window.show_page(2);
            nav->set_mode(altair::ui::NavigationMode::Hidden);
            window.resize(1024, 640);
            if (halt) halt->trigger();
            QApplication::processEvents();
            check(window.grab().save(QString::fromLocal8Bit(argv[1]) + ".terminal.png"),
                  "small terminal with hidden navigation image saved");
        }
    }
    QDir::setCurrent(cwd);
    std::printf("Navigation shell: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
