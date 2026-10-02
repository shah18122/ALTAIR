// CX06 N1-N3: isolated native Qt navigation, no accounts, network or engine.
#include "../workspace_navigation.hpp"
#include <QApplication>
#include <QSet>
#include <QTemporaryDir>
#include <cstdio>

namespace {
int failures = 0;
void check(bool ok, const char* description) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", description);
    if (!ok) ++failures;
}
void key(QWidget* widget, int code) {
    QKeyEvent event(QEvent::KeyPress, code, Qt::NoModifier);
    QApplication::sendEvent(widget, &event);
}
void routes(QMenu* menu, QList<QAction*>& out) {
    for (auto* action : menu->actions()) {
        if (action->menu()) routes(action->menu(), out);
        else if (action->data().isValid()) out.append(action);
    }
}
using namespace altair::ui;
void registry_contract() {
    const QStringList expected{
        "Live Grid", "Chart", "Terminal", "Models", "Data Flow", "Kite Account",
        "Cost", "Analytics", "Pair Trading", "Value — DCF", "Aggregator", "Audit Trail",
        "Execution", "Volatility", "Risk — VaR", "Portfolio", "ML — Trees", "Regimes",
        "Forecast", "Strategies", "Overnight Gap", "Options", "Basis", "Flagging",
        "Microstructure", "Sizing & Limits", "Cointegration", "Memory", "Link Kite",
        "Neural", "Features", "Arbitrage", "Model Atlas", "FYERS Primary", "Brokers",
        "Demo Trading"};
    check(nav_page_names() == expected, "all legacy labels plus FYERS, Brokers and Demo Trading are registered");
    QSet<QString> ids;
    QSet<int> order;
    for (int i = 0; i < expected.size(); ++i) {
        ids.insert(nav_page_id(i));
        check(nav_page_index(expected[i]) == i && nav_page_index(nav_page_id(i)) == i,
              "label and stable ID resolve to the same legacy page");
    }
    for (const int i : kNavigationOrder) order.insert(i);
    check(ids.size() == 36 && order.size() == 31 && !ids.contains(QString{}),
          "36 legacy identities and 31 visible destinations");
    for (const int hidden : {0, 1, 5, 28, 33})
        check(!order.contains(hidden) && !nav_visible(hidden), "removed routes never appear in traversal");
    check(nav_page_index(QStringLiteral("  MODEL   ATLAS ")) == 32 &&
          nav_page_index(QStringLiteral("market.terminal")) == 2 &&
          nav_page_index(QStringLiteral("strategies.ratio-spread")) == 8 &&
          nav_page_index(QStringLiteral("Ratio Spread")) == 8 &&
          nav_page_index(QStringLiteral("accounts.fyers")) == 33 &&
          nav_page_index(QStringLiteral("accounts.brokers")) == 34 &&
          nav_page_index(QStringLiteral("strategies.demo-trading")) == 35 &&
          nav_page_index(QStringLiteral("not a page")) == -1 &&
          nav_page_id(-1).isEmpty() && nav_page_id(36).isEmpty(), "lookup boundary behavior");
}
void tree_and_menu() {
    WorkspaceNavigation nav;
    nav.resize(258, 720);
    nav.show();
    QApplication::processEvents();
    int invoked = -1, calls = 0;
    nav.activate = [&](int index) { invoked = index; ++calls; };
    auto* tree = nav.findChild<QTreeWidget*>();
    tree->setCurrentItem(tree->topLevelItem(1));
    check(calls == 0 && nav.current_page() == 2, "group selection never routes to page zero");
    tree->topLevelItem(1)->setExpanded(false);
    key(tree, Qt::Key_Right);
    check(tree->topLevelItem(1)->isExpanded() && calls == 0, "native right arrow expands group without routing");
    key(tree, Qt::Key_Left);
    check(!tree->topLevelItem(1)->isExpanded(), "native left arrow collapses group");
    for (int g = 1; g < tree->topLevelItemCount(); ++g) {
        auto* group = tree->topLevelItem(g);
        group->setExpanded(true);
        for (int j = 0; j < group->childCount(); ++j) {
            auto* item = group->child(j);
            tree->setCurrentItem(item);
            check(invoked == item->data(0, Qt::UserRole).toInt(), "every tree page routes by legacy index");
        }
    }
    QMenu menu;
    nav.populate_menu(&menu);
    QList<QAction*> actions;
    routes(&menu, actions);
    check(actions.size() == 31 + nav.recent_pages().size(), "seven native submenus and recent routes expose visible pages");
    int checked = 0;
    for (auto* action : actions) checked += action->isChecked() ? 1 : 0;
    check(checked >= 1, "workspace menus visibly mark the current route");
    for (auto* action : actions) {
        action->trigger();
        check(invoked == action->data().toInt(), "submenu action routes to its page");
    }
    for (int i = 0; i < 50; ++i) nav.populate_menu(&menu);
    check(menu.findChildren<QMenu*>(QString{}, Qt::FindDirectChildrenOnly).size() == 9,
          "reopening workspace menu does not accumulate submenu objects");
    const auto buttons = nav.findChildren<QToolButton*>();
    check(buttons.size() == 7, "compact rail has seven groups");
    int compact_routes = 0;
    for (auto* button : buttons) {
        QList<QAction*> entries;
        routes(button->menu(), entries);
        compact_routes += static_cast<int>(entries.size());
        for (auto* entry : entries) {
            entry->trigger();
            check(invoked == entry->data().toInt(), "compact popup retains page mapping");
        }
    }
    check(compact_routes == 31, "all visible pages reachable in compact mode");
    int compact_checked = 0;
    for (auto* button : buttons)
        for (auto* entry : button->menu()->actions())
            compact_checked += entry->isChecked() ? 1 : 0;
    check(compact_checked == 1, "compact navbar marks exactly one current page");
}
void modes_and_favourites() {
    WorkspaceNavigation nav;
    nav.set_mode(NavigationMode::Compact);
    nav.toggle_visibility();
    check(nav.mode() == NavigationMode::Hidden && nav.isHidden(), "hidden sidebar consumes no layout");
    nav.toggle_visibility();
    check(nav.mode() == NavigationMode::Compact && !nav.isHidden(), "restore remembers compact mode");
    nav.set_mode(NavigationMode::Expanded);
    nav.toggle_visibility();
    nav.toggle_visibility();
    check(nav.mode() == NavigationMode::Expanded, "restore remembers expanded mode");
    nav.select_page(32);
    nav.toggle_favourite();
    check(nav.is_favourite(), "save current page");
    nav.toggle_favourite();
    check(!nav.is_favourite(), "saved page toggle removes without duplicates");
    nav.select_page(1000);
    check(nav.current_page() == 32, "out-of-range programmatic routing ignored");
}
void settings_contract() {
    QTemporaryDir directory;
    check(directory.isValid(), "isolated temporary settings available");
    QSettings settings(directory.filePath(QStringLiteral("nav.ini")), QSettings::IniFormat);
    WorkspaceNavigation nav;
    nav.select_page(32);
    nav.toggle_favourite();
    nav.set_mode(NavigationMode::Compact);
    nav.toggle_visibility();
    nav.save_state(settings);
    settings.sync();
    WorkspaceNavigation restored;
    restored.restore_state(settings);
    check(restored.current_page() == 32 && restored.is_favourite() &&
          restored.mode() == NavigationMode::Hidden, "settings round trip preserves hidden page and favourites");
    restored.toggle_visibility();
    check(restored.mode() == NavigationMode::Compact, "round trip preserves last visible mode");
    settings.setValue(QStringLiteral("favourites"), QStringList{"models.atlas", "models.atlas"});
    restored.restore_state(settings);
    restored.save_state(settings);
    check(settings.value(QStringLiteral("favourites")).toStringList().size() == 1,
          "duplicate saved IDs are canonicalized");
    for (const QString& bad : {QStringLiteral("removed.page"), QStringLiteral("32"), QStringLiteral("Model Atlas")}) {
        settings.setValue(QStringLiteral("page"), bad);
        restored.restore_state(settings);
        check(restored.current_page() == 2 && restored.mode() == NavigationMode::Expanded &&
              !restored.recovery_notice().isEmpty(), "invalid persisted identity visibly recovers to Terminal");
        restored.save_state(settings);
    }
    check(settings.childGroups().contains(QStringLiteral("recovery")), "malformed settings backup preserved");
    for (const QVariant& bad : {QVariant(-1), QVariant(3), QVariant(QStringLiteral("broken"))}) {
        settings.setValue(QStringLiteral("mode"), bad);
        restored.restore_state(settings);
        check(restored.mode() == NavigationMode::Expanded && !restored.recovery_notice().isEmpty(),
              "invalid mode visibly recovers");
        restored.save_state(settings);
    }
    settings.setValue(QStringLiteral("expanded"), QStringList{"unknown"});
    restored.restore_state(settings);
    check(!restored.recovery_notice().isEmpty(), "unknown group ID rejected");
    restored.save_state(settings);
    settings.setValue(QStringLiteral("expanded"), 123);
    restored.restore_state(settings);
    check(!restored.recovery_notice().isEmpty(), "non-list group setting visibly recovers");
    restored.save_state(settings);
    settings.remove(QStringLiteral("favourites"));
    restored.restore_state(settings);
    check(!restored.recovery_notice().isEmpty(), "incomplete settings schema visibly recovers");
}
void search_keyboard() {
    int invoked = -1;
    NavigationSearch search({"models.atlas"}, [&](int i) { invoked = i; });
    search.show();
    auto* query = search.findChild<QLineEdit*>();
    auto* results = search.findChild<QListWidget*>();
    check(results->count() == 31, "search exposes only canonical visible destinations");
    for (const auto& alias : {QStringLiteral("Kite"), QStringLiteral("FYERS"), QStringLiteral("Link Kite")}) {
        query->setText(alias);
        check(results->count() == 1 && results->item(0)->data(Qt::UserRole).toInt() == 34,
              "old broker labels search to the single Brokers destination");
    }
    query->setText(QStringLiteral("market.chart"));
    check(results->count() == 1 && results->item(0)->data(Qt::UserRole).toInt() == 2,
          "removed chart search redirects to Terminal");
    query->setText(QStringLiteral("MODELS.ATLAS"));
    check(results->count() == 1, "search supports stable IDs without case sensitivity");
    key(query, Qt::Key_Return);
    check(invoked == 32, "Enter activates matching page");
    search.show();
    query->setText(QStringLiteral("favourite"));
    check(results->count() == 1, "saved pages are searchable");
    query->setText(QStringLiteral("zz-no-matches"));
    key(query, Qt::Key_Return);
    check(search.isVisible() && results->count() == 0, "empty search never activates page zero");
    query->clear();
    const int first = results->currentRow();
    key(query, Qt::Key_Down);
    check(results->currentRow() == first + 1, "Down moves selection while typing");
    key(query, Qt::Key_Up);
    check(results->currentRow() == first, "Up moves selection while typing");
    key(query, Qt::Key_Escape);
    check(!search.isVisible(), "Escape dismisses search without changing fullscreen");
}
void schema_migration_and_layout() {
    QTemporaryDir directory;
    QSettings settings(directory.filePath("migration.ini"), QSettings::IniFormat);
    NavigationState old;
    old.page = 32;
    old.mode = NavigationMode::Compact;
    old.favourites = {"models.atlas"};
    write_navigation_state(settings, old);
    settings.setValue("schema", 1);
    settings.remove("expandedWidth");
    settings.remove("recent");
    bool recovered = false;
    const auto migrated = read_navigation_state(settings, recovered);
    check(!recovered && migrated.page == 32 && migrated.mode == NavigationMode::Compact &&
          migrated.favourites == old.favourites && migrated.expanded_width == 258 &&
          migrated.recent == QStringList{"models.atlas"}, "schema1 migrates without losing preferences");
    WorkspaceNavigation nav;
    nav.restore_state(settings);
    check(nav.set_expanded_width(320) && nav.expanded_width() == 320, "valid sidebar width accepted");
    check(!nav.set_expanded_width(219) && !nav.set_expanded_width(361) &&
          nav.expanded_width() == 320, "invalid widths refuse without mutation");
    nav.save_state(settings);
    WorkspaceNavigation restored;
    restored.restore_state(settings);
    check(restored.expanded_width() == 320 && restored.current_page() == 32,
          "schema2 width and page survive roundtrip");
    for (const int bad : {-1, 219, 361, 1000000}) {
        settings.setValue("expandedWidth", bad);
        restored.restore_state(settings);
        check(restored.expanded_width() == 258 && !restored.recovery_notice().isEmpty(),
              "corrupt saved width visibly recovers");
        nav.save_state(settings);
    }
    settings.setValue("recent", QStringList{"missing.page"});
    restored.restore_state(settings);
    check(!restored.recovery_notice().isEmpty(), "unknown recent identity refused");
    nav.save_state(settings);
    settings.setValue("schema", 99);
    restored.restore_state(settings);
    check(!restored.recovery_notice().isEmpty(), "unknown future schema refused");
}
void recent_route_order() {
    WorkspaceNavigation nav;
    for (int i = 0; i < 36; ++i) nav.select_page(i);
    nav.select_page(2);
    nav.select_page(32);
    const auto recent = nav.recent_pages();
    check(recent.size() == 31 && recent[0] == "models.atlas" && recent[1] == "market.terminal",
          "recent history contains 31 canonical identities without duplicates");
    QMenu menu;
    nav.populate_menu(&menu);
    QMenu* recent_menu = nullptr;
    for (auto* action : menu.actions())
        if (action->text() == "Recent pages") recent_menu = action->menu();
    check(recent_menu && recent_menu->actions().size() == 31, "recent submenu contains full bounded visible history");
    int activated = -1;
    nav.activate = [&](int index) { activated = index; };
    if (recent_menu) for (auto* action : recent_menu->actions()) {
        action->trigger();
        check(activated == action->data().toInt(), "recent route retains legacy destination");
    }
}
void legacy_route_migration() {
    QTemporaryDir directory;
    QSettings settings(directory.filePath("legacy.ini"), QSettings::IniFormat);
    for (const int old_page : {0, 1, 5, 28, 33}) {
        NavigationState state;
        state.page = old_page;
        state.favourites = {"market.grid", "market.chart", "accounts.kite", "accounts.connections", "accounts.fyers"};
        state.recent = {nav_page_id(old_page), "market.grid", "accounts.kite", "accounts.fyers"};
        state.expanded_width = 320;
        write_navigation_state(settings, state);
        WorkspaceNavigation nav;
        nav.restore_state(settings);
        check(nav.current_page() == nav_destination(old_page) && nav.expanded_width() == 320,
              "saved alias migrates without losing sidebar width");
        check(!nav.recovery_notice().isEmpty(), "migration notice is visible");
        nav.save_state(settings);
        check(settings.value("favourites").toStringList()
              == QStringList{"market.terminal", "accounts.brokers"}, "aliased favourites deduplicate");
        for (const auto& id : nav.recent_pages())
            check(nav_visible(nav_page_index(id)), "recent history removes retired identities");
    }
}
void mouse_resize() {
    WorkspaceNavigation nav;
    nav.resize(258, 720);
    nav.show();
    QApplication::processEvents();
    auto* edge = nav.findChild<QWidget*>("navigationResizeHandle");
    check(edge && edge->isVisible() && edge->width() == 6, "visible sidebar drag edge");
    if (!edge) return;
    const QPointF local{3, 100};
    const QPointF start = edge->mapToGlobal(local.toPoint());
    QMouseEvent press(QEvent::MouseButtonPress, local, start, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(edge, &press);
    QMouseEvent move(QEvent::MouseMove, local, start + QPointF{62, 0}, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(edge, &move);
    check(nav.expanded_width() == 320, "mouse drag changes width in logical pixels");
    QMouseEvent huge(QEvent::MouseMove, local, start + QPointF{1000, 0}, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(edge, &huge);
    check(nav.expanded_width() == 320, "out of bounds drag refuses without mutation");
    QMouseEvent release(QEvent::MouseButtonRelease, local, start + QPointF{62, 0}, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(edge, &release);
    key(edge, Qt::Key_Left);
    check(nav.expanded_width() == 310, "keyboard resizes focused edge");
    QTemporaryDir directory;
    QSettings settings(directory.filePath("drag.ini"), QSettings::IniFormat);
    nav.save_state(settings);
    WorkspaceNavigation restored;
    restored.restore_state(settings);
    check(restored.expanded_width() == 310, "dragged width persists");
    nav.set_mode(NavigationMode::Compact);
    check(edge->isHidden(), "drag edge hidden in compact mode");
    nav.set_mode(NavigationMode::Expanded);
    QMouseEvent reset(QEvent::MouseButtonDblClick, local, start, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(edge, &reset);
    check(nav.expanded_width() == 258, "double click restores default width");
}
void atlas_catalogue_routes() {
    WorkspaceNavigation nav;
    QMenu menu;
    nav.populate_menu(&menu);
    QString selected;
    nav.open_atlas_model = [&](QString model) { selected = model; };
    int entries = 0, absent = 0;
    const auto menus = menu.findChildren<QMenu*>();
    for (auto* child : menus) for (auto* action : child->actions()) {
        if (!action->property("atlasModel").isValid()) continue;
        ++entries;
        const QString raw_model = action->property("atlasModel").toString();
        check(raw_model.startsWith(QStringLiteral("atlas.")),
              "Atlas menu item carries a stable model identity");
        if (raw_model.contains('&'))
            check(action->text().contains("&&") && action->toolTip().size() > 20,
                  "model mnemonics are escaped while stable model names remain available");
        if (action->text().contains("[ABSENT]")) ++absent;
        action->trigger();
        check(nav.current_page() == 32 && selected == raw_model,
              "catalogue item opens its exact Atlas description");
    }
    int expected_absent = 0;
    for (const auto& row : kAtlasRows) if (row.status == AtlasStatus::Absent) ++expected_absent;
    check(entries == static_cast<int>(kAtlasCount) && absent == expected_absent,
          "all Atlas models including absent entries exposed in navigation");
}
} // namespace
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setFont(QFont(QStringLiteral("Segoe UI"), 9));
    registry_contract();
    tree_and_menu();
    modes_and_favourites();
    settings_contract();
    search_keyboard();
    schema_migration_and_layout();
    recent_route_order();
    legacy_route_migration();
    mouse_resize();
    atlas_catalogue_routes();
    if (argc == 2) {
        WorkspaceNavigation nav;
        nav.select_page(32);
        nav.toggle_favourite();
        nav.resize(258, 760);
        nav.show();
        QApplication::processEvents();
        check(nav.grab().save(QString::fromLocal8Bit(argv[1])), "native navigation render saved");
    }
    std::printf("Navigation: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
