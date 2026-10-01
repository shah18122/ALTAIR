// Native cold-path workspace navigation. No market events or broker handles.
#pragma once
#include "navigation_search.hpp"
#include "navigation_state.hpp"
#include "navigation_resize_handle.hpp"
#include "atlas_data.hpp"
#include <QDateTime>
#include <QMenu>
#include <QPushButton>
#include <QSignalBlocker>
#include <QToolButton>
#include <QTreeWidget>
#include <array>
#include <functional>

namespace altair::ui {
class WorkspaceNavigation final : public QWidget {
public:
    std::function<void(int)> activate;
    std::function<void(QString)> open_atlas_model;
    std::function<void()> on_state_changed;

    explicit WorkspaceNavigation(QWidget* parent = nullptr) : QWidget(parent) {
        setObjectName(QStringLiteral("workspaceNavigation"));
        setAccessibleName(QStringLiteral("Workspace navigation"));
        setStyleSheet(QStringLiteral(
            "#workspaceNavigation{background:#11181D;color:#D5DDE2;}"
            "#navBrand{background:#182229;border:1px solid #2A3943;"
            "border-radius:7px;padding:8px;}"
            "#navBrandTitle{color:#F0B765;font-size:15px;font-weight:700;"
            "letter-spacing:1px;}"
            "#navBrandSub{color:#8295A0;font-size:10px;letter-spacing:1px;}"
            "#navSummary{color:#7F929D;font-size:10px;padding:5px 2px;}"
            "#navSearch{color:#D5DDE2;background:#202D35;border:1px solid #344751;"
            "border-radius:5px;padding:7px;text-align:left;}"
            "#navSearch:hover{background:#2A3A44;border-color:#B47A3A;}"
            "#navSearch:pressed{background:#172229;}"
            "#workspaceTree{background:#11181D;color:#D5DDE2;border:0;"
            "font-size:12px;outline:0;}"
            "#workspaceTree::item{height:29px;padding:2px 4px;}"
            "#workspaceTree::item:selected{background:#2B3A44;color:#F0B765;"
            "border-left:2px solid #D89143;}"
            "#workspaceTree::item:hover{background:#1D2A32;}"
            "#workspaceNavigation QToolButton{color:#C4D0D6;background:#1B2830;"
            "border:1px solid #2F414B;border-radius:4px;padding:8px 4px;"
            "text-align:left;}"
            "#workspaceNavigation QToolButton:hover{background:#253640;"
            "border-color:#B47A3A;}"
            "#workspaceNavigation QToolButton:checked{color:#F0B765;"
            "background:#26343B;border-color:#B47A3A;}"));
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(8, 10, 8, 8);
        auto* brand = new QWidget(this);
        brand->setObjectName(QStringLiteral("navBrand"));
        auto* brand_layout = new QVBoxLayout(brand);
        brand_layout->setContentsMargins(8, 6, 8, 6);
        heading_ = new QLabel(QStringLiteral("ALTAIR / WORKSPACE"), brand);
        heading_->setObjectName(QStringLiteral("navBrandTitle"));
        auto* subheading = new QLabel(QStringLiteral("CONTROL ROOM  ·  PAPER-SAFE UI"), brand);
        subheading->setObjectName(QStringLiteral("navBrandSub"));
        brand_layout->addWidget(heading_);
        brand_layout->addWidget(subheading);
        layout->addWidget(brand);
        search_button_ = new QPushButton(QStringLiteral("⌕  Find workspace"), this);
        search_button_->setObjectName(QStringLiteral("navSearch"));
        search_button_->setAccessibleName(QStringLiteral("Search workspaces"));
        search_button_->setToolTip(QStringLiteral(
            "Search pages and saved favourites · Ctrl+K"));
        layout->addWidget(search_button_);
        summary_ = new QLabel(QStringLiteral("%1 workspaces  ·  7 groups")
                             .arg(kNavigationOrder.size()), this);
        summary_->setObjectName(QStringLiteral("navSummary"));
        layout->addWidget(summary_);
        tree_ = new QTreeWidget(this);
        tree_->setObjectName(QStringLiteral("workspaceTree"));
        tree_->setAccessibleName(QStringLiteral("Workspaces grouped by function"));
        tree_->setHeaderHidden(true);
        tree_->setIndentation(14);
        tree_->setUniformRowHeights(true);
        tree_->setExpandsOnDoubleClick(false);
        tree_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        layout->addWidget(tree_, 1);
        compact_ = new QWidget(this);
        auto* rail = new QVBoxLayout(compact_);
        rail->setContentsMargins(0, 0, 0, 0);
        for (std::size_t g = 0; g < kNavigationGroups.size(); ++g) {
            const auto& group = kNavigationGroups[g];
            auto* button = new QToolButton(compact_);
            button->setText(QString::fromUtf8(group.short_label));
            button->setAccessibleName(QString::fromUtf8(group.label));
            button->setToolTip(QString::fromUtf8(group.label));
            button->setCheckable(true);
            button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            button->setPopupMode(QToolButton::InstantPopup);
            auto* menu = new QMenu(button);
            fill_group(menu, static_cast<int>(g));
            button->setMenu(menu);
            rail->addWidget(button);
            rail_buttons_[g] = button;
        }
        rail->addStretch();
        layout->addWidget(compact_, 1);
        notice_ = new QLabel(this);
        notice_->setWordWrap(true);
        notice_->setStyleSheet(QStringLiteral("color:#FFBE67;padding:4px;"));
        notice_->hide();
        layout->addWidget(notice_);
        resize_handle_ = new NavigationResizeHandle(this);
        resize_handle_->request_width = [this](int width) { set_expanded_width(width); };
        connect(search_button_, &QPushButton::clicked, this, [this] { show_search(); });
        connect(tree_, &QTreeWidget::currentItemChanged, this,
                [this](QTreeWidgetItem* item, QTreeWidgetItem*) {
            if (applying_ || !item || !item->data(0, Qt::UserRole).isValid()) return;
            const QString model = item->data(0, Qt::UserRole + 2).toString();
            request_page(item->data(0, Qt::UserRole).toInt());
            if (!model.isEmpty() && open_atlas_model) open_atlas_model(model);
        });
        connect(tree_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item, int) {
            if (item && !item->data(0, Qt::UserRole).isValid())
                item->setExpanded(!item->isExpanded());
        });
        connect(tree_, &QTreeWidget::itemExpanded, this, [this] { record_expansion(); });
        connect(tree_, &QTreeWidget::itemCollapsed, this, [this] { record_expansion(); });
        rebuild_tree();
        apply_mode(false);
    }
    [[nodiscard]] int current_page() const noexcept { return state_.page; }
    [[nodiscard]] NavigationMode mode() const noexcept { return state_.mode; }
    [[nodiscard]] bool is_favourite() const { return state_.favourites.contains(nav_page_id(state_.page)); }
    [[nodiscard]] QString recovery_notice() const { return notice_->text(); }
    /// GUI thread; expanded sidebar width in logical pixels.
    [[nodiscard]] int expanded_width() const noexcept { return state_.expanded_width; }
    /// GUI thread; stable IDs, unique and most-recent-first.
    [[nodiscard]] QStringList recent_pages() const { return state_.recent; }
    /// GUI thread; refuse widths outside220..360 logical pixels without mutation.
    bool set_expanded_width(int width) {
        if (width < 220 || width > 360) return false;
        state_.expanded_width = width;
        apply_mode();
        notify();
        return true;
    }

    // Invalid legacy indices do not change selection or invoke the route callback.
    void select_page(int index) {
        index = nav_destination(index);
        if (nav_page_id(index).isEmpty()) return;
        const bool changed = state_.page != index;
        state_.page = index;
        if (changed) {
            const auto id = nav_page_id(index);
            state_.recent.removeAll(id);
            state_.recent.prepend(id);
            // One entry per registry identity; uniqueness proves the maximum size.
        }
        applying_ = true;
        const auto group = static_cast<std::size_t>(kNavigationPages[static_cast<std::size_t>(index)].group);
        if (changed) groups_[group]->setExpanded(true);
        tree_->setCurrentItem(pages_[static_cast<std::size_t>(index)]);
        if (changed) tree_->scrollToItem(pages_[static_cast<std::size_t>(index)]);
        for (std::size_t g = 0; g < rail_buttons_.size(); ++g) rail_buttons_[g]->setChecked(g == group);
        for (auto* button : rail_buttons_) {
            if (!button || !button->menu()) continue;
            for (auto* action : button->menu()->actions()) {
                if (action->data().isValid())
                    action->setChecked(action->data().toInt() == state_.page);
            }
        }
        applying_ = false;
        if (changed) { record_expansion(false); notify(); }
    }
    void set_mode(NavigationMode mode) {
        if (mode != NavigationMode::Expanded && mode != NavigationMode::Compact &&
            mode != NavigationMode::Hidden) return;
        state_.mode = mode;
        if (mode != NavigationMode::Hidden) state_.last_visible = mode;
        apply_mode();
        notify();
    }
    void toggle_visibility() {
        set_mode(state_.mode == NavigationMode::Hidden ? state_.last_visible : NavigationMode::Hidden);
    }
    void toggle_favourite() {
        const auto id = nav_page_id(state_.page);
        if (state_.favourites.contains(id)) state_.favourites.removeAll(id);
        else state_.favourites.append(id);
        rebuild_tree();
        notify();
    }
    void show_search() {
        NavigationSearch dialog(state_.favourites, [this](int index) { request_page(index); }, window());
        dialog.exec();
    }
    // Caller owns menu. Rebuild immediately before showing so stars stay current.
    void populate_menu(QMenu* menu) {
        // QMenu::clear removes actions, not parent-owned submenu objects.
        const auto submenus = menu->findChildren<QMenu*>(QString{}, Qt::FindDirectChildrenOnly);
        for (auto* submenu : submenus) delete submenu;
        menu->clear();
        auto* saved = menu->addMenu(QStringLiteral("Favourites"));
        saved->setEnabled(!state_.favourites.isEmpty());
        for (const auto& id : state_.favourites) add_route(saved, nav_page_index(id));
        auto* recent = menu->addMenu(QStringLiteral("Recent pages"));
        for (const auto& id : state_.recent) add_route(recent, nav_page_index(id));
        menu->addSeparator();
        for (std::size_t g = 0; g < kNavigationGroups.size(); ++g)
            fill_group(menu->addMenu(QString::fromUtf8(kNavigationGroups[g].label)
                .replace(QLatin1Char('&'), QStringLiteral("&&"))), static_cast<int>(g));
    }
    void restore_state(QSettings& settings) {
        bool recovered = false;
        state_ = read_navigation_state(settings, recovered);
        if (recovered) {
            // Preserve the malformed values before the caller saves recovered defaults.
            const auto backup = QStringLiteral("recovery/%1/").arg(QDateTime::currentMSecsSinceEpoch());
            const QStringList keys{QStringLiteral("schema"), QStringLiteral("page"), QStringLiteral("mode"),
                QStringLiteral("lastVisible"), QStringLiteral("expanded"), QStringLiteral("favourites"),
                QStringLiteral("expandedWidth"), QStringLiteral("recent")};
            for (const auto& key : keys)
                if (settings.contains(key)) settings.setValue(backup + key, settings.value(key));
            notice_->setText(QStringLiteral("Navigation preferences were invalid. Defaults restored; backup saved."));
        } else {
            const auto old_page = settings.value(QStringLiteral("page")).toString();
            const bool migrated = (!old_page.isEmpty() && old_page != nav_page_id(state_.page))
                || settings.value(QStringLiteral("favourites")).toStringList() != state_.favourites
                || (settings.contains(QStringLiteral("recent"))
                    && settings.value(QStringLiteral("recent")).toStringList() != state_.recent);
            notice_->setText(migrated
                ? QStringLiteral("Navigation updated: broker links open Brokers; Grid and Chart links open Terminal.")
                : QString{});
        }
        notice_->setVisible(!notice_->text().isEmpty());
        rebuild_tree();
        apply_mode();
    }
    void save_state(QSettings& settings) const { write_navigation_state(settings, state_); }
protected:
    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        if (resize_handle_) {
            resize_handle_->setGeometry(width() - 6, 0, 6, height());
            resize_handle_->raise();
        }
    }
private:
    void notify() { if (!applying_ && on_state_changed) on_state_changed(); }
    void request_page(int index) {
        index = nav_destination(index);
        if (index < 0) return;
        select_page(index);
        if (activate) activate(index);
    }
    void add_route(QMenu* menu, int index) {
        const auto& page = kNavigationPages[static_cast<std::size_t>(index)];
        auto* action = menu->addAction(QString::fromUtf8(page.label)
            .replace(QLatin1Char('&'), QStringLiteral("&&")));
        action->setData(index);
        action->setCheckable(true);
        action->setChecked(index == state_.page);
        connect(action, &QAction::triggered, this, [this, index] { request_page(index); });
    }
    void fill_group(QMenu* menu, int group) {
        for (const int index : kNavigationOrder)
            if (kNavigationPages[static_cast<std::size_t>(index)].group == group) add_route(menu, index);
        if (group == 3) {
            auto* catalogue = menu->addMenu(QStringLiteral("Atlas models"));
            QMenu* family = nullptr;
            QString previous;
            for (const auto& row : kAtlasRows) {
                QString name = QString::fromUtf8(row.family);
                if (name != previous) {
                    family = catalogue->addMenu(name.replace(QLatin1Char('&'), QStringLiteral("&&")));
                    previous = QString::fromUtf8(row.family);
                }
                const QString model = QStringLiteral("atlas.%1").arg(
                    QString::number(static_cast<qulonglong>(atlas_route_id(row)), 16));
                QString display = QString::fromUtf8(row.model);
                auto* action = family->addAction(display.replace(QLatin1Char('&'), QStringLiteral("&&")) +
                    QStringLiteral("  [%1]").arg(atlas_status_text(row.status)));
                action->setProperty("atlasModel", model);
                action->setToolTip(QString::fromUtf8(row.what));
                connect(action, &QAction::triggered, this, [this, model] {
                    request_page(32);
                    if (open_atlas_model) open_atlas_model(model);
                });
            }
        }
    }
    void rebuild_tree() {
        applying_ = true;
        const QSignalBlocker blocker(tree_);
        tree_->clear();
        pages_.fill(nullptr);
        auto* saved = new QTreeWidgetItem(tree_, {QStringLiteral("Favourites")});
        for (const auto& id : state_.favourites) {
            const int index = nav_page_index(id);
            auto* item = new QTreeWidgetItem(saved, {QString::fromUtf8(kNavigationPages[static_cast<std::size_t>(index)].label)});
            item->setData(0, Qt::UserRole, index);
        }
        saved->setExpanded(true);
        saved->setHidden(state_.favourites.isEmpty());
        for (std::size_t g = 0; g < kNavigationGroups.size(); ++g) {
            const auto& group = kNavigationGroups[g];
            std::size_t page_count = 0;
            for (const int index : kNavigationOrder)
                if (kNavigationPages[static_cast<std::size_t>(index)].group == static_cast<int>(g)) ++page_count;
            groups_[g] = new QTreeWidgetItem(tree_,
                {QString::fromUtf8(group.label) + QStringLiteral("  ·  ")
                 + QString::number(static_cast<qulonglong>(page_count))});
            groups_[g]->setToolTip(0, QString::fromUtf8(group.label));
            QFont group_font = groups_[g]->font(0);
            group_font.setWeight(QFont::DemiBold);
            groups_[g]->setFont(0, group_font);
            groups_[g]->setForeground(0, QColor(QStringLiteral("#AEBFC8")));
            groups_[g]->setData(0, Qt::UserRole + 1, QString::fromUtf8(group.id));
            groups_[g]->setExpanded(state_.expanded.contains(QString::fromUtf8(group.id)));
        }
        for (const int index : kNavigationOrder) {
            const auto& page = kNavigationPages[static_cast<std::size_t>(index)];
            auto* item = new QTreeWidgetItem(groups_[static_cast<std::size_t>(page.group)],
                {(state_.favourites.contains(nav_page_id(index)) ? QStringLiteral("★  ") : QString{}) +
                 QString::fromUtf8(page.label)});
            item->setData(0, Qt::UserRole, index);
            item->setToolTip(0, nav_page_id(index));
            pages_[static_cast<std::size_t>(index)] = item;
        }
        QTreeWidgetItem* family = nullptr;
        QString previous;
        for (const auto& row : kAtlasRows) {
            const QString name = QString::fromUtf8(row.family);
            if (name != previous) {
                family = new QTreeWidgetItem(pages_[32], {name});
                previous = name;
            }
            const QString model = QString::fromUtf8(row.model);
            auto* entry = new QTreeWidgetItem(family,
                {model + QStringLiteral("  [%1]").arg(atlas_status_text(row.status))});
            entry->setData(0, Qt::UserRole, 32);
            entry->setData(0, Qt::UserRole + 2, model);
            entry->setToolTip(0, QString::fromUtf8(row.what));
        }
        applying_ = false;
        select_page(state_.page);
    }
    void record_expansion(bool announce = true) {
        if (applying_) return;
        state_.expanded.clear();
        for (std::size_t g = 0; g < groups_.size(); ++g)
            if (groups_[g]->isExpanded()) state_.expanded.append(QString::fromUtf8(kNavigationGroups[g].id));
        if (announce) notify();
    }
    void apply_mode(bool update_visibility = true) {
        const bool expanded = state_.mode == NavigationMode::Expanded;
        setFixedWidth(expanded ? state_.expanded_width : 76);
        heading_->parentWidget()->setVisible(expanded);
        search_button_->setVisible(expanded);
        summary_->setVisible(expanded);
        tree_->setVisible(expanded);
        compact_->setVisible(!expanded);
        resize_handle_->setVisible(expanded);
        if (update_visibility) setVisible(state_.mode != NavigationMode::Hidden);
    }
    NavigationState state_;
    bool applying_ = false;
    QTreeWidget* tree_ = nullptr;
    QWidget* compact_ = nullptr;
    QLabel* heading_ = nullptr;
    QLabel* summary_ = nullptr;
    QPushButton* search_button_ = nullptr;
    QLabel* notice_ = nullptr;
    NavigationResizeHandle* resize_handle_ = nullptr;
    std::array<QTreeWidgetItem*, 7> groups_{};
    std::array<QTreeWidgetItem*, kNavigationPages.size()> pages_{};
    std::array<QToolButton*, 7> rail_buttons_{};
};
} // namespace altair::ui
