// desktop/trade_reason.hpp -- why a trade was taken, from what was on disk.
//
// Smit's rule is that every trade carries its reasoning. The trade rows hold
// a why-in and a why-out; this page of a trade adds what the system was
// thinking around it, read from the files it already writes:
//
//   data/live/paper/decisions.csv   the model's own words, minute by minute
//                                    (live/ledger.hpp), from 30 minutes
//                                    before the entry to the exit;
//   data/live/paper/fills.csv       every fill of the trade, at what quote
//                                    and why;
//   data/verified/forecast_log/     what each forecast model said that day
//                                    for NIFTY, BANKNIFTY and India VIX, and
//                                    whether it was right
//                                    (app/forecast_curriculum_main.cpp).
//
// Reads files; decides nothing.
#pragma once

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QTextStream>

#include <cmath>
#include <vector>

namespace altair::ui {

/// One CSV line: commas, double quotes, doubled quotes inside them.
[[nodiscard]] inline QStringList csv_split(const QString& line) {
    QStringList out;
    QString cell;
    bool quoted = false;
    for (qsizetype i = 0; i < line.size(); ++i) {
        const QChar c = line[i];
        if (quoted) {
            if (c == QLatin1Char('"')) {
                if (i + 1 < line.size() && line[i + 1] == QLatin1Char('"')) { cell += c; ++i; }
                else quoted = false;
            } else {
                cell += c;
            }
        } else if (c == QLatin1Char('"')) {
            quoted = true;
        } else if (c == QLatin1Char(',')) {
            out << cell;
            cell.clear();
        } else {
            cell += c;
        }
    }
    out << cell;
    return out;
}

/// What is known about one trade, from its row.
struct TradeContext {
    QString model, symbol, date, entry_time, exit_time, why_in, why_out;
};

/// The trade's fields out of any trade log's header and row: the live
/// ledger's columns, or a backtest's (entry_date, reason, variant, ...).
[[nodiscard]] inline TradeContext trade_context_from_row(const QStringList& header, const QStringList& row) {
    const auto pick = [&](std::initializer_list<const char*> names) {
        for (const char* n : names)
            for (int c = 0; c < header.size(); ++c)
                if (header[c].compare(QLatin1String(n), Qt::CaseInsensitive) == 0 && c < row.size()) return row[c].trimmed();
        return QString{};
    };
    TradeContext t;
    t.model = pick({"model", "strategy", "variant", "rule"});
    t.symbol = pick({"symbol", "instrument", "underlying", "pair", "series"});
    t.entry_time = pick({"entry_time", "entry_ts", "in_time", "entry_date", "entry_day"});
    t.exit_time = pick({"exit_time", "exit_ts", "out_time", "exit_date", "exit_day"});
    t.date = pick({"date", "day", "entry_date", "exit_date"});
    if (t.date.isEmpty()) t.date = t.entry_time.left(10);
    t.date = t.date.left(10);
    t.why_in = pick({"why_in", "reason_in", "entry_reason", "reason", "why", "signal"});
    t.why_out = pick({"why_out", "reason_out", "exit_reason", "exit_why"});
    return t;
}

namespace trade_reason_detail {

/// Rows of a CSV whose `key` column passes `keep`, header first. At most
/// `limit` rows are kept (the last ones when there are more).
template <class Keep>
[[nodiscard]] inline std::vector<QStringList> rows(const QString& path, Keep keep, int limit = 400) {
    std::vector<QStringList> out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return out;
    QTextStream in(&f);
    const QStringList head = csv_split(in.readLine());
    out.push_back(head);
    while (!in.atEnd()) {
        const QString line = in.readLine();
        if (line.isEmpty()) continue;
        QStringList r = csv_split(line);
        if (!keep(head, r)) continue;
        out.push_back(std::move(r));
    }
    if (static_cast<int>(out.size()) > limit + 1) out.erase(out.begin() + 1, out.end() - limit);
    return out;
}

[[nodiscard]] inline QString cell(const QStringList& head, const QStringList& r, const char* name) {
    const int c = head.indexOf(QString::fromLatin1(name));
    return c >= 0 && c < r.size() ? r[c] : QString{};
}

/// "2026-10-24 09:20:00" from either that or "2026-10-24T09:20:00+05:30".
[[nodiscard]] inline QString stamp(QString t) {
    t = t.trimmed();
    if (t.size() > 10 && t[10] == QLatin1Char('T')) t[10] = QLatin1Char(' ');
    return t.left(19);
}

/// Thirty minutes before `t` ("YYYY-MM-DD HH:MM:SS"), same format; `t` when unparsable.
[[nodiscard]] inline QString minus_30m(const QString& t) {
    QDateTime d = QDateTime::fromString(t.left(19), QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    if (!d.isValid()) return t;
    return d.addSecs(-30 * 60).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

/// The forecast tracks that bear on `symbol`: its own index, and India VIX always.
[[nodiscard]] inline QStringList tracks_for(const QString& root, const QString& symbol) {
    const QDir dir(root + QStringLiteral("/data/verified/forecast_log"));
    const QString s = symbol.toUpper();
    const bool bank = s.contains(QLatin1String("BANKNIFTY")) || s.contains(QLatin1String("NIFTY BANK"));
    QStringList out;
    for (const QString& f : dir.entryList({QStringLiteral("*.csv")}, QDir::Files, QDir::Name)) {
        const QString n = f.toLower();
        const bool vix = n.startsWith(QLatin1String("india_vix")) || n.startsWith(QLatin1String("indiavix"));
        const bool bnf = n.startsWith(QLatin1String("banknifty"));
        const bool nifty = n.startsWith(QLatin1String("nifty"));
        if (vix || (bank ? bnf : nifty) || (s.isEmpty() && (bnf || nifty))) out << dir.filePath(f);
    }
    return out;
}

} // namespace trade_reason_detail

/// What each forecast model said on `date` for the tracks behind `symbol`:
/// one line per track (calls up and down, how many were right, the mean
/// forecast against what happened). Empty when the curriculum never ran.
[[nodiscard]] inline QString forecasts_on_html(const QString& root, const QString& symbol, const QString& date) {
    using namespace trade_reason_detail;
    if (date.size() < 10) return {};
    QString h;
    for (const QString& path : tracks_for(root, symbol)) {
        const auto rs = rows(path, [&date](const QStringList& head, const QStringList& r) {
            return stamp(cell(head, r, "time")).startsWith(date);
        });
        if (rs.size() < 2) continue;
        const QStringList& head = rs.front();
        int up = 0, down = 0, right = 0, scored = 0;
        double fsum = 0.0, last = 0.0, next = 0.0;
        QStringList models;
        for (std::size_t i = 1; i < rs.size(); ++i) {
            const auto& r = rs[i];
            const QString call = cell(head, r, "call"), result = cell(head, r, "result");
            if (call == QLatin1String("UP")) ++up;
            else if (call == QLatin1String("DOWN")) ++down;
            if (result == QLatin1String("RIGHT") || result == QLatin1String("WRONG")) {
                ++scored;
                if (result == QLatin1String("RIGHT")) ++right;
            }
            fsum += cell(head, r, "forecast_price").toDouble();
            last = cell(head, r, "last_price").toDouble();
            next = cell(head, r, "next_price").toDouble();
            const QString m = cell(head, r, "model");
            if (!models.contains(m) && models.size() < 6) models << m;
        }
        const double n = static_cast<double>(rs.size() - 1);
        const QString track = QFileInfo(path).completeBaseName();
        h += QStringLiteral("<tr><td><b>%1</b></td><td align='right'>%2</td><td align='right'>%3 up · %4 down</td>"
                            "<td align='right'>%5</td><td align='right'>%6</td><td align='right'>%7</td><td>%8</td></tr>")
                 .arg(track.toHtmlEscaped())
                 .arg(rs.size() - 1)
                 .arg(up).arg(down)
                 .arg(QString::number(last, 'f', 2), QString::number(fsum / n, 'f', 2), QString::number(next, 'f', 2))
                 .arg(scored > 0 ? QStringLiteral("%1 / %2 right").arg(right).arg(scored) : QStringLiteral("not scored yet"));
    }
    if (h.isEmpty()) return {};
    return QStringLiteral("<table border='1' cellspacing='0' cellpadding='3'><tr><th>Forecast track</th><th>Calls</th>"
                          "<th>Direction</th><th>Last</th><th>Mean forecast</th><th>Next</th><th>Scored</th></tr>%1</table>")
               .arg(h)
           + QStringLiteral("<p style='color:#8B949E'>Last, mean forecast and next are prices of the track's series; "
                            "a VIX row is the volatility forecast. From data/verified/forecast_log/.</p>");
}

/// The trade's reasoning, as HTML: why in and out, the model's decisions from
/// half an hour before the entry to the exit, its fills, and the forecasts
/// that day.
[[nodiscard]] inline QString trade_reason_html(const QString& root, const TradeContext& t) {
    using namespace trade_reason_detail;
    QString h = QStringLiteral("<h3>Why this trade</h3>");
    h += QStringLiteral("<table cellpadding='3'><tr><td><b>Model</b></td><td>%1</td></tr>"
                        "<tr><td><b>Instrument</b></td><td>%2</td></tr>"
                        "<tr><td><b>In</b></td><td>%3</td></tr><tr><td><b>Why in</b></td><td>%4</td></tr>"
                        "<tr><td><b>Out</b></td><td>%5</td></tr><tr><td><b>Why out</b></td><td>%6</td></tr></table>")
             .arg(t.model.isEmpty() ? QStringLiteral("—") : t.model.toHtmlEscaped(),
                  t.symbol.isEmpty() ? QStringLiteral("—") : t.symbol.toHtmlEscaped(),
                  t.entry_time.isEmpty() ? t.date.toHtmlEscaped() : t.entry_time.toHtmlEscaped(),
                  t.why_in.isEmpty() ? QStringLiteral("<i>not recorded</i>") : t.why_in.toHtmlEscaped(),
                  t.exit_time.isEmpty() ? QStringLiteral("—") : t.exit_time.toHtmlEscaped(),
                  t.why_out.isEmpty() ? QStringLiteral("<i>not recorded</i>") : t.why_out.toHtmlEscaped());

    const QString paper = root + QStringLiteral("/data/live/paper");
    const QString from = minus_30m(stamp(t.entry_time)), to = stamp(t.exit_time);
    const bool timed = t.entry_time.size() >= 16 && !t.model.isEmpty();
    if (timed) {
        const auto within = [&](const QString& time) {
            const QString s = stamp(time);
            return s >= from && (to.isEmpty() || s <= to);
        };
        const auto mine = [&](const QString& model) { return model == t.model || model.startsWith(t.model + QLatin1Char(' ')); };
        const auto ds = rows(paper + QStringLiteral("/decisions.csv"), [&](const QStringList& head, const QStringList& r) {
            return mine(cell(head, r, "model")) && within(cell(head, r, "time"));
        }, 120);
        h += QStringLiteral("<h4>The model's decisions, %1 to %2</h4>").arg(from.mid(11, 5), to.isEmpty() ? QStringLiteral("now") : to.mid(11, 5));
        if (ds.size() < 2) {
            h += QStringLiteral("<p><i>None in data/live/paper/decisions.csv for this window.</i></p>");
        } else {
            h += QStringLiteral("<table border='1' cellspacing='0' cellpadding='3'><tr><th>Time</th><th>Decision</th></tr>");
            for (std::size_t i = 1; i < ds.size(); ++i)
                h += QStringLiteral("<tr><td>%1</td><td>%2</td></tr>")
                         .arg(cell(ds.front(), ds[i], "time").mid(11, 8).toHtmlEscaped(),
                              cell(ds.front(), ds[i], "decision").toHtmlEscaped());
            h += QStringLiteral("</table>");
        }
        const auto fs = rows(paper + QStringLiteral("/fills.csv"), [&](const QStringList& head, const QStringList& r) {
            const QString s = stamp(cell(head, r, "time"));
            return mine(cell(head, r, "model")) && s >= stamp(t.entry_time) && (to.isEmpty() || s <= to)
                   && (t.symbol.isEmpty() || cell(head, r, "symbol") == t.symbol);
        }, 40);
        if (fs.size() >= 2) {
            h += QStringLiteral("<h4>Fills</h4><table border='1' cellspacing='0' cellpadding='3'><tr><th>Time</th><th>Side</th>"
                                "<th>Qty</th><th>Price</th><th>Quote at the time</th><th>Reason</th></tr>");
            for (std::size_t i = 1; i < fs.size(); ++i) {
                const auto& r = fs[i];
                h += QStringLiteral("<tr><td>%1</td><td>%2</td><td align='right'>%3</td><td align='right'>%4</td><td>%5</td><td>%6</td></tr>")
                         .arg(cell(fs.front(), r, "time").mid(11, 8).toHtmlEscaped(), cell(fs.front(), r, "side").toHtmlEscaped(),
                              cell(fs.front(), r, "qty").toHtmlEscaped(), cell(fs.front(), r, "price").toHtmlEscaped(),
                              cell(fs.front(), r, "at_quote").toHtmlEscaped(), cell(fs.front(), r, "reason").toHtmlEscaped());
            }
            h += QStringLiteral("</table>");
        }
    }
    const QString fc = forecasts_on_html(root, t.symbol, t.date);
    h += QStringLiteral("<h4>What the forecasts said on %1</h4>").arg(t.date.isEmpty() ? QStringLiteral("the day") : t.date.toHtmlEscaped());
    h += fc.isEmpty() ? QStringLiteral("<p><i>No forecast log for that day (the curriculum writes data/verified/forecast_log/ "
                                       "when it runs).</i></p>")
                      : fc;
    return h;
}

} // namespace altair::ui
