// Stable page identity is independent of visual group order. Cold UI path only.
#pragma once
#include <QStringList>
#include <array>

namespace altair::ui {
struct NavigationPage { const char* id; const char* label; int group; };
struct NavigationGroup { const char* id; const char* label; const char* short_label; const char* glyph; };
// The group ID is stable (saved state uses it); its label, glyph and place in
// the sidebar are presentation.
inline constexpr std::array<NavigationGroup, 7> kNavigationGroups{{
    {"market", "Live Market", "LIV", "\u25B2"},
    {"trades", "Trades & Logs", "LOG", "\u2261"},
    {"strategies", "Strategies & Tests", "STR", "\u265E"},
    {"models", "Models & Signals", "MDL", "\u25C6"},
    {"risk", "Risk & Portfolio", "RSK", "\u25CE"},
    {"data", "Data & Recorder", "DAT", "\u21C4"},
    {"system", "System & Broker", "SYS", "\u2699"},
}};
/// Group IDs saved by builds before 2026-11-05; read back as nothing rather
/// than as a corrupt preference (navigation_state.hpp drops them).
inline constexpr std::array<const char*, 2> kRetiredNavigationGroups{{"options", "accounts"}};
/// The order groups appear in, the order of a trading day: the live market,
/// what the models say, the strategies, the trades they made, risk, the data
/// behind them, and the system and broker underneath.
inline constexpr std::array<int, 7> kNavigationGroupOrder{{0, 3, 2, 1, 4, 5, 6}};
inline constexpr std::array<NavigationPage, 45> kNavigationPages{{
    {"market.grid", "Live Grid", 0},
    {"market.chart", "Chart", 0},
    {"market.terminal", "Terminal", 0},
    {"models.overview", "Models", 3},
    {"system.data-flow", "Data Flow", 5},
    {"accounts.account", "Broker Account", 6},
    {"risk.cost", "Cost", 4},
    {"options.analytics", "Analytics", 3},
    {"strategies.pair-trading", "Pair Trading", 2},
    {"models.dcf", "Value — DCF", 3},
    {"models.aggregator", "Aggregator", 3},
    {"system.audit", "Audit Trail", 1},
    {"accounts.execution", "Execution", 1},
    {"options.volatility", "Volatility", 3},
    {"risk.var", "Risk — VaR", 4},
    {"risk.portfolio", "Portfolio", 4},
    {"models.trees", "ML — Trees", 3},
    {"models.regimes", "Regimes", 3},
    {"models.forecast", "Forecast", 3},
    {"strategies.overview", "Strategies", 2},
    {"strategies.overnight", "Overnight Gap", 2},
    {"options.chain", "Options", 0},
    {"strategies.basis", "Basis", 2},
    {"system.flagging", "Flagging", 6},
    {"market.microstructure", "Microstructure", 0},
    {"risk.sizing", "Sizing & Limits", 4},
    {"strategies.cointegration", "Cointegration", 2},
    {"models.memory", "Memory", 3},
    {"accounts.connections", "Link Broker", 6},
    {"models.neural", "Neural", 3},
    {"models.features", "Features", 3},
    {"strategies.parity-calendar", "Arbitrage", 2},
    {"models.atlas", "Model Atlas", 3},
    {"accounts.fyers", "FYERS Primary", 6},
    {"accounts.brokers", "Brokers", 6},
    {"strategies.demo-trading", "Demo Trading", 2},
    {"strategies.threshold", "Threshold", 2},
    {"system.about", "About", 6},
    {"market.start", "Start here", 0},
    {"strategies.backtests", "Backtests", 2},
    {"models.findings", "Daily findings", 3},
    {"system.work-log", "Work log", 1},
    {"market.order-book-imbalance", "Order Book Imbalance", 0},
    {"system.server", "Server", 6},
    {"data.pipeline", "Who Does What", 5},
}};
// Traversal order is NOT a legacy page index. Groups by workflow (2026-11-05):
//   Live Market         Start here, Terminal, Order Book Imbalance, Options
//   Models & Signals    Daily findings, Model Atlas, Models
//   Strategies & Tests  Backtests (every trade log) first, then the strategies
//   Trades & Logs       Execution, Work log, Audit Trail
//   Risk & Portfolio    Portfolio, VaR, Cost
//   Data & Recorder     Who Does What (desktop/pipeline_page.hpp), Data Flow
//   System & Broker     Brokers (login), Server (jobs, latency), About
// The model pages (DCF, Aggregator, ML Trees, Regimes, Forecast, Memory,
// Neural, Features, Volatility, Analytics, Microstructure, Cointegration,
// Sizing, Flagging) are not listed: they open from the Model Atlas, which has
// the way back. Their routes stay so saved links and the Atlas still work.
inline constexpr std::array<int, 25> kNavigationOrder{{
    38, 2, 42, 21,
    40, 32, 3,
    39, 36, 31, 19, 8, 22, 20,
    12, 41, 11,
    15, 14, 6,
    44, 4,
    34, 43, 37
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
