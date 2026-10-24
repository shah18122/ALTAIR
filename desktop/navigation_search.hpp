// Native, keyboard-first page search. Navigation only; no order actions.
#pragma once
#include "navigation_registry.hpp"
#include <QDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QVBoxLayout>
#include <functional>
#include <utility>
#include <vector>

namespace altair::ui {
class NavigationSearch final : public QDialog {
public:
    NavigationSearch(const QStringList& favourites, std::function<void(int)> activate,
                     QWidget* parent = nullptr)
        : QDialog(parent), favourites_(favourites), activate_(std::move(activate)) {
        setWindowTitle(QStringLiteral("Find a workspace"));
        setWindowModality(Qt::WindowModal);
        resize(620, 440);
        auto* layout = new QVBoxLayout(this);
        query_ = new QLineEdit(this);
        query_->setObjectName(QStringLiteral("navigationQuery"));
        query_->setPlaceholderText(QStringLiteral("Page, group or favourite…"));
        query_->setAccessibleName(QStringLiteral("Search workspaces"));
        query_->installEventFilter(this);
        results_ = new QListWidget(this);
        results_->setObjectName(QStringLiteral("navigationResults"));
        results_->setAccessibleName(QStringLiteral("Matching workspaces"));
        results_->installEventFilter(this);
        layout->addWidget(query_);
        layout->addWidget(results_, 1);
        layout->addWidget(new QLabel(QStringLiteral("↑ ↓ choose    Enter open    Esc close"), this));
        connect(query_, &QLineEdit::textChanged, this, [this] { filter(); });
        connect(results_, &QListWidget::itemClicked, this, [this] { choose(); });
        filter();
        query_->setFocus();
    }
protected:
    bool eventFilter(QObject* object, QEvent* event) override {
        if ((object == query_ || object == results_) && event->type() == QEvent::KeyPress) {
            const auto key = static_cast<QKeyEvent*>(event)->key();
            if (key == Qt::Key_Return || key == Qt::Key_Enter) { choose(); return true; }
            if (object == query_ && (key == Qt::Key_Down || key == Qt::Key_Up)) {
                const int n = results_->count();
                if (n > 0) results_->setCurrentRow(
                    (results_->currentRow() + (key == Qt::Key_Down ? 1 : n - 1)) % n);
                return true;
            }
        }
        return QDialog::eventFilter(object, event);
    }
private:
    void filter() {
        results_->clear();
        const auto want = query_->text().simplified();
        // The sidebar's pages first, then the ones that open from the Model
        // Atlas, so a model page is never out of reach.
        std::vector<int> order(kNavigationOrder.begin(), kNavigationOrder.end());
        for (int i = 0; i < static_cast<int>(kNavigationPages.size()); ++i)
            if (nav_visible(i) && !nav_listed(i)) order.push_back(i);
        for (const int index : order) {
            const auto& page = kNavigationPages[static_cast<std::size_t>(index)];
            const auto group = QString::fromUtf8(kNavigationGroups[static_cast<std::size_t>(page.group)].label);
            const bool saved = favourites_.contains(nav_page_id(index));
            const auto label = QString::fromUtf8(page.label);
            auto haystack = label + QLatin1Char(' ') + group + QLatin1Char(' ') +
                                  nav_page_id(index) + (saved ? QStringLiteral(" favourite saved") : QString{});
            // Old labels remain search terms, but produce one canonical result.
            for (int legacy = 0; legacy < static_cast<int>(kNavigationPages.size()); ++legacy) {
                if (legacy == index || nav_destination(legacy) != index) continue;
                haystack += QLatin1Char(' ') + nav_page_id(legacy) + QLatin1Char(' ')
                    + QString::fromUtf8(kNavigationPages[static_cast<std::size_t>(legacy)].label);
            }
            if (!haystack.contains(want, Qt::CaseInsensitive)) continue;
            auto* item = new QListWidgetItem((saved ? QStringLiteral("★  ") : QString{}) +
                                            label + QStringLiteral("    /    ") + group, results_);
            item->setData(Qt::UserRole, index);
        }
        if (results_->count() > 0) results_->setCurrentRow(0);
    }
    void choose() {
        const auto* item = results_->currentItem();
        if (!item) return;
        const int index = item->data(Qt::UserRole).toInt();
        accept();
        if (activate_) activate_(index);
    }
    QLineEdit* query_ = nullptr;
    QListWidget* results_ = nullptr;
    QStringList favourites_;
    std::function<void(int)> activate_;
};
} // namespace altair::ui
