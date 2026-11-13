// desktop/ui_perf.hpp -- render settings for a desktop seen through a remote screen.
//
// ALTAIR's desktop runs on the EC2 server and is watched through Amazon DCV.
// There is no GPU: Qt paints in software and DCV encodes every changed pixel
// into video on the CPU, on the same housekeeping core. Anything that moves on
// its own (a scrolling ticker, menu fades) costs a video frame on every step,
// whether anyone is looking or not, and delays the frames that answer a click.
//
// "Low motion" (the default) keeps the screen still unless the data changes:
// no menu/tooltip/combo animations, and the news bar steps between headlines
// instead of scrolling. ALTAIR_UI_MOTION=smooth turns the animations back on
// (for a local screen with a GPU).
#pragma once

#include <QApplication>
#include <QShowEvent>
#include <QWidget>
#include <QtGlobal>

namespace altair::ui {

[[nodiscard]] inline bool low_motion() {
    static const bool v = qEnvironmentVariable("ALTAIR_UI_MOTION").compare(QLatin1String("smooth"), Qt::CaseInsensitive) != 0;
    return v;
}

inline void apply_render_settings() {
    if (!low_motion()) return;
    for (const auto e : {Qt::UI_AnimateMenu, Qt::UI_FadeMenu, Qt::UI_AnimateCombo, Qt::UI_AnimateTooltip,
                         Qt::UI_FadeTooltip, Qt::UI_AnimateToolBox})
        QApplication::setEffectEnabled(e, false);
}

/// True when `w` is really on screen: shown, and its window not minimised.
/// Timers that only feed a display check this before reading files or
/// rebuilding tables.
[[nodiscard]] inline bool on_screen(const QWidget* w) {
    return w != nullptr && w->isVisible() && !w->window()->isMinimized();
}

} // namespace altair::ui
