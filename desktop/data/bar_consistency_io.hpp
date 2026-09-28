// Strict, read-only CSV adapter for the standalone cold-path reconciliation tool.
#pragma once
#include "bar_consistency.hpp"
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

namespace altair::ui::barcheck {

/// Parse nonnegative integral source volume, refusing signs, exponents and overflow.
inline bool parse_volume(const QByteArray& text, qint64& units) {
    if (text.isEmpty()) return false;
    qint64 value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') return false;
        const int digit = ch - '0';
        if (value > (std::numeric_limits<qint64>::max() - digit) / 10) return false;
        value = value * 10 + digit;
    }
    units = value;
    return true;
}

/// Parse positive decimal rupees to integer paise without floating-point conversion.
/// Nonzero subpaise digits require explicit half-up rounding, counted via rounded.
inline bool parse_price(const QByteArray& text, bool allow_rounding,
                        qint64& paise, bool& rounded) {
    rounded = false;
    if (text.isEmpty()) return false;
    const auto dot = text.indexOf('.');
    const QByteArray whole_text = dot < 0 ? text : text.left(dot);
    qint64 whole = 0;
    if (!parse_volume(whole_text, whole) || whole > std::numeric_limits<qint64>::max() / 100)
        return false;
    const QByteArray fraction = dot < 0 ? QByteArray{} : text.mid(dot + 1);
    if (dot >= 0 && fraction.isEmpty()) return false;
    qint64 tail = 0;
    for (qsizetype i = 0; i < fraction.size(); ++i) {
        const char ch = fraction[i];
        if (ch < '0' || ch > '9') return false;
        if (i < 2) tail += (ch - '0') * (i == 0 ? 10 : 1);
        else if (ch != '0') rounded = true;
    }
    if (rounded && !allow_rounding) return false;
    if (fraction.size() > 2 && fraction[2] >= '5') ++tail;
    const qint64 scaled = whole * 100;
    if (tail > std::numeric_limits<qint64>::max() - scaled) return false;
    const qint64 value = scaled + tail;
    if (value <= 0) return false;
    paise = value;
    return true;
}

namespace detail {
inline bool parse_row(const QList<QByteArray>& fields, Bar& bar, bool round_subpaise,
                      bool zero_volume_absent, Report& report,
                      const QString& file, qint64 row) {
    static const QRegularExpression timestamp_pattern(
        QStringLiteral("^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}\\+05:30$"));
    const QString timestamp = QString::fromLatin1(fields[0]);
    if (!timestamp_pattern.match(timestamp).hasMatch()) {
        report.issue("invalid_timestamp", "Require second-precision ISO bar-open timestamp with +05:30", file, row);
        return false;
    }
    const QDateTime parsed = QDateTime::fromString(timestamp, Qt::ISODate);
    if (!parsed.isValid() || parsed.toString(Qt::ISODate) != timestamp) {
        report.issue("invalid_timestamp", timestamp + " is not a valid ISO date/time", file, row);
        return false;
    }
    bar.start = parsed.toSecsSinceEpoch();
    for (qsizetype i = 0; i < 4; ++i) {
        bool rounded = false;
        if (!parse_price(fields[i + 1], round_subpaise,
                         bar.price[static_cast<std::size_t>(i)], rounded)) {
            report.issue("invalid_price", "OHLC field " + QString::number(i + 1) +
                " must be a positive decimal within int64 paise; subpaise requires --round-subpaise", file, row);
            return false;
        }
        if (rounded) ++report.rounded_fields;
    }
    if (!fields[5].isEmpty()) {
        qint64 volume = 0;
        if (!parse_volume(fields[5], volume)) {
            report.issue("invalid_volume", "Volume must be empty or a nonnegative int64 integer", file, row);
            return false;
        }
        if (volume != 0 || !zero_volume_absent) bar.volume = volume;
    }
    return true;
}

inline bool read_file(const QString& path, int minutes, bool round_subpaise,
                      bool zero_volume_absent, Report& report, Series& output,
                      qint64& input_rows) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        report.issue("io_error", file.errorString(), path);
        return true;
    }
    qint64 line_number = 0;
    qsizetype field_count = 0;
    while (!file.atEnd()) {
        // QIODevice reserves one byte for a terminator: request cap + 2 so
        // any overlong physical line returns at least cap + 1 bytes.
        QByteArray line = file.readLine(4098);
        ++line_number;
        if (line.size() > 4096) {
            report.issue("line_limit", "Physical line exceeds 4096 bytes; rest of file refused", path, line_number);
            return true;
        }
        if (line.isEmpty() && file.error() != QFileDevice::NoError) {
            report.issue("io_error", file.errorString(), path, line_number);
            return true;
        }
        if (line.endsWith('\n')) line.chop(1);
        if (line.endsWith('\r')) line.chop(1);
        if (line_number == 1) {
            if (line.startsWith("\xEF\xBB\xBF")) line.remove(0, 3);
            if (line == "time,open,high,low,close,volume") field_count = 6;
            else if (line == "time,open,high,low,close,volume,oi") field_count = 7;
            else {
                report.issue("invalid_schema", "Expected time,open,high,low,close,volume[,oi] header", path, 1);
                return true;
            }
            continue;
        }
        if (input_rows >= 2000000) {
            report.issue("row_limit", "More than 2000000 data rows; remaining input refused", path, line_number);
            return false;
        }
        ++input_rows;
        const auto fields = line.split(',');
        if (fields.size() != field_count || line.contains('"')) {
            report.issue("invalid_row", "Column count must match header; quoted CSV is unsupported", path, line_number);
            continue;
        }
        Bar bar;
        if (!parse_row(fields, bar, round_subpaise, zero_volume_absent, report, path, line_number)) continue;
        if (!valid_bar(bar, minutes, report, path + ":" + QString::number(line_number))) continue;
        if (!output.emplace(bar.start, bar).second)
            report.issue("duplicate_timestamp", stamp(bar.start) + " already exists; not silently deduplicated", path, line_number);
        else
            ++report.loaded_rows;
    }
    if (file.error() != QFileDevice::NoError)
        report.issue("io_error", file.errorString(), path, line_number);
    if (line_number == 0) report.issue("invalid_schema", "Empty file has no CSV header", path);
    return true;
}
} // namespace detail

/// Read one CSV or one nonrecursive CSV directory; never writes source files.
/// Units are rupees-on-disk/paise-in-memory, UTC seconds, integral source volume.
inline Series load(const QString& path, int minutes, bool round_subpaise,
                   bool zero_volume_absent, Report& report) {
    Series output;
    if (minutes <= 0 || minutes > 375) {
        report.issue("invalid_duration", "Input duration must be 1-375 minutes", path);
        return output;
    }
    QFileInfo input(path);
    QStringList files;
    if (input.isFile()) {
        files.append(input.absoluteFilePath());
    } else if (input.isDir()) {
        const QDir directory(input.absoluteFilePath());
        const auto entries = directory.entryList({"*.csv"}, QDir::Files, QDir::Name);
        if (entries.size() > 10000) {
            report.issue("file_limit", "More than 10000 CSV files; entire input refused", path);
            return output;
        }
        for (const QString& entry : entries) files.append(directory.absoluteFilePath(entry));
    } else {
        report.issue("io_error", "Input does not exist or is not a regular file/directory", path);
        return output;
    }
    if (files.isEmpty()) report.issue("empty_input", "Directory contains no CSV files", path);
    qint64 input_rows = 0;
    for (const QString& file : files)
        if (!detail::read_file(file, minutes, round_subpaise, zero_volume_absent,
                               report, output, input_rows)) break;
    return output;
}

} // namespace altair::ui::barcheck
