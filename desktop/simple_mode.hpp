// desktop/simple_mode.hpp -- one Simple / Advanced switch for the whole app.
//
// SIMPLE (the default) shows what a trader reads every minute and says it in
// plain words: the watch's price columns, the chain's prices, the models'
// state and P&L, the backtests' summaries and verdicts. ADVANCED shows every
// column and tab as before. Nothing is computed differently -- only shown.
//
// Screens subscribe with `watch(owner, fn)`: `fn` runs now and on every
// change, and is dropped when `owner` is destroyed. The setting is kept per
// user (QSettings "Altair/Desktop", view/simple); tests turn storage off.

#pragma once

#include <QObject>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>

#include <functional>
#include <utility>
#include <vector>

namespace altair::ui::simple_mode {

namespace detail {
struct State {
    bool loaded = false;
    bool on = true;
    bool store = true;
    std::vector<std::pair<QPointer<QObject>, std::function<void(bool)>>> watchers;
};
inline State& state() {
    static State s;
    return s;
}
inline QSettings settings() {
    return QSettings(QSettings::defaultFormat(), QSettings::UserScope, QStringLiteral("Altair"), QStringLiteral("Desktop"));
}
} // namespace detail

/// Tests: keep the switch in memory, away from the user's settings.
inline void use_settings(bool store) { detail::state().store = store; }

[[nodiscard]] inline bool on() {
    auto& s = detail::state();
    if (!s.loaded) {
        s.loaded = true;
        if (s.store) s.on = detail::settings().value(QStringLiteral("view/simple"), true).toBool();
    }
    return s.on;
}

inline void set(bool simple) {
    auto& s = detail::state();
    (void)on();
    if (s.on == simple) return;
    s.on = simple;
    if (s.store) detail::settings().setValue(QStringLiteral("view/simple"), simple);
    // Copy first: a watcher may add another.
    auto watchers = s.watchers;
    for (auto& [owner, fn] : watchers)
        if (owner) fn(simple);
    std::erase_if(s.watchers, [](const auto& w) { return w.first.isNull(); });
}

/// Run `fn(on())` now and whenever the switch changes, while `owner` lives.
inline void watch(QObject* owner, std::function<void(bool)> fn) {
    fn(on());
    detail::state().watchers.emplace_back(owner, std::move(fn));
}

/// The switch as a button ("Simple view" / "Advanced view"), kept in step
/// with every other copy of it.
[[nodiscard]] inline QPushButton* button(QWidget* parent) {
    auto* b = new QPushButton(parent);
    b->setObjectName(QStringLiteral("simpleSwitch"));
    b->setCheckable(true);
    b->setToolTip(QStringLiteral("Simple: the columns and tabs you read every minute, in plain words. "
                                 "Advanced: every column and tab. Nothing is computed differently."));
    QObject::connect(b, &QPushButton::toggled, b, [](bool simple) { set(simple); });
    watch(b, [b](bool simple) {
        const QSignalBlocker block(b);
        b->setChecked(simple);
        b->setText(simple ? QStringLiteral("Simple view") : QStringLiteral("Advanced view"));
    });
    return b;
}

} // namespace altair::ui::simple_mode
