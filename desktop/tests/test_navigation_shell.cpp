// Native shell integration; isolated files/settings, empty tape, no network actions.
#include "../auth.hpp"
#include "../main_window.hpp"
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QToolButton>
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
void shell_laptop_geometry(altair::ui::MainWindow& window, altair::ui::WorkspaceNavigation* nav) {
    auto* viewport = window.findChild<QScrollArea*>("workspaceViewport");
    check(viewport != nullptr, "workspace has a scrolling viewport");
    const QList<QSize> sizes{{1024, 640}, {1280, 720}, {1366, 768}, {1920, 1080}};
    for (const auto mode : {altair::ui::NavigationMode::Expanded, altair::ui::NavigationMode::Compact}) {
        nav->set_mode(mode);
        for (const auto size : sizes) {
            window.resize(size);
            for (const int page : {2, 32}) {
                window.show_page(page);
                QApplication::processEvents();
                if (window.size() != size)
                    std::printf("Geometry requested %dx%d actual %dx%d min %dx%d\n", size.width(), size.height(),
                        window.width(), window.height(), window.minimumWidth(), window.minimumHeight());
                check(window.size() == size && window.statusBar()->isVisible(),
                      "active page cannot enlarge requested laptop window; status stays visible");
            }
        }
    }
    nav->set_mode(altair::ui::NavigationMode::Expanded);
    window.resize(1280, 720);
    for (int page = 0; page < static_cast<int>(altair::ui::kNavigationPages.size()); ++page) {
        window.show_page(page);
        QApplication::processEvents();
        check(window.size() == QSize(1280, 720), "all 39 page layouts stay within laptop viewport");
    }
    window.show_page(2);
    QApplication::processEvents();
    if (viewport) {
        auto* horizontal = viewport->horizontalScrollBar();
        horizontal->setValue(horizontal->maximum());
        check(horizontal->value() == horizontal->maximum(), "wide terminal content remains reachable by scrolling");
        horizontal->setValue(0);
    }
}
void halt_chord(altair::ui::MainWindow& window) {
    using altair::ui::HaltChord;
    auto* chord = window.halt_chord_filter();
    auto* terminal = window.findChild<altair::ui::TerminalPage*>();
    check(chord != nullptr && terminal != nullptr && !terminal->halted(), "the shell watches for F11+F12; not halted at start");
    if (chord == nullptr || terminal == nullptr) return;
    chord->key(Qt::Key_F11, true, false);
    check(!terminal->halted(), "F11 alone does nothing");
    chord->key(Qt::Key_F11, false, false);
    chord->key(Qt::Key_F12, true, false);
    check(!terminal->halted(), "F12 alone does nothing");
    chord->key(Qt::Key_F11, true, false);
    check(terminal->halted() && QFile::exists(QStringLiteral("data/kill_request.json")) && !terminal->live_on(),
          "F11+F12 together writes the halt request; LIVE is off");
    auto* badge = window.findChild<QLabel*>(QStringLiteral("haltBadge"));
    QApplication::processEvents();
    QElapsedTimer waited;
    waited.start();
    while (waited.elapsed() < 1200) QApplication::processEvents(QEventLoop::AllEvents, 50);
    check(badge != nullptr && badge->isVisible() && terminal->live_switch()->text().contains(QStringLiteral("HALTED")),
          "the status bar and the LIVE switch say HALTED");
    // Holding the chord does not fire twice; nothing is resumed without a reason.
    chord->key(Qt::Key_F12, true, true);
    window.set_resume_answer([] { return QString(); });
    chord->key(Qt::Key_F11, false, false);
    chord->key(Qt::Key_F12, false, false);
    chord->key(Qt::Key_F12, true, false);
    chord->key(Qt::Key_F11, true, false);
    check(terminal->halted(), "the chord again offers resume; no reason, stays halted");
    chord->key(Qt::Key_F11, false, false);
    chord->key(Qt::Key_F12, false, false);
    window.set_resume_answer([] { return QStringLiteral("checked the book"); });
    chord->key(Qt::Key_F11, true, false);
    chord->key(Qt::Key_F12, true, false);
    check(!terminal->halted() && !QFile::exists(QStringLiteral("data/kill_request.json"))
              && QFile::exists(QStringLiteral("data/kill_request.json.cleared.json")),
          "resume with a reason clears the halt and records who and why");
    chord->key(Qt::Key_F11, false, false);
    chord->key(Qt::Key_F12, false, false);
}
void workstation(altair::ui::MainWindow& window, QStackedWidget* stack) {
    // Ctrl+K: type, Enter, there.
    auto* palette = window.palette();
    check(palette != nullptr, "Ctrl+K has a command palette");
    if (palette == nullptr) return;
    window.open_palette();
    palette->type(QStringLiteral("model atl"));
    check(palette->first_title() == QStringLiteral("Model Atlas") && palette->run_first() && stack->currentIndex() == 32,
          "typing a few letters finds the Model Atlas; Enter opens it");
    window.open_palette();
    palette->type(QStringLiteral("daily find"));
    check(palette->run_first() && stack->currentIndex() == 40, "the palette reaches Daily findings");
    window.open_palette();
    palette->type(QStringLiteral("garch"));
    check(palette->shown_count() > 0, "Atlas models are in the palette");
    palette->type(QStringLiteral("zzzzqqq"));
    check(palette->shown_count() == 0 && !palette->run_first(), "no match runs nothing");
    palette->hide();
    bool trades = false;
    window.open_palette();
    for (const char* q : {"halt", "live on", "buy", "sell", "order"}) {
        palette->type(QString::fromLatin1(q));
        const QString t = palette->first_title().toLower();
        trades = trades || t.startsWith(QStringLiteral("halt")) || t.contains(QStringLiteral("live on"));
    }
    palette->hide();
    check(!trades, "nothing in the palette halts, arms LIVE or places an order");

    // Layouts.
    auto* nav = static_cast<altair::ui::WorkspaceNavigation*>(window.findChild<QWidget*>(QStringLiteral("workspaceNavigation")));
    window.apply_layout(altair::ui::builtin_layouts()[1]);
    check(stack->currentIndex() == 2 && nav->mode() == altair::ui::NavigationMode::Compact && window.news_ticker()->isVisible(),
          "the Trading layout opens the Terminal with the sidebar collapsed and the news on");
    window.apply_layout(altair::ui::builtin_layouts()[2]);
    check(stack->currentIndex() == 39 && nav->mode() == altair::ui::NavigationMode::Expanded && !window.news_ticker()->isVisible(),
          "the Review layout opens Backtests, sidebar out, news off");
    window.show_page(QStringLiteral("strategies.basis"));
    check(window.save_current_layout(QStringLiteral("  my   basis ")) && !window.save_current_layout(QStringLiteral("   ")),
          "a layout saves under a name; an empty name is refused");
    const auto mine = window.saved_layouts();
    check(mine.size() == 1 && mine[0].name == QStringLiteral("my basis") && mine[0].page == QStringLiteral("strategies.basis")
              && !mine[0].news,
          "the saved layout remembers the page and the news bar");
    window.apply_layout(altair::ui::builtin_layouts()[0]);
    check(stack->currentIndex() == 40 && window.news_ticker()->isVisible(), "the Morning layout opens Daily findings with the news");
    window.apply_layout(mine[0]);
    check(stack->currentIndex() == 22 && !window.news_ticker()->isVisible(), "a saved layout restores");
    window.apply_layout(altair::ui::builtin_layouts()[1]);
    nav->set_mode(altair::ui::NavigationMode::Expanded);

    // PAPER / LIVE / HALTED on every page.
    auto* terminal = window.findChild<altair::ui::TerminalPage*>();
    auto* edge = window.findChild<QFrame*>(QStringLiteral("tradingModeBar"));
    auto* sw = window.findChild<QPushButton*>(QStringLiteral("tradingModeSwitch"));
    QApplication::processEvents();
    window.show_page(QStringLiteral("models.atlas"));
    QElapsedTimer waited;
    waited.start();
    while (waited.elapsed() < 700) QApplication::processEvents(QEventLoop::AllEvents, 50);
    check(window.trading_mode() == QStringLiteral("PAPER") && edge != nullptr && edge->isVisible()
              && edge->property("mode").toString() == QStringLiteral("paper") && sw != nullptr && sw->isVisible()
              && sw->text().startsWith(QStringLiteral("PAPER")),
          "away from the Terminal, the edge and the status bar say PAPER");
    window.set_halt_answer([] { return false; });
    if (sw != nullptr) sw->click();
    check(terminal != nullptr && !terminal->halted(), "the switch asks first; No keeps trading");
    window.set_halt_answer([] { return true; });
    if (sw != nullptr) sw->click();
    waited.restart();
    while (waited.elapsed() < 700) QApplication::processEvents(QEventLoop::AllEvents, 50);
    check(terminal != nullptr && terminal->halted() && window.trading_mode() == QStringLiteral("HALTED")
              && edge->property("mode").toString() == QStringLiteral("halted") && sw->text().contains(QStringLiteral("HALTED"))
              && sw->toolTip().contains(QStringLiteral("status bar switch")),
          "Yes halts from any page; the edge and switch say HALTED and why");
    window.set_resume_answer([] { return QStringLiteral("checked again"); });
    if (sw != nullptr) sw->click();
    waited.restart();
    while (waited.elapsed() < 700) QApplication::processEvents(QEventLoop::AllEvents, 50);
    check(terminal != nullptr && !terminal->halted() && sw->text().startsWith(QStringLiteral("PAPER"))
              && sw->toolTip().contains(QStringLiteral("Last halt")),
          "the switch resumes with a reason; its tooltip keeps the last halt");
}
void atlas_back(altair::ui::MainWindow& window, QStackedWidget* stack) {
    const altair::ui::AtlasRow* forecast = nullptr;
    for (const auto& r : altair::ui::kAtlasRows) if (r.page == 18) { forecast = &r; break; }
    check(forecast != nullptr, "an Atlas row opens the Forecast page");
    if (forecast == nullptr) return;
    const QString id = QStringLiteral("atlas.%1").arg(QString::number(static_cast<qulonglong>(altair::ui::atlas_route_id(*forecast)), 16));
    window.show_page(32);
    check(window.open_model(id) && stack->currentIndex() == 18, "a model opens its page from the Atlas");
    QApplication::processEvents();
    check(window.atlas_bar()->isVisible() && window.findChild<QPushButton*>(QStringLiteral("atlasPageInfo")) != nullptr,
          "the model page carries Back to Model Atlas and the info button");
    check(!altair::ui::atlas_info_html(*forecast).isEmpty()
              && QString::fromUtf8(altair::ui::atlas_info(*forecast).trained).size() > 10,
          "the info says how the model was trained, on what data and what it does live");
    QKeyEvent back(QEvent::KeyPress, Qt::Key_Backspace, Qt::NoModifier);
    QApplication::sendEvent(&window, &back);
    check(stack->currentIndex() == 32 && !window.atlas_bar()->isVisible(), "Backspace returns to the Model Atlas");
    QKeyEvent again(QEvent::KeyPress, Qt::Key_Backspace, Qt::NoModifier);
    QApplication::sendEvent(&window, &again);
    check(stack->currentIndex() == 32, "Backspace with no model open stays put");
    window.show_page(4);
    check(!window.atlas_bar()->isVisible(), "a page opened from the sidebar has no Atlas bar");
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
        altair::ui::MainWindow window(nullptr, 0, altair::ui::Role::Admin,
                                      QStringLiteral("cx06-test"), 1);
        window.resize(1440, 900);
        window.show();
        QApplication::processEvents();
        auto* nav = static_cast<altair::ui::WorkspaceNavigation*>(
            window.findChild<QWidget*>(QStringLiteral("workspaceNavigation")));
        auto* stack = window.findChild<QStackedWidget*>(QStringLiteral("workspacePages"));
        check(nav && stack, "real shell owns navigation and pages");
        if (!nav || !stack) return 1;
        check(window.windowTitle() == QStringLiteral("ALTAIR : GAUTAM GLOBAL LLP : SMIT SHAH"), "the title names the firm and the licensee");
        check(window.findChild<QToolBar*>(QStringLiteral("workspaceToolbar")) == nullptr && !window.menuBar()->isVisible()
                  && window.findChild<QLabel*>(QStringLiteral("tradingModeBadge")) == nullptr,
              "full screen: no workspace toolbar, menu bar or mode badge");
        bool halt_action = false, nav_toggle = false, save_page = false;
        for (auto* action : window.findChildren<QAction*>()) {
            halt_action = halt_action || action->text() == QStringLiteral("Halt controls");
            nav_toggle = nav_toggle || action->text().contains(QStringLiteral("navigation"));
            save_page = save_page || action->text().contains(QStringLiteral("Save page"));
        }
        check(!halt_action && !nav_toggle && !save_page, "Halt controls, Hide navigation and Save page are gone");
        check(nav->findChild<QPushButton*>(QStringLiteral("navMinimise")) && nav->findChild<QPushButton*>(QStringLiteral("navClose")),
              "minimise and close sit beside the ALTAIR name");
        check(stack->count() == static_cast<int>(altair::ui::kNavigationPages.size()) && stack->currentIndex() == 2, "fresh workspace opens existing Terminal");
        for (int i = 0; i < 39; ++i) {
            const int destination = altair::ui::nav_destination(i);
            check(window.show_page(altair::ui::nav_page_id(i)) && stack->currentIndex() == destination &&
                  nav->current_page() == destination, "legacy routes resolve to canonical page stack destinations");
        }
        check(!window.show_page(QStringLiteral("invalid.page")) && stack->currentIndex() == 38,
              "invalid route does not select another page");
        check(stack->widget(37)->findChild<QLabel*>(QStringLiteral("aboutText")) != nullptr
                  && stack->widget(37)->findChild<QLabel*>(QStringLiteral("aboutText"))->text().contains(QStringLiteral("LIFETIME")),
              "About carries the lifetime licence");
        check(stack->widget(38)->objectName() == QStringLiteral("startPage"), "Start here is the last page of the stack");
        window.show_page(QStringLiteral("strategies.threshold"));
        check(stack->currentWidget()->findChild<QWidget*>(QStringLiteral("strategyRecord")) != nullptr,
              "Threshold page shows the OHL record");
        window.show_page(QStringLiteral("strategies.parity-calendar"));
        check(stack->currentWidget()->findChild<QWidget*>(QStringLiteral("arbRecord_NSE \u2194 BSE record")) != nullptr
                  && stack->currentWidget()->findChild<QWidget*>(QStringLiteral("arbRecord_Option arbitrage record")) != nullptr
                  && stack->currentWidget()->findChildren<QWidget*>(QStringLiteral("strategyRecord")).size() == 2,
              "Arbitrage shows the NSE-BSE and option arbitrage records, demo and real");
        for (const char* page : {"strategies.overview", "strategies.overnight", "strategies.pair-trading", "strategies.basis"}) {
            window.show_page(QString::fromLatin1(page));
            auto* tabs = qobject_cast<QTabWidget*>(stack->currentWidget());
            check(tabs != nullptr && tabs->objectName() == QStringLiteral("tradeLogTabs") && tabs->tabText(0) == QStringLiteral("Study")
                      && tabs->tabText(tabs->count() - 1) == QStringLiteral("Saved runs"),
                  "a strategy page opens on its study with its trades in tabs beside it");
        }
        window.show_page(QStringLiteral("strategies.overview"));
        check(window.compute_current(), "the study's own button still runs from the trade tabs");
        window.show_page(QStringLiteral("models.findings"));
        check(stack->currentWidget()->objectName() == QStringLiteral("dailyFindings"), "Daily findings has its page");
        window.show_page(QStringLiteral("system.work-log"));
        check(stack->currentWidget()->objectName() == QStringLiteral("workLog"), "Work log has its page");
        window.show_page(QStringLiteral("system.data-flow"));
        check(stack->currentWidget()->findChild<QWidget*>(QStringLiteral("liveDataflow")) != nullptr,
              "Data Flow shows today's live path");
        bool f11 = false, global_escape = false;
        for (auto* action : window.actions()) {
            f11 = f11 || action->shortcuts().contains(QKeySequence(Qt::Key_F11));
            global_escape = global_escape || action->shortcuts().contains(QKeySequence(Qt::Key_Escape));
        }
        check(!f11 && !global_escape, "F11 is no longer a full-screen toggle; no window Escape action");
        halt_chord(window);
        atlas_back(window, stack);
        workstation(window, stack);
        window.show_page(QStringLiteral("accounts.brokers"));
        QApplication::processEvents();
        auto* brokers = window.findChild<QWidget*>(QStringLiteral("brokerControlCenter"));
        auto* broker_tabs = window.findChild<QTabWidget*>(QStringLiteral("brokerWorkspaceTabs"));
        check(brokers && broker_tabs && broker_tabs->count() >= 1,
              "Brokers control plane exposes its tabs");
        check(brokers && brokers->findChild<QLabel*>(QStringLiteral("fyersConnectionState")),
              "FYERS card keeps its attributed connection state");
        {
            auto* legacy = brokers ? brokers->findChild<QToolButton*>(QStringLiteral("legacyKiteToggle")) : nullptr;
            auto* kite_state = brokers ? brokers->findChild<QLabel*>(QStringLiteral("kiteConnectionState")) : nullptr;
            const auto kite_tabs_visible = [&] {
                int n = 0;
                for (int i = 0; i < broker_tabs->count(); ++i)
                    n += broker_tabs->isTabVisible(i) && broker_tabs->tabText(i).contains(QStringLiteral("Legacy")) ? 1 : 0;
                return n;
            };
            check(legacy != nullptr && kite_state != nullptr && !legacy->isChecked() && !kite_state->isVisible()
                      && kite_tabs_visible() == 0,
                  "Kite is stashed in a collapsed Legacy: Kite corner: no card, no tabs");
            if (legacy != nullptr && kite_state != nullptr) {
                legacy->setChecked(true);
                QApplication::processEvents();
                check(kite_state->isVisible() && kite_tabs_visible() == 2, "opened, the legacy Kite card and its two tabs show");
                legacy->setChecked(false);
            }
        }
        shell_laptop_geometry(window, nav);
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
            window.show_page(QStringLiteral("system.about"));
            QApplication::processEvents();
            check(window.grab().save(QString::fromLocal8Bit(argv[1]) + ".about.png"), "about image saved");
            window.show_page(QStringLiteral("system.data-flow"));
            QApplication::processEvents();
            check(window.grab().save(QString::fromLocal8Bit(argv[1]) + ".dataflow.png"), "data flow image saved");
            window.show_page(2);
            window.resize(1600, 900);
            QApplication::processEvents();
            check(window.grab().save(QString::fromLocal8Bit(argv[1]) + ".terminal.png"), "terminal image saved");
        }
    }
    QDir::setCurrent(cwd);
    std::printf("Navigation shell: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
