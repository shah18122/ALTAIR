// Cold GUI layout only: inactive pages must not set the window's minimum size.
#pragma once
#include <QLayout>
#include <QStackedWidget>

namespace altair::ui {
class WorkspacePages final : public QStackedWidget {
public:
    /// GUI thread; sizes are logical pixels. Parent usually lives in QScrollArea.
    explicit WorkspacePages(QWidget* parent = nullptr) : QStackedWidget(parent) {
        setObjectName(QStringLiteral("workspacePages"));
        // QStackedLayout otherwise aggregates minimum sizes from EVERY page.
        layout()->setSizeConstraint(QLayout::SetNoConstraint);
        connect(this, &QStackedWidget::currentChanged, this, [this] {
            setMinimumSize(minimumSizeHint());
            updateGeometry();
        });
    }
    /// Logical pixels, based on the current page only.
    QSize sizeHint() const override {
        return currentWidget() ? currentWidget()->sizeHint() : QSize(640, 480);
    }
    /// Preserve active-page minima; a surrounding scroll area makes them reachable.
    QSize minimumSizeHint() const override {
        return currentWidget() ? currentWidget()->minimumSizeHint().expandedTo(currentWidget()->minimumSize())
                               : QSize(0, 0);
    }
    /// The current page only. QStackedLayout asks EVERY page, which laid out
    /// each hidden page's word-wrapped text on every resize and on first show.
    bool hasHeightForWidth() const override {
        return currentWidget() != nullptr && currentWidget()->hasHeightForWidth();
    }
    int heightForWidth(int width) const override {
        return currentWidget() != nullptr ? currentWidget()->heightForWidth(width) : -1;
    }
};
} // namespace altair::ui
