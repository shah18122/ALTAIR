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
#include "theme.hpp"

#include <analytics/greeks.hpp>
#include <analytics/iv.hpp>

#include <QAbstractTableModel>
#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QDateEdit>
#include <QDir>
#include <QEvent>
#include <QKeyEvent>
#include <QMenu>
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
#include <unordered_map>
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
            if (f.last > 0) {
                f.dir = p->last_paise > f.last ? 1 : -1;
                f.until_ms = QDateTime::currentMSecsSinceEpoch() + kFlashMs;
            }
            f.last = p->last_paise;
        }
        dirty_lo_ = std::min(dirty_lo_, r);
        dirty_hi_ = std::max(dirty_hi_, r);
    }

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
        case Symbol: return row.symbol;
        case Ltp: {
            if (ltp <= 0) return none;
            const Flash& f = flash_[static_cast<std::size_t>(idx.row())];
            const QString arrow = f.dir > 0 ? QStringLiteral(" ▲") : f.dir < 0 ? QStringLiteral(" ▼") : QString();
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

    void flush() {
        // Lit cells that have just gone dark must repaint too.
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        for (std::size_t i = 0; i < flash_.size(); ++i) {
            if (flash_[i].until_ms != 0 && now >= flash_[i].until_ms) {
                flash_[i].until_ms = 0;
                dirty_lo_ = std::min(dirty_lo_, static_cast<int>(i));
                dirty_hi_ = std::max(dirty_hi_, static_cast<int>(i));
            }
        }
        if (dirty_lo_ > dirty_hi_) return;
        Q_EMIT dataChanged(index(dirty_lo_, 0), index(dirty_hi_, ColumnCount - 1));
        dirty_lo_ = std::numeric_limits<int>::max();
        dirty_hi_ = -1;
    }

    const PriceClient* client_;
    std::vector<LiveRow> rows_;
    std::vector<Flash> flash_;
    std::unordered_map<quint32, int> index_;
    QTimer paint_;
    int dirty_lo_ = std::numeric_limits<int>::max();
    int dirty_hi_ = -1;
};

/// Group and text filter over the watch.
class LiveWatchFilter final : public QSortFilterProxyModel {
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;
    void set_group(const QString& g) { group_ = g; invalidateFilter(); }
    void set_text(const QString& t) { text_ = t.trimmed(); invalidateFilter(); }
    /// Scrips removed from the watch (Delete).
    void set_hidden(QSet<quint32> h) { hidden_ = std::move(h); invalidateFilter(); }

protected:
    [[nodiscard]] bool filterAcceptsRow(int r, const QModelIndex& parent) const override {
        const QModelIndex i = sourceModel()->index(r, 0, parent);
        if (!hidden_.isEmpty()
            && hidden_.contains(static_cast<quint32>(sourceModel()->data(i, LiveWatchModel::TokenRole).toULongLong())))
            return false;
        if (!group_.isEmpty() && sourceModel()->data(i, LiveWatchModel::GroupRole).toString() != group_) return false;
        return text_.isEmpty() || sourceModel()->data(i, Qt::DisplayRole).toString().contains(text_, Qt::CaseInsensitive);
    }

private:
    QString group_, text_;
    QSet<quint32> hidden_;
};

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

class LiveChainView final : public QWidget {
public:
    enum Col { COi, CVol, CIv, CDelta, CLtp, CBid, CAsk, Strike, PBid, PAsk, PLtp, PDelta, PIv, PVol, POi, Cols };

    LiveChainView(const PriceClient* client, QWidget* parent = nullptr) : QWidget(parent), client_(client) {
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(0, 4, 0, 0);
        auto* bar = new QHBoxLayout;
        under_ = new QComboBox(this);
        under_->addItems({QStringLiteral("NIFTY"), QStringLiteral("BANKNIFTY")});
        bar->addWidget(new QLabel(QStringLiteral("Underlying"), this));
        bar->addWidget(under_);
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
        grid_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        grid_->horizontalHeader()->setDefaultSectionSize(70);
        // OI and volume in Indian grouping need the room.
        for (int c : {COi, CVol, PVol, POi}) grid_->setColumnWidth(c, 96);
        v->addWidget(grid_, 1);
        grid_->setSelectionMode(QAbstractItemView::SingleSelection);
        grid_->setSelectionBehavior(QAbstractItemView::SelectItems);
        connect(under_, &QComboBox::currentIndexChanged, this, [this](int) { refresh(); });
        // A cell on the call side picks the call, on the put side the put:
        // that is the scrip + / − and the market picture act on.
        connect(grid_, &QTableWidget::currentCellChanged, this, [this](int r, int c, int, int) {
            const quint32 tok = token_at(r, c);
            if (tok != 0 && on_pick) on_pick(tok);
        });
    }

    void set_rows(const std::vector<LiveRow>& rows) { rows_ = rows; refresh(); }
    [[nodiscard]] QTableWidget* grid() const noexcept { return grid_; }
    /// The option under a cell: calls left of the strike, puts right of it.
    [[nodiscard]] quint32 token_at(int r, int c) const {
        if (r < 0 || r >= static_cast<int>(row_tokens_.size()) || c == Strike) return 0;
        return c < Strike ? row_tokens_[static_cast<std::size_t>(r)].first : row_tokens_[static_cast<std::size_t>(r)].second;
    }
    std::function<void(quint32)> on_pick;

    void refresh() {
        const QString under = under_->currentText();
        // The strikes of the nearest expiry this feed streams, and their tokens.
        struct Pair { quint32 ce = 0, pe = 0; };
        std::map<double, Pair> strikes;
        qint64 expiry = 0;
        quint32 fut = 0, spot = under == QLatin1String("NIFTY") ? 256265u : 260105u;
        qint64 fut_expiry = 0;
        for (const auto& r : rows_) {
            if (r.underlying != under) continue;
            if (r.kind == QLatin1String("future") && (fut == 0 || r.expiry_day < fut_expiry)) { fut = r.token; fut_expiry = r.expiry_day; }
            if (r.kind != QLatin1String("call") && r.kind != QLatin1String("put")) continue;
            if (expiry == 0 || r.expiry_day < expiry) expiry = r.expiry_day;
        }
        for (const auto& r : rows_) {
            if (r.underlying != under || r.expiry_day != expiry) continue;
            if (r.kind == QLatin1String("call")) strikes[r.strike].ce = r.token;
            if (r.kind == QLatin1String("put")) strikes[r.strike].pe = r.token;
        }
        const auto ltp = [this](quint32 tok) -> qint64 {
            const LivePrice* p = client_ != nullptr ? client_->price(tok) : nullptr;
            return p != nullptr ? p->last_paise : 0;
        };
        const auto mid = [this](quint32 tok) -> qint64 {
            const LivePrice* p = client_ != nullptr ? client_->price(tok) : nullptr;
            if (p == nullptr) return 0;
            if (p->has_quote && p->quote.has(kQuoteHasTop) && p->quote.bid > 0 && p->quote.ask > p->quote.bid)
                return (p->quote.bid + p->quote.ask) / 2;
            return p->last_paise;
        };
        // THE CLOCK IS THE FEED'S: time to expiry from the underlying's own
        // last stamp, so a replayed or simulated session is valued at its time.
        const LivePrice* sp = client_ != nullptr ? client_->price(spot) : nullptr;
        const std::int64_t now_ns = sp != nullptr && sp->exchange_ts_ns > 0 ? sp->exchange_ts_ns
                                  : QDateTime::currentMSecsSinceEpoch() * 1'000'000LL;
        const double years = expiry > 0
            ? static_cast<double>((expiry * 86400 + 36000) * 1'000'000'000LL - now_ns) / (365.0 * 86400.0 * 1e9) : 0.0;
        const qint64 spot_px = ltp(spot);
        double forward = static_cast<double>(mid(fut));
        QString fwd_from = QStringLiteral("future mid");
        if (!(forward > 0.0) && spot_px > 0 && years > 0.0) {
            forward = static_cast<double>(spot_px) * std::exp(rate_ * years);
            fwd_from = QStringLiteral("spot × e^rT");
        }
        if (fut != 0 && fut_expiry != expiry && forward > 0.0 && spot_px > 0) {
            // The near future expires after the options: carry it back to the
            // options' expiry rather than use a later forward.
            const double tf = static_cast<double>((fut_expiry - expiry) * 86400) / (365.0 * 86400.0);
            forward *= std::exp(-rate_ * tf);
            fwd_from = QStringLiteral("future mid, carried to expiry");
        }
        double atm = 0.0;
        for (const auto& [k, pr] : strikes) if (atm == 0.0 || std::fabs(k - forward / 100.0) < std::fabs(atm - forward / 100.0)) atm = k;
        head_->setText(expiry == 0
            ? QStringLiteral("<span style='color:%1'>No options streamed for %2. Start the feed with the option chain.</span>")
                  .arg(QString::fromLatin1(theme_token::kTextMuted), under)
            : QStringLiteral("<span style='color:%1'>expiry</span> <b>%2</b> &nbsp; <span style='color:%1'>T</span> %3 d"
                             " &nbsp; <span style='color:%1'>forward</span> <b>%4</b> (%5) &nbsp; "
                             "<span style='color:%1'>IV and Δ are inverted from the market mid with Black-76 at %6 %</span>")
                  .arg(QString::fromLatin1(theme_token::kTextMuted), QDate(1970, 1, 1).addDays(expiry).toString(QStringLiteral("dd-MMM-yyyy")))
                  .arg(years * 365.0, 0, 'f', 2).arg(forward / 100.0, 0, 'f', 2).arg(fwd_from).arg(rate_ * 100.0, 0, 'f', 2));
        grid_->setRowCount(static_cast<int>(strikes.size()));
        row_tokens_.clear();
        for (const auto& [k, pr] : strikes) row_tokens_.emplace_back(pr.ce, pr.pe);
        int r = 0;
        const QColor itm(0x2A, 0x26, 0x18), plain(0x15, 0x18, 0x1D), atm_bg(0x3A, 0x32, 0x12);
        const QColor fg(0xD0, 0xD6, 0xDE), strike_fg(0xF4, 0xC9, 0x5D);
        for (const auto& [k, pr] : strikes) {
            const auto side = [&](quint32 tok, bool call, int c_oi, int c_vol, int c_iv, int c_d, int c_ltp, int c_bid, int c_ask) {
                const LivePrice* p = client_ != nullptr ? client_->price(tok) : nullptr;
                const bool top = p != nullptr && p->has_quote && p->quote.has(kQuoteHasTop);
                const LiveIv iv = live_iv(call, mid(tok), forward, k, years, rate_);
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
            side(pr.ce, true, COi, CVol, CIv, CDelta, CLtp, CBid, CAsk);
            live_set(grid_, r, Strike, QString::number(k, 'f', k == std::floor(k) ? 0 : 2), strike_fg);
            if (auto* it = grid_->item(r, Strike)) {
                it->setTextAlignment(Qt::AlignCenter);
                it->setBackground(k == atm ? atm_bg : plain);
            }
            side(pr.pe, false, POi, PVol, PIv, PDelta, PLtp, PBid, PAsk);
            ++r;
        }
    }

private:
    const PriceClient* client_;
    std::vector<LiveRow> rows_;
    QComboBox* under_ = nullptr;
    QLabel* head_ = nullptr;
    QTableWidget* grid_ = nullptr;
    std::vector<std::pair<quint32, quint32>> row_tokens_;
    double rate_ = 0.065;
};

// ---------------------------------------------------------------------------
// The page
// ---------------------------------------------------------------------------

/// One row of the instrument master the scrip search offers.
struct MasterScrip {
    quint32 token = 0;
    QString symbol, exchange, segment, display;
};

/// The NSE equities and NSE F&O contracts of data/instruments.csv (the Kite
/// master), for the scrip search. Read on first use: it is ~9 MB.
[[nodiscard]] inline std::vector<MasterScrip> load_master_scrips(const QString& path) {
    std::vector<MasterScrip> out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return out;
    QTextStream in(&f);
    const auto split = [](const QString& line) {
        QStringList fields;
        QString cur;
        bool quoted = false;
        for (const QChar c : line) {
            if (c == QLatin1Char('"')) { quoted = !quoted; continue; }
            if (c == QLatin1Char(',') && !quoted) { fields << cur; cur.clear(); continue; }
            cur += c;
        }
        fields << cur;
        return fields;
    };
    const QStringList head = split(in.readLine());
    const int c_tok = static_cast<int>(head.indexOf(QStringLiteral("instrument_token")));
    const int c_sym = static_cast<int>(head.indexOf(QStringLiteral("tradingsymbol")));
    const int c_name = static_cast<int>(head.indexOf(QStringLiteral("name")));
    const int c_exp = static_cast<int>(head.indexOf(QStringLiteral("expiry")));
    const int c_type = static_cast<int>(head.indexOf(QStringLiteral("instrument_type")));
    const int c_seg = static_cast<int>(head.indexOf(QStringLiteral("segment")));
    const int c_ex = static_cast<int>(head.indexOf(QStringLiteral("exchange")));
    if (c_tok < 0 || c_sym < 0 || c_seg < 0 || c_ex < 0 || c_type < 0) return out;
    const int need = std::max({c_tok, c_sym, c_seg, c_ex, c_type, c_name, c_exp}) + 1;
    out.reserve(100000);
    while (!in.atEnd()) {
        const QStringList c = split(in.readLine());
        if (c.size() < need) continue;
        const QString ex = c[c_ex], seg = c[c_seg], type = c[c_type];
        const bool eq = ex == QLatin1String("NSE") && seg == QLatin1String("NSE") && type == QLatin1String("EQ");
        const bool fo = ex == QLatin1String("NFO") && (seg == QLatin1String("NFO-FUT") || seg == QLatin1String("NFO-OPT"));
        if (!eq && !fo) continue;
        MasterScrip m;
        m.token = c[c_tok].toUInt();
        if (m.token == 0) continue;
        m.symbol = c[c_sym];
        m.exchange = ex;
        m.segment = seg;
        const QString name = c_name >= 0 ? c[c_name] : QString();
        const QString expiry = c_exp >= 0 ? c[c_exp] : QString();
        m.display = eq ? QStringLiteral("%1  ·  NSE EQ  ·  %2").arg(m.symbol, name)
                       : QStringLiteral("%1  ·  %2  ·  %3").arg(m.symbol, seg == QLatin1String("NFO-FUT") ? QStringLiteral("FUT") : type, expiry);
        out.push_back(std::move(m));
    }
    return out;
}

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

        // ---- row 1: group, find, add a scrip, the live feed --------------
        auto* bar = new QHBoxLayout;
        group_ = new QComboBox(this);
        group_->addItem(QStringLiteral("All"), QString());
        for (const char* g : {"Indices", "Futures", "NIFTY options", "BANKNIFTY options", "NIFTY 50", "Stock futures", "Watchlist"})
            group_->addItem(QString::fromLatin1(g), QString::fromLatin1(g));
        search_ = new QLineEdit(this);
        search_->setObjectName(QStringLiteral("watchFind"));
        search_->setPlaceholderText(QStringLiteral("Find in watch  (Ctrl+F)"));
        search_->setClearButtonEnabled(true);
        search_->setMaximumWidth(200);
        add_ = new QComboBox(this);
        add_->setObjectName(QStringLiteral("scripSearch"));
        add_->setEditable(true);
        add_->setInsertPolicy(QComboBox::NoInsert);
        add_->setMinimumWidth(320);
        add_->lineEdit()->setPlaceholderText(QStringLiteral("＋ Add scrip: type a symbol, e.g. SBIN or NIFTY26OCT  (Insert)"));
        add_->setToolTip(QStringLiteral("Search NSE equities and NSE F&O from the instrument master; pick one to add it to the "
                                        "watch. The feed starts streaming it within a few seconds."));
        start_live_ = new QPushButton(QStringLiteral("▶ Start live feed"), this);
        start_live_->setObjectName(QStringLiteral("startLiveFeed"));
        start_live_->setToolTip(QStringLiteral(
            "Run altair_price_service --live --go: FYERS when its session is good today, else Kite. Live ticks, quotes and "
            "depth for the indices, near futures, both option chains (ATM ±20), the NIFTY 50 and your added scrips."));
        stop_ = new QPushButton(QStringLiteral("■ Stop"), this);
        stop_->setEnabled(false);
        auto_start_ = new QCheckBox(QStringLiteral("Live on open"), this);
        auto_start_->setToolTip(QStringLiteral("Start the live feed by itself when the Terminal opens and nothing is streaming"));
        bar->addWidget(group_);
        bar->addWidget(search_);
        bar->addWidget(add_, 1);
        bar->addWidget(start_live_);
        bar->addWidget(stop_);
        bar->addWidget(auto_start_);
        v->addLayout(bar);

        // ---- row 2: status, and the simulator for a chosen day ------------
        auto* bar2 = new QHBoxLayout;
        status_ = new QLabel(this);
        status_->setTextFormat(Qt::RichText);
        status_->setWordWrap(true);
        bar2->addWidget(status_, 1);
        sim_date_ = new QDateEdit(QDate::currentDate(), this);
        sim_date_->setObjectName(QStringLiteral("simDate"));
        sim_date_->setCalendarPopup(true);
        sim_date_->setDisplayFormat(QStringLiteral("dd-MMM-yyyy"));
        sim_date_->setMaximumDate(QDate::currentDate());
        sim_date_->setToolTip(QStringLiteral(
            "The day to simulate. The previous closes are the session before it; when dataset/ has that day's "
            "1-minute bars, NIFTY, BANKNIFTY and INDIA VIX follow them minute by minute."));
        sim_from_ = new QTimeEdit(QTime(9, 15), this);
        sim_from_->setDisplayFormat(QStringLiteral("HH:mm"));
        sim_from_->setTimeRange(QTime(9, 15), QTime(15, 29));
        sim_speed_ = new QComboBox(this);
        for (int x : {1, 5, 10, 30, 60, 120, 300})
            sim_speed_->addItem(QStringLiteral("%1×").arg(x), x);
        start_sim_ = new QPushButton(QStringLiteral("Start SIM"), this);
        start_sim_->setObjectName(QStringLiteral("startSimFeed"));
        start_sim_->setToolTip(QStringLiteral("Run altair_price_service --sim for the chosen day, time and speed. Every price "
                                              "it shows is marked SIM."));
        bar2->addWidget(new QLabel(QStringLiteral("SIM"), this));
        bar2->addWidget(sim_date_);
        bar2->addWidget(new QLabel(QStringLiteral("from"), this));
        bar2->addWidget(sim_from_);
        bar2->addWidget(sim_speed_);
        bar2->addWidget(start_sim_);
        v->addLayout(bar2);

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
        view_->verticalHeader()->hide();
        view_->verticalHeader()->setDefaultSectionSize(21);
        view_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        view_->horizontalHeader()->setSectionsMovable(true);
        view_->horizontalHeader()->setDefaultSectionSize(78);
        view_->setColumnWidth(LiveWatchModel::Symbol, 168);
        view_->setColumnWidth(LiveWatchModel::Ltp, 104);   // room for the price and its arrow
        view_->setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
        chain_ = new LiveChainView(client_, this);
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
        auto* depth_title = new QLabel(QStringLiteral("MARKET DEPTH — best five"), side_);
        depth_title->setObjectName(QStringLiteral("sectionKicker"));
        dv->addWidget(depth_title);
        depth_ = make_live_table(6, {QStringLiteral("Orders"), QStringLiteral("Bid Qty"), QStringLiteral("Bid"),
                                     QStringLiteral("Ask"), QStringLiteral("Ask Qty"), QStringLiteral("Orders")}, side_);
        depth_->setMaximumHeight(160);
        dv->addWidget(depth_);
        auto* tape_title = new QLabel(QStringLiteral("TIME & SALES — every trade, newest first"), side_);
        tape_title->setObjectName(QStringLiteral("sectionKicker"));
        dv->addWidget(tape_title);
        tape_ = make_live_table(3, {QStringLiteral("Time (IST)"), QStringLiteral("Price"), QStringLiteral("Qty")}, side_);
        dv->addWidget(tape_, 1);

        auto* split = new QSplitter(Qt::Horizontal, this);
        split->addWidget(main_);
        split->addWidget(side_);
        split->setStretchFactor(0, 68);
        split->setStretchFactor(1, 32);
        v->addWidget(split, 1);

        connect(group_, &QComboBox::currentIndexChanged, this,
                [this](int) { filter_->set_group(group_->currentData().toString()); });
        connect(search_, &QLineEdit::textChanged, this, [this](const QString& t) { filter_->set_text(t); });
        connect(view_->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
                [this](const QModelIndex& cur, const QModelIndex&) {
                    const QModelIndex src = filter_->mapToSource(cur);
                    if (src.isValid()) pick(model_->rows()[static_cast<std::size_t>(src.row())].token);
                });
        chain_->on_pick = [this](quint32 tok) { pick(tok); };
        connect(view_, &QTableView::customContextMenuRequested, this, [this](const QPoint& at) { context_menu(at); });
        view_->installEventFilter(this);
        chain_->grid()->installEventFilter(this);
        add_->installEventFilter(this);
        connect(add_, &QComboBox::activated, this, [this](int) { add_from_box(); });
        connect(add_->lineEdit(), &QLineEdit::returnPressed, this, [this] { add_from_box(); });
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
        connect(start_live_, &QPushButton::clicked, this, [this] { start_feed(false); });
        connect(start_sim_, &QPushButton::clicked, this, [this] { start_feed(true); });
        connect(stop_, &QPushButton::clicked, this, [this] { stop_feed(); });

        side_timer_.setInterval(250);
        connect(&side_timer_, &QTimer::timeout, this, [this] {
            if (side_dirty_) { side_dirty_ = false; refresh_side(); }
            if (++chain_tick_ % 2 == 0 && main_->currentWidget() == chain_ && isVisible()) chain_->refresh();
        });
        side_timer_.start();
        status_timer_.setInterval(2000);
        connect(&status_timer_, &QTimer::timeout, this, [this] { reload_universe(false); refresh_status(); });
        status_timer_.start();
        load_hidden();
        reload_universe(true);
        refresh_status();
    }

    ~LiveMarketWatch() override { stop_feed(); }
    LiveMarketWatch(const LiveMarketWatch&) = delete;
    LiveMarketWatch& operator=(const LiveMarketWatch&) = delete;

    /// For tests and the Terminal's hotkeys.
    [[nodiscard]] LiveWatchModel* model() const noexcept { return model_; }
    [[nodiscard]] QTableView* view() const noexcept { return view_; }
    [[nodiscard]] LiveChainView* chain() const noexcept { return chain_; }
    [[nodiscard]] QTableWidget* tape() const noexcept { return tape_; }
    [[nodiscard]] QTableWidget* depth() const noexcept { return depth_; }
    [[nodiscard]] QComboBox* scrip_search() const noexcept { return add_; }
    [[nodiscard]] QString status_text() const { return status_->text(); }
    [[nodiscard]] quint32 selected_token() const noexcept { return selected_; }
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
    void focus_find() { search_->setFocus(); search_->selectAll(); }
    void focus_add() {
        ensure_master();
        add_->setFocus();
        add_->lineEdit()->selectAll();
    }

    void set_rows_for_test(std::vector<LiveRow> rows) {
        model_->set_rows(rows);
        chain_->set_rows(rows);
    }
    /// Tests: use this master instead of reading data/instruments.csv.
    void set_master_for_test(std::vector<MasterScrip> m) { install_master(std::move(m)); }

    /// Add a scrip to the watch by Kite token: written to
    /// data/live/watchlist.csv, which the price service streams. False when
    /// the master does not know it.
    bool add_scrip(quint32 token) {
        ensure_master();
        const auto it = std::find_if(master_.begin(), master_.end(), [token](const MasterScrip& m) { return m.token == token; });
        if (it == master_.end()) return false;
        hidden_.remove(token);
        save_hidden();
        filter_->set_hidden(hidden_);
        QStringList lines = read_watch_file();
        const QString tok = QString::number(token);
        bool have = false;
        for (const QString& l : lines) have = have || l.section(QLatin1Char(','), 0, 0) == tok;
        if (!have) lines << QStringLiteral("%1,%2").arg(tok, it->symbol);
        write_watch_file(lines);
        if (model_->row_of(token) < 0) {
            LiveRow r;
            r.token = token;
            r.symbol = it->symbol;
            r.fyers = it->exchange == QLatin1String("NSE") ? QStringLiteral("NSE:%1-EQ").arg(it->symbol)
                                                           : QStringLiteral("NSE:%1").arg(it->symbol);
            r.group = QStringLiteral("Watchlist");
            r.kind = it->segment == QLatin1String("NSE") ? QStringLiteral("equity")
                   : it->segment == QLatin1String("NFO-FUT") ? QStringLiteral("future") : QStringLiteral("option");
            pending_.push_back(r);
            auto rows = model_->rows();
            rows.push_back(r);
            model_->set_rows(rows);
        }
        select_token(token);
        note_ = QStringLiteral("Added %1. %2").arg(it->symbol,
            feed_ != nullptr || (client_ != nullptr && client_->connected())
                ? QStringLiteral("The feed picks it up in a few seconds.")
                : QStringLiteral("It streams once a feed is running."));
        refresh_status();
        return true;
    }

    /// Remove a scrip from the watch (Delete). It stays out until added again.
    void remove_scrip(quint32 token) {
        if (token == 0) return;
        hidden_.insert(token);
        save_hidden();
        QStringList lines = read_watch_file();
        lines.erase(std::remove_if(lines.begin(), lines.end(), [token](const QString& l) {
                        return l.section(QLatin1Char(','), 0, 0) == QString::number(token);
                    }), lines.end());
        write_watch_file(lines);
        filter_->set_hidden(hidden_);
        const LiveRow* r = row_of_token(token);
        note_ = QStringLiteral("Removed %1 from the watch.").arg(r != nullptr ? r->symbol : QString::number(token));
        refresh_status();
    }
    void restore_removed() {
        hidden_.clear();
        save_hidden();
        filter_->set_hidden(hidden_);
    }

    /// Start the live feed by itself (once) when nothing is streaming a few
    /// seconds after the Terminal opens.
    void set_autostart(bool on) {
        auto_start_->setChecked(on);
        if (!on || autostart_armed_) return;
        autostart_armed_ = true;
        QTimer::singleShot(3500, this, [this] {
            if (auto_start_->isChecked() && feed_ == nullptr && (client_ == nullptr || !client_->connected())) {
                auto_started_ = true;
                start_feed(false);
            }
        });
    }
    [[nodiscard]] QCheckBox* autostart_box() const noexcept { return auto_start_; }

    /// + and − on the watch or the chain.
    std::function<void(bool buy)> on_order_key;
    std::function<void(View)> on_view_changed;

protected:
    bool eventFilter(QObject* obj, QEvent* e) override {
        if (e->type() == QEvent::KeyPress) {
            const auto* k = static_cast<QKeyEvent*>(e);
            if (obj == add_) {
                if (k->key() == Qt::Key_Escape) { add_->clearEditText(); view_->setFocus(); return true; }
                return false;
            }
            if (k->key() == Qt::Key_Plus || (k->key() == Qt::Key_Equal && (k->modifiers() & Qt::ShiftModifier))) {
                if (on_order_key) on_order_key(true);
                return true;
            }
            if (k->key() == Qt::Key_Minus || k->key() == Qt::Key_Underscore) {
                if (on_order_key) on_order_key(false);
                return true;
            }
            if (obj == view_ && k->key() == Qt::Key_Delete) { remove_scrip(selected_); return true; }
            if (k->key() == Qt::Key_Insert) { focus_add(); return true; }
        }
        if (e->type() == QEvent::FocusIn && obj == add_) ensure_master();
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
        auto* buy = menu.addAction(QStringLiteral("Buy  (+ / F1)"));
        auto* sell = menu.addAction(QStringLiteral("Sell  (− / F2)"));
        menu.addSeparator();
        auto* add = menu.addAction(QStringLiteral("Add scrip…  (Insert)"));
        auto* remove = menu.addAction(QStringLiteral("Remove scrip  (Delete)"));
        auto* restore = menu.addAction(QStringLiteral("Restore removed scrips (%1)").arg(hidden_.size()));
        restore->setEnabled(!hidden_.isEmpty());
        QAction* chosen = menu.exec(view_->viewport()->mapToGlobal(at));
        if (chosen == buy && on_order_key) on_order_key(true);
        else if (chosen == sell && on_order_key) on_order_key(false);
        else if (chosen == add) focus_add();
        else if (chosen == remove) remove_scrip(selected_);
        else if (chosen == restore) restore_removed();
    }

    // ---- the scrip search -------------------------------------------------
    void ensure_master() {
        if (master_loaded_) return;
        master_loaded_ = true;
        install_master(load_master_scrips(root_ + QStringLiteral("/data/instruments.csv")));
        if (master_.empty()) {
            note_ = QStringLiteral("No instrument master at data/instruments.csv: run altair_kite_update (or the "
                                   "fetch scripts) to get it, then scrips can be added.");
            refresh_status();
        }
    }
    void install_master(std::vector<MasterScrip> m) {
        master_loaded_ = true;
        master_ = std::move(m);
        QStringList names;
        names.reserve(static_cast<qsizetype>(master_.size()));
        for (const auto& x : master_) names << x.display;
        auto* list = new QStringListModel(names, add_);
        auto* completer = new QCompleter(list, add_);
        completer->setCaseSensitivity(Qt::CaseInsensitive);
        completer->setFilterMode(Qt::MatchContains);
        completer->setMaxVisibleItems(14);
        completer->setCompletionMode(QCompleter::PopupCompletion);
        add_->setCompleter(completer);
        connect(completer, qOverload<const QString&>(&QCompleter::activated), this,
                [this](const QString& text) { add_by_display(text); });
    }
    void add_by_display(const QString& text) {
        const QString t = text.trimmed();
        if (t.isEmpty()) return;
        const QString sym = t.section(QStringLiteral("  ·  "), 0, 0).trimmed();
        for (const auto& m : master_)
            if (m.display == t || m.symbol.compare(sym, Qt::CaseInsensitive) == 0) {
                add_scrip(m.token);
                add_->clearEditText();
                view_->setFocus();
                return;
            }
        note_ = QStringLiteral("No NSE equity or NSE F&O scrip called %1 in the master.").arg(t.toHtmlEscaped());
        refresh_status();
    }
    void add_from_box() { ensure_master(); add_by_display(add_->currentText()); }

    // ---- watchlist and removed-scrip files ----------------------------------
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
    void load_hidden() {
        hidden_.clear();
        QFile f(live_path("watch_removed.csv"));
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream in(&f);
            while (!in.atEnd()) {
                const quint32 t = in.readLine().section(QLatin1Char(','), 0, 0).toUInt();
                if (t != 0) hidden_.insert(t);
            }
        }
        filter_->set_hidden(hidden_);
    }
    void save_hidden() const {
        QDir().mkpath(root_ + QStringLiteral("/data/live"));
        QFile f(live_path("watch_removed.csv"));
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return;
        QTextStream out(&f);
        for (quint32 t : hidden_) out << t << '\n';
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
        model_->set_rows(rows);
        chain_->set_rows(rows);
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
                                               : QStringLiteral("no feed running — Start live feed, or Start SIM for a chosen day."));
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
        }
        const QString uni = from_file_ ? QString()
            : QStringLiteral(" &nbsp; <span style='color:%1'>(indices only until a feed writes data/live/universe.csv)</span>").arg(muted);
        const QString note = note_.isEmpty() ? QString()
            : QStringLiteral("<br><span style='color:#E3B341'>%1</span>").arg(note_);
        status_->setText(stream + feed + uni + note);
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
        connect(feed_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
            const QString out = QString::fromLocal8Bit(feed_->readAll()).trimmed();
            feed_->deleteLater();
            feed_ = nullptr;
            start_live_->setEnabled(true);
            start_sim_->setEnabled(true);
            stop_->setEnabled(false);
            if (code == 2 && out.contains(QStringLiteral("no live source today"))) {
                note_ = QStringLiteral("No live feed: %1 — log in on the Brokers page, or Start SIM.")
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
            args << QStringLiteral("--live") << QStringLiteral("--go");
            note_ = auto_started_ ? QStringLiteral("Starting the live feed by itself (FYERS, else Kite)…")
                                  : QStringLiteral("Starting the live feed (FYERS, else Kite)…");
        }
        feed_->start(exe, args);
        start_live_->setEnabled(false);
        start_sim_->setEnabled(false);
        stop_->setEnabled(true);
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
    LiveWatchModel* model_ = nullptr;
    LiveWatchFilter* filter_ = nullptr;
    QTableView* view_ = nullptr;
    QStackedWidget* main_ = nullptr;
    QWidget* side_ = nullptr;
    QLabel* sel_ = nullptr;
    QTableWidget* depth_ = nullptr;
    QTableWidget* tape_ = nullptr;
    LiveChainView* chain_ = nullptr;
    QComboBox* group_ = nullptr;
    QLineEdit* search_ = nullptr;
    QComboBox* add_ = nullptr;
    QPushButton* start_live_ = nullptr;
    QPushButton* start_sim_ = nullptr;
    QPushButton* stop_ = nullptr;
    QCheckBox* auto_start_ = nullptr;
    QDateEdit* sim_date_ = nullptr;
    QTimeEdit* sim_from_ = nullptr;
    QComboBox* sim_speed_ = nullptr;
    QLabel* status_ = nullptr;
    QProcess* feed_ = nullptr;
    QTimer side_timer_, status_timer_;
    std::vector<MasterScrip> master_;
    std::vector<LiveRow> pending_;
    QSet<quint32> hidden_;
    QString note_;
    quint32 selected_ = 0;
    bool side_dirty_ = false;
    bool labelled_ = false;
    bool from_file_ = false;
    bool master_loaded_ = false;
    bool autostart_armed_ = false;
    bool auto_started_ = false;
    qint64 universe_stamp_ = -1;
    int chain_tick_ = 0;
};

} // namespace altair::ui
