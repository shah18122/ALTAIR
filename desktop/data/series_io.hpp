// desktop/data/series_io.hpp -- read a dataset series: closes and their
// timestamps, and the path to one.
//
// Lifted out of quant_pages.hpp so the Terminal can read a spot and a VIX
// close without including thirty headers of model zoo to get them. There is
// still exactly ONE definition of each -- quant_pages.hpp includes this file
// -- and cmake/NamespaceAudit.cmake would refuse a second.

#pragma once

#include <QDateTime>
#include <QFile>
#include <QLatin1String>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <QTimeZone>

#include <cstdint>
#include <vector>

namespace altair::ui {

/// Daily closes from `dataset/`, read by the UI's own loader.
///
/// A SECOND reader, deliberately -- the model tests each have their own. Two
/// independent paths to the same series is what makes a number a property of
/// the data rather than of one parser, and it is the reason the figures on
/// these pages can be compared against the test output at all.
/// Bar stamps in nanoseconds, alongside the closes. P32-06.
///
/// `ui_load_closes` throws the time column away, which is why the spot
/// forecast could not say which bar it was about. This keeps it.
///
/// THE FILES ARE NOT ALL THE SAME SHAPE. Two forms appear on disk:
/// `2026-09-08` and `2026-09-08T15:29:00+05:30`. Both are parsed; a row whose
/// stamp parses as neither is DROPPED ALONG WITH ITS CLOSE, so the two
/// vectors stay the same length. Dropping the close and keeping the price
/// would silently shift every later stamp by one bar, which is the failure
/// mode this function exists to prevent.
struct UiStamped {
    std::vector<double> closes;
    std::vector<std::int64_t> stamps_ns;
};

[[nodiscard]] inline UiStamped ui_load_stamped(const QString& path) {
    UiStamped out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) { return out; }
    QTextStream ts(&f);
    ts.readLine();                                  // header
    while (!ts.atEnd()) {
        const QString line = ts.readLine();
        if (line.trimmed().isEmpty()) { continue; }
        const QStringList col = line.split(QLatin1Char(','));
        if (col.size() < 5) { continue; }
        bool ok = false;
        const double close = col[4].toDouble(&ok);
        if (!ok || !(close > 0.0)) { continue; }

        const QString stamp = col[0].trimmed();
        QDateTime dt = QDateTime::fromString(stamp, Qt::ISODate);
        if (!dt.isValid()) {
            dt = QDateTime::fromString(stamp.left(10),
                                       QStringLiteral("yyyy-MM-dd"));
            // A DATE IS NOT AN INSTANT UNTIL SOMEBODY SAYS WHICH ZONE. A
            // bare 2026-09-08 in an NSE file is an IST trading day, and
            // reading it as UTC lands it 5.5 hours early -- which is the same
            // class of bug P11Q-11 hit on the watchlist's expiry column,
            // where it printed a contract expiring the day before it does.
            if (dt.isValid()) {
                dt.setTimeZone(QTimeZone(5 * 3600 + 30 * 60));
            }
        }
        if (!dt.isValid()) { continue; }
        out.closes.push_back(close);
        out.stamps_ns.push_back(
            static_cast<std::int64_t>(dt.toMSecsSinceEpoch()) * 1'000'000LL);
    }
    return out;
}

/// `<root>/spot/<sym>/<interval>/` -- one place, so a page cannot invent a
/// path shape that does not exist on disk.
[[nodiscard]] inline QString spot_path(const QString& root, const QString& sym,
                                       const char* interval,
                                       const char* file = "all.csv") {
    return QStringLiteral("%1/spot/%2/%3/%4")
        .arg(root, sym, QLatin1String(interval), QLatin1String(file));
}

}  // namespace altair::ui
