// live/trade_stats.hpp -- one stats sheet for every model: the same numbers,
// computed the same way, whatever produced the trades (altair_tune's replays,
// the paper ledger, a backtest). The idea is vectorbt's Portfolio.stats() and
// nautilus's PortfolioAnalyzer; the numbers are the ones a desk reads first.
//
//   net, gross, expenses, cost share   rupees; cost share = expenses / gross
//                                       (above 1: the costs ate more than
//                                       the market gave)
//   trades, hit rate, avg win/loss,
//   expectancy, profit factor          per round trip, net of expenses
//   Sharpe, Sortino                    on DAILY nets, x sqrt(252); NaN under
//                                       2 days (a day is the unit of evidence)
//   max drawdown, Calmar               on the cumulative daily net; Calmar =
//                                       annualised net / max drawdown
//   avg hold                           seconds
//   markouts 1 s / 10 s / 60 s         rupees a trade: where the mid went after
//                                       the entry filled, in the trade's favour.
//                                       Negative at 1 s means the fills are
//                                       picked off (adverse selection) before
//                                       the strategy's idea has had time to work.
//
// Pure arithmetic. NaN where a number does not exist; never a zero standing in
// for "unknown".

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace altair::live {

inline constexpr std::array<std::int64_t, 3> kMarkoutNs{1'000'000'000LL, 10'000'000'000LL, 60'000'000'000LL};

/// What one model did: its round trips and its days.
struct TradeRecord {
    std::vector<double> trade_net;    ///< per round trip, rupees, net of expenses
    std::vector<double> hold_s;       ///< per round trip
    std::vector<double> daily_net;    ///< per day traded or not, in date order
    double gross = 0.0, expenses = 0.0;
    std::array<double, 3> markout_sum{};   ///< rupees, summed over entries
    std::array<std::size_t, 3> markout_n{};
};

struct StatsSheet {
    double net = 0.0, gross = 0.0, expenses = 0.0, cost_share = 0.0;
    std::size_t trades = 0, days = 0;
    double hit_rate = 0.0, avg_win = 0.0, avg_loss = 0.0, expectancy = 0.0, profit_factor = 0.0;
    double sharpe = 0.0, sortino = 0.0, max_drawdown = 0.0, calmar = 0.0, avg_hold_s = 0.0;
    std::array<double, 3> markout{};
};

[[nodiscard]] inline StatsSheet stats_sheet(const TradeRecord& r) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    StatsSheet s;
    s.trades = r.trade_net.size();
    s.days = r.daily_net.size();
    s.gross = r.gross;
    s.expenses = r.expenses;
    s.net = 0.0;
    for (const double d : r.daily_net) s.net += d;
    s.cost_share = r.gross > 0.0 ? r.expenses / r.gross : nan;
    double win = 0.0, loss = 0.0;
    std::size_t nw = 0, nl = 0;
    for (const double t : r.trade_net) {
        if (t > 0.0) { win += t; ++nw; }
        else if (t < 0.0) { loss += t; ++nl; }
    }
    s.hit_rate = s.trades > 0 ? static_cast<double>(nw) / static_cast<double>(s.trades) : nan;
    s.avg_win = nw > 0 ? win / static_cast<double>(nw) : nan;
    s.avg_loss = nl > 0 ? loss / static_cast<double>(nl) : nan;
    s.expectancy = s.trades > 0 ? (win + loss) / static_cast<double>(s.trades) : nan;
    s.profit_factor = loss < 0.0 ? win / -loss : (win > 0.0 ? std::numeric_limits<double>::infinity() : nan);
    double hold = 0.0;
    for (const double h : r.hold_s) hold += h;
    s.avg_hold_s = r.hold_s.empty() ? nan : hold / static_cast<double>(r.hold_s.size());
    // Daily: Sharpe, Sortino, the drawdown and Calmar.
    if (s.days >= 2) {
        const double n = static_cast<double>(s.days);
        const double mean = s.net / n;
        double var = 0.0, down = 0.0;
        for (const double d : r.daily_net) {
            var += (d - mean) * (d - mean);
            if (d < 0.0) down += d * d;
        }
        const double sd = std::sqrt(var / (n - 1.0)), dd = std::sqrt(down / n);
        s.sharpe = sd > 0.0 ? mean / sd * std::sqrt(252.0) : nan;
        s.sortino = dd > 0.0 ? mean / dd * std::sqrt(252.0) : nan;
    } else {
        s.sharpe = s.sortino = nan;
    }
    double peak = 0.0, cum = 0.0, mdd = 0.0;
    for (const double d : r.daily_net) {
        cum += d;
        peak = std::max(peak, cum);
        mdd = std::max(mdd, peak - cum);
    }
    s.max_drawdown = mdd;
    s.calmar = mdd > 0.0 && s.days > 0 ? (s.net / static_cast<double>(s.days) * 252.0) / mdd : nan;
    for (std::size_t k = 0; k < 3; ++k)
        s.markout[k] = r.markout_n[k] > 0 ? r.markout_sum[k] / static_cast<double>(r.markout_n[k]) : nan;
    return s;
}

inline constexpr const char* kStatsHeader =
    "net,gross,expenses,cost_share,trades,days,hit_rate,avg_win,avg_loss,expectancy,profit_factor,"
    "sharpe,sortino,max_drawdown,calmar,avg_hold_s,markout_1s,markout_10s,markout_60s";

/// The sheet as one CSV row (kStatsHeader's order); NaN is an empty field.
[[nodiscard]] inline std::string stats_row(const StatsSheet& s) {
    std::string o;
    const auto add = [&](double v, int digits) {
        if (!o.empty()) o.push_back(',');
        if (!std::isfinite(v)) return;
        char b[48];
        std::snprintf(b, sizeof b, "%.*f", digits, v);
        o += b;
    };
    add(s.net, 2); add(s.gross, 2); add(s.expenses, 2); add(s.cost_share, 3);
    add(static_cast<double>(s.trades), 0); add(static_cast<double>(s.days), 0);
    add(s.hit_rate, 3); add(s.avg_win, 2); add(s.avg_loss, 2); add(s.expectancy, 2); add(s.profit_factor, 3);
    add(s.sharpe, 3); add(s.sortino, 3); add(s.max_drawdown, 2); add(s.calmar, 3); add(s.avg_hold_s, 1);
    add(s.markout[0], 2); add(s.markout[1], 2); add(s.markout[2], 2);
    return o;
}

/// One line for people.
[[nodiscard]] inline std::string stats_text(const StatsSheet& s) {
    const auto f = [](double v, int digits) {
        if (!std::isfinite(v)) return std::string("-");
        char b[48];
        std::snprintf(b, sizeof b, "%.*f", digits, v);
        return std::string(b);
    };
    return "net Rs " + f(s.net, 0) + " over " + std::to_string(s.trades) + " trade(s) / " + std::to_string(s.days)
         + " day(s); hit " + f(100.0 * s.hit_rate, 0) + " %, expectancy Rs " + f(s.expectancy, 0) + ", PF "
         + f(s.profit_factor, 2) + ", Sharpe " + f(s.sharpe, 2) + ", max DD Rs " + f(s.max_drawdown, 0)
         + ", costs " + f(100.0 * s.cost_share, 0) + " % of gross, markout 1s/10s/60s Rs " + f(s.markout[0], 0) + "/"
         + f(s.markout[1], 0) + "/" + f(s.markout[2], 0);
}

} // namespace altair::live
