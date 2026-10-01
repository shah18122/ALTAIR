// Stable page identity is independent of visual group order. Cold UI path only.
#pragma once
#include <QStringList>
#include <array>

namespace altair::ui {
struct NavigationPage { const char* id; const char* label; int group; };
struct NavigationGroup { const char* id; const char* label; const char* short_label; };
inline constexpr std::array<NavigationGroup, 7> kNavigationGroups{{
    {"market", "Market & Terminal", "MKT"},
    {"options", "Options & Volatility", "OPT"},
    {"strategies", "Strategies", "STR"},
    {"models", "Models & Research", "MDL"},
    {"risk", "Risk & Portfolio", "RSK"},
    {"accounts", "Execution & Accounts", "ACC"},
    {"system", "System & Audit", "SYS"},
}};
inline constexpr std::array<NavigationPage, 36> kNavigationPages{{
    {"market.grid", "Live Grid", 0},
    {"market.chart", "Chart", 0},
    {"market.terminal", "Terminal", 0},
    {"models.overview", "Models", 3},
    {"system.data-flow", "Data Flow", 6},
    {"accounts.kite", "Kite Account", 5},
    {"risk.cost", "Cost", 4},
    {"options.analytics", "Analytics", 1},
    {"strategies.pair-trading", "Pair Trading", 2},
    {"models.dcf", "Value — DCF", 3},
    {"models.aggregator", "Aggregator", 3},
    {"system.audit", "Audit Trail", 6},
    {"accounts.execution", "Execution", 5},
    {"options.volatility", "Volatility", 1},
    {"risk.var", "Risk — VaR", 4},
    {"risk.portfolio", "Portfolio", 4},
    {"models.trees", "ML — Trees", 3},
    {"models.regimes", "Regimes", 3},
    {"models.forecast", "Forecast", 3},
    {"strategies.overview", "Strategies", 2},
    {"strategies.overnight", "Overnight Gap", 2},
    {"options.chain", "Options", 1},
    {"strategies.basis", "Basis", 2},
    {"system.flagging", "Flagging", 6},
    {"market.microstructure", "Microstructure", 0},
    {"risk.sizing", "Sizing & Limits", 4},
    {"strategies.cointegration", "Cointegration", 2},
    {"models.memory", "Memory", 3},
    {"accounts.connections", "Link Kite", 5},
    {"models.neural", "Neural", 3},
    {"models.features", "Features", 3},
    {"strategies.parity-calendar", "Arbitrage", 2},
    {"models.atlas", "Model Atlas", 3},
    {"accounts.fyers", "FYERS Primary", 5},
    {"accounts.brokers", "Brokers", 5},
    {"strategies.demo-trading", "Demo Trading", 2},
}};
// Traversal order is NOT a legacy page index.
inline constexpr std::array<int, 31> kNavigationOrder{{
    2, 24, 21, 13, 7, 19, 8, 22, 31, 26, 20, 35, 32, 3, 18, 17, 16, 29, 10, 30, 27, 9, 15, 14, 25, 6, 34, 12, 4, 23, 11
}};
/// Resolve old routes without renumbering model/page-stack references.
[[nodiscard]] constexpr int nav_destination(int index) noexcept {
    if (index < 0 || index >= static_cast<int>(kNavigationPages.size())) return -1;
    switch (index) {
    case 0: case 1: return 2;
    case 5: case 28: case 33: return 34;
    default: return index;
    }
}
/// Only canonical routes appear in navigation, search and saved lists.
[[nodiscard]] inline bool nav_visible(int index) noexcept {
    return index >= 0 && nav_destination(index) == index;
}
[[nodiscard]] inline QStringList nav_page_names() {
    QStringList result;
    for (const auto& page : kNavigationPages) result.append(QString::fromUtf8(page.label));
    return result;
}
[[nodiscard]] inline QString nav_page_id(int index) {
    return index >= 0 && index < static_cast<int>(kNavigationPages.size())
        ? QString::fromUtf8(kNavigationPages[static_cast<std::size_t>(index)].id) : QString{};
}
// Retain CLI's case/whitespace-insensitive legacy labels; also accept stable IDs.
[[nodiscard]] inline int nav_page_index(const QString& name) {
    const auto want = name.simplified().toCaseFolded();
    // Saved routes from the pre-P5 pair page remain valid after the visible
    // label changed from Ratio Spread to Pair Trading.
    if (want == QLatin1String("strategies.ratio-spread")
        || want == QLatin1String("ratio spread")) {
        return 8;
    }
    if (want == QLatin1String("parity & calendar")) return 31;
    for (int i = 0; i < static_cast<int>(kNavigationPages.size()); ++i) {
        const auto& page = kNavigationPages[static_cast<std::size_t>(i)];
        if (want == QString::fromUtf8(page.id) ||
            want == QString::fromUtf8(page.label).simplified().toCaseFolded()) return i;
    }
    return -1;
}
[[nodiscard]] inline bool nav_known_id(const QString& id) {
    const int i = nav_page_index(id);
    return i >= 0 && nav_page_id(i) == id;
}
} // namespace altair::ui
