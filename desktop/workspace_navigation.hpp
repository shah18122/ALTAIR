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
            "#workspaceNavigation{background:#0D1318;color:#D5DDE2;border-right:1px solid #1E2A32;}"
            "#navBrand{background:transparent;border:0;border-bottom:1px solid #1E2A32;padding:2px 2px 10px 2px;}"
            "#navBrandTitle{color:#F0B765;font-size:18px;font-weight:800;letter-spacing:3px;}"
            "#navBrandSub{color:#6E8390;font-size:10px;letter-spacing:1px;}"
            "#navSearch{color:#9FB0B9;background:#141D23;border:1px solid #24323B;"
            "border-radius:8px;padding:8px 10px;text-align:left;}"
            "#navSearch:hover{background:#1A262E;border-color:#B47A3A;color:#D5DDE2;}"
            "#navSearch:pressed{background:#10171C;}"
            "#workspaceTree{background:#0D1318;color:#C9D4DA;border:0;font-size:12px;outline:0;}"
            "#workspaceTree::item{height:28px;padding:1px 6px;border-radius:6px;margin:1px 2px;}"
            "#workspaceTree::item:selected{background:#2A2418;color:#F0B765;}"
            "#workspaceTree::item:hover:!selected{background:#17222A;}"
            "#workspaceTree::branch{background:#0D1318;}"
            "#workspaceNavigation QToolButton{color:#C4D0D6;background:#141D23;"
            "border:1px solid #24323B;border-radius:8px;padding:8px 2px;font-size:15px;}"
            "#workspaceNavigation QToolButton:hover{background:#1D2A32;border-color:#B47A3A;}"
            "#workspaceNavigation QToolButton:checked{color:#F0B765;background:#2A2418;border-color:#B47A3A;}"
            "#workspaceNavigation QToolButton::menu-indicator{image:none;width:0;}"));
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(8, 10, 8, 8);
        auto* brand = new QWidget(this);
        brand->setObjectName(QStringLiteral("navBrand"));
        auto* brand_layout = new QVBoxLayout(brand);
        brand_layout->setContentsMargins(8, 6, 8, 6);
        // Full screen has no title bar: minimise and close live beside the name.
        auto* title_row = new QHBoxLayout;
        title_row->setContentsMargins(0, 0, 0, 0);
        heading_ = new QLabel(QStringLiteral("ALTAIR"), brand);
        heading_->setObjectName(QStringLiteral("navBrandTitle"));
        title_row->addWidget(heading_);
        title_row->addStretch();
        const auto chrome_button = [brand, title_row](const QString& text, const QString& name, const QString& tip) {
            auto* b = new QPushButton(text, brand);
            b->setObjectName(name);
            b->setToolTip(tip);
            b->setFlat(true);
            b->setFixedSize(24, 22);
            b->setStyleSheet(QStringLiteral("QPushButton{color:#8FA3AE;background:transparent;border:0;font-size:13px;}"
                                            "QPushButton:hover{color:#F0B765;background:#1A262E;border-radius:4px;}"));
            title_row->addWidget(b);
            return b;
        };
        auto* minimise = chrome_button(QStringLiteral("\u2013"), QStringLiteral("navMinimise"), QStringLiteral("Minimise"));
        auto* close = chrome_button(QStringLiteral("\u2715"), QStringLiteral("navClose"), QStringLiteral("Close ALTAIR"));
        connect(minimise, &QPushButton::clicked, this, [this] { window()->showMinimized(); });
        connect(close, &QPushButton::clicked, this, [this] { window()->close(); });
        auto* subheading = new QLabel(QStringLiteral("GAUTAM GLOBAL LLP : SMIT SHAH"), brand);
        subheading->setObjectName(QStringLiteral("navBrandSub"));
        brand_layout->addLayout(title_row);
        brand_layout->addWidget(subheading);
        layout->addWidget(brand);
        search_button_ = new QPushButton(QStringLiteral("⌕   Search pages…        Ctrl+K"), this);
        search_button_->setObjectName(QStringLiteral("navSearch"));
        search_button_->setAccessibleName(QStringLiteral("Search workspaces"));
        search_button_->setToolTip(QStringLiteral(
            "Search pages and saved favourites · Ctrl+K"));
        layout->addWidget(search_button_);
        // The old "31 workspaces · 7 groups" count line said nothing a
        // trader uses; the tree below is the summary.
        summary_ = new QLabel(this);
        summary_->setObjectName(QStringLiteral("navSummary"));
        summary_->hide();
        tree_ = new QTreeWidget(this);
        tree_->setObjectName(QStringLiteral("workspaceTree"));
        tree_->setAccessibleName(QStringLiteral("Workspaces grouped by function"));
        tree_->setHeaderHidden(true);
        tree_->setIndentation(10);
        tree_->setAnimated(true);
        tree_->setUniformRowHeights(true);
        tree_->setExpandsOnDoubleClick(false);
        tree_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        layout->addWidget(tree_, 1);
        compact_ = new QWidget(this);
        auto* rail = new QVBoxLayout(compact_);
        rail->setContentsMargins(0, 0, 0, 0);
        for (const int gi : kNavigationGroupOrder) {
            const auto g = static_cast<std::size_t>(gi);
            const auto& group = kNavigationGroups[g];
            auto* button = new QToolButton(compact_);
            button->setText(QString::fromUtf8(group.glyph));
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
        // A model page opened from the Model Atlas is not in the sidebar: the
        // Atlas stays highlighted, which is where Back (Backspace) returns.
        const int shown = nav_listed(index) ? index : 32;
        const auto group = static_cast<std::size_t>(kNavigationPages[static_cast<std::size_t>(shown)].group);
        if (changed) groups_[group]->setExpanded(true);
        tree_->setCurrentItem(pages_[static_cast<std::size_t>(shown)]);
        if (changed) tree_->scrollToItem(pages_[static_cast<std::size_t>(shown)]);
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
        for (const int g : kNavigationGroupOrder)
            fill_group(menu->addMenu(QString::fromUtf8(kNavigationGroups[static_cast<std::size_t>(g)].label)
                .replace(QLatin1Char('&'), QStringLiteral("&&"))), g);
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
    }
    void rebuild_tree() {
        applying_ = true;
        const QSignalBlocker blocker(tree_);
        tree_->clear();
        pages_.fill(nullptr);
        for (const int gi : kNavigationGroupOrder) {
            const auto g = static_cast<std::size_t>(gi);
            const auto& group = kNavigationGroups[g];
            groups_[g] = new QTreeWidgetItem(tree_,
                {QStringLiteral("%1   %2").arg(QString::fromUtf8(group.glyph), QString::fromUtf8(group.label).toUpper())});
            groups_[g]->setToolTip(0, QString::fromUtf8(group.label));
            QFont group_font = groups_[g]->font(0);
            group_font.setWeight(QFont::Bold);
            group_font.setPointSizeF(group_font.pointSizeF() * 0.85);
            group_font.setLetterSpacing(QFont::AbsoluteSpacing, 1.2);
            groups_[g]->setFont(0, group_font);
            groups_[g]->setForeground(0, QColor(QStringLiteral("#7F95A1")));
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
