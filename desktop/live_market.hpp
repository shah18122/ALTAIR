// desktop/live_market.hpp -- the live market watch: every tick, as it prints.
//
// Smit's complaint, and the reason this exists: the Terminal "is not even
// like Greeksoft GETS". It opened on an account table, its prices were
// yesterday's closes or a replay, and nothing on it moved. A trader's terminal
// opens on the market watch, and the market watch ticks.
//
// WHAT IS ON IT
//   * The watch: one row per streamed instrument (data/live/universe.csv,
//     written by altair_price_service): LTP, change, % change, best bid and
//     ask with sizes, last quantity, volume, ATP, OI, open, high, low,
//     previous close, last-trade time, and how many trades it has printed.
//     A cell that moves flashes green or red. Grouped (indices, futures, the
//     two chains, the NIFTY 50) and searchable.
//   * Depth: the selected instrument's five levels a side, with order counts.
//   * Time and sales: every trade the selected instrument printed, newest
//     first -- tick by tick, not sampled.
//   * The option chain, live: CE | STRIKE | PE with OI, volume, LTP, bid,
//     ask, and IMPLIED VOLATILITY AND DELTA FROM THE MARKET PRICE -- inverted
//     with Black-76 off the live future, not modelled at VIX.
//
// HOW IT STAYS FAST AND STILL SHOWS EVERY TICK. Every frame updates its row's
// state the moment it arrives (PriceClient), and the time-and-sales tape keeps
// every trade. Painting is coalesced: changed rows repaint every 100 ms, which
// is faster than a person reads and slower than the feed can flood. A trade
// is never lost to coalescing; only redundant repaints are.
//
// LIVE, REPLAY AND SIM ARE NEVER CONFUSED. The status line says which one the
// stream is, from the frames' own flags. A simulated session says SIM in
// amber on every screen it reaches.
//
// IT CANNOT TRADE. The feed is a separate process (altair_price_service) that
// this page can start and stop; the page links no broker and no OMS.

#pragma once

#include "price_client.hpp"
#include "scrip_master.hpp"
#include "theme.hpp"

#include <analytics/greeks.hpp>
#include <analytics/iv.hpp>

#include <QAbstractTableModel>
#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QDateEdit>
#include <QDialog>
#include <QFormLayout>
#include <QDir>
#include <QEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QSaveFile>
#include <QScrollBar>
#include <QFileDialog>
#include <QInputDialog>
#include <QSet>
#include <QStackedWidget>
#include <QStringListModel>
#include <QTimeEdit>
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QProcess>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QTabWidget>
#include <QTableView>
#include <QTableWidget>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include <thread>
#include <vector>

namespace altair::ui {

// ---------------------------------------------------------------------------
// The universe the feed streams
// ---------------------------------------------------------------------------

struct LiveRow {
    quint32 token = 0;
    QString symbol, fyers, underlying, kind, group;
    qint64 expiry_day = 0;   ///< days since 1970-01-01; 0 for none
    double strike = 0.0;
    qint64 lot = 1;
    double tick = 0.05;
};

namespace live_detail {

[[nodiscard]] inline qint64 parse_day(const QString& s) {
    const QDate d = QDate::fromString(s, Qt::ISODate);
    return d.isValid() ? QDate(1970, 1, 1).daysTo(d) : 0;
}

[[nodiscard]] inline QString px(qint64 paise) {
    if (paise == 0) return QStringLiteral("—");
    return QString::number(static_cast<double>(paise) / 100.0, 'f', 2);
}

[[nodiscard]] inline QString signed_px(qint64 paise) {
    return (paise > 0 ? QStringLiteral("+") : QString()) + QString::number(static_cast<double>(paise) / 100.0, 'f', 2);
}

[[nodiscard]] inline QString qty(qint64 q) { return q == 0 ? QStringLiteral("—") : QLocale(QLocale::English, QLocale::India).toString(q); }

/// "hh:mm:ss" (or with milliseconds) in IST, from ns since the epoch, without
/// a time-zone database: IST is a fixed +05:30.
[[nodiscard]] inline QString ist(std::int64_t ns, bool ms) {
    if (ns <= 0) return QStringLiteral("—");
    const std::int64_t ist_ms = ns / 1'000'000 + 19'800'000;
    const std::int64_t day_ms = ((ist_ms % 86'400'000) + 86'400'000) % 86'400'000;
    const int h = static_cast<int>(day_ms / 3'600'000), m = static_cast<int>(day_ms / 60'000 % 60),
              sec = static_cast<int>(day_ms / 1000 % 60), milli = static_cast<int>(day_ms % 1000);
    return ms ? QStringLiteral("%1:%2:%3.%4").arg(h, 2, 10, QChar('0')).arg(m, 2, 10, QChar('0'))
                    .arg(sec, 2, 10, QChar('0')).arg(milli, 3, 10, QChar('0'))
              : QStringLiteral("%1:%2:%3").arg(h, 2, 10, QChar('0')).arg(m, 2, 10, QChar('0')).arg(sec, 2, 10, QChar('0'));
}

[[nodiscard]] inline QString find_helper(const QString& name) {
#if defined(_WIN32)
    const QString exe = name + QStringLiteral(".exe");
#else
    const QString exe = name;
#endif
    const QString app = QCoreApplication::applicationDirPath();
    QStringList tried{app + QStringLiteral("/") + exe, app + QStringLiteral("/../app/") + exe,
                      app + QStringLiteral("/../Helpers/") + exe, app + QStringLiteral("/../../net/app/") + exe};
#ifdef ALTAIR_SOURCE_DIR
    tried << QStringLiteral(ALTAIR_SOURCE_DIR "/build/net/app/") + exe;
#endif
    for (const QString& path : tried) {
        const QFileInfo file(path);
        if (file.exists() && file.isFile()) return file.canonicalFilePath();
    }
    return {};
}

} // namespace live_detail

/// The three indices, for a tree where the feed has never written its universe.
[[nodiscard]] inline std::vector<LiveRow> default_live_universe() {
    std::vector<LiveRow> rows;
    const auto add = [&rows](quint32 tok, const char* sym, const char* under) {
        LiveRow r;
        r.token = tok; r.symbol = QString::fromLatin1(sym); r.underlying = QString::fromLatin1(under);
        r.kind = QStringLiteral("index"); r.group = QStringLiteral("Indices");
        rows.push_back(r);
    };
    add(256265u, "NIFTY 50", "NIFTY");
    add(260105u, "NIFTY BANK", "BANKNIFTY");
    add(264969u, "INDIA VIX", "INDIAVIX");
    return rows;
}

/// data/live/universe.csv (live/universe.hpp writes it). Empty on any doubt.
[[nodiscard]] inline std::vector<LiveRow> load_live_universe(const QString& path) {
    std::vector<LiveRow> rows;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return rows;
    QTextStream in(&f);
    if (!in.readLine().startsWith(QStringLiteral("token,"))) return rows;
    while (!in.atEnd()) {
        const QStringList c = in.readLine().split(QLatin1Char(','));
        if (c.size() < 11) continue;
        LiveRow r;
        bool ok = false;
        r.token = c[0].toUInt(&ok);
        if (!ok || r.token == 0) continue;
        r.fyers = c[1]; r.symbol = c[2]; r.underlying = c[3]; r.kind = c[4];
        r.expiry_day = live_detail::parse_day(c[5]);
        r.strike = c[6].toDouble();
        r.lot = c[7].toLongLong();
        r.tick = c[8].toDouble();
        r.group = c[9];
        rows.push_back(r);
    }
    return rows;
}

// ---------------------------------------------------------------------------
// The watch
// ---------------------------------------------------------------------------

class LiveWatchModel final : public QAbstractTableModel {
public:
    enum Column { Symbol, Ltp, Chg, ChgPct, BidQty, Bid, Ask, AskQty, Ltq, Volume, Atp, Oi, Open, High, Low,
                  PrevClose, Ltt, Trades, ColumnCount };
    static constexpr int SortRole = Qt::UserRole + 1;
    static constexpr int GroupRole = Qt::UserRole + 2;
    static constexpr int TokenRole = Qt::UserRole + 3;
    /// How long a moved cell stays lit.
    static constexpr qint64 kFlashMs = 450;

    LiveWatchModel(const PriceClient* client, QObject* parent = nullptr)
        : QAbstractTableModel(parent), client_(client) {
        paint_.setInterval(100);
        connect(&paint_, &QTimer::timeout, this, [this] { flush(); });
        paint_.start();
    }

    void set_rows(std::vector<LiveRow> rows) {
        beginResetModel();
        rows_ = std::move(rows);
        flash_.assign(rows_.size(), Flash{});
        dirty_.assign(rows_.size(), 0);
        dirty_list_.clear();
        index_.clear();
        for (std::size_t i = 0; i < rows_.size(); ++i) index_[rows_[i].token] = static_cast<int>(i);
        endResetModel();
    }
    [[nodiscard]] const std::vector<LiveRow>& rows() const noexcept { return rows_; }
    [[nodiscard]] int row_of(quint32 token) const {
        const auto it = index_.find(token);
        return it == index_.end() ? -1 : it->second;
    }

    /// A frame arrived for `token`: note the move, repaint on the next flush.
    void touch(quint32 token) {
        const int r = row_of(token);
        if (r < 0 || client_ == nullptr) return;
        const LivePrice* p = client_->price(token);
        if (p == nullptr) return;
        Flash& f = flash_[static_cast<std::size_t>(r)];
        if (p->last_paise != f.last && p->last_paise > 0) {
            // A move this burst carried (the tick rule, per trade) lights the
            // cell even when the row's first sighting already includes it.
            if (p->tick_dir != 0 && (f.last > 0 || p->trades > 1)) {
                f.dir = p->tick_dir;
                f.until_ms = QDateTime::currentMSecsSinceEpoch() + kFlashMs;
            }
            f.last = p->last_paise;
        }
        mark(r);
    }
    /// Whether a source row is on screen now. Only those repaint; a row
    /// scrolled into view is painted from the live state when it appears, so
    /// nothing off screen ever needs a repaint of its own. Unset: every row.
    std::function<bool(int)> row_visible;
    /// Rows repainted by the last flush (tests).
    [[nodiscard]] int last_flush_rows() const noexcept { return last_flush_rows_; }

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : static_cast<int>(rows_.size());
    }
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : ColumnCount;
    }

    [[nodiscard]] QVariant headerData(int section, Qt::Orientation o, int role) const override {
        if (o != Qt::Horizontal || role != Qt::DisplayRole) return {};
        static const char* names[] = {"Symbol", "LTP", "Chg", "%Chg", "Bid Qty", "Bid", "Ask", "Ask Qty", "LTQ",
                                      "Volume", "ATP", "OI", "Open", "High", "Low", "Prev Close", "LTT", "Trades"};
        return section >= 0 && section < ColumnCount ? QString::fromLatin1(names[section]) : QVariant{};
    }

    [[nodiscard]] QVariant data(const QModelIndex& idx, int role) const override {
        if (!idx.isValid() || idx.row() < 0 || idx.row() >= rowCount()) return {};
        const LiveRow& row = rows_[static_cast<std::size_t>(idx.row())];
        if (role == GroupRole) return row.group;
        if (role == TokenRole) return static_cast<qulonglong>(row.token);
        const LivePrice* p = client_ != nullptr ? client_->price(row.token) : nullptr;
        const QuotePayload* q = p != nullptr && p->has_quote ? &p->quote : nullptr;
        const qint64 ltp = p != nullptr ? p->last_paise : 0;
        const qint64 pc = q != nullptr && q->has(kQuoteHasPrevClose) ? q->prev_close : 0;
        const qint64 chg = ltp > 0 && pc > 0 ? ltp - pc : 0;
        const bool top = q != nullptr && q->has(kQuoteHasTop);
        const bool ohlc = q != nullptr && q->has(kQuoteHasOhlc);
        const int col = idx.column();

        if (role == Qt::TextAlignmentRole)
            return col == Symbol ? int(Qt::AlignLeft | Qt::AlignVCenter) : int(Qt::AlignRight | Qt::AlignVCenter);
        if (role == Qt::ForegroundRole) {
            if (col == Ltp || col == Chg || col == ChgPct) {
                if (chg > 0) return QColor(0x7F, 0xD1, 0x7F);
                if (chg < 0) return QColor(0xF0, 0x7A, 0x6A);
            }
            if (col == Bid || col == BidQty) return QColor(0x7F, 0xB8, 0xF0);
            if (col == Ask || col == AskQty) return QColor(0xF0, 0xA0, 0x7F);
            return {};
        }
        if (role == Qt::BackgroundRole) {
            const Flash& f = flash_[static_cast<std::size_t>(idx.row())];
            if ((col == Ltp || col == Chg || col == ChgPct || col == Ltq) && f.dir != 0
                && QDateTime::currentMSecsSinceEpoch() < f.until_ms)
                return f.dir > 0 ? QColor(0x1F, 0x4D, 0x2B) : QColor(0x5A, 0x1F, 0x1F);
            return {};
        }
        if (role == Qt::ToolTipRole && col == Symbol) {
            return row.fyers.isEmpty() ? row.symbol : QStringLiteral("%1  ·  token %2  ·  lot %3")
                                                          .arg(row.fyers).arg(row.token).arg(row.lot);
        }
        if (role == SortRole) {
            switch (col) {
            case Symbol: return row.symbol;
            case Ltp: return QVariant(static_cast<qlonglong>(ltp));
            case Chg: return QVariant(static_cast<qlonglong>(chg));
            case ChgPct: return pc > 0 ? static_cast<double>(chg) / static_cast<double>(pc) : 0.0;
            case BidQty: return QVariant(static_cast<qlonglong>(top ? q->bid_qty : 0));
            case Bid: return QVariant(static_cast<qlonglong>(top ? q->bid : 0));
            case Ask: return QVariant(static_cast<qlonglong>(top ? q->ask : 0));
            case AskQty: return QVariant(static_cast<qlonglong>(top ? q->ask_qty : 0));
            case Ltq: return QVariant(static_cast<qlonglong>(p != nullptr ? p->last_qty : 0));
            case Volume: return QVariant(static_cast<qlonglong>(p != nullptr && p->has_volume ? p->volume : 0));
            case Atp: return QVariant(static_cast<qlonglong>(q != nullptr && q->has(kQuoteHasAtp) ? q->avg_price : 0));
            case Oi: return QVariant(static_cast<qlonglong>(p != nullptr && p->has_oi ? p->oi : 0));
            case Open: return QVariant(static_cast<qlonglong>(ohlc ? q->open : 0));
            case High: return QVariant(static_cast<qlonglong>(ohlc ? q->high : 0));
            case Low: return QVariant(static_cast<qlonglong>(ohlc ? q->low : 0));
            case PrevClose: return QVariant(static_cast<qlonglong>(pc));
            case Ltt: return QVariant(static_cast<qlonglong>(q != nullptr && q->has(kQuoteHasLtt) ? q->last_trade_ns : (p != nullptr ? p->exchange_ts_ns : 0)));
            case Trades: return p != nullptr ? static_cast<qulonglong>(p->trades) : 0;
            default: return {};
            }
        }
        if (role != Qt::DisplayRole) return {};
        // ABSENCE IS NOT ZERO: a field the feed has not sent is a dash, never 0.
        const QString none = QStringLiteral("—");
        switch (col) {
        case Symbol: return row.fyers.startsWith(QLatin1String("BSE:")) ? row.symbol + QStringLiteral(" · BSE") : row.symbol;
        case Ltp: {
            if (ltp <= 0) return none;
            const int dir = p != nullptr ? p->tick_dir : 0;
            const QString arrow = dir > 0 ? QStringLiteral(" ▲") : dir < 0 ? QStringLiteral(" ▼") : QString();
            return live_detail::px(ltp) + arrow;
        }
        case Chg: return ltp > 0 && pc > 0 ? live_detail::signed_px(chg) : none;
        case ChgPct: return ltp > 0 && pc > 0 ? QStringLiteral("%1%2%").arg(chg > 0 ? QStringLiteral("+") : QString()).arg(100.0 * static_cast<double>(chg) / static_cast<double>(pc), 0, 'f', 2) : none;
        case BidQty: return top ? live_detail::qty(q->bid_qty) : none;
        case Bid: return top ? live_detail::px(q->bid) : none;
        case Ask: return top ? live_detail::px(q->ask) : none;
        case AskQty: return top ? live_detail::qty(q->ask_qty) : none;
        case Ltq: return p != nullptr && p->last_qty > 0 ? live_detail::qty(p->last_qty) : none;
        case Volume: return p != nullptr && p->has_volume ? live_detail::qty(p->volume) : none;
        case Atp: return q != nullptr && q->has(kQuoteHasAtp) ? live_detail::px(q->avg_price) : none;
        case Oi: return p != nullptr && p->has_oi ? live_detail::qty(p->oi) : none;
        case Open: return ohlc ? live_detail::px(q->open) : none;
        case High: return ohlc ? live_detail::px(q->high) : none;
        case Low: return ohlc ? live_detail::px(q->low) : none;
        case PrevClose: return pc > 0 ? live_detail::px(pc) : none;
        case Ltt: {
            const std::int64_t t = q != nullptr && q->has(kQuoteHasLtt) ? q->last_trade_ns : (p != nullptr ? p->exchange_ts_ns : 0);
            return live_detail::ist(t, false);
        }
        case Trades: return p != nullptr && p->trades > 0 ? QString::number(p->trades) : none;
        default: return {};
        }
    }

private:
    struct Flash {
        qint64 last = 0;
        int dir = 0;
        qint64 until_ms = 0;
    };

    void mark(int r) {
        if (r < 0 || static_cast<std::size_t>(r) >= dirty_.size() || dirty_[static_cast<std::size_t>(r)] != 0) return;
        dirty_[static_cast<std::size_t>(r)] = 1;
        dirty_list_.push_back(r);
    }
    void flush() {
        // Lit cells that have just gone dark must repaint too.
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        for (std::size_t i = 0; i < flash_.size(); ++i) {
            if (flash_[i].until_ms != 0 && now >= flash_[i].until_ms) {
                flash_[i].until_ms = 0;
                mark(static_cast<int>(i));
            }
        }
        last_flush_rows_ = 0;
        if (dirty_list_.empty()) return;
        // One repaint per changed row that is ON SCREEN, contiguous rows merged.
        std::sort(dirty_list_.begin(), dirty_list_.end());
        int lo = -1, hi = -1;
        const auto emit_run = [this, &lo, &hi] {
            if (lo >= 0) Q_EMIT dataChanged(index(lo, 0), index(hi, ColumnCount - 1));
            lo = hi = -1;
        };
        for (const int r : dirty_list_) {
            dirty_[static_cast<std::size_t>(r)] = 0;
            if (row_visible && !row_visible(r)) continue;
            ++last_flush_rows_;
            if (lo >= 0 && r == hi + 1) { hi = r; continue; }
            emit_run();
            lo = hi = r;
        }
        emit_run();
        dirty_list_.clear();
    }

    const PriceClient* client_;
    std::vector<LiveRow> rows_;
    std::vector<Flash> flash_;
    std::unordered_map<quint32, int> index_;
    QTimer paint_;
    std::vector<char> dirty_;
    std::vector<int> dirty_list_;
    int last_flush_rows_ = 0;
};

/// The watch shows the active watchlist's scrips (and a text filter). The
/// models stream the whole universe whatever the watch shows.
class LiveWatchFilter final : public QSortFilterProxyModel {
public:
    explicit LiveWatchFilter(QObject* parent = nullptr) : QSortFilterProxyModel(parent) {
        // Re-sorting on every tick is what a sorted proxy does by default, and
        // it is a full sort of the watch per price: sorting is on a click only.
        setDynamicSortFilter(false);
    }
    void set_members(QSet<quint32> m) { members_ = std::move(m); invalidateFilter(); }
    void set_text(const QString& t) { text_ = t.trimmed(); invalidateFilter(); }
    [[nodiscard]] const QSet<quint32>& members() const noexcept { return members_; }

protected:
    [[nodiscard]] bool filterAcceptsRow(int r, const QModelIndex& parent) const override {
        const QModelIndex i = sourceModel()->index(r, 0, parent);
        if (!members_.contains(static_cast<quint32>(sourceModel()->data(i, LiveWatchModel::TokenRole).toULongLong())))
            return false;
        return text_.isEmpty() || sourceModel()->data(i, Qt::DisplayRole).toString().contains(text_, Qt::CaseInsensitive);
    }

private:
    QString text_;
    QSet<quint32> members_;
};

// ---------------------------------------------------------------------------
// Named watchlists: data/live/watchlists/<name>.csv ("token,symbol")
// ---------------------------------------------------------------------------

namespace watchlists {

[[nodiscard]] inline QString dir(const QString& root) { return root + QStringLiteral("/data/live/watchlists"); }
/// A list name: letters, digits, space, - and _, at most 40 characters.
[[nodiscard]] inline QString clean_name(const QString& name) {
    QString out;
    for (const QChar c : name.trimmed())
        if (c.isLetterOrNumber() || c == QLatin1Char(' ') || c == QLatin1Char('-') || c == QLatin1Char('_')) out += c;
    return out.left(40).trimmed();
}
[[nodiscard]] inline QString path(const QString& root, const QString& name) {
    return dir(root) + QLatin1Char('/') + clean_name(name) + QStringLiteral(".csv");
}
/// Every saved list, Default first.
[[nodiscard]] inline QStringList names(const QString& root) {
    QStringList out;
    for (const QFileInfo& f : QDir(dir(root)).entryInfoList({QStringLiteral("*.csv")}, QDir::Files, QDir::Name))
        out << f.completeBaseName();
    out.removeAll(QStringLiteral("Default"));
    out.prepend(QStringLiteral("Default"));
    return out;
}
/// The list's scrips, in order: (token, symbol).
[[nodiscard]] inline std::vector<std::pair<quint32, QString>> load(const QString& file) {
    std::vector<std::pair<quint32, QString>> out;
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return out;
    QTextStream in(&f);
    while (!in.atEnd()) {
        const QString l = in.readLine().trimmed();
        if (l.isEmpty() || !l.at(0).isDigit()) continue;
        const quint32 t = l.section(QLatin1Char(','), 0, 0).toUInt();
        if (t == 0 || std::any_of(out.begin(), out.end(), [t](const auto& e) { return e.first == t; })) continue;
        out.emplace_back(t, l.section(QLatin1Char(','), 1, 1));
    }
    return out;
}
[[nodiscard]] inline bool save(const QString& file, const std::vector<std::pair<quint32, QString>>& entries) {
    QDir().mkpath(QFileInfo(file).absolutePath());
    QSaveFile f(file);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    QTextStream out(&f);
    out << "token,symbol\n";
    for (const auto& [t, sym] : entries) out << t << ',' << QString(sym).remove(QLatin1Char(',')) << '\n';
    out.flush();
    return f.commit();
}
[[nodiscard]] inline QString active(const QString& root) {
    QFile f(dir(root) + QStringLiteral("/active.txt"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QStringLiteral("Default");
    const QString n = clean_name(QString::fromUtf8(f.readAll()));
    return n.isEmpty() ? QStringLiteral("Default") : n;
}
inline void set_active(const QString& root, const QString& name) {
    QDir().mkpath(dir(root));
    QSaveFile f(dir(root) + QStringLiteral("/active.txt"));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return;
    f.write(clean_name(name).toUtf8());
    (void)f.commit();
}

} // namespace watchlists

// ---------------------------------------------------------------------------
// Depth and time-and-sales for the selected instrument
// ---------------------------------------------------------------------------

[[nodiscard]] inline QTableWidget* make_live_table(int cols, const QStringList& heads, QWidget* parent) {
    auto* t = new QTableWidget(0, cols, parent);
    t->setHorizontalHeaderLabels(heads);
    t->verticalHeader()->setVisible(false);
    t->verticalHeader()->setDefaultSectionSize(20);
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->setSelectionMode(QAbstractItemView::NoSelection);
    t->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    t->setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
    return t;
}

inline void live_set(QTableWidget* t, int r, int c, const QString& text, const QColor& fg = {}) {
    auto* it = t->item(r, c);
    if (it == nullptr) {
        it = new QTableWidgetItem;
        it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        t->setItem(r, c, it);
    }
    if (it->text() != text) it->setText(text);
    if (fg.isValid()) it->setForeground(fg);
}

/// Every level the feed sends a side (five, or fifty from the FYERS 50-level
/// book) and the totals. Never fewer than five rows, so the ladder keeps its shape.
inline void show_live_depth(QTableWidget* t, const LivePrice* p) {
    const int rows = std::max(5, p != nullptr ? static_cast<int>(std::min<std::size_t>(p->levels, kMaxDepthLevels)) : 0);
    t->setRowCount(rows + 1);
    const QColor bid(0x7F, 0xB8, 0xF0), ask(0xF0, 0xA0, 0x7F);
    qint64 tb = 0, ta = 0;
    for (int k = 0; k < rows; ++k) {
        const bool have = p != nullptr && k < p->levels;
        const PriceLevel* b = have ? &p->bids[k] : nullptr;
        const PriceLevel* a = have ? &p->asks[k] : nullptr;
        live_set(t, k, 0, have && b->qty > 0 ? QString::number(b->orders) : QString(), bid);
        live_set(t, k, 1, have && b->qty > 0 ? live_detail::qty(b->qty) : QString(), bid);
        live_set(t, k, 2, have && b->qty > 0 ? live_detail::px(b->price_paise) : QString(), bid);
        live_set(t, k, 3, have && a->qty > 0 ? live_detail::px(a->price_paise) : QString(), ask);
        live_set(t, k, 4, have && a->qty > 0 ? live_detail::qty(a->qty) : QString(), ask);
        live_set(t, k, 5, have && a->qty > 0 ? QString::number(a->orders) : QString(), ask);
        if (have) { tb += b->qty; ta += a->qty; }
    }
    const int last = rows;
    const bool any = p != nullptr && p->levels > 0;
    live_set(t, last, 0, QStringLiteral("Total"));
    live_set(t, last, 1, any ? live_detail::qty(tb) : QStringLiteral("no book"), bid);
    live_set(t, last, 2, QString());
    live_set(t, last, 3, QString());
    live_set(t, last, 4, any ? live_detail::qty(ta) : QString(), ask);
    live_set(t, last, 5, QString());
}

/// Every trade, newest first.
inline void show_live_tape(QTableWidget* t, const LivePrice* p) {
    const int n = p != nullptr ? static_cast<int>(p->tape.size()) : 0;
    t->setRowCount(n);
    const QColor up(0x7F, 0xD1, 0x7F), down(0xF0, 0x7A, 0x6A), flat(0xD0, 0xD6, 0xDE);
    for (int i = 0; i < n; ++i) {
        const auto k = static_cast<std::size_t>(n - 1 - i);
        const LiveTapePrint& tp = p->tape[k];
        const qint64 before = k > 0 ? p->tape[k - 1].price_paise : tp.price_paise;
        const QColor c = tp.price_paise > before ? up : tp.price_paise < before ? down : flat;
        live_set(t, i, 0, live_detail::ist(tp.ts_ns, true), flat);
        live_set(t, i, 1, live_detail::px(tp.price_paise), c);
        live_set(t, i, 2, tp.qty > 0 ? live_detail::qty(tp.qty) : QStringLiteral("—"), c);
    }
}

// ---------------------------------------------------------------------------
// The live option chain
// ---------------------------------------------------------------------------

/// Implied vol and delta from a market price; NaN when it cannot be inverted
/// (no price, or a price outside the no-arbitrage bounds).
struct LiveIv { double iv = std::numeric_limits<double>::quiet_NaN(), delta = std::numeric_limits<double>::quiet_NaN(); };

[[nodiscard]] inline LiveIv live_iv(bool call, qint64 price_paise, double forward_paise, double strike_rupees,
                                    double years, double rate) {
    LiveIv out;
    if (price_paise <= 0 || !(forward_paise > 0.0) || !(years > 0.0)) return out;
    const auto right = call ? OptionRight::Call : OptionRight::Put;
    const auto iv = implied_vol_black76(right, Price{price_paise}, Price{static_cast<std::int64_t>(std::llround(forward_paise))},
                                        Price{static_cast<std::int64_t>(std::llround(strike_rupees * 100.0))}, Years{years}, rate);
    if (!iv) return out;
    out.iv = iv->vol.raw();
    const auto g = altair::detail::black76_unchecked(right, forward_paise, strike_rupees * 100.0, years, out.iv, rate);
    out.delta = g.delta;
    return out;
}

/// The mid of an instrument's best bid and ask, else its last trade.
[[nodiscard]] inline qint64 live_mid(const PriceClient* client, quint32 tok) {
    const LivePrice* p = client != nullptr && tok != 0 ? client->price(tok) : nullptr;
    if (p == nullptr) return 0;
    if (p->has_quote && p->quote.has(kQuoteHasTop) && p->quote.bid > 0 && p->quote.ask > p->quote.bid)
        return (p->quote.bid + p->quote.ask) / 2;
    return p->last_paise;
}

/// The cash index each index's options are written on (Kite tokens).
[[nodiscard]] inline quint32 index_spot_token(const QString& under) {
    if (under == QLatin1String("NIFTY")) return 256265u;
    if (under == QLatin1String("BANKNIFTY")) return 260105u;
    if (under == QLatin1String("FINNIFTY")) return 257801u;
    if (under == QLatin1String("MIDCPNIFTY")) return 288009u;
    if (under == QLatin1String("NIFTYNXT50")) return 270857u;
    if (under == QLatin1String("SENSEX")) return 265u;
    if (under == QLatin1String("BANKEX")) return 274441u;
    return 0;
}

/// One option of a chain.
struct ChainContract {
    quint32 token = 0;
    QString symbol, exchange;
    double strike = 0.0;
    qint64 expiry_day = 0;
    qint64 lot = 1;
    double tick = 0.05;
    bool call = true;
};

/// The forward an option is valued on: the near future's mid carried to the
/// options' expiry; else the spot grown at the rate; else put-call parity at
/// the strike where call and put are closest (an index with no future).
struct ChainForward { double paise = 0.0; QString from; };

[[nodiscard]] inline ChainForward chain_forward(const PriceClient* client, quint32 fut, qint64 fut_expiry, quint32 spot,
                                                qint64 expiry, double years, double rate,
                                                const std::map<double, std::pair<ChainContract, ChainContract>>* strikes = nullptr) {
    ChainForward f;
    f.paise = static_cast<double>(live_mid(client, fut));
    f.from = QStringLiteral("future mid");
    const qint64 spot_px = live_mid(client, spot);
    if (!(f.paise > 0.0) && spot_px > 0 && years > 0.0) {
        f.paise = static_cast<double>(spot_px) * std::exp(rate * years);
        f.from = QStringLiteral("spot × e^rT");
    }
    if (fut != 0 && fut_expiry != expiry && f.paise > 0.0 && f.from == QLatin1String("future mid")) {
        // The near future expires after the options: carry it back to the
        // options' expiry rather than use a later forward.
        const double tf = static_cast<double>((fut_expiry - expiry) * 86400) / (365.0 * 86400.0);
        f.paise *= std::exp(-rate * tf);
        f.from = QStringLiteral("future mid, carried to expiry");
    }
    if (!(f.paise > 0.0) && strikes != nullptr && years > 0.0) {
        double best = std::numeric_limits<double>::infinity();
        for (const auto& [k, pr] : *strikes) {
            const qint64 c = live_mid(client, pr.first.token), p = live_mid(client, pr.second.token);
            if (c <= 0 || p <= 0) continue;
            const double gap = std::fabs(static_cast<double>(c - p));
            if (gap < best) {
                best = gap;
                f.paise = k * 100.0 + static_cast<double>(c - p) * std::exp(rate * years);
            }
        }
        if (f.paise > 0.0) f.from = QStringLiteral("put-call parity");
    }
    return f;
}

class LiveChainView final : public QWidget {
public:
    enum Col { COi, CVol, CIv, CDelta, CLtp, CBid, CAsk, Strike, PBid, PAsk, PLtp, PDelta, PIv, PVol, POi, Cols };
    /// Strikes either side of the money asked of the feed when a chain opens.
    static constexpr int kRequestStrikes = 15;

    LiveChainView(const PriceClient* client, QWidget* parent = nullptr) : QWidget(parent), client_(client) {
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(0, 4, 0, 0);
        auto* bar = new QHBoxLayout;
        back_ = new QPushButton(QStringLiteral("← Watch"), this);
        back_->setObjectName(QStringLiteral("chainBack"));
        back_->setToolTip(QStringLiteral("Back to the market watch  ·  Backspace or Esc"));
        bar->addWidget(back_);
        under_ = new QComboBox(this);
        under_->setObjectName(QStringLiteral("chainUnderlying"));
        under_->setEditable(true);
        under_->setInsertPolicy(QComboBox::NoInsert);
        under_->setMinimumWidth(140);
        under_->addItems({QStringLiteral("NIFTY"), QStringLiteral("BANKNIFTY")});
        bar->addWidget(new QLabel(QStringLiteral("Underlying"), this));
        bar->addWidget(under_);
        expiry_ = new QComboBox(this);
        expiry_->setObjectName(QStringLiteral("chainExpiry"));
        bar->addWidget(new QLabel(QStringLiteral("Expiry"), this));
        bar->addWidget(expiry_);
        head_ = new QLabel(this);
        head_->setTextFormat(Qt::RichText);
        head_->setWordWrap(true);
        bar->addWidget(head_, 1);
        v->addLayout(bar);
        grid_ = make_live_table(Cols, {QStringLiteral("OI"), QStringLiteral("Vol"), QStringLiteral("IV"),
                                       QStringLiteral("Δ"), QStringLiteral("CE LTP"), QStringLiteral("Bid"),
                                       QStringLiteral("Ask"), QStringLiteral("STRIKE"), QStringLiteral("Bid"),
                                       QStringLiteral("Ask"), QStringLiteral("PE LTP"), QStringLiteral("Δ"),
                                       QStringLiteral("IV"), QStringLiteral("Vol"), QStringLiteral("OI")},
                                this);
        grid_->setObjectName(QStringLiteral("chainGrid"));
        grid_->setToolTip(QStringLiteral("Enter or double-click a CE or PE cell: Greek Watch for that option"));
        grid_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        grid_->horizontalHeader()->setDefaultSectionSize(70);
        // OI and volume in Indian grouping need the room.
        for (int c : {COi, CVol, PVol, POi}) grid_->setColumnWidth(c, 96);
        v->addWidget(grid_, 1);
        grid_->setSelectionMode(QAbstractItemView::SingleSelection);
        grid_->setSelectionBehavior(QAbstractItemView::SelectItems);
        connect(back_, &QPushButton::clicked, this, [this] { if (on_back) on_back(); });
        connect(under_, &QComboBox::activated, this, [this](int) { open(under_->currentText()); });
        connect(under_->lineEdit(), &QLineEdit::returnPressed, this, [this] { open(under_->currentText()); });
        connect(expiry_, &QComboBox::currentIndexChanged, this, [this](int) { rebuild(); });
        // A cell on the call side picks the call, on the put side the put:
        // that is the scrip + / − and the market picture act on.
        connect(grid_, &QTableWidget::currentCellChanged, this, [this](int r, int c, int, int) {
            const quint32 tok = token_at(r, c);
            if (tok != 0 && on_pick) on_pick(tok);
        });
        connect(grid_, &QTableWidget::cellDoubleClicked, this, [this](int r, int c) { open_greek(r, c); });
        // A row scrolled into view is filled at once, not on the next tick.
        connect(grid_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int) { refresh(); });
    }

    /// The instrument master (any underlying, any expiry). Without one the
    /// chain is built from what the feed streams.
    void set_master(const std::vector<MasterScrip>* master) {
        master_ = master;
        if (master_ == nullptr) return;
        std::set<QString> names;
        for (const auto& m : *master_)
            if ((m.type == QLatin1String("CE") || m.type == QLatin1String("PE")) && m.expiry >= today_) names.insert(m.name);
        QStringList list(names.begin(), names.end());
        const QString keep = under_->currentText();
        const QSignalBlocker b(under_);
        under_->clear();
        under_->addItems(list);
        auto* c = new QCompleter(list, under_);
        c->setCaseSensitivity(Qt::CaseInsensitive);
        c->setFilterMode(Qt::MatchContains);   // "nifty" offers BANKNIFTY, FINNIFTY too
        c->setMaxVisibleItems(16);
        under_->setCompleter(c);
        under_->setCurrentText(list.contains(keep) ? keep : QStringLiteral("NIFTY"));
    }
    void set_today(const QString& iso) { today_ = iso; }
    void set_rows(const std::vector<LiveRow>& rows) {
        rows_ = rows;
        if (master_ == nullptr || master_->empty()) rebuild(); else refresh();
    }
    /// Open the chain of an underlying (an index, a stock): nearest expiry.
    bool open(const QString& underlying) {
        const QString u = underlying.trimmed().toUpper();
        if (u.isEmpty()) return false;
        {
            const QSignalBlocker b(under_);
            if (under_->findText(u) < 0) under_->addItem(u);
            under_->setCurrentText(u);
        }
        fill_expiries();
        rebuild();
        center_on_money();
        return !strikes_.empty();
    }
    [[nodiscard]] QString underlying() const { return under_->currentText(); }
    [[nodiscard]] QStringList expiries() const {
        QStringList out;
        for (int i = 0; i < expiry_->count(); ++i) out << expiry_->itemText(i);
        return out;
    }
    [[nodiscard]] QTableWidget* grid() const noexcept { return grid_; }
    /// The option under a cell: calls left of the strike, puts right of it.
    [[nodiscard]] quint32 token_at(int r, int c) const {
        if (r < 0 || r >= static_cast<int>(row_tokens_.size()) || c == Strike) return 0;
        return c < Strike ? row_tokens_[static_cast<std::size_t>(r)].first : row_tokens_[static_cast<std::size_t>(r)].second;
    }
    [[nodiscard]] const ChainContract* contract(quint32 token) const {
        for (const auto& [k, pr] : strikes_) {
            if (pr.first.token == token) return &pr.first;
            if (pr.second.token == token) return &pr.second;
        }
        return nullptr;
    }
    [[nodiscard]] quint32 future_token() const noexcept { return fut_; }
    [[nodiscard]] qint64 future_expiry() const noexcept { return fut_expiry_; }
    [[nodiscard]] quint32 spot_token() const noexcept { return spot_; }
    [[nodiscard]] double rate() const noexcept { return rate_; }
    [[nodiscard]] QString header_text() const { return head_->text(); }
    /// The tokens last asked of the feed for this chain.
    [[nodiscard]] const std::vector<std::pair<quint32, QString>>& requested() const noexcept { return requested_; }

    std::function<void(quint32)> on_pick;
    /// Enter / double-click on an option: Greek Watch.
    std::function<void(quint32)> on_greek;
    /// Backspace / Esc / ← Watch.
    std::function<void()> on_back;
    /// The tokens the chain needs streamed (the feed adds them live).
    std::function<void(const std::vector<std::pair<quint32, QString>>&)> on_request;

    void open_greek(int r, int c) {
        const quint32 tok = token_at(r, c);
        if (tok != 0 && on_greek) on_greek(tok);
    }

    /// Re-read the strikes (an expiry or the universe changed).
    void rebuild() {
        strikes_.clear();
        fut_ = 0; fut_expiry_ = 0; spot_ = 0; expiry_day_ = 0;
        const QString under = under_->currentText().trimmed().toUpper();
        if (master_ != nullptr && !master_->empty()) {
            expiry_day_ = live_detail::parse_day(expiry_->currentText());
            for (const auto& m : *master_) {
                if (m.name != under) continue;
                if (m.type == QLatin1String("FUT")) {
                    const qint64 e = live_detail::parse_day(m.expiry);
                    if (m.expiry >= today_ && (fut_ == 0 || e < fut_expiry_)) { fut_ = m.token; fut_expiry_ = e; }
                    continue;
                }
                if ((m.type != QLatin1String("CE") && m.type != QLatin1String("PE")) || m.expiry != expiry_->currentText()) continue;
                ChainContract c;
                c.token = m.token; c.symbol = m.symbol; c.exchange = m.exchange; c.strike = m.strike;
                c.expiry_day = expiry_day_; c.lot = m.lot; c.tick = m.tick; c.call = m.type == QLatin1String("CE");
                (c.call ? strikes_[m.strike].first : strikes_[m.strike].second) = c;
            }
            spot_ = index_spot_token(under);
            if (spot_ == 0)
                for (const auto& m : *master_)
                    if (m.exchange == QLatin1String("NSE") && m.type == QLatin1String("EQ") && m.symbol == under) { spot_ = m.token; break; }
        } else {
            // The streamed universe: the nearest expiry it carries.
            for (const auto& r : rows_) {
                if (r.underlying != under) continue;
                if (r.kind == QLatin1String("future") && (fut_ == 0 || r.expiry_day < fut_expiry_)) { fut_ = r.token; fut_expiry_ = r.expiry_day; }
                if (r.kind != QLatin1String("call") && r.kind != QLatin1String("put")) continue;
                if (expiry_day_ == 0 || r.expiry_day < expiry_day_) expiry_day_ = r.expiry_day;
            }
            for (const auto& r : rows_) {
                if (r.underlying != under || r.expiry_day != expiry_day_) continue;
                if (r.kind != QLatin1String("call") && r.kind != QLatin1String("put")) continue;
                ChainContract c;
                c.token = r.token; c.symbol = r.symbol; c.strike = r.strike; c.expiry_day = r.expiry_day; c.lot = r.lot;
                c.tick = r.tick; c.call = r.kind == QLatin1String("call");
                c.exchange = r.fyers.startsWith(QLatin1String("BSE:")) ? QStringLiteral("BFO") : QStringLiteral("NFO");
                (c.call ? strikes_[r.strike].first : strikes_[r.strike].second) = c;
            }
            spot_ = index_spot_token(under);
            if (spot_ == 0)
                for (const auto& r : rows_)
                    if (r.kind == QLatin1String("equity") && r.symbol == under) { spot_ = r.token; break; }
        }
        row_tokens_.clear();
        for (const auto& [k, pr] : strikes_) row_tokens_.emplace_back(pr.first.token, pr.second.token);
        grid_->setRowCount(static_cast<int>(strikes_.size()));
        int r = 0;
        for (const auto& [k, pr] : strikes_) {
            live_set(grid_, r, Strike, QString::number(k, 'f', k == std::floor(k) ? 0 : 2), QColor(0xF4, 0xC9, 0x5D));
            if (auto* it = grid_->item(r, Strike)) it->setTextAlignment(Qt::AlignCenter);
            ++r;
        }
        request();
        refresh();
    }

    /// The live numbers, for the rows on screen.
    void refresh() {
        const QString under = under_->currentText();
        // THE CLOCK IS THE FEED'S: time to expiry from the underlying's own
        // last stamp, so a replayed or simulated session is valued at its time.
        const LivePrice* clock = client_ != nullptr ? client_->price(spot_ != 0 ? spot_ : 256265u) : nullptr;
        if (clock == nullptr && client_ != nullptr && fut_ != 0) clock = client_->price(fut_);
        const std::int64_t now_ns = clock != nullptr && clock->exchange_ts_ns > 0 ? clock->exchange_ts_ns
                                  : QDateTime::currentMSecsSinceEpoch() * 1'000'000LL;
        const double years = expiry_day_ > 0
            ? static_cast<double>((expiry_day_ * 86400 + 36000) * 1'000'000'000LL - now_ns) / (365.0 * 86400.0 * 1e9) : 0.0;
        const ChainForward fwd = chain_forward(client_, fut_, fut_expiry_, spot_, expiry_day_, years, rate_, &strikes_);
        const double forward = fwd.paise;
        double atm = 0.0;
        for (const auto& [k, pr] : strikes_) if (atm == 0.0 || std::fabs(k - forward / 100.0) < std::fabs(atm - forward / 100.0)) atm = k;
        atm_ = atm;
        const QString muted = QString::fromLatin1(theme_token::kTextMuted);
        head_->setText(strikes_.empty()
            ? QStringLiteral("<span style='color:%1'>No options for %2%3.</span>")
                  .arg(muted, under.toHtmlEscaped(),
                       master_ == nullptr || master_->empty() ? QStringLiteral(" are streaming: start the feed") : QString())
            : QStringLiteral("<span style='color:%1'>T</span> %2 d &nbsp; <span style='color:%1'>forward</span> <b>%3</b> (%4)"
                             " &nbsp; <span style='color:%1'>IV and Δ from the market mid, Black-76 at %5 %</span>")
                  .arg(muted).arg(years * 365.0, 0, 'f', 2)
                  .arg(forward > 0.0 ? QString::number(forward / 100.0, 'f', 2) : QStringLiteral("—"))
                  .arg(forward > 0.0 ? fwd.from : QStringLiteral("waiting for prices")).arg(rate_ * 100.0, 0, 'f', 2));
        if (forward > 0.0 && atm != requested_center_) request();   // the money moved: stream the strikes around it
        // ONLY WHAT IS ON SCREEN. A NIFTY chain is a hundred strikes; the rows
        // scrolled away are filled when they scroll back (the scroll bar asks).
        const int n = static_cast<int>(strikes_.size());
        int first = 0, last = n - 1;
        if (grid_->isVisible() && n > 0) {
            const int a = grid_->rowAt(0), b = grid_->rowAt(grid_->viewport()->height() - 1);
            first = a < 0 ? 0 : std::max(0, a - 1);
            last = b < 0 ? n - 1 : std::min(n - 1, b + 1);
        }
        const QColor itm(0x2A, 0x26, 0x18), plain(0x15, 0x18, 0x1D), atm_bg(0x3A, 0x32, 0x12);
        const QColor fg(0xD0, 0xD6, 0xDE), strike_fg(0xF4, 0xC9, 0x5D);
        int r = 0;
        for (const auto& [k, pr] : strikes_) {
            if (r < first || r > last) { ++r; continue; }
            const auto side = [&](quint32 tok, bool call, int c_oi, int c_vol, int c_iv, int c_d, int c_ltp, int c_bid, int c_ask) {
                const LivePrice* p = client_ != nullptr && tok != 0 ? client_->price(tok) : nullptr;
                const bool top = p != nullptr && p->has_quote && p->quote.has(kQuoteHasTop);
                const LiveIv iv = live_iv(call, live_mid(client_, tok), forward, k, years, rate_);
                live_set(grid_, r, c_oi, p != nullptr && p->has_oi ? live_detail::qty(p->oi) : QStringLiteral("—"), fg);
                live_set(grid_, r, c_vol, p != nullptr && p->has_volume ? live_detail::qty(p->volume) : QStringLiteral("—"), fg);
                live_set(grid_, r, c_iv, std::isfinite(iv.iv) ? QString::number(iv.iv * 100.0, 'f', 1) : QStringLiteral("—"), fg);
                live_set(grid_, r, c_d, std::isfinite(iv.delta) ? QString::number(iv.delta, 'f', 2) : QStringLiteral("—"), fg);
                live_set(grid_, r, c_ltp, p != nullptr && p->last_paise > 0 ? live_detail::px(p->last_paise) : QStringLiteral("—"), strike_fg);
                live_set(grid_, r, c_bid, top ? live_detail::px(p->quote.bid) : QStringLiteral("—"), QColor(0x7F, 0xB8, 0xF0));
                live_set(grid_, r, c_ask, top ? live_detail::px(p->quote.ask) : QStringLiteral("—"), QColor(0xF0, 0xA0, 0x7F));
                const bool in_money = forward > 0.0 && (call ? k < forward / 100.0 : k > forward / 100.0);
                for (int c : {c_oi, c_vol, c_iv, c_d, c_ltp, c_bid, c_ask})
                    if (auto* it = grid_->item(r, c)) it->setBackground(in_money ? itm : plain);
            };
            side(pr.first.token, true, COi, CVol, CIv, CDelta, CLtp, CBid, CAsk);
            if (auto* it = grid_->item(r, Strike)) it->setBackground(k == atm ? atm_bg : plain);
            side(pr.second.token, false, POi, PVol, PIv, PDelta, PLtp, PBid, PAsk);
            ++r;
        }
    }

    /// Scroll the at-the-money strike to the middle of the grid.
    void center_on_money() {
        refresh();
        int r = 0;
        for (const auto& [k, pr] : strikes_) {
            if (k == atm_) { grid_->scrollToItem(grid_->item(r, Strike), QAbstractItemView::PositionAtCenter); break; }
            ++r;
        }
    }

private:
    void fill_expiries() {
        const QString under = under_->currentText().trimmed().toUpper();
        std::set<QString> ex;
        if (master_ != nullptr)
            for (const auto& m : *master_)
                if (m.name == under && (m.type == QLatin1String("CE") || m.type == QLatin1String("PE")) && m.expiry >= today_)
                    ex.insert(m.expiry);
        const QSignalBlocker b(expiry_);
        expiry_->clear();
        for (const auto& e : ex) expiry_->addItem(e);
        expiry_->setEnabled(!ex.empty());
    }
    /// Ask the feed for the future, the cash and the strikes around the money.
    void request() {
        if (master_ == nullptr || master_->empty() || !on_request || strikes_.empty()) return;
        const qint64 fwd = live_mid(client_, fut_) > 0 ? live_mid(client_, fut_) : live_mid(client_, spot_);
        std::vector<double> ks;
        for (const auto& [k, pr] : strikes_) ks.push_back(k);
        std::size_t c = ks.size() / 2;
        if (fwd > 0) {
            c = 0;
            for (std::size_t i = 0; i < ks.size(); ++i)
                if (std::fabs(ks[i] - static_cast<double>(fwd) / 100.0) < std::fabs(ks[c] - static_cast<double>(fwd) / 100.0)) c = i;
        }
        const double center = ks[c];
        if (fwd > 0 && center == requested_center_ && !requested_.empty()) return;
        requested_center_ = fwd > 0 ? center : 0.0;
        requested_.clear();
        const auto add = [this](quint32 tok, const QString& sym) { if (tok != 0) requested_.emplace_back(tok, sym); };
        add(fut_, QStringLiteral("future"));
        add(spot_, QStringLiteral("spot"));
        const std::size_t lo = c > static_cast<std::size_t>(kRequestStrikes) ? c - kRequestStrikes : 0;
        const std::size_t hi = std::min(ks.size() - 1, c + kRequestStrikes);
        for (std::size_t i = lo; i <= hi; ++i) {
            const auto& pr = strikes_.at(ks[i]);
            add(pr.first.token, pr.first.symbol);
            add(pr.second.token, pr.second.symbol);
        }
        on_request(requested_);
    }

    const PriceClient* client_;
    const std::vector<MasterScrip>* master_ = nullptr;
    std::vector<LiveRow> rows_;
    QPushButton* back_ = nullptr;
    QComboBox* under_ = nullptr;
    QComboBox* expiry_ = nullptr;
    QLabel* head_ = nullptr;
    QTableWidget* grid_ = nullptr;
    std::map<double, std::pair<ChainContract, ChainContract>> strikes_;
    std::vector<std::pair<quint32, quint32>> row_tokens_;
    std::vector<std::pair<quint32, QString>> requested_;
    quint32 fut_ = 0, spot_ = 0;
    qint64 fut_expiry_ = 0, expiry_day_ = 0;
    double atm_ = 0.0, requested_center_ = 0.0;
    double rate_ = 0.065;
    QString today_ = QDate::currentDate().toString(Qt::ISODate);
};

// ---------------------------------------------------------------------------
// The page
// ---------------------------------------------------------------------------

class LiveMarketWatch final : public QWidget {
public:
    enum class View { Watch, Chain };

    /// `root`: where data/live/ is (the source tree by default).
    LiveMarketWatch(PriceClient* client, QString root = {}, QWidget* parent = nullptr)
        : QWidget(parent), client_(client), root_(std::move(root)) {
#ifdef ALTAIR_SOURCE_DIR
        if (root_.isEmpty()) root_ = QStringLiteral(ALTAIR_SOURCE_DIR);
#endif
        setObjectName(QStringLiteral("liveMarketWatch"));
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(6, 6, 6, 6);
        v->setSpacing(5);

        // ---- row 1: the loader (Exchange > Segment > Symbol > Expiry > Type > Strike)
        loader_ = new AddScripBar(this);
        loader_->on_add = [this](quint32 tok) { add_scrip(tok); };
        v->addWidget(loader_);

        // ---- row 2: find, and the SIMULATION dialog ------------------------
        // The live feed has no button: it runs by itself from 09:00 to 15:45
        // IST on weekdays (market_tick). Watchlists live in a dialog (Ctrl+S)
        // and the simulator in another, so the watch keeps its height.
        auto* bar = new QHBoxLayout;
        search_ = new QLineEdit(this);
        search_->setObjectName(QStringLiteral("watchFind"));
        search_->setPlaceholderText(QStringLiteral("Find in watch  (Ctrl+F)"));
        search_->setClearButtonEnabled(true);
        search_->setMaximumWidth(180);
        sim_btn_ = new QPushButton(QStringLiteral("SIMULATION"), this);
        sim_btn_->setObjectName(QStringLiteral("simulationButton"));
        sim_btn_->setToolTip(QStringLiteral("Replay a chosen day through the simulator: date, start time, speed, start and stop"));
        bar->addWidget(search_);
        bar->addStretch();
        bar->addWidget(sim_btn_);
        v->addLayout(bar);

        // ---- the watchlist dialog (Ctrl+S) ---------------------------------
        lists_dialog_ = new QDialog(this);
        lists_dialog_->setObjectName(QStringLiteral("watchlistDialog"));
        lists_dialog_->setWindowTitle(QStringLiteral("Watchlists (Ctrl+S)"));
        {
            auto* lv = new QVBoxLayout(lists_dialog_);
            auto* hint = new QLabel(QStringLiteral(
                "The watch shows the active list; whatever it holds streams. Load one, save the watch under its name "
                "or a new one, or delete one (Default stays)."), lists_dialog_);
            hint->setWordWrap(true);
            lv->addWidget(hint);
            auto* lrow = new QHBoxLayout;
            list_ = new QComboBox(lists_dialog_);
            list_->setObjectName(QStringLiteral("watchlistName"));
            list_->setToolTip(QStringLiteral("Load a saved watchlist"));
            list_->setMinimumWidth(200);
            lrow->addWidget(new QLabel(QStringLiteral("Watchlist"), lists_dialog_));
            lrow->addWidget(list_, 1);
            lv->addLayout(lrow);
            auto* brow = new QHBoxLayout;
            save_ = new QPushButton(QStringLiteral("Save"), lists_dialog_);
            save_->setObjectName(QStringLiteral("watchlistSave"));
            save_as_ = new QPushButton(QStringLiteral("Save as…"), lists_dialog_);
            save_as_->setObjectName(QStringLiteral("watchlistSaveAs"));
            delete_list_ = new QPushButton(QStringLiteral("Delete"), lists_dialog_);
            delete_list_->setObjectName(QStringLiteral("watchlistDelete"));
            auto* imp = new QPushButton(QStringLiteral("Import…"), lists_dialog_);
            imp->setToolTip(QStringLiteral("Import a watchlist from a file"));
            auto* exp = new QPushButton(QStringLiteral("Export…"), lists_dialog_);
            exp->setToolTip(QStringLiteral("Export this watchlist to a file"));
            connect(imp, &QPushButton::clicked, this, [this] { import_list(); });
            connect(exp, &QPushButton::clicked, this, [this] { export_list(); });
            auto* close = new QPushButton(QStringLiteral("Close"), lists_dialog_);
            connect(close, &QPushButton::clicked, lists_dialog_, &QDialog::hide);
            for (QPushButton* b : {save_, save_as_, delete_list_, imp, exp}) brow->addWidget(b);
            brow->addStretch();
            brow->addWidget(close);
            lv->addLayout(brow);
        }

        // ---- the SIMULATION dialog ---------------------------------------------
        sim_dialog_ = new QDialog(this);
        sim_dialog_->setObjectName(QStringLiteral("simulationDialog"));
        sim_dialog_->setWindowTitle(QStringLiteral("SIMULATION"));
        sim_date_ = new QDateEdit(QDate::currentDate(), sim_dialog_);
        sim_date_->setObjectName(QStringLiteral("simDate"));
        sim_date_->setCalendarPopup(true);
        sim_date_->setDisplayFormat(QStringLiteral("dd-MMM-yyyy"));
        sim_date_->setMaximumDate(QDate::currentDate());
        sim_date_->setToolTip(QStringLiteral(
            "The day to simulate. The previous closes are the session before it; when dataset/ has that day's "
            "1-minute bars, NIFTY, BANKNIFTY and INDIA VIX follow them minute by minute."));
        sim_from_ = new QTimeEdit(QTime(9, 15), sim_dialog_);
        sim_from_->setDisplayFormat(QStringLiteral("HH:mm"));
        sim_from_->setTimeRange(QTime(9, 15), QTime(15, 29));
        sim_speed_ = new QComboBox(sim_dialog_);
        for (int x : {1, 5, 10, 30, 60, 120, 300})
            sim_speed_->addItem(QStringLiteral("%1×").arg(x), x);
        start_sim_ = new QPushButton(QStringLiteral("Start SIM"), sim_dialog_);
        start_sim_->setObjectName(QStringLiteral("startSimFeed"));
        start_sim_->setToolTip(QStringLiteral("Run altair_price_service --sim for the chosen day, time and speed. Every price "
                                              "it shows is marked SIM. A live feed running is stopped first and comes back "
                                              "by itself when the SIM stops (market hours)."));
        stop_ = new QPushButton(QStringLiteral("■ Stop SIM"), sim_dialog_);
        stop_->setObjectName(QStringLiteral("stopSimFeed"));
        stop_->setEnabled(false);
        {
            auto* form = new QFormLayout(sim_dialog_);
            auto* hint = new QLabel(QStringLiteral(
                "Simulated prices, marked SIM everywhere; the models paper-trade them. The live feed runs by itself "
                "09:00–15:45 IST on weekdays and pauses while a SIM runs."), sim_dialog_);
            hint->setWordWrap(true);
            form->addRow(hint);
            form->addRow(QStringLiteral("Day"), sim_date_);
            form->addRow(QStringLiteral("From (IST)"), sim_from_);
            form->addRow(QStringLiteral("Speed"), sim_speed_);
            auto* srow = new QHBoxLayout;
            srow->addWidget(start_sim_);
            srow->addWidget(stop_);
            auto* close = new QPushButton(QStringLiteral("Close"), sim_dialog_);
            connect(close, &QPushButton::clicked, sim_dialog_, &QDialog::hide);
            srow->addStretch();
            srow->addWidget(close);
            form->addRow(srow);
        }
        connect(sim_btn_, &QPushButton::clicked, this, [this] { show_simulation(); });

        status_ = new QLabel(this);
        status_->setTextFormat(Qt::RichText);
        status_->setWordWrap(true);
        v->addWidget(status_);

        // ---- the watch and the chain, with depth and trades beside them ---
        model_ = new LiveWatchModel(client_, this);
        filter_ = new LiveWatchFilter(this);
        filter_->setSourceModel(model_);
        filter_->setSortRole(LiveWatchModel::SortRole);
        view_ = new QTableView(this);
        view_->setObjectName(QStringLiteral("liveWatch"));
        view_->setModel(filter_);
        view_->setSortingEnabled(true);
        view_->sortByColumn(-1, Qt::AscendingOrder);
        view_->setSelectionBehavior(QAbstractItemView::SelectRows);
        view_->setSelectionMode(QAbstractItemView::SingleSelection);
        view_->setAlternatingRowColors(true);
        view_->setContextMenuPolicy(Qt::CustomContextMenu);
        view_->setToolTip(QStringLiteral("Enter or double-click: the option chain of the scrip's underlying"));
        view_->verticalHeader()->hide();
        view_->verticalHeader()->setDefaultSectionSize(21);
        view_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        view_->horizontalHeader()->setSectionsMovable(true);
        view_->horizontalHeader()->setDefaultSectionSize(78);
        view_->setColumnWidth(LiveWatchModel::Symbol, 168);
        view_->setColumnWidth(LiveWatchModel::Ltp, 104);   // room for the price and its arrow
        view_->setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
        // Repaint only rows on screen: a ticking row scrolled away costs nothing.
        model_->row_visible = [this](int src) {
            if (!view_->isVisible()) return false;
            const QModelIndex pi = filter_->mapFromSource(model_->index(src, 0));
            if (!pi.isValid()) return false;
            const QRect r = view_->visualRect(pi);
            return r.isValid() && r.bottom() >= 0 && r.top() <= view_->viewport()->height();
        };
        chain_ = new LiveChainView(client_, this);
        chain_->on_request = [this](const std::vector<std::pair<quint32, QString>>& t) { write_chain_request(t); };
        chain_->on_back = [this] { set_view(View::Watch); };
        chain_->on_greek = [this](quint32 tok) { if (on_greek) on_greek(tok); };
        main_ = new QStackedWidget(this);
        main_->addWidget(view_);
        main_->addWidget(chain_);

        side_ = new QWidget(this);
        auto* dv = new QVBoxLayout(side_);
        dv->setContentsMargins(0, 0, 0, 0);
        sel_ = new QLabel(QStringLiteral("Select a row."), side_);
        sel_->setTextFormat(Qt::RichText);
        sel_->setWordWrap(true);
        dv->addWidget(sel_);
        auto* depth_title = new QLabel(QStringLiteral("MARKET DEPTH — every level the feed sends (50 from FYERS)"), side_);
        depth_title->setObjectName(QStringLiteral("sectionKicker"));
        dv->addWidget(depth_title);
        // Depth over Time & Sales in a splitter: drag the handle down (or
        // double-click the depth's title) and the book takes the full height,
        // all 50 levels at once in compact rows.
        side_split_ = new QSplitter(Qt::Vertical, side_);
        side_split_->setObjectName(QStringLiteral("depthSplit"));
        side_split_->setChildrenCollapsible(true);
        depth_ = make_live_table(6, {QStringLiteral("Orders"), QStringLiteral("Bid Qty"), QStringLiteral("Bid"),
                                     QStringLiteral("Ask"), QStringLiteral("Ask Qty"), QStringLiteral("Orders")}, side_split_);
        depth_->setObjectName(QStringLiteral("liveDepth"));
        depth_->verticalHeader()->setDefaultSectionSize(16);
        depth_->verticalHeader()->setMinimumSectionSize(14);
        side_split_->addWidget(depth_);
        auto* tape_box = new QWidget(side_split_);
        auto* tv = new QVBoxLayout(tape_box);
        tv->setContentsMargins(0, 4, 0, 0);
        auto* tape_title = new QLabel(QStringLiteral("TIME & SALES — every trade, newest first"), tape_box);
        tape_title->setObjectName(QStringLiteral("sectionKicker"));
        tv->addWidget(tape_title);
        tape_ = make_live_table(3, {QStringLiteral("Time (IST)"), QStringLiteral("Price"), QStringLiteral("Qty")}, tape_box);
        tv->addWidget(tape_, 1);
        side_split_->addWidget(tape_box);
        side_split_->setStretchFactor(0, 3);
        side_split_->setStretchFactor(1, 1);
        dv->addWidget(side_split_, 1);
        depth_title->installEventFilter(this);
        depth_title_ = depth_title;

        auto* split = new QSplitter(Qt::Horizontal, this);
        split->addWidget(main_);
        split->addWidget(side_);
        split->setStretchFactor(0, 68);
        split->setStretchFactor(1, 32);
        v->addWidget(split, 1);

        connect(search_, &QLineEdit::textChanged, this, [this](const QString& t) { filter_->set_text(t); });
        connect(view_->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
                [this](const QModelIndex& cur, const QModelIndex&) {
                    const QModelIndex src = filter_->mapToSource(cur);
                    if (src.isValid()) pick(model_->rows()[static_cast<std::size_t>(src.row())].token);
                });
        connect(view_, &QTableView::doubleClicked, this, [this](const QModelIndex&) { open_chain(); });
        chain_->on_pick = [this](quint32 tok) { pick(tok); };
        connect(view_, &QTableView::customContextMenuRequested, this, [this](const QPoint& at) { context_menu(at); });
        view_->installEventFilter(this);
        chain_->grid()->installEventFilter(this);
        connect(list_, &QComboBox::activated, this, [this](int) { load_list(list_->currentText()); });
        connect(save_, &QPushButton::clicked, this, [this] { save_list(); });
        connect(save_as_, &QPushButton::clicked, this, [this] {
            bool ok = false;
            const QString name = QInputDialog::getText(this, QStringLiteral("Save watchlist as"), QStringLiteral("Name"),
                                                       QLineEdit::Normal, QString(), &ok);
            if (ok) save_list_as(name);
        });
        connect(delete_list_, &QPushButton::clicked, this, [this] { delete_list(list_name_); });
        if (client_ != nullptr) {
            connect(client_, &PriceClient::priceUpdated, this, [this](unsigned tok) {
                model_->touch(tok);
                if (tok == selected_) side_dirty_ = true;
                // Say LIVE, REPLAY or SIM as soon as the first index frame
                // says which, not on the next status tick.
                if (!labelled_ && tok == 256265u) { labelled_ = true; refresh_status(); }
            });
            connect(client_, &PriceClient::statusChanged, this, [this] { labelled_ = false; });
            connect(client_, &PriceClient::statusChanged, this, [this] { refresh_status(); });
        }
        connect(start_sim_, &QPushButton::clicked, this, [this] { start_sim(); });
        connect(stop_, &QPushButton::clicked, this, [this] { stop_feed(); });
        // The live feed's own clock: every 30 s, on during market hours.
        market_timer_.setInterval(30'000);
        connect(&market_timer_, &QTimer::timeout, this, [this] { market_tick(); });

        side_timer_.setInterval(250);
        connect(&side_timer_, &QTimer::timeout, this, [this] {
            if (!isVisible()) return;                      // nothing on screen, nothing to paint
            if (side_dirty_) { side_dirty_ = false; refresh_side(); }
            if (++chain_tick_ % 2 == 0 && main_->currentWidget() == chain_) chain_->refresh();
        });
        side_timer_.start();
        status_timer_.setInterval(2000);
        connect(&status_timer_, &QTimer::timeout, this, [this] { reload_universe(false); refresh_status(); });
        status_timer_.start();
        reload_universe(true);
        load_list(watchlists::active(root_));
        refresh_status();
        // The master is ~10 MB: read on a worker thread once the window is up,
        // so the Terminal never waits for it.
        QTimer::singleShot(0, this, [this] { load_master_async(); });
    }

    ~LiveMarketWatch() override {
        stop_feed();
        if (master_thread_.joinable()) master_thread_.join();
    }
    LiveMarketWatch(const LiveMarketWatch&) = delete;
    LiveMarketWatch& operator=(const LiveMarketWatch&) = delete;

    /// For tests and the Terminal's hotkeys.
    [[nodiscard]] LiveWatchModel* model() const noexcept { return model_; }
    [[nodiscard]] LiveWatchFilter* filter() const noexcept { return filter_; }
    [[nodiscard]] QTableView* view() const noexcept { return view_; }
    [[nodiscard]] LiveChainView* chain() const noexcept { return chain_; }
    [[nodiscard]] AddScripBar* loader() const noexcept { return loader_; }
    [[nodiscard]] QTableWidget* tape() const noexcept { return tape_; }
    [[nodiscard]] QTableWidget* depth() const noexcept { return depth_; }
    [[nodiscard]] QString status_text() const { return status_->text(); }
    [[nodiscard]] quint32 selected_token() const noexcept { return selected_; }
    [[nodiscard]] const std::vector<MasterScrip>& master() const noexcept { return master_; }
    [[nodiscard]] const LiveRow* row_of_token(quint32 token) const {
        const int r = model_->row_of(token);
        return r >= 0 ? &model_->rows()[static_cast<std::size_t>(r)] : nullptr;
    }
    void select_token(quint32 token) {
        const int r = model_->row_of(token);
        if (r < 0) return;
        view_->setCurrentIndex(filter_->mapFromSource(model_->index(r, 0)));
        pick(token);
    }

    /// Market Watch or Option Chain in the main area.
    void set_view(View v) {
        main_->setCurrentWidget(v == View::Chain ? static_cast<QWidget*>(chain_) : view_);
        if (v == View::Chain) { chain_->refresh(); chain_->grid()->setFocus(); }
        else view_->setFocus();
        if (on_view_changed) on_view_changed(v);
    }
    [[nodiscard]] View view_mode() const { return main_->currentWidget() == chain_ ? View::Chain : View::Watch; }
    void show_chain() { set_view(View::Chain); }
    /// Enter / double-click: the option chain of the selected scrip's
    /// underlying -- the index, the stock, or the future's or option's own.
    bool open_chain() {
        const LiveRow* r = row_of_token(selected_);
        if (r == nullptr) return false;
        QString under = r->underlying.isEmpty() ? r->symbol : r->underlying;
        if (r->kind == QLatin1String("index")) {
            if (r->symbol == QLatin1String("NIFTY 50")) under = QStringLiteral("NIFTY");
            else if (r->symbol == QLatin1String("NIFTY BANK")) under = QStringLiteral("BANKNIFTY");
        }
        ensure_master();
        chain_->open(under);
        set_view(View::Chain);
        return true;
    }
    void focus_find() { search_->setFocus(); search_->selectAll(); }
    /// Insert: the loader's symbol box.
    void focus_add() { ensure_master(); loader_->focus(); }

    void set_rows_for_test(std::vector<LiveRow> rows) {
        model_->set_rows(rows);
        chain_->set_rows(rows);
        // Tests see every row they set.
        QSet<quint32> all;
        for (const auto& r : rows) all.insert(r.token);
        members_.clear();
        for (const auto& r : rows) members_.emplace_back(r.token, r.symbol);
        filter_->set_members(all);
    }
    /// Tests: use this master instead of reading data/instruments.csv.
    void set_master_for_test(std::vector<MasterScrip> m) { install_master(std::move(m)); }
    /// Tests: what the Greek Watch is asked to open.
    std::function<void(quint32)> on_greek;
    /// Tokens streamed besides the open chain's (Greek Watch legs).
    void set_extra_requests(std::vector<std::pair<quint32, QString>> extra) {
        extra_requests_ = std::move(extra);
        write_chain_request(chain_->requested());
    }

    /// Add a scrip to the active watchlist by Kite token. It is written to
    /// data/live/watchlist.csv, which the price service streams (added to the
    /// running socket, no reconnect). False when the master does not know it.
    bool add_scrip(quint32 token) {
        ensure_master();
        const auto it = std::find_if(master_.begin(), master_.end(), [token](const MasterScrip& m) { return m.token == token; });
        if (it == master_.end()) return false;
        if (std::none_of(members_.begin(), members_.end(), [token](const auto& e) { return e.first == token; }))
            members_.emplace_back(token, it->symbol);
        apply_members();
        (void)save_list();
        QStringList lines = read_watch_file();
        const QString tok = QString::number(token);
        bool have = false;
        for (const QString& l : lines) have = have || l.section(QLatin1Char(','), 0, 0) == tok;
        if (!have) lines << QStringLiteral("%1,%2").arg(tok, it->symbol);
        write_watch_file(lines);
        if (model_->row_of(token) < 0) {
            LiveRow r = row_from_master(*it);
            pending_.push_back(r);
            auto rows = model_->rows();
            rows.push_back(r);
            model_->set_rows(rows);
        }
        select_token(token);
        note_ = QStringLiteral("Added %1 to %2. %3").arg(it->symbol, list_name_,
            feed_ != nullptr || (client_ != nullptr && client_->connected())
                ? QStringLiteral("It streams within a few seconds.")
                : QStringLiteral("It streams once a feed is running."));
        refresh_status();
        return true;
    }

    /// Remove a scrip from the active watchlist (Delete).
    void remove_scrip(quint32 token) {
        if (token == 0) return;
        members_.erase(std::remove_if(members_.begin(), members_.end(), [token](const auto& e) { return e.first == token; }),
                       members_.end());
        apply_members();
        (void)save_list();
        QStringList lines = read_watch_file();
        lines.erase(std::remove_if(lines.begin(), lines.end(), [token](const QString& l) {
                        return l.section(QLatin1Char(','), 0, 0) == QString::number(token);
                    }), lines.end());
        write_watch_file(lines);
        const LiveRow* r = row_of_token(token);
        note_ = QStringLiteral("Removed %1 from %2.").arg(r != nullptr ? r->symbol : QString::number(token), list_name_);
        refresh_status();
    }

    // ---- watchlists ------------------------------------------------------------
    [[nodiscard]] QString list_name() const { return list_name_; }
    [[nodiscard]] QStringList list_names() const { return watchlists::names(root_); }
    [[nodiscard]] std::vector<quint32> list_tokens() const {
        std::vector<quint32> out;
        for (const auto& e : members_) out.push_back(e.first);
        return out;
    }
    /// Load a saved list (Default is made from the indices and near futures
    /// the first time). The feed is asked to stream its scrips.
    bool load_list(const QString& name) {
        const QString n = watchlists::clean_name(name).isEmpty() ? QStringLiteral("Default") : watchlists::clean_name(name);
        const QString file = watchlists::path(root_, n);
        if (!QFileInfo::exists(file) && n != QLatin1String("Default")) return false;
        list_name_ = n;
        members_ = watchlists::load(file);
        if (members_.empty() && n == QLatin1String("Default") && !QFileInfo::exists(file)) {
            for (const auto& r : model_->rows())
                if (r.group == QLatin1String("Indices") || r.group == QLatin1String("Futures")) members_.emplace_back(r.token, r.symbol);
            (void)save_list();
        }
        watchlists::set_active(root_, list_name_);
        apply_members();
        // Stream every scrip of the list: the feed adds what it does not carry.
        QStringList lines = read_watch_file();
        for (const auto& [t, sym] : members_)
            if (model_->row_of(t) < 0
                && std::none_of(lines.begin(), lines.end(), [t](const QString& l) { return l.section(QLatin1Char(','), 0, 0) == QString::number(t); }))
                lines << QStringLiteral("%1,%2").arg(t).arg(sym);
        write_watch_file(lines);
        refresh_lists();
        return true;
    }
    bool save_list() { return watchlists::save(watchlists::path(root_, list_name_), members_); }
    bool save_list_as(const QString& name) {
        const QString n = watchlists::clean_name(name);
        if (n.isEmpty()) return false;
        list_name_ = n;
        if (!save_list()) return false;
        watchlists::set_active(root_, list_name_);
        refresh_lists();
        note_ = QStringLiteral("Saved watchlist %1 (%2 scrips).").arg(n).arg(members_.size());
        refresh_status();
        return true;
    }
    bool delete_list(const QString& name) {
        const QString n = watchlists::clean_name(name);
        if (n.isEmpty() || n == QLatin1String("Default")) return false;
        if (!QFile::remove(watchlists::path(root_, n))) return false;
        if (n == list_name_) load_list(QStringLiteral("Default"));
        refresh_lists();
        return true;
    }

    /// Run the live feed by itself on market hours (off for scripted runs
    /// and tests): checked a few seconds after the Terminal opens, then
    /// every 30 s.
    void set_autostart(bool on) {
        autostart_ = on;
        if (!on) { market_timer_.stop(); return; }
        market_timer_.start();
        QTimer::singleShot(3500, this, [this] { market_tick(); });
    }
    [[nodiscard]] bool autostart() const noexcept { return autostart_; }

    /// The live feed's schedule: on from 09:00 to 15:45 IST, Monday to
    /// Friday, unless a SIM runs, a feed already streams, or FYERS refused it
    /// in the last five minutes (no login today, a holiday) -- then it is
    /// tried again after five. At 15:45 a live feed is stopped (the service
    /// also stops itself then: --until 15:45). True when this tick started it.
    bool market_tick() {
        if (!autostart_) return false;
        const QDateTime ist = now_ist ? now_ist() : QDateTime::currentDateTimeUtc().addSecs(19800);
        if (!in_market_hours(ist)) {
            if (feed_ != nullptr && !sim_running_) stop_feed();
            return false;
        }
        if (feed_ != nullptr || sim_running_) return false;
        if (client_ != nullptr && client_->connected()) return false;   // something else already streams
        if (refused_at_.isValid() && refused_at_.secsTo(ist) < 300) return false;
        auto_started_ = true;
        start_feed(false);
        return true;
    }
    /// IST now (UTC + 5:30 as a UTC QDateTime); tests set a clock.
    std::function<QDateTime()> now_ist;
    static constexpr int kFeedOnMinute = 9 * 60, kFeedOffMinute = 15 * 60 + 45;
    /// 09:00 to 15:45 IST, Monday to Friday (exchange holidays: the feed says so).
    [[nodiscard]] static bool in_market_hours(const QDateTime& ist) {
        const int dow = ist.date().dayOfWeek();
        const int minute = ist.time().hour() * 60 + ist.time().minute();
        return dow >= 1 && dow <= 5 && minute >= kFeedOnMinute && minute < kFeedOffMinute;
    }
    [[nodiscard]] bool feed_running() const noexcept { return feed_ != nullptr; }
    [[nodiscard]] bool sim_running() const noexcept { return sim_running_; }

    /// Ctrl+S: the watchlists (load, save, save as, delete, import, export).
    void show_watchlists() {
        refresh_lists();
        lists_dialog_->show();
        lists_dialog_->raise();
        lists_dialog_->activateWindow();
        list_->setFocus();
    }
    [[nodiscard]] QDialog* watchlist_dialog() const noexcept { return lists_dialog_; }
    /// The SIMULATION dialog: day, start, speed, start and stop.
    void show_simulation() {
        sim_dialog_->show();
        sim_dialog_->raise();
        sim_dialog_->activateWindow();
    }
    [[nodiscard]] QDialog* simulation_dialog() const noexcept { return sim_dialog_; }
    /// Start SIM: a live feed running is stopped first (one bus, one port);
    /// the schedule brings it back once the SIM stops.
    void start_sim() {
        if (feed_ != nullptr && !sim_running_) stop_feed();
        start_feed(true);
    }
    /// Shift+S: the scrip selection, Exchange first with its list open; Tab
    /// and Shift+Tab walk the dropdowns, Enter adds.
    void focus_scrip_selection() { ensure_master(); loader_->open_selection(); }
    /// The depth/trades splitter, for tests: depth at full height or shared.
    [[nodiscard]] QSplitter* depth_split() const noexcept { return side_split_; }
    void toggle_full_depth() {
        const QList<int> sz = side_split_->sizes();
        if (sz.size() != 2) return;
        const int total = sz[0] + sz[1];
        if (sz[1] == 0) side_split_->setSizes({total * 3 / 4, total - total * 3 / 4});
        else side_split_->setSizes({total, 0});
    }

    /// + and − on the watch or the chain.
    std::function<void(bool buy)> on_order_key;
    std::function<void(View)> on_view_changed;

protected:
    bool eventFilter(QObject* obj, QEvent* e) override {
        if (obj == depth_title_ && e->type() == QEvent::MouseButtonDblClick) { toggle_full_depth(); return true; }
        if (e->type() == QEvent::KeyPress) {
            const auto* k = static_cast<QKeyEvent*>(e);
            if (k->key() == Qt::Key_Plus || (k->key() == Qt::Key_Equal && (k->modifiers() & Qt::ShiftModifier))) {
                if (on_order_key) on_order_key(true);
                return true;
            }
            if (k->key() == Qt::Key_Minus || k->key() == Qt::Key_Underscore) {
                if (on_order_key) on_order_key(false);
                return true;
            }
            const bool enter = k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter;
            if (obj == view_) {
                if (enter) { open_chain(); return true; }
                if (k->key() == Qt::Key_Delete) { remove_scrip(selected_); return true; }
            }
            if (obj == chain_->grid()) {
                if (enter) { chain_->open_greek(chain_->grid()->currentRow(), chain_->grid()->currentColumn()); return true; }
                if (k->key() == Qt::Key_Backspace || k->key() == Qt::Key_Escape) { set_view(View::Watch); return true; }
            }
            if (k->key() == Qt::Key_Insert) { focus_add(); return true; }
        }
        return QWidget::eventFilter(obj, e);
    }

private:
    void pick(quint32 token) {
        if (token == 0) return;
        selected_ = token;
        refresh_side();
    }

    void context_menu(const QPoint& at) {
        QMenu menu(this);
        auto* chain = menu.addAction(QStringLiteral("Option chain  (Enter)"));
        menu.addSeparator();
        auto* buy = menu.addAction(QStringLiteral("Buy  (+ / F1)"));
        auto* sell = menu.addAction(QStringLiteral("Sell  (− / F2)"));
        menu.addSeparator();
        auto* add = menu.addAction(QStringLiteral("Add scrip…  (Insert)"));
        auto* remove = menu.addAction(QStringLiteral("Remove scrip  (Delete)"));
        QAction* chosen = menu.exec(view_->viewport()->mapToGlobal(at));
        if (chosen == chain) open_chain();
        else if (chosen == buy && on_order_key) on_order_key(true);
        else if (chosen == sell && on_order_key) on_order_key(false);
        else if (chosen == add) focus_add();
        else if (chosen == remove) remove_scrip(selected_);
    }

    [[nodiscard]] static LiveRow row_from_master(const MasterScrip& m) {
        LiveRow r;
        r.token = m.token;
        r.symbol = m.symbol;
        // FYERS names until the feed's universe.csv brings the real ones (live/fyers_names.hpp):
        // NSE equity "-EQ" unless the symbol carries a two-letter series, BSE group A, F&O as Kite names them.
        r.fyers = m.exchange == QLatin1String("NSE")
                      ? (m.symbol.size() > 3 && m.symbol.lastIndexOf(QLatin1Char('-')) == m.symbol.size() - 3
                             ? QStringLiteral("NSE:%1").arg(m.symbol)
                             : QStringLiteral("NSE:%1-EQ").arg(m.symbol))
                : m.exchange == QLatin1String("BSE") ? QStringLiteral("BSE:%1-A").arg(m.symbol)
                : m.exchange == QLatin1String("BFO") ? QStringLiteral("BSE:%1").arg(m.symbol)
                                                     : QStringLiteral("NSE:%1").arg(m.symbol);
        r.group = QStringLiteral("Watchlist");
        r.kind = m.type == QLatin1String("EQ")  ? QStringLiteral("equity")
               : m.type == QLatin1String("FUT") ? QStringLiteral("future")
               : m.type == QLatin1String("CE")  ? QStringLiteral("call")
               : m.type == QLatin1String("PE")  ? QStringLiteral("put")
               : m.segment == QLatin1String("NSE") || m.segment == QLatin1String("BSE") ? QStringLiteral("equity")
                                                                                       : QStringLiteral("option");
        r.underlying = r.kind == QLatin1String("equity") ? m.symbol : m.name;
        r.lot = m.lot;
        r.tick = m.tick;
        r.strike = m.strike;
        r.expiry_day = live_detail::parse_day(m.expiry);
        return r;
    }

    // ---- the instrument master ------------------------------------------------
    void ensure_master() {
        if (master_loaded_) return;
        master_loaded_ = true;
        install_master(load_master_scrips(root_ + QStringLiteral("/data/instruments.csv")));
        if (master_.empty()) {
            note_ = QStringLiteral("No instrument master at data/instruments.csv: run altair_kite_update (or the "
                                   "fetch scripts) to get it, then scrips can be added and any chain opened.");
            refresh_status();
        }
    }
    void load_master_async() {
        if (master_loaded_ || master_thread_.joinable()) return;
        const QString path = root_ + QStringLiteral("/data/instruments.csv");
        if (!QFileInfo::exists(path)) { ensure_master(); return; }
        master_thread_ = std::thread([this, path] {
            auto m = load_master_scrips(path);
            QMetaObject::invokeMethod(this, [this, m = std::move(m)]() mutable {
                if (!master_loaded_) install_master(std::move(m));
            }, Qt::QueuedConnection);
        });
    }
    void install_master(std::vector<MasterScrip> m) {
        master_loaded_ = true;
        master_ = std::move(m);
        loader_->set_master(&master_);
        chain_->set_master(&master_);
    }

    // ---- watchlist, chain-request and feed files ------------------------------
    void apply_members() {
        QSet<quint32> s;
        for (const auto& e : members_) s.insert(e.first);
        filter_->set_members(s);
    }
    void refresh_lists() {
        const QSignalBlocker b(list_);
        list_->clear();
        list_->addItems(watchlists::names(root_));
        if (list_->findText(list_name_) < 0) list_->addItem(list_name_);
        list_->setCurrentText(list_name_);
        delete_list_->setEnabled(list_name_ != QLatin1String("Default"));
    }
    void import_list() {
        const QString file = QFileDialog::getOpenFileName(this, QStringLiteral("Import watchlist"), QString(),
                                                          QStringLiteral("Watchlist (*.csv)"));
        if (file.isEmpty()) return;
        members_ = watchlists::load(file);
        (void)save_list_as(QFileInfo(file).completeBaseName());
        load_list(list_name_);
    }
    void export_list() {
        const QString file = QFileDialog::getSaveFileName(this, QStringLiteral("Export watchlist"), list_name_ + QStringLiteral(".csv"),
                                                          QStringLiteral("Watchlist (*.csv)"));
        if (!file.isEmpty() && watchlists::save(file, members_))
            note_ = QStringLiteral("Exported %1 to %2.").arg(list_name_, file.toHtmlEscaped());
        refresh_status();
    }
    /// data/live/chain_request.csv: the open chain's strikes plus the extra
    /// tokens (Greek Watch). The feed adds them to the running socket.
    void write_chain_request(const std::vector<std::pair<quint32, QString>>& tokens) const {
        QDir().mkpath(root_ + QStringLiteral("/data/live"));
        QSaveFile f(live_path("chain_request.csv"));
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return;
        QTextStream out(&f);
        out << "token,symbol\n";
        QSet<quint32> seen;
        for (const auto* list : {&tokens, &extra_requests_})
            for (const auto& [t, sym] : *list)
                if (!seen.contains(t)) { seen.insert(t); out << t << ',' << QString(sym).remove(QLatin1Char(',')) << '\n'; }
        out.flush();
        (void)f.commit();
    }
    [[nodiscard]] QString live_path(const char* name) const { return root_ + QStringLiteral("/data/live/") + QString::fromLatin1(name); }
    [[nodiscard]] QStringList read_watch_file() const {
        QStringList out;
        QFile f(live_path("watchlist.csv"));
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return out;
        QTextStream in(&f);
        while (!in.atEnd()) {
            const QString l = in.readLine().trimmed();
            if (!l.isEmpty() && l.at(0).isDigit()) out << l;
        }
        return out;
    }
    void write_watch_file(const QStringList& lines) const {
        QDir().mkpath(root_ + QStringLiteral("/data/live"));
        QFile f(live_path("watchlist.csv"));
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return;
        QTextStream out(&f);
        out << "token,symbol\n";
        for (const QString& l : lines) out << l << '\n';
    }

    void reload_universe(bool force) {
        const QString path = live_path("universe.csv");
        const QFileInfo fi(path);
        const qint64 stamp = fi.exists() ? fi.lastModified().toMSecsSinceEpoch() : 0;
        if (!force && stamp == universe_stamp_) return;
        universe_stamp_ = stamp;
        auto rows = load_live_universe(path);
        from_file_ = !rows.empty();
        if (rows.empty()) rows = default_live_universe();
        // Scrips added here that the feed has not written back yet.
        for (auto it = pending_.begin(); it != pending_.end();) {
            const bool streamed = std::any_of(rows.begin(), rows.end(), [&](const LiveRow& r) { return r.token == it->token; });
            if (streamed) { it = pending_.erase(it); continue; }
            rows.push_back(*it);
            ++it;
        }
        // A list's scrip the feed does not stream yet still has its row.
        if (!master_.empty())
            for (const auto& [t, sym] : members_)
                if (std::none_of(rows.begin(), rows.end(), [t](const LiveRow& r) { return r.token == t; })) {
                    const auto m = std::find_if(master_.begin(), master_.end(), [t](const MasterScrip& x) { return x.token == t; });
                    if (m != master_.end()) rows.push_back(row_from_master(*m));
                }
        model_->set_rows(rows);
        chain_->set_rows(rows);
        // The first universe the feed writes makes the Default list.
        if (from_file_ && list_name_ == QLatin1String("Default") && members_.size() <= 3) {
            bool grew = false;
            for (const auto& r : rows)
                if ((r.group == QLatin1String("Indices") || r.group == QLatin1String("Futures"))
                    && std::none_of(members_.begin(), members_.end(), [&r](const auto& e) { return e.first == r.token; })) {
                    members_.emplace_back(r.token, r.symbol);
                    grew = true;
                }
            if (grew) (void)save_list();
        }
        apply_members();
        if (selected_ == 0) {
            // Open on the near NIFTY future: the instrument with a book and a
            // tape, so depth and time and sales show something at once.
            for (const auto& r : rows)
                if (r.kind == QLatin1String("future") && r.underlying == QLatin1String("NIFTY")) { selected_ = r.token; break; }
            if (selected_ == 0 && !rows.empty()) selected_ = rows.front().token;
        }
        const int r = model_->row_of(selected_);
        if (r >= 0) view_->setCurrentIndex(filter_->mapFromSource(model_->index(r, 0)));
        refresh_side();
    }

    void refresh_side() {
        const LivePrice* p = client_ != nullptr && selected_ != 0 ? client_->price(selected_) : nullptr;
        const int r = model_->row_of(selected_);
        if (r >= 0) {
            const LiveRow& row = model_->rows()[static_cast<std::size_t>(r)];
            const QuotePayload* q = p != nullptr && p->has_quote ? &p->quote : nullptr;
            sel_->setText(QStringLiteral("<b>%1</b> <span style='color:%2'>%3 · lot %4</span>%5")
                              .arg(row.symbol, QString::fromLatin1(theme_token::kTextMuted), row.fyers)
                              .arg(row.lot)
                              .arg(q != nullptr && q->has(kQuoteHasCircuit)
                                       ? QStringLiteral(" <span style='color:%1'>· circuit %2 – %3 · total buy %4 / sell %5</span>")
                                             .arg(QString::fromLatin1(theme_token::kTextMuted), live_detail::px(q->lower_circuit),
                                                  live_detail::px(q->upper_circuit), live_detail::qty(q->total_buy),
                                                  live_detail::qty(q->total_sell))
                                       : QString()));
        } else if (const ChainContract* c = chain_->contract(selected_)) {
            sel_->setText(QStringLiteral("<b>%1</b> <span style='color:%2'>%3 · lot %4</span>")
                              .arg(c->symbol, QString::fromLatin1(theme_token::kTextMuted), c->exchange).arg(c->lot));
        }
        show_live_depth(depth_, p);
        show_live_tape(tape_, p);
    }

    void refresh_status() {
        const QString muted = QString::fromLatin1(theme_token::kTextMuted);
        QString stream;
        if (client_ == nullptr || !client_->connected()) {
            stream = QStringLiteral("<span style='color:%1'>● NOT CONNECTED</span> <span style='color:%2'>%3</span>")
                         .arg(QStringLiteral("#8A93A2"), muted,
                              feed_ != nullptr ? QStringLiteral("starting the feed…")
                                               : QStringLiteral("no feed running — the live feed starts by itself 09:00–15:45 IST on "
                                                                "weekdays; SIMULATION replays a chosen day."));
        } else {
            const LivePrice* n = client_->price(256265u);
            const bool sim = n != nullptr && n->simulated, rep = n != nullptr && n->replay;
            const QString tag = sim ? QStringLiteral("SIM — simulated, not market data")
                              : rep ? QStringLiteral("REPLAY") : QStringLiteral("LIVE");
            const QString col = sim || rep ? QStringLiteral("#F4C95D") : QStringLiteral("#7FD17F");
            stream = QStringLiteral("<span style='color:%1'>● %2</span> <span style='color:%3'>%4 instruments · "
                                    "%5 frames · %6 gaps · feed time %7 IST</span>")
                         .arg(col, tag, muted).arg(client_->instruments()).arg(client_->frames()).arg(client_->gaps())
                         .arg(live_detail::ist(n != nullptr ? n->exchange_ts_ns : 0, false));
        }
        QString feed;
        QFile f(live_path("feed_status.json"));
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
            const qint64 age = QDateTime::currentSecsSinceEpoch() - o.value(QStringLiteral("written_unix")).toInteger();
            if (age < 120)
                feed = QStringLiteral(" &nbsp; <span style='color:%1'>source %2 · %3%4</span>")
                           .arg(muted, o.value(QStringLiteral("source")).toString().toUpper(),
                                o.value(QStringLiteral("state")).toString())
                           .arg(o.value(QStringLiteral("error")).toString().isEmpty()
                                    ? QString() : QStringLiteral(" · ") + o.value(QStringLiteral("error")).toString().toHtmlEscaped());
            // How much of what is subscribed has prices and books (every 30 s).
            const int subs = o.value(QStringLiteral("subscribed")).toInt();
            if (age < 120 && subs > 0) {
                const int priced = o.value(QStringLiteral("priced")).toInt();
                feed += QStringLiteral(" &nbsp; <span style='color:%1'>prices %2/%3 · 50-level books %4</span>")
                            .arg(priced < subs ? QStringLiteral("#E3B341") : muted)
                            .arg(priced).arg(subs).arg(o.value(QStringLiteral("depth50")).toInt());
                const QJsonArray none = o.value(QStringLiteral("no_price")).toArray();
                if (!none.isEmpty()) {
                    QStringList names;
                    for (int i = 0; i < none.size() && i < 4; ++i) names << none.at(i).toString().toHtmlEscaped();
                    feed += QStringLiteral(" <span style='color:#E3B341'>no price yet: %1%2 (data/live/price_service.log)</span>")
                                .arg(names.join(QStringLiteral(", ")),
                                     subs - priced > 4 ? QStringLiteral(" +%1").arg(subs - priced - 4) : QString());
                }
            }
            // Scrips FYERS refused by name: their rows stay empty, so say which.
            const QJsonArray unknown = o.value(QStringLiteral("unknown")).toArray();
            if (age < 120 && !unknown.isEmpty()) {
                QStringList names;
                for (int i = 0; i < unknown.size() && i < 5; ++i) names << unknown.at(i).toString().toHtmlEscaped();
                const int more = o.value(QStringLiteral("unknown_symbols")).toInt() - static_cast<int>(names.size());
                feed += QStringLiteral(" &nbsp; <span style='color:#E3B341'>FYERS does not know %1%2 — no data for them</span>")
                            .arg(names.join(QStringLiteral(", ")),
                                 more > 0 ? QStringLiteral(" (+%1)").arg(more) : QString());
            }
        }
        const QString uni = from_file_ ? QString()
            : QStringLiteral(" &nbsp; <span style='color:%1'>(indices only until a feed writes data/live/universe.csv)</span>").arg(muted);
        const QString note = note_.isEmpty() ? QString()
            : QStringLiteral(" &nbsp; <span style='color:#E3B341'>%1</span>").arg(note_);
        const QString list = QStringLiteral(" &nbsp; <span style='color:%1'>watchlist <b>%2</b> (Ctrl+S)</span>")
                                 .arg(muted, list_name_.toHtmlEscaped());
        status_->setText(stream + list + feed + uni + note);
    }

    void start_feed(bool sim) {
        if (feed_ != nullptr) return;
        const QString exe = live_detail::find_helper(QStringLiteral("altair_price_service"));
        if (exe.isEmpty()) {
            note_ = QStringLiteral("altair_price_service is not built (it needs Boost; build the net preset: .\\build.bat net).");
            refresh_status();
            return;
        }
        feed_ = new QProcess(this);
        feed_->setWorkingDirectory(root_);
        feed_->setProcessChannelMode(QProcess::MergedChannels);
        // Everything the feed says goes to data/live/price_service.log (the
        // previous run's to price_service.prev.log), not into memory for the
        // whole day: what it subscribed, what FYERS refused, the 30-second
        // price report. Send that file when prices are missing.
        const QString log = live_path("price_service.log");
        QDir().mkpath(QFileInfo(log).absolutePath());
        QFile::remove(live_path("price_service.prev.log"));
        QFile::rename(log, live_path("price_service.prev.log"));
        feed_->setStandardOutputFile(log, QIODevice::Truncate);
        connect(feed_, &QProcess::finished, this, [this, log](int code, QProcess::ExitStatus) {
            QString out;
            {
                QFile f(log);
                if (f.open(QIODevice::ReadOnly)) {
                    if (f.size() > 8192) f.seek(f.size() - 8192);
                    out = QString::fromLocal8Bit(f.readAll()).trimmed();
                }
            }
            feed_->deleteLater();
            feed_ = nullptr;
            const bool was_sim = sim_running_;
            sim_running_ = false;
            start_sim_->setEnabled(true);
            stop_->setEnabled(false);
            // A live feed that exits with an error is not restarted for five
            // minutes (market_tick): no login, a holiday, a refused session.
            if (!was_sim && code != 0)
                refused_at_ = now_ist ? now_ist() : QDateTime::currentDateTimeUtc().addSecs(19800);
            if (code == 2 && out.contains(QStringLiteral("no live source today"))) {
                note_ = QStringLiteral("No live feed: %1 — log in to FYERS on the Brokers page; it is tried again in 5 minutes.")
                            .arg(out.section(QStringLiteral("no live source today."), 1).simplified().toHtmlEscaped());
            } else if (code != 0) {
                note_ = QStringLiteral("feed exited %1: %2").arg(code).arg(out.section(QChar('\n'), -2).toHtmlEscaped());
            } else {
                note_ = QStringLiteral("feed stopped.");
            }
            refresh_status();
        });
        QStringList args;
        if (sim) {
            args << QStringLiteral("--sim") << QStringLiteral("--speed") << QString::number(sim_speed_->currentData().toInt())
                 << QStringLiteral("--from") << sim_from_->time().toString(QStringLiteral("HH:mm"));
            if (sim_date_->date() != QDate::currentDate())
                args << QStringLiteral("--date") << sim_date_->date().toString(Qt::ISODate);
            note_ = QStringLiteral("SIM %1 from %2 at %3.").arg(sim_date_->date().toString(QStringLiteral("dd-MMM-yyyy")),
                                                                 sim_from_->time().toString(QStringLiteral("HH:mm")),
                                                                 sim_speed_->currentText());
        } else {
            // Until 15:45 IST: the market's day, as the schedule runs it.
            args << QStringLiteral("--live") << QStringLiteral("--go") << QStringLiteral("--until") << QStringLiteral("15:45");
            note_ = auto_started_ ? QStringLiteral("Starting the FYERS live feed by itself (market hours)…")
                                  : QStringLiteral("Starting the FYERS live feed…");
        }
        sim_running_ = sim;
        feed_->start(exe, args);
        start_sim_->setEnabled(false);
        stop_->setEnabled(sim);
        refresh_status();
        // Connect once it has had a moment to bind; the Terminal also retries.
        QTimer::singleShot(1500, this, [this] {
            if (client_ != nullptr && !client_->connected()) client_->start(QStringLiteral("127.0.0.1"), 7421);
        });
    }

    void stop_feed() {
        if (feed_ == nullptr) return;
        feed_->terminate();
        if (!feed_->waitForFinished(3000)) feed_->kill();
    }

    PriceClient* client_;
    QString root_;
    AddScripBar* loader_ = nullptr;
    LiveWatchModel* model_ = nullptr;
    LiveWatchFilter* filter_ = nullptr;
    QTableView* view_ = nullptr;
    QStackedWidget* main_ = nullptr;
    QWidget* side_ = nullptr;
    QLabel* sel_ = nullptr;
    QTableWidget* depth_ = nullptr;
    QTableWidget* tape_ = nullptr;
    LiveChainView* chain_ = nullptr;
    QComboBox* list_ = nullptr;
    QPushButton* save_ = nullptr;
    QPushButton* save_as_ = nullptr;
    QPushButton* delete_list_ = nullptr;
    QLineEdit* search_ = nullptr;
    QPushButton* sim_btn_ = nullptr;
    QDialog* sim_dialog_ = nullptr;
    QDialog* lists_dialog_ = nullptr;
    QSplitter* side_split_ = nullptr;
    QLabel* depth_title_ = nullptr;
    QTimer market_timer_;
    bool autostart_ = false;
    bool sim_running_ = false;
    QDateTime refused_at_;
    QPushButton* start_sim_ = nullptr;
    QPushButton* stop_ = nullptr;
    QDateEdit* sim_date_ = nullptr;
    QTimeEdit* sim_from_ = nullptr;
    QComboBox* sim_speed_ = nullptr;
    QLabel* status_ = nullptr;
    QProcess* feed_ = nullptr;
    QTimer side_timer_, status_timer_;
    std::vector<MasterScrip> master_;
    std::thread master_thread_;
    std::vector<LiveRow> pending_;
    std::vector<std::pair<quint32, QString>> members_;   ///< the active watchlist, in order
    std::vector<std::pair<quint32, QString>> extra_requests_;
    QString list_name_ = QStringLiteral("Default");
    QString note_;
    quint32 selected_ = 0;
    bool side_dirty_ = false;
    bool labelled_ = false;
    bool from_file_ = false;
    bool master_loaded_ = false;
    bool auto_started_ = false;
    qint64 universe_stamp_ = -1;
    int chain_tick_ = 0;
};

} // namespace altair::ui
