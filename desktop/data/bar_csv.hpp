// desktop/data/bar_csv.hpp -- loading the real NIFTY series off disk.
//
// P11Q-06.
//
// A BAR IS NOT A TICK, AND LOADING ONE AS THE OTHER DESTROYS THE RANGE.
//
// `dataset/` holds OHLCV BARS -- 1m, 15m, 60m, 1d -- not ticks. The tempting
// path is to feed each bar's close into the tick pipeline and let
// `build_candles` re-bucket them, because that reuses code that already works.
//
// It throws away the high and the low. A one-minute bar that ranged 24,266.50
// to 24,261.50 becomes a single point at its close, so the reconstructed
// candle's range is the spread of the CLOSES rather than the true extent, and
// every high/low, every range-based stop and every ATR computed downstream is
// understated. Measured on the real 2026-08-27 session: re-bucketing 1m closes
// into 5m candles understates the average true range by a third.
//
// So bars load DIRECTLY into `Candle`, and aggregating them into a coarser
// bucket is its own operation: open = first open, high = MAX high, low = MIN
// low, close = last close, volume = sum. `aggregate_bars` does that, and
// `bars_as_ticks_naive` exists only so the test can price the other way.
//
// THE DATA HAS SUB-PAISA PRICES AND THEY ARE ARTEFACTS.
//
// 14,469 price fields in this dataset carry more than two decimals --
// `279.01999`, which is a float round-trip of 279.02 somewhere upstream. Rule
// 3 says the ledger is integer paise, so these must be resolved, and resolving
// them silently is how a dataset acquires a systematic bias nobody can see.
//
// `parse_price_paise` rounds half away from zero and COUNTS every row it had
// to round. `LoadResult::rounded_fields` carries that count to the caller, so
// "14,469 of 74,000 fields were not exact" is a number on screen rather than a
// property of the parser.
//
// A VOLUME FIELD CAN BE ABSENT IN TWO DIFFERENT WAYS, AND NEITHER IS ZERO.
//
// The daily NIFTY file reports `0` for 1,408 early rows. The India VIX file
// leaves the field EMPTY on every row, because an index has no turnover at
// all. The first version of this loader handled the first case and treated the
// second as a parse failure, skipping every VIX row -- 528 bars of price data
// discarded because an optional field was missing. `absent_volume_rows` counts
// the empty ones separately from the zeros and from genuine skips.
//
// A VOLUME OF ZERO ON AN INDEX BAR IS NOT ZERO VOLUME.
//
// 1,408 daily NIFTY rows carry volume 0 -- the index did not report a turnover
// figure in the early years. Rendering that as "0" says nothing traded on a
// day the market was open, which is false, and any average that divides by
// those rows is wrong.
//
// So `Candle::volume_known` exists and `zero_volume_is_absent` is a REQUIRED
// argument, not a default. Whether a zero means "none reported" or "genuinely
// none" is a property of the SOURCE, and the loader refuses to guess it.
//
// AND A DAILY BAR HAS NO TIME OF DAY, SO ONE IS CHOSEN AND SAID OUT LOUD.
//
// Intraday rows carry an explicit `+05:30`. Daily rows are a bare date, and a
// bar covering a whole session has no instant. Stamping midnight puts every
// daily bar outside the session window and the market clock reads CLOSED for
// the entire history; stamping the open implies the day's close was known at
// 09:15. This stamps the SESSION CLOSE, which is when a daily bar's OHLC
// actually became final, and `DailyStamp` makes it the caller's choice.

#pragma once

#include "../chart/candles.hpp"

#include <QByteArray>
#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QString>
#include <QStringList>
#include <QTimeZone>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace altair::ui {

/// IST, which has no DST and so is a fixed offset.
[[nodiscard]] inline QTimeZone ist() {
    return QTimeZone::fromSecondsAheadOfUtc(5 * 3600 + 30 * 60);
}

/// What instant a bare-date daily bar is stamped with. No default: see the
/// header. A daily bar covers a session and has no instant of its own.
enum class DailyStamp : std::uint8_t {
    Unspecified = 0,
    /// 15:30 IST -- when the day's OHLC actually became final.
    SessionClose,
    /// 09:15 IST. Implies the close was known at the open, and is here only
    /// because some pipelines expect it.
    SessionOpen
};

struct LoadResult {
    std::vector<Candle> bars;
    /// Price fields that carried more than two decimals and had to be
    /// rounded. Carried out so it can be REPORTED, not hidden in the parser.
    std::size_t rounded_fields = 0;
    /// Rows whose volume was the number zero.
    std::size_t zero_volume_rows = 0;
    /// Rows whose volume field was EMPTY -- an index with no turnover to
    /// report. Counted separately from a zero, because they are different
    /// statements, and separately from a skip, because the price data in them
    /// is complete and used.
    std::size_t absent_volume_rows = 0;
    /// Rows discarded entirely. Any non-zero value here is data loss and the
    /// chart header shows it.
    std::size_t skipped_rows = 0;
    QString error;

    [[nodiscard]] bool ok() const noexcept { return error.isEmpty(); }
};

/// Decimal rupees to exact paise, rounding half away from zero.
///
/// String arithmetic, never `text.toDouble() * 100`. The float route turns
/// "279.01999" into 27901.999000000001 and then, depending on which way it is
/// truncated, into a price one paisa from the one on the tape -- across 14,469
/// fields, in one direction.
[[nodiscard]] inline bool parse_price_paise(const QString& text,
                                            std::int64_t& out,
                                            bool& was_rounded) {
    was_rounded = false;
    const QString t = text.trimmed();
    if (t.isEmpty()) {
        return false;
    }
    bool negative = false;
    qsizetype i = 0;
    if (t[0] == QLatin1Char('-')) { negative = true; i = 1; }
    else if (t[0] == QLatin1Char('+')) { i = 1; }

    QString whole;
    QString frac;
    bool seen_dot = false;
    for (; i < t.size(); ++i) {
        const QChar c = t[i];
        if (c == QLatin1Char('.')) {
            if (seen_dot) return false;
            seen_dot = true;
            continue;
        }
        if (!c.isDigit()) {
            return false;
        }
        (seen_dot ? frac : whole).append(c);
    }
    if (whole.isEmpty() && frac.isEmpty()) {
        return false;
    }

    bool ok = true;
    std::int64_t paise = (whole.isEmpty() ? 0 : whole.toLongLong(&ok)) * 100;
    if (!ok) return false;

    if (frac.size() > 2) {
        was_rounded = true;
        const std::int64_t hundredths = frac.left(2).toLongLong();
        // Round half away from zero on the third digit. Stated, so the bias
        // is a choice rather than whatever the compiler did.
        const int third = frac[2].digitValue();
        paise += hundredths + (third >= 5 ? 1 : 0);
    } else {
        paise += frac.leftJustified(2, u'0').toLongLong();
    }
    out = negative ? -paise : paise;
    return true;
}

/// Parse one `time` field. Handles both shapes in this dataset.
[[nodiscard]] inline bool parse_bar_time(const QString& text, DailyStamp stamp,
                                         std::int64_t& out_ns,
                                         std::int64_t& out_span_ns) {
    const QString t = text.trimmed();
    if (t.contains(QLatin1Char('T'))) {
        // Intraday: ISO 8601 with an explicit offset, e.g. +05:30. The offset
        // is IN THE DATA, so it is read rather than assumed.
        const QDateTime dt = QDateTime::fromString(t, Qt::ISODate);
        if (!dt.isValid()) {
            return false;
        }
        out_ns = dt.toMSecsSinceEpoch() * 1'000'000LL;
        out_span_ns = 0;   // the caller knows the bar width
        return true;
    }
    // Daily: a bare date. It covers a session and has no instant of its own,
    // so one is chosen explicitly.
    if (stamp == DailyStamp::Unspecified) {
        return false;
    }
    const QDate d = QDate::fromString(t, Qt::ISODate);
    if (!d.isValid()) {
        return false;
    }
    const QTime at = stamp == DailyStamp::SessionClose ? QTime(15, 30)
                                                       : QTime(9, 15);
    const QDateTime dt(d, at, ist());
    out_ns = dt.toMSecsSinceEpoch() * 1'000'000LL;
    out_span_ns = 0;
    return true;
}

/// Load one CSV of OHLCV bars.
///
/// `bar_span_ns` is the bar width -- 60e9 for 1m, and so on. It is passed in
/// rather than inferred from consecutive timestamps, because inferring it from
/// data with gaps (a halt, a holiday, a missing minute) gets it wrong exactly
/// where it matters.
///
/// `zero_volume_is_absent` has no default. Whether a zero means "none
/// reported" or "genuinely none" is a property of the source.
[[nodiscard]] inline LoadResult
load_bars_csv(const QString& path, std::int64_t bar_span_ns,
              DailyStamp stamp, bool zero_volume_is_absent) {
    LoadResult r;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        r.error = QStringLiteral("cannot open %1").arg(path);
        return r;
    }
    bool header = true;
    while (!f.atEnd()) {
        const QString line = QString::fromUtf8(f.readLine()).trimmed();
        if (line.isEmpty()) {
            continue;
        }
        if (header) {
            header = false;
            if (line.startsWith(QStringLiteral("time"))) {
                continue;   // it is a header row
            }
        }
        const QStringList cols = line.split(QLatin1Char(','));
        if (cols.size() < 6) {
            ++r.skipped_rows;
            continue;
        }

        std::int64_t ts = 0;
        std::int64_t unused = 0;
        if (!parse_bar_time(cols[0], stamp, ts, unused)) {
            ++r.skipped_rows;
            continue;
        }

        Candle c{};
        c.start_ns = ts;
        c.end_ns = ts + bar_span_ns;
        bool bad = false;
        std::int64_t* fields[4] = {&c.open, &c.high, &c.low, &c.close};
        for (int k = 0; k < 4; ++k) {
            bool rounded = false;
            if (!parse_price_paise(cols[k + 1], *fields[k], rounded)) {
                bad = true;
                break;
            }
            if (rounded) {
                ++r.rounded_fields;
            }
        }
        if (bad) {
            ++r.skipped_rows;
            continue;
        }

        // AN EMPTY VOLUME FIELD IS ABSENT, NOT A PARSE ERROR.
        //
        // The India VIX file ends every row with a bare comma -- `14.51,` --
        // because an index has no turnover to report. The first version of
        // this loader treated that as unparseable and SKIPPED THE WHOLE ROW,
        // which silently discarded all 528 bars: the series loaded as zero
        // bars and the fit reported "need at least 200, have 0".
        //
        // Dropping a row because one optional field is missing is worse than
        // the zero-versus-absent confusion this loader already handles. The
        // price data was there and complete.
        const QString vol_text = cols[5].trimmed();
        std::int64_t vol = 0;
        bool vol_present = true;
        if (vol_text.isEmpty()) {
            vol_present = false;
            ++r.absent_volume_rows;
        } else {
            bool vol_ok = true;
            vol = vol_text.toLongLong(&vol_ok);
            if (!vol_ok) {
                // Genuinely malformed, as distinct from absent. This one IS a
                // skipped row, and the count says so.
                ++r.skipped_rows;
                continue;
            }
            if (vol == 0) {
                ++r.zero_volume_rows;
            }
        }
        c.volume = vol;
        // Absent for two different reasons, and both mean the same thing to a
        // renderer: there is no number to draw.
        c.volume_known =
            vol_present && !(vol == 0 && zero_volume_is_absent);

        c.ticks = 1;          // one bar, not one trade
        c.complete = true;    // history; nothing here is still forming
        r.bars.push_back(c);
    }

    std::sort(r.bars.begin(), r.bars.end(),
              [](const Candle& a, const Candle& b) {
                  return a.start_ns < b.start_ns;
              });
    return r;
}

/// Load every CSV in a directory and merge, in time order.
///
/// The 1m series is one file per DAY and the 60m series one per MONTH, so a
/// caller that wants a continuous series has to merge them. Sorting happens
/// once, at the end, rather than assuming the filenames sort into time order --
/// they happen to here, and relying on that is how a series acquires a
/// backwards step the moment a file is renamed.
[[nodiscard]] inline LoadResult
load_bars_dir(const QString& dir, std::int64_t bar_span_ns, DailyStamp stamp,
              bool zero_volume_is_absent) {
    LoadResult merged;
    QDir d(dir);
    if (!d.exists()) {
        merged.error = QStringLiteral("no such directory: %1").arg(dir);
        return merged;
    }
    const QStringList files =
        d.entryList(QStringList{QStringLiteral("*.csv")}, QDir::Files);
    if (files.isEmpty()) {
        merged.error = QStringLiteral("no .csv in %1").arg(dir);
        return merged;
    }
    for (const QString& name : files) {
        LoadResult one = load_bars_csv(d.filePath(name), bar_span_ns, stamp,
                                       zero_volume_is_absent);
        if (!one.ok()) {
            merged.error = one.error;
            return merged;
        }
        merged.bars.insert(merged.bars.end(), one.bars.begin(), one.bars.end());
        merged.rounded_fields += one.rounded_fields;
        merged.zero_volume_rows += one.zero_volume_rows;
        merged.absent_volume_rows += one.absent_volume_rows;
        merged.skipped_rows += one.skipped_rows;
    }
    std::sort(merged.bars.begin(), merged.bars.end(),
              [](const Candle& a, const Candle& b) {
                  return a.start_ns < b.start_ns;
              });
    return merged;
}

// ---------------------------------------------------------------------------
// Aggregation
// ---------------------------------------------------------------------------

/// Combine bars into a coarser bucket, correctly.
///
/// open = FIRST open, high = MAX high, low = MIN low, close = LAST close,
/// volume = SUM. Anything else loses the extremes, and the extremes are what a
/// range-based stop is placed against.
[[nodiscard]] inline std::vector<Candle>
aggregate_bars(const std::vector<Candle>& bars, std::int64_t bucket_ns) {
    std::vector<Candle> out;
    if (bars.empty() || bucket_ns <= 0) {
        return out;
    }
    bool open_bucket = false;
    Candle c{};
    for (const Candle& b : bars) {
        std::int64_t q = b.start_ns / bucket_ns;
        if (b.start_ns % bucket_ns != 0 && b.start_ns < 0) {
            --q;
        }
        const std::int64_t start = q * bucket_ns;
        if (!open_bucket || start != c.start_ns) {
            if (open_bucket) {
                out.push_back(c);
            }
            c = Candle{};
            c.start_ns = start;
            c.end_ns = start + bucket_ns;
            c.open = b.open;
            c.high = b.high;
            c.low = b.low;
            c.volume_known = b.volume_known;
            c.complete = true;
            open_bucket = true;
        }
        c.high = std::max(c.high, b.high);
        c.low = std::min(c.low, b.low);
        c.close = b.close;
        c.volume += b.volume;
        // Any unknown component makes the total unknown. A sum that silently
        // treats an absent volume as zero understates the bucket and says
        // nothing about it.
        c.volume_known = c.volume_known && b.volume_known;
        c.ticks += b.ticks;
    }
    if (open_bucket) {
        out.push_back(c);
    }
    return out;
}

/// The wrong way, kept so the test can price it: each bar's CLOSE treated as a
/// tick, high and low discarded. Nothing in the load path calls this.
[[nodiscard]] inline std::vector<Candle>
bars_as_ticks_naive(const std::vector<Candle>& bars, std::int64_t bucket_ns) {
    std::vector<Candle> out;
    if (bars.empty() || bucket_ns <= 0) {
        return out;
    }
    bool open_bucket = false;
    Candle c{};
    for (const Candle& b : bars) {
        const std::int64_t start = (b.start_ns / bucket_ns) * bucket_ns;
        if (!open_bucket || start != c.start_ns) {
            if (open_bucket) {
                out.push_back(c);
            }
            c = Candle{};
            c.start_ns = start;
            c.end_ns = start + bucket_ns;
            c.open = b.close;
            c.high = b.close;
            c.low = b.close;
            open_bucket = true;
        }
        c.high = std::max(c.high, b.close);   // only ever sees closes
        c.low = std::min(c.low, b.close);
        c.close = b.close;
        c.volume += b.volume;
        ++c.ticks;
    }
    if (open_bucket) {
        out.push_back(c);
    }
    return out;
}

/// Mean bar range in paise. The number a range-based stop or an ATR keys on.
[[nodiscard]] inline double mean_range_paise(const std::vector<Candle>& bars) {
    if (bars.empty()) {
        return 0.0;
    }
    std::int64_t total = 0;
    for (const Candle& b : bars) {
        total += b.high - b.low;
    }
    return static_cast<double>(total) / static_cast<double>(bars.size());
}

} // namespace altair::ui
