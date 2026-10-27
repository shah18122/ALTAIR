// desktop/saved_layouts.hpp -- the window arranged for a time of day.
//
// A layout is: the page open, the Terminal's view, the sidebar expanded or
// collapsed, the news bar shown or hidden, the Simple/Advanced view, and the
// Terminal's pane sizes. Three come built in (Morning: yesterday's findings
// and the news; Trading: the Terminal; Review: every trade); any arrangement
// can be saved under a name and restored with one click, from the status
// bar's Layout menu or Ctrl+K.
//
// Stored per user in the window's own settings, beside the sidebar state.
// Presentation only: a layout never touches LIVE, orders or a strategy.
#pragma once

#include <QSettings>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <vector>

namespace altair::ui {

struct SavedLayout {
    QString name;
    QString page;            ///< stable page id (navigation_registry.hpp)
    QString terminal_view;   ///< watch, chain, models; empty leaves it
    bool sidebar_compact = false;
    bool news = true;
    int simple = -1;         ///< 1 Simple, 0 Advanced, -1 leave it
    QStringList terminal_sizes;   ///< the Terminal splitter, empty leaves it
};

[[nodiscard]] inline std::vector<SavedLayout> builtin_layouts() {
    return {
        {QStringLiteral("Morning"), QStringLiteral("models.findings"), {}, false, true, -1, {}},
        {QStringLiteral("Trading"), QStringLiteral("market.terminal"), QStringLiteral("watch"), true, true, -1, {}},
        {QStringLiteral("Review"), QStringLiteral("strategies.backtests"), {}, false, false, -1, {}},
    };
}

namespace saved_layouts_detail {
inline const QString kGroup = QStringLiteral("layouts/v1");
}

/// The layouts the person saved, by name.
[[nodiscard]] inline std::vector<SavedLayout> load_layouts(QSettings& s) {
    std::vector<SavedLayout> out;
    s.beginGroup(saved_layouts_detail::kGroup);
    for (const QString& key : s.childGroups()) {
        s.beginGroup(key);
        SavedLayout l;
        l.name = s.value(QStringLiteral("name")).toString();
        l.page = s.value(QStringLiteral("page")).toString();
        l.terminal_view = s.value(QStringLiteral("terminal_view")).toString();
        l.sidebar_compact = s.value(QStringLiteral("sidebar_compact"), false).toBool();
        l.news = s.value(QStringLiteral("news"), true).toBool();
        l.simple = s.value(QStringLiteral("simple"), -1).toInt();
        l.terminal_sizes = s.value(QStringLiteral("terminal_sizes")).toStringList();
        s.endGroup();
        if (!l.name.isEmpty() && !l.page.isEmpty()) out.push_back(std::move(l));
    }
    s.endGroup();
    std::sort(out.begin(), out.end(), [](const SavedLayout& a, const SavedLayout& b) { return a.name < b.name; });
    return out;
}

/// Save (or replace) `l` under its name. Hex keys: a name cannot reach another setting.
inline void save_layout(QSettings& s, const SavedLayout& l) {
    s.beginGroup(saved_layouts_detail::kGroup);
    s.beginGroup(QString::fromLatin1(l.name.toUtf8().toHex()));
    s.setValue(QStringLiteral("name"), l.name);
    s.setValue(QStringLiteral("page"), l.page);
    s.setValue(QStringLiteral("terminal_view"), l.terminal_view);
    s.setValue(QStringLiteral("sidebar_compact"), l.sidebar_compact);
    s.setValue(QStringLiteral("news"), l.news);
    s.setValue(QStringLiteral("simple"), l.simple);
    s.setValue(QStringLiteral("terminal_sizes"), l.terminal_sizes);
    s.endGroup();
    s.endGroup();
}

inline void remove_layout(QSettings& s, const QString& name) {
    s.beginGroup(saved_layouts_detail::kGroup);
    s.remove(QString::fromLatin1(name.toUtf8().toHex()));
    s.endGroup();
}

} // namespace altair::ui
