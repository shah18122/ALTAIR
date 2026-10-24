// desktop/trade_stats.hpp -- what a list of round trips adds up to, and what
// each one cost, head by head.
//
// STATISTICS (trade_stats): trips, win rate, average win and loss, profit
// factor, expectancy, best and worst, gross / expenses / net, and over the
// DAILY net (trips summed by date) the Sharpe and Sortino ratios, annualised
// by sqrt(252), and the maximum drawdown of the cumulative net. A trip whose
// expenses could not be priced has no net: it counts in gross only, and the
// line says how many were left out.
//
// EXPENSES (fill_charges): one fill's brokerage, STT, exchange transaction
// charge, SEBI fee, stamp duty, IPFT and GST from config/charges.toml through
// app/demo_costs.hpp -- the same function the engine and the demos charge
// with -- on the right exchange (BSE fills pay BSE's rate). trip_detail_html
// lays a round trip out as its fills, each with its heads, and walks gross to
// net, so every figure on screen can be checked by hand.

#pragma once

#include <QDate>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <vector>

#if ALTAIR_HAVE_CHARGES_TOML
#include <app/demo_costs.hpp>
#include <risk/charges_toml.hpp>
#endif

namespace altair::ui {

struct TradeStatRow {
    QString date;                                         ///< YYYY-MM-DD
    double gross = 0.0;
    double expenses = std::numeric_limits<double>::quiet_NaN();
    double net = std::numeric_limits<double>::quiet_NaN();
};

struct TradeStats {
    int trips = 0, priced = 0, wins = 0, losses = 0, days = 0;
    double gross = 0.0, expenses = 0.0, net = 0.0;        ///< expenses and net over the priced trips
    double avg_win = 0.0, avg_loss = 0.0;                 ///< rupees, losses negative
    double win_rate = std::numeric_limits<double>::quiet_NaN();
    double profit_factor = std::numeric_limits<double>::quiet_NaN();   ///< gross wins / gross losses (net of expenses)
    double expectancy = std::numeric_limits<double>::quiet_NaN();      ///< mean net a trip
    double best = std::numeric_limits<double>::quiet_NaN(), worst = std::numeric_limits<double>::quiet_NaN();
    double sharpe = std::numeric_limits<double>::quiet_NaN();          ///< daily net, annualised
    double sortino = std::numeric_limits<double>::quiet_NaN();         ///< daily net over its downside deviation, annualised
    double max_drawdown = 0.0;                                         ///< rupees, cumulative daily net, peak to trough
};

[[nodiscard]] inline TradeStats trade_stats(const std::vector<TradeStatRow>& rows) {
    TradeStats s;
    double win_sum = 0.0, loss_sum = 0.0;
    std::map<QString, double> by_day;
    for (const auto& r : rows) {
        ++s.trips;
        s.gross += r.gross;
        if (!std::isfinite(r.net) || !std::isfinite(r.expenses)) continue;
        ++s.priced;
        s.expenses += r.expenses;
        s.net += r.net;
        if (r.net > 0.0) { ++s.wins; win_sum += r.net; }
        else if (r.net < 0.0) { ++s.losses; loss_sum += r.net; }
        s.best = std::isfinite(s.best) ? std::max(s.best, r.net) : r.net;
        s.worst = std::isfinite(s.worst) ? std::min(s.worst, r.net) : r.net;
        by_day[r.date] += r.net;
    }
    if (s.priced > 0) {
        s.win_rate = static_cast<double>(s.wins) / static_cast<double>(s.priced);
        s.expectancy = s.net / static_cast<double>(s.priced);
    }
    if (s.wins > 0) s.avg_win = win_sum / s.wins;
    if (s.losses > 0) s.avg_loss = loss_sum / s.losses;
    if (loss_sum < 0.0) s.profit_factor = win_sum / -loss_sum;
    else if (win_sum > 0.0) s.profit_factor = std::numeric_limits<double>::infinity();
    s.days = static_cast<int>(by_day.size());
    if (s.days >= 2) {
        double mean = 0.0;
        for (const auto& [d, v] : by_day) mean += v;
        mean /= s.days;
        double ss = 0.0, down = 0.0;
        for (const auto& [d, v] : by_day) {
            ss += (v - mean) * (v - mean);
            down += v < 0.0 ? v * v : 0.0;
        }
        const double sd = std::sqrt(ss / (s.days - 1));
        const double dd = std::sqrt(down / s.days);
        if (sd > 0.0) s.sharpe = mean / sd * std::sqrt(252.0);
        if (dd > 0.0) s.sortino = mean / dd * std::sqrt(252.0);
        else if (mean > 0.0) s.sortino = std::numeric_limits<double>::infinity();
    }
    double cum = 0.0, peak = 0.0;
    for (const auto& [d, v] : by_day) {
        cum += v;
        peak = std::max(peak, cum);
        s.max_drawdown = std::max(s.max_drawdown, peak - cum);
    }
    return s;
}

namespace trade_stats_detail {
[[nodiscard]] inline QString num(double v, int places = 2) {
    if (std::isinf(v)) return v > 0 ? QStringLiteral("∞") : QStringLiteral("−∞");
    if (!std::isfinite(v)) return QStringLiteral("—");
    return (v < 0 ? QStringLiteral("−") : QString()) + QString::number(std::abs(v), 'f', places);
}
[[nodiscard]] inline QString money(double v) {
    if (!std::isfinite(v)) return QStringLiteral("—");
    return (v < 0 ? QStringLiteral("−") : QString()) + QStringLiteral("Rs ") + QString::number(std::abs(v), 'f', 2);
}
} // namespace trade_stats_detail

/// One line: the ratios a decision rests on.
[[nodiscard]] inline QString trade_stats_html(const TradeStats& s) {
    using trade_stats_detail::money;
    using trade_stats_detail::num;
    QString t = QStringLiteral("%1 trip(s) over %2 day(s) · win rate %3 · avg win %4 / avg loss %5 · profit factor %6 · "
                               "expectancy %7 a trip · best %8 / worst %9 · <b>Sharpe %10</b> · <b>Sortino %11</b> · "
                               "max drawdown %12")
                    .arg(s.trips)
                    .arg(s.days)
                    .arg(std::isfinite(s.win_rate) ? num(100.0 * s.win_rate, 1) + QStringLiteral(" %") : QStringLiteral("—"),
                         money(s.avg_win), money(s.avg_loss), num(s.profit_factor), money(s.expectancy), money(s.best),
                         money(s.worst), num(s.sharpe), num(s.sortino))
                    .arg(money(s.max_drawdown));
    if (s.priced < s.trips)
        t += QStringLiteral(" · <span style='color:#F0B429'>%1 with unpriced expenses left out of the net</span>").arg(s.trips - s.priced);
    if (s.days < 2) t += QStringLiteral(" · Sharpe and Sortino need two days or more");
    return t;
}

// ---- expenses, head by head ----------------------------------------------------------

struct FillCharges {
    QString what;                    ///< "buy 70 RELIANCE on BSE at 998.60"
    bool priced = false;
    double turnover = 0.0;
    double brokerage = 0.0, stt = 0.0, exchange = 0.0, sebi = 0.0, stamp = 0.0, ipft = 0.0, gst = 0.0, total = 0.0;
};

/// What a symbol is, for the schedule: a futures, an option, or a stock.
enum class ChargeKind { Equity, Future, Option };

[[nodiscard]] inline ChargeKind charge_kind_of(const QString& symbol) {
    if (symbol.endsWith(QLatin1String("FUT"))) return ChargeKind::Future;
    if (symbol.endsWith(QLatin1String("CE")) || symbol.endsWith(QLatin1String("PE"))) return ChargeKind::Option;
    return ChargeKind::Equity;
}

/// One fill's charges by the shipped schedule; unpriced when none applies.
[[nodiscard]] inline FillCharges fill_charges(ChargeKind kind, bool buy, double qty, double price, const QDate& day, bool bse,
                                              const QString& what) {
    FillCharges c;
    c.what = what;
    c.turnover = qty * price;
#if ALTAIR_HAVE_CHARGES_TOML
    static const std::vector<ChargeSchedule> schedules = [] {
        std::vector<ChargeSchedule> s;
        if (!load_charges_file(ALTAIR_CHARGES_TOML, s)) s.clear();
        for (auto& x : s) x.verified = true;   // shown as computed; the file says whether it was checked
        return s;
    }();
    if (schedules.empty() || !day.isValid()) return c;
    const Segment seg = kind == ChargeKind::Future ? Segment::Fut : kind == ChargeKind::Option ? Segment::Opt : Segment::Cash;
    const std::int64_t ist = QDate(1970, 1, 1).daysTo(day) * 86400 + 12 * 3600;
    const auto f = demo_costs::fill(seg, buy ? Side::Buy : Side::Sell, qty, price, ist, schedules, bse ? Exchange::BSE : Exchange::NSE);
    if (!f.priced) return c;
    c.priced = true;
    c.brokerage = f.brokerage; c.stt = f.stt; c.exchange = f.exchange; c.sebi = f.sebi; c.stamp = f.stamp; c.ipft = f.ipft;
    c.gst = f.gst; c.total = f.total;
#else
    (void)kind; (void)buy; (void)qty; (void)price; (void)day; (void)bse;
#endif
    return c;
}

/// A round trip as its fills and their charges, gross walked to net.
/// `symbol` "RELIANCE BSE->NSE" is a netted cross-exchange pair: bought on the
/// first exchange at `entry`, sold on the second at `exit`, two fills. Any
/// other symbol is one instrument opened at `entry` and closed at `exit`.
[[nodiscard]] inline QString trip_detail_html(const QString& date, const QString& symbol, const QString& side, qint64 qty,
                                              double entry, double exit, double gross, double expenses, double net,
                                              const QString& why_in, const QString& why_out) {
    using trade_stats_detail::money;
    const QDate day = QDate::fromString(date.left(10), Qt::ISODate);
    std::vector<FillCharges> fills;
    const bool pair = symbol.contains(QLatin1String("->"));
    const double q = static_cast<double>(qty);
    if (pair) {
        const QString base = symbol.section(QLatin1Char(' '), 0, 0);
        const QString route = symbol.section(QLatin1Char(' '), 1, 1);
        const QString from = route.section(QLatin1String("->"), 0, 0), to = route.section(QLatin1String("->"), 1, 1);
        fills.push_back(fill_charges(ChargeKind::Equity, true, q, entry, day, from == QLatin1String("BSE"),
                                     QStringLiteral("buy %1 %2 on %3 at %4 (its ask)").arg(qty).arg(base, from, QString::number(entry, 'f', 2))));
        fills.push_back(fill_charges(ChargeKind::Equity, false, q, exit, day, to == QLatin1String("BSE"),
                                     QStringLiteral("sell %1 %2 on %3 at %4 (its bid)").arg(qty).arg(base, to, QString::number(exit, 'f', 2))));
    } else {
        const ChargeKind k = charge_kind_of(symbol);
        const bool longs = !side.startsWith(QLatin1String("short"), Qt::CaseInsensitive);
        const bool bse = symbol.contains(QLatin1String("BSE")) || symbol.startsWith(QLatin1String("SENSEX"))
                      || symbol.startsWith(QLatin1String("BANKEX"));
        fills.push_back(fill_charges(k, longs, q, entry, day, bse,
                                     QStringLiteral("%1 %2 %3 at %4 (entry)").arg(longs ? QStringLiteral("buy") : QStringLiteral("sell"))
                                         .arg(qty).arg(symbol, QString::number(entry, 'f', 2))));
        fills.push_back(fill_charges(k, !longs, q, exit, day, bse,
                                     QStringLiteral("%1 %2 %3 at %4 (exit)").arg(longs ? QStringLiteral("sell") : QStringLiteral("buy"))
                                         .arg(qty).arg(symbol, QString::number(exit, 'f', 2))));
    }
    QString h = QStringLiteral("<h3>%1 · %2</h3>").arg(symbol.toHtmlEscaped(), date.toHtmlEscaped());
    h += QStringLiteral("<p><b>Why in:</b> %1<br><b>Why out:</b> %2</p>").arg(why_in.toHtmlEscaped(), why_out.toHtmlEscaped());
    h += QStringLiteral("<table border='1' cellspacing='0' cellpadding='3'><tr><th>Fill</th><th>Turnover</th><th>Brokerage</th>"
                        "<th>STT</th><th>Exchange</th><th>SEBI</th><th>Stamp</th><th>IPFT</th><th>GST</th><th>Total</th></tr>");
    double computed = 0.0;
    bool all_priced = true;
    for (const auto& f : fills) {
        all_priced = all_priced && f.priced;
        computed += f.total;
        const auto cell = [&f](double v) { return f.priced ? QString::number(v, 'f', 2) : QStringLiteral("—"); };
        h += QStringLiteral("<tr><td>%1</td><td align='right'>%2</td><td align='right'>%3</td><td align='right'>%4</td>"
                            "<td align='right'>%5</td><td align='right'>%6</td><td align='right'>%7</td><td align='right'>%8</td>"
                            "<td align='right'>%9</td><td align='right'><b>%10</b></td></tr>")
                 .arg(f.what.toHtmlEscaped(), QString::number(f.turnover, 'f', 2), cell(f.brokerage), cell(f.stt), cell(f.exchange),
                      cell(f.sebi), cell(f.stamp), cell(f.ipft), cell(f.gst))
                 .arg(cell(f.total));
    }
    h += QStringLiteral("</table>");
    if (pair)
        h += QStringLiteral("<p>The clearing corporation nets the two legs: no exit fill, no exit expenses.</p>");
    h += QStringLiteral("<p><b>Gross</b> = %1 %2 = <b>%3</b><br>")
             .arg(pair ? QStringLiteral("(sell − buy) × qty =") : (side.startsWith(QLatin1String("short")) ? QStringLiteral("(entry − exit) × qty =")
                                                                                                         : QStringLiteral("(exit − entry) × qty =")),
                  QStringLiteral("(%1 − %2) × %3")
                      .arg(QString::number(pair || !side.startsWith(QLatin1String("short")) ? exit : entry, 'f', 2),
                           QString::number(pair || !side.startsWith(QLatin1String("short")) ? entry : exit, 'f', 2))
                      .arg(qty),
                  money(gross));
    h += QStringLiteral("<b>Expenses booked</b> = %1 (the fills above add to %2%3)<br>")
             .arg(money(expenses), all_priced ? money(computed) : QStringLiteral("— (a fill is unpriced)"),
                  all_priced && std::isfinite(expenses) && std::abs(computed - expenses) > 0.05
                      ? QStringLiteral("; the booked figure counts brokerage per actual fill, so a part-filled order pays it more than once")
                      : QString());
    h += QStringLiteral("<b>Net</b> = gross − expenses = %1 − %2 = <b>%3</b></p>").arg(money(gross), money(expenses), money(net));
    return h;
}

} // namespace altair::ui
