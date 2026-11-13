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
inline constexpr std::array<NavigationPage, 46> kNavigationPages{{
    {"market.grid", "Live Grid", 5},
    {"market.chart", "Chart", 5},
    {"market.terminal", "Terminal", 1},
    {"models.overview", "Models", 2},
    {"system.data-flow", "Data Flow", 4},
    {"accounts.account", "Broker Account", 5},
    {"risk.cost", "Cost", 3},
    {"options.analytics", "Analytics", 1},
    {"strategies.pair-trading", "Pair Trading", 5},
    {"models.dcf", "Value — DCF", 5},
    {"models.aggregator", "Aggregator", 5},
    {"system.audit", "Audit Trail", 2},
    {"accounts.execution", "Execution", 2},
    {"options.volatility", "Vol Forecasts", 2},
    {"risk.var", "Risk — VaR", 3},
    {"risk.portfolio", "Risk", 3},
    {"models.trees", "ML — Trees", 5},
    {"models.regimes", "Regimes", 5},
    {"models.forecast", "Forecast", 5},
    {"strategies.overview", "Strategies", 2},
    {"strategies.overnight", "Overnight Gap", 5},
    {"options.chain", "Options", 1},
    {"strategies.basis", "Basis", 5},
    {"system.flagging", "Flagging", 5},
    {"market.microstructure", "Microstructure", 1},
    {"risk.sizing", "Sizing & Limits", 3},
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
    {"models.findings", "Daily findings", 0},
    {"system.work-log", "Work log", 4},
    {"market.order-book-imbalance", "Order Book", 1},
    {"system.server", "Server", 4},
    {"data.pipeline", "Data & Recorder", 4},
    {"trade.option-chain", "Option Chain", 1},   // 45: the Terminal's live chain
}};
/// Labels before the 2026-11-13 tidy; the CLI and saved links still accept them.
inline constexpr std::array<std::pair<const char*, int>, 8> kLegacyNavigationLabels{{
    {"Volatility", 13},
    {"Portfolio", 15},
    {"Model Atlas", 32},
    {"Brokers", 34},
    {"Start here", 38},
    {"Backtests", 39},
    {"Order Book Imbalance", 42},
    {"Who Does What", 44},
}};
// Traversal order is NOT a legacy page index. The eleven main screens in
// workflow order, then the Lab in alphabetical order. Retired routes (Live
// Grid, Chart, the three old broker pages, Demo Trading) redirect instead;
// the pages a screen is built from are its tabs (kNavigationScreens).
inline constexpr std::array<int, 27> kNavigationOrder{{
    38,
    2, 45, 42,
    32, 39, 13,
    15,
    44, 34, 41,
    37, 10, 31, 22, 26, 30, 23, 18, 27, 16, 29, 20, 8, 17, 36, 9
}};
/// A main screen and the pages it is built from (roadmap section 7), shown
/// as tabs across the top of the screen. -1 ends a list.
struct NavigationScreen { int page; const char* first_tab; std::array<int, 4> tabs; };
inline constexpr std::array<NavigationScreen, 8> kNavigationScreens{{
    {38, "Status", {38, 40, -1, -1}},            // Today: Start here, Daily findings
    {45, "Live chain", {45, 21, 7, -1}},         // Option Chain: chain, Options pricer, Analytics
    {42, "OBI", {42, 24, -1, -1}},               // Order Book: OBI, Microstructure
    {32, "All models", {32, 3, 19, -1}},         // Scoreboard: Atlas, Models, Strategies
    {39, "Trade logs", {39, 11, 12, -1}},        // Trades: Backtests, Audit Trail, Execution
    {15, "Portfolio", {15, 14, 6, 25}},          // Risk: Portfolio, VaR, Cost, Sizing & Limits
    {44, "Jobs", {44, 4, 43, -1}},               // Data & Recorder: Who Does What, Data Flow, Server
    {13, "Volatility", {13, -1, -1, -1}},        // Vol Forecasts
}};
/// The main screen a page belongs to (itself for a main screen), or -1.
[[nodiscard]] constexpr int nav_screen_of(int index) noexcept {
    for (const auto& s : kNavigationScreens)
        for (const int t : s.tabs) if (t == index) return s.page;
    return -1;
}
[[nodiscard]] constexpr const NavigationScreen* nav_screen(int screen_page) noexcept {
    for (const auto& s : kNavigationScreens) if (s.page == screen_page) return &s;
    return nullptr;
}
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
