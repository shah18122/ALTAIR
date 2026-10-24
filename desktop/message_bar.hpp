// desktop/message_bar.hpp -- the Terminal's message bar: every order log in
// one place, newest at the bottom.
//
// What reaches it:
//   * everything the message log (F10) gets: paper orders placed, executed,
//     rejected and cancelled; LIVE requests and what FYERS made of them; the
//     order router's own lines (its limit refusals among them); HALT/RESUME;
//   * DEMO: every fill the live engine's models take on paper, read as it is
//     appended to data/live/paper/fills.csv;
//   * RMS: every entry the engine's risk gate refused (position, per-model,
//     gross notional, margin and daily-loss limits; a halt; a stale feed),
//     read from data/live/paper/decisions.csv, and the engine's own square-off
//     and roll notes.
//
// The engine's files are tailed, never re-read whole: the first look takes the
// last 256 KB and keeps today's lines only, then each look takes what was
// appended. A file that shrinks (a new day, a restore) is followed from its
// new end.

#pragma once

#include <QDate>
#include <QDateTime>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QPlainTextEdit>
#include <QString>
#include <QStringList>

#include <algorithm>

namespace altair::ui {

/// Lines appended to a file since the last look.
class FileTail {
public:
    explicit FileTail(QString path = {}) : path_(std::move(path)) {}
    void set_path(const QString& p) { path_ = p; offset_ = -1; }
    [[nodiscard]] const QString& path() const noexcept { return path_; }

    /// New complete lines. The first call starts `first_tail` bytes from the
    /// end (at a line start); a later one where the last stopped.
    QStringList take(qint64 first_tail = 256 * 1024) {
        QStringList out;
        QFile f(path_);
        if (!f.open(QIODevice::ReadOnly)) return out;
        const qint64 size = f.size();
        bool skip_partial = false;
        if (offset_ < 0 || size < offset_) {
            offset_ = size > first_tail ? size - first_tail : 0;
            skip_partial = offset_ > 0;
        }
        if (size == offset_ || !f.seek(offset_)) return out;
        const QByteArray bytes = f.read(size - offset_);
        const qsizetype last_nl = bytes.lastIndexOf('\n');
        if (last_nl < 0) return out;                         // no complete line yet
        offset_ += last_nl + 1;
        QString text = QString::fromUtf8(bytes.left(last_nl));
        out = text.split(QLatin1Char('\n'));
        if (skip_partial && !out.isEmpty()) out.removeFirst();
        for (QString& l : out) if (l.endsWith(QLatin1Char('\r'))) l.chop(1);
        return out;
    }

private:
    QString path_;
    qint64 offset_ = -1;
};

namespace message_bar {

/// One CSV line into fields (quotes doubled inside quoted fields).
[[nodiscard]] inline QStringList fields(const QString& line) {
    QStringList out;
    QString cur;
    bool quoted = false;
    for (qsizetype i = 0; i < line.size(); ++i) {
        const QChar c = line.at(i);
        if (quoted) {
            if (c == QLatin1Char('"')) {
                if (i + 1 < line.size() && line.at(i + 1) == QLatin1Char('"')) { cur += c; ++i; }
                else quoted = false;
            } else {
                cur += c;
            }
        } else if (c == QLatin1Char('"')) {
            quoted = true;
        } else if (c == QLatin1Char(',')) {
            out << cur;
            cur.clear();
        } else {
            cur += c;
        }
    }
    out << cur;
    return out;
}

/// A fills.csv row (time,model,symbol,token,side,qty,price,expenses,at_quote,
/// reason,source,costs) as a DEMO line; empty for the header or a short row.
[[nodiscard]] inline QString demo_fill(const QString& line) {
    const QStringList f = fields(line);
    if (f.size() < 10 || f[0] == QLatin1String("time")) return {};
    const QString time = f[0].section(QLatin1Char(' '), 1);
    const bool sim = f.size() > 10 && f[10] == QLatin1String("SIM");
    QString s = QStringLiteral("%1  DEMO%2 %3: %4 %5 %6 @ %7")
                    .arg(time.isEmpty() ? f[0] : time, sim ? QStringLiteral(" (SIM)") : QString(), f[1],
                         f[4].toUpper(), f[5], f[2], f[6]);
    if (!f[7].isEmpty()) s += QStringLiteral(" · expenses %1").arg(f[7]);
    if (f[9].startsWith(QLatin1String("netted"))) s += QStringLiteral(" · netted (no exit fill)");
    else if (!f[9].isEmpty()) s += QStringLiteral(" · ") + f[9].left(120);
    return s;
}

/// A decisions.csv row (time,ns,model,decision) as an RMS or ENGINE line when
/// it is a refusal by the risk gate, a halt, or the engine's own note; empty
/// for everything else (a model's routine "gate shut" is not a message).
[[nodiscard]] inline QString rms_decision(const QString& line) {
    const QStringList f = fields(line);
    if (f.size() < 4 || f[0] == QLatin1String("time")) return {};
    const QString time = f[0].section(QLatin1Char(' '), 1);
    const QString& model = f[2];
    const QString& text = f[3];
    const bool risk = text.contains(QLatin1String("risk: ")) || text.contains(QLatin1String("halted"))
                      || text.contains(QLatin1String("kill request")) || text.contains(QLatin1String(" limit"));
    if (model == QLatin1String("engine")) return QStringLiteral("%1  ENGINE: %2").arg(time, text.left(160));
    if (!risk) return {};
    return QStringLiteral("%1  RMS %2: %3").arg(time, model, text.left(160));
}

/// Today's date as the engine's files write it (IST).
[[nodiscard]] inline QString today_ist() {
    return QDateTime::currentDateTimeUtc().addSecs(19800).date().toString(Qt::ISODate);
}

} // namespace message_bar

/// The bar itself: a few lines high, read-only, monospaced, newest last.
class MessageBar final : public QPlainTextEdit {
public:
    explicit MessageBar(QWidget* parent = nullptr) : QPlainTextEdit(parent) {
        setObjectName(QStringLiteral("messageBar"));
        setReadOnly(true);
        setMaximumBlockCount(3000);
        setLineWrapMode(QPlainTextEdit::NoWrap);
        QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        mono.setPointSizeF(std::max(7.5, mono.pointSizeF() - 1.0));
        setFont(mono);
        setFixedHeight(fontMetrics().lineSpacing() * 4 + 10);
        setToolTip(QStringLiteral("Every order log: paper, LIVE (FYERS), the order router, the models' DEMO fills, "
                                  "and RMS refusals. F10 opens the full log."));
        setPlaceholderText(QStringLiteral("Messages: orders, DEMO fills and RMS limits appear here."));
    }
    void add(const QString& line) {
        if (line.isEmpty()) return;
        appendPlainText(line);
        lines_ += 1;
    }
    [[nodiscard]] int lines() const noexcept { return lines_; }

    /// Read what the engine appended: DEMO fills and RMS refusals. On the
    /// first look only today's lines are kept.
    void poll(const QString& paper_dir) {
        if (fills_.path().isEmpty()) {
            fills_.set_path(paper_dir + QStringLiteral("/fills.csv"));
            decisions_.set_path(paper_dir + QStringLiteral("/decisions.csv"));
        }
        const QString today = message_bar::today_ist();
        const bool first = !polled_;
        polled_ = true;
        for (const QString& l : fills_.take())
            if (!first || l.startsWith(today)) add(message_bar::demo_fill(l));
        for (const QString& l : decisions_.take())
            if (!first || l.startsWith(today)) add(message_bar::rms_decision(l));
    }

private:
    FileTail fills_, decisions_;
    bool polled_ = false;
    int lines_ = 0;
};

} // namespace altair::ui
