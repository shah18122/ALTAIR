// User-scoped presentation preferences only. No trading or authentication state.
#pragma once
#include "navigation_registry.hpp"
#include <QSettings>

namespace altair::ui {
enum class NavigationMode { Expanded = 0, Compact = 1, Hidden = 2 };
struct NavigationState {
    int page = 2;
    NavigationMode mode = NavigationMode::Expanded;
    NavigationMode last_visible = NavigationMode::Expanded;
    QStringList expanded{QStringLiteral("market"), QStringLiteral("models")};
    QStringList favourites;
    int expanded_width = 258; // Logical pixels; only220..360 are accepted.
    QStringList recent{QStringLiteral("market.terminal")};
};
[[nodiscard]] inline bool nav_known_group(const QString& id) {
    for (const auto& group : kNavigationGroups)
        if (id == QString::fromUtf8(group.id)) return true;
    return false;
}
// A caller supplies a dedicated settings group (per user/workspace).
// Missing state is a first launch. Malformed/unknown state is explicitly recovered.
[[nodiscard]] inline NavigationState read_navigation_state(QSettings& settings, bool& recovered) {
    recovered = false;
    NavigationState state;
    if (!settings.contains(QStringLiteral("schema"))) {
        recovered = settings.contains(QStringLiteral("page")) ||
                    settings.contains(QStringLiteral("mode")) ||
                    settings.contains(QStringLiteral("lastVisible")) ||
                    settings.contains(QStringLiteral("expanded")) ||
                    settings.contains(QStringLiteral("favourites")) ||
                    settings.contains(QStringLiteral("expandedWidth")) ||
                    settings.contains(QStringLiteral("recent"));
        return state;
    }
    bool schema_ok = false, mode_ok = false, last_ok = false;
    const int schema = settings.value(QStringLiteral("schema")).toString().toInt(&schema_ok);
    const int mode = settings.value(QStringLiteral("mode")).toString().toInt(&mode_ok);
    const int last = settings.value(QStringLiteral("lastVisible")).toString().toInt(&last_ok);
    const QString page = settings.value(QStringLiteral("page")).toString();
    const auto raw_expanded = settings.value(QStringLiteral("expanded"));
    const auto raw_favourites = settings.value(QStringLiteral("favourites"));
    const auto expanded = raw_expanded.toStringList();
    auto favourites = raw_favourites.toStringList();
    // QSettings writes an EMPTY list as @Invalid() and reads it back as an
    // invalid QVariant. A present key holding nothing is an empty list, not
    // a corrupt one; only a missing key or a non-list value is malformed.
    const auto list_ok = [&settings](const QString& key, const QVariant& v) {
        return settings.contains(key) && (!v.isValid() || v.canConvert<QStringList>());
    };
    bool valid = schema_ok && (schema == 1 || schema == 2) && mode_ok && mode >= 0 && mode <= 2 &&
                 last_ok && last >= 0 && last <= 1 && nav_known_id(page) &&
                 list_ok(QStringLiteral("expanded"), raw_expanded) &&
                 list_ok(QStringLiteral("favourites"), raw_favourites) &&
                 expanded.size() <= 7 && favourites.size() <= 36;
    for (const auto& id : expanded) valid = valid && nav_known_group(id);
    for (const auto& id : favourites) valid = valid && nav_known_id(id);
    if (schema == 2) {
        bool width_ok = false;
        state.expanded_width = settings.value(QStringLiteral("expandedWidth")).toString().toInt(&width_ok);
        const auto raw_recent = settings.value(QStringLiteral("recent"));
        state.recent = raw_recent.toStringList();
        valid = valid && width_ok && state.expanded_width >= 220 && state.expanded_width <= 360 &&
                raw_recent.canConvert<QStringList>() && !state.recent.isEmpty() && state.recent.size() <= 36;
        for (const auto& id : state.recent) valid = valid && nav_known_id(id);
        state.recent.removeDuplicates();
        valid = valid && !state.recent.isEmpty() && state.recent.first() == page;
    } else state.recent = {page};
    if (!valid) { recovered = true; return NavigationState{}; }
    state.page = nav_destination(nav_page_index(page));
    state.mode = static_cast<NavigationMode>(mode);
    state.last_visible = mode == 2 ? static_cast<NavigationMode>(last) : state.mode;
    state.expanded = expanded;
    state.expanded.removeDuplicates();
    for (auto& id : favourites) id = nav_page_id(nav_destination(nav_page_index(id)));
    for (auto& id : state.recent) id = nav_page_id(nav_destination(nav_page_index(id)));
    favourites.removeDuplicates();
    state.recent.removeDuplicates();
    state.favourites = favourites;
    return state;
}
inline void write_navigation_state(QSettings& settings, const NavigationState& state) {
    settings.setValue(QStringLiteral("schema"), 2);
    settings.setValue(QStringLiteral("page"), nav_page_id(state.page));
    settings.setValue(QStringLiteral("mode"), static_cast<int>(state.mode));
    settings.setValue(QStringLiteral("lastVisible"), static_cast<int>(state.last_visible));
    settings.setValue(QStringLiteral("expanded"), state.expanded);
    settings.setValue(QStringLiteral("favourites"), state.favourites);
    settings.setValue(QStringLiteral("expandedWidth"), state.expanded_width);
    settings.setValue(QStringLiteral("recent"), state.recent);
}
} // namespace altair::ui
