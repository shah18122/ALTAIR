// GUI-thread sidebar edge. Widths are logical pixels; owner validates bounds.
#pragma once
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWidget>
#include <functional>

namespace altair::ui {
class NavigationResizeHandle final : public QWidget {
public:
    std::function<void(int)> request_width;
    explicit NavigationResizeHandle(QWidget* parent) : QWidget(parent) {
        setObjectName(QStringLiteral("navigationResizeHandle"));
        setAccessibleName(QStringLiteral("Resize navigation"));
        setToolTip(QStringLiteral("Drag to resize navigation · Arrow keys adjust · Double-click resets"));
        setCursor(Qt::SplitHCursor);
        setFocusPolicy(Qt::StrongFocus);
        setStyleSheet(QStringLiteral(
            "#navigationResizeHandle{background:#293942;}"
            "#navigationResizeHandle:hover,#navigationResizeHandle:focus{background:#B5803F;}"));
    }
protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) { event->ignore(); return; }
        dragging_ = true;
        start_x_ = event->globalPosition().x();
        start_width_ = parentWidget()->width();
        setFocus(Qt::MouseFocusReason);
        event->accept();
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (!dragging_ || !(event->buttons() & Qt::LeftButton)) return;
        const qreal wanted = start_width_ + event->globalPosition().x() - start_x_;
        // Refuse out-of-range coordinates before narrowing. No hidden clamp.
        if (wanted >= 220 && wanted <= 360 && request_width)
            request_width(static_cast<int>(wanted));
        event->accept();
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) dragging_ = false;
        event->accept();
    }
    void mouseDoubleClickEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && request_width) request_width(258);
        dragging_ = false;
        event->accept();
    }
    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right) {
            if (request_width) request_width(parentWidget()->width()
                + (event->key() == Qt::Key_Left ? -10 : 10));
            event->accept();
        } else QWidget::keyPressEvent(event);
    }
    void hideEvent(QHideEvent* event) override {
        dragging_ = false;
        QWidget::hideEvent(event);
    }
private:
    bool dragging_{};
    qreal start_x_{};
    int start_width_{};
};
} // namespace altair::ui
