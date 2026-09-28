// Presentation-only terminal geometry. No orders, credentials or selected contracts.
#pragma once
#include <QSettings>
#include <QSplitter>

namespace altair::ui {
/// GUI thread; require the existing three horizontal panes. Sizes are logical pixels.
/// Missing preferences leave the existing layout alone; malformed preferences refuse.
inline bool restore_terminal_layout(QSplitter* splitter, QSettings& settings) {
    if (!splitter || splitter->count() != 3 || splitter->orientation() != Qt::Horizontal) return false;
    const auto id = settings.value(QStringLiteral("terminalLayout/id"));
    const auto raw = settings.value(QStringLiteral("terminalLayout/sizes"));
    if (!id.isValid() && !raw.isValid()) return true;
    if (id.toString() != QStringLiteral("terminal-three-pane/v1") || !raw.canConvert<QStringList>())
        return false;
    const auto values = raw.toStringList();
    if (values.size() != 3) return false;
    QList<int> sizes;
    for (const auto& value : values) {
        bool ok = false;
        const int size = value.toInt(&ok);
        if (!ok || size < 1 || size > 100000) return false;
        sizes.append(size);
    }
    splitter->setSizes(sizes);
    return true;
}
/// GUI thread; save positive sizes for exactly three horizontal panes, or refuse.
inline bool save_terminal_layout(QSplitter* splitter, QSettings& settings) {
    if (!splitter || splitter->count() != 3 || splitter->orientation() != Qt::Horizontal) return false;
    QStringList sizes;
    for (const int size : splitter->sizes()) {
        if (size < 1 || size > 100000) return false;
        sizes.append(QString::number(size));
    }
    settings.setValue(QStringLiteral("terminalLayout/id"), QStringLiteral("terminal-three-pane/v1"));
    settings.setValue(QStringLiteral("terminalLayout/sizes"), sizes);
    return true;
}
} // namespace altair::ui
