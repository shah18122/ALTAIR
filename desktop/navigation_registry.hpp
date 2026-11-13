// Stable page identity is independent of visual group order. Cold UI path only.
#pragma once
#include <QStringList>
#include <array>
#include <utility>

namespace altair::ui {
struct NavigationPage { const char* id; const char* label; int group; };
struct NavigationGroup { const char* id; const char* label; const char* short_label; const char* glyph; };
// The group ID is stable (saved state uses it); its label, glyph and place in
// the sidebar are presentation. Roadmap section 7 (2026-11-13): five main
// groups holding eleven screens, and one Lab group, collapsed by default, with
// every other page so nothing is lost.
inline constexpr std::array<NavigationGroup, 6> kNavigationGroups{{
    {"today", "Today", "DAY", "\u25B2"},
    {"trade", "Trade", "TRD", "\u21C5"},
    {"models", "Models", "MDL", "\u25C6"},
    {"risk", "Risk", "RSK", "\u25CE"},
    {"system", "System", "SYS", "\u2699"},
    {"lab", "Lab", "LAB", "\u2697"},
}};
inline constexpr std::size_t kLabGroup = 5;
/// Group IDs saved by older builds. navigation_state.hpp maps each to the
/// group that now holds its pages (nullptr: dropped) instead of treating the
/// saved preference as corrupt.
struct RetiredNavigationGroup { const char* id; const char* now; };
inline constexpr std::array<RetiredNavigationGroup, 6> kRetiredNavigationGroups{{
    {"options", nullptr}, {"accounts", nullptr},
    {"market", "trade"}, {"trades", "models"}, {"strategies", "models"}, {"data", "system"},
}};
inline constexpr std::array<int, 6> kNavigationGroupOrder{{0, 1, 2, 3, 4, 5}};
inline constexpr std::array<NavigationPage, 45> kNavigationPages{{
    {"market.grid", "Live Grid", 5},
    {"market.chart", "Chart", 5},
    {"market.terminal", "Terminal", 1},
    {"models.overview", "Models", 5},
    {"system.data-flow", "Data Flow", 5},
    {"accounts.account", "Broker Account", 5},
    {"risk.cost", "Cost", 5},
    {"options.analytics", "Analytics", 5},
    {"strategies.pair-trading", "Pair Trading", 5},
    {"models.dcf", "Value — DCF", 5},
    {"models.aggregator", "Aggregator", 5},
    {"system.audit", "Audit Trail", 5},
    {"accounts.execution", "Execution", 5},
    {"options.volatility", "Vol Forecasts", 2},
    {"risk.var", "Risk — VaR", 5},
    {"risk.portfolio", "Risk", 3},
    {"models.trees", "ML — Trees", 5},
    {"models.regimes", "Regimes", 5},
    {"models.forecast", "Forecast", 5},
    {"strategies.overview", "Strategies", 5},
    {"strategies.overnight", "Overnight Gap", 5},
    {"options.chain", "Option Chain", 1},
    {"strategies.basis", "Basis", 5},
    {"system.flagging", "Flagging", 5},
    {"market.microstructure", "Microstructure", 5},
    {"risk.sizing", "Sizing & Limits", 5},
    {"strategies.cointegration", "Cointegration", 5},
    {"models.memory", "Memory", 5},
    {"accounts.connections", "Link Broker", 5},
    {"models.neural", "Neural", 5},
    {"models.features", "Features", 5},
    {"strategies.parity-calendar", "Arbitrage", 5},
    {"models.atlas", "Scoreboard", 2},
    {"accounts.fyers", "FYERS Primary", 5},
    {"accounts.brokers", "Broker", 4},
    {"strategies.demo-trading", "Demo Trading", 5},
    {"strategies.threshold", "Threshold", 5},
    {"system.about", "About", 5},
    {"market.start", "Today", 0},
    {"strategies.backtests", "Trades", 2},
    {"models.findings", "Daily findings", 5},
    {"system.work-log", "Work log", 4},
    {"market.order-book-imbalance", "Order Book", 1},
    {"system.server", "Server", 5},
    {"data.pipeline", "Data & Recorder", 4},
}};
/// Labels before the 2026-11-13 tidy; the CLI and saved links still accept them.
inline constexpr std::array<std::pair<const char*, int>, 9> kLegacyNavigationLabels{{
    {"Volatility", 13},
    {"Portfolio", 15},
    {"Options", 21},
    {"Model Atlas", 32},
    {"Brokers", 34},
    {"Start here", 38},
    {"Backtests", 39},
    {"Order Book Imbalance", 42},
    {"Who Does What", 44},
}};
// Traversal order is NOT a legacy page index. The eleven main screens in
// workflow order, then the Lab in alphabetical order. Retired routes (Live
// Grid, Chart, the three old broker pages, Demo Trading) redirect instead.
inline constexpr std::array<int, 39> kNavigationOrder{{
    38,
    2, 21, 42,
    32, 39, 13,
    15,
    44, 34, 41,
    37, 10, 7, 31, 11, 22, 26, 6, 40, 4, 12, 30, 23, 18, 27, 24, 16, 3, 29, 20, 8, 17, 14, 43, 25, 19, 36, 9
}};
/// A page the sidebar lists (the rest open from the Model Atlas or search).
[[nodiscard]] constexpr bool nav_listed(int index) noexcept {
    for (const int i : kNavigationOrder) if (i == index) return true;
    return false;
}
/// Resolve old routes without renumbering model/page-stack references.
[[nodiscard]] constexpr int nav_destination(int index) noexcept {
    if (index < 0 || index >= static_cast<int>(kNavigationPages.size())) return -1;
    switch (index) {
    case 0: case 1: return 2;
    case 5: case 28: case 33: return 34;
    case 35: return 39;   // Demo Trading, retired 2026-11-12: Backtests
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
    // The page's first name, for a day: Order Flow.
    if (want == QLatin1String("market.order-flow") || want == QLatin1String("order flow")) return 42;
    for (const auto& [label, index] : kLegacyNavigationLabels)
        if (want == QString::fromUtf8(label).simplified().toCaseFolded()) return index;
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
