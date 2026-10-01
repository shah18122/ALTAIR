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
#include <QComboBox>
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

protected:
    [[nodiscard]] bool filterAcceptsRow(int r, const QModelIndex& parent) const override {
        const QModelIndex i = sourceModel()->index(r, 0, parent);
        if (!group_.isEmpty() && sourceModel()->data(i, LiveWatchModel::GroupRole).toString() != group_) return false;
        return text_.isEmpty() || sourceModel()->data(i, Qt::DisplayRole).toString().contains(text_, Qt::CaseInsensitive);
    }

private:
    QString group_, text_;
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

/// Five levels a side and the totals.
inline void show_live_depth(QTableWidget* t, const LivePrice* p) {
    t->setRowCount(static_cast<int>(kMaxDepthLevels) + 1);
    const QColor bid(0x7F, 0xB8, 0xF0), ask(0xF0, 0xA0, 0x7F);
    qint64 tb = 0, ta = 0;
    for (int k = 0; k < static_cast<int>(kMaxDepthLevels); ++k) {
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
    const int last = static_cast<int>(kMaxDepthLevels);
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
        v->addWidget(grid_, 1);
        connect(under_, &QComboBox::currentIndexChanged, this, [this](int) { refresh(); });
    }

    void set_rows(const std::vector<LiveRow>& rows) { rows_ = rows; refresh(); }
    [[nodiscard]] QTableWidget* grid() const noexcept { return grid_; }

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
    double rate_ = 0.065;
};

// ---------------------------------------------------------------------------
// The page
// ---------------------------------------------------------------------------

class LiveMarketWatch final : public QWidget {
public:
    /// `root`: where data/live/ is (the source tree by default).
    LiveMarketWatch(PriceClient* client, QString root = {}, QWidget* parent = nullptr)
        : QWidget(parent), client_(client), root_(std::move(root)) {
#ifdef ALTAIR_SOURCE_DIR
        if (root_.isEmpty()) root_ = QStringLiteral(ALTAIR_SOURCE_DIR);
#endif
        setObjectName(QStringLiteral("liveMarketWatch"));
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(6, 6, 6, 6);
        v->setSpacing(6);

        auto* bar = new QHBoxLayout;
        group_ = new QComboBox(this);
        group_->addItem(QStringLiteral("All"), QString());
        for (const char* g : {"Indices", "Futures", "NIFTY options", "BANKNIFTY options", "NIFTY 50"})
            group_->addItem(QString::fromLatin1(g), QString::fromLatin1(g));
        search_ = new QLineEdit(this);
        search_->setPlaceholderText(QStringLiteral("Search symbol…"));
        search_->setClearButtonEnabled(true);
        search_->setMaximumWidth(220);
        start_live_ = new QPushButton(QStringLiteral("Start FYERS feed"), this);
        start_live_->setToolTip(QStringLiteral(
            "Run altair_price_service --fyers --go: live ticks, quotes and depth for the indices, near futures, "
            "both option chains (ATM ±20) and the NIFTY 50. Needs a FYERS login for today (altair_fyers_login)."));
        start_sim_ = new QPushButton(QStringLiteral("Start SIM feed"), this);
        start_sim_->setToolTip(QStringLiteral(
            "Run altair_price_service --sim: the same universe, SIMULATED, for when the market is shut. "
            "Every price it shows is marked SIM."));
        stop_ = new QPushButton(QStringLiteral("Stop feed"), this);
        stop_->setEnabled(false);
        bar->addWidget(new QLabel(QStringLiteral("Watch"), this));
        bar->addWidget(group_);
        bar->addWidget(search_);
        bar->addStretch();
        bar->addWidget(start_live_);
        bar->addWidget(start_sim_);
        bar->addWidget(stop_);
        v->addLayout(bar);

        status_ = new QLabel(this);
        status_->setTextFormat(Qt::RichText);
        status_->setWordWrap(true);
        v->addWidget(status_);

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
        view_->verticalHeader()->hide();
        view_->verticalHeader()->setDefaultSectionSize(21);
        view_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        view_->horizontalHeader()->setSectionsMovable(true);
        view_->horizontalHeader()->setDefaultSectionSize(78);
        view_->setColumnWidth(LiveWatchModel::Symbol, 168);
        view_->setColumnWidth(LiveWatchModel::Ltp, 104);   // room for the price and its arrow
        view_->setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);

        side_ = new QTabWidget(this);
        auto* dt = new QWidget(side_);
        auto* dv = new QVBoxLayout(dt);
        dv->setContentsMargins(0, 4, 0, 0);
        sel_ = new QLabel(QStringLiteral("Select a row."), dt);
        sel_->setTextFormat(Qt::RichText);
        sel_->setWordWrap(true);
        dv->addWidget(sel_);
        depth_ = make_live_table(6, {QStringLiteral("Orders"), QStringLiteral("Bid Qty"), QStringLiteral("Bid"),
                                     QStringLiteral("Ask"), QStringLiteral("Ask Qty"), QStringLiteral("Orders")}, dt);
        depth_->setMaximumHeight(160);
        dv->addWidget(depth_);
        auto* tape_title = new QLabel(QStringLiteral("TIME & SALES — every trade, newest first"), dt);
        tape_title->setObjectName(QStringLiteral("sectionKicker"));
        dv->addWidget(tape_title);
        tape_ = make_live_table(3, {QStringLiteral("Time (IST)"), QStringLiteral("Price"), QStringLiteral("Qty")}, dt);
        dv->addWidget(tape_, 1);
        side_->addTab(dt, QStringLiteral("Depth && trades"));
        chain_ = new LiveChainView(client_, side_);
        side_->addTab(chain_, QStringLiteral("Option chain"));

        auto* split = new QSplitter(Qt::Horizontal, this);
        split->addWidget(view_);
        split->addWidget(side_);
        split->setStretchFactor(0, 62);
        split->setStretchFactor(1, 38);
        v->addWidget(split, 1);

        connect(group_, &QComboBox::currentIndexChanged, this,
                [this](int) { filter_->set_group(group_->currentData().toString()); });
        connect(search_, &QLineEdit::textChanged, this, [this](const QString& t) { filter_->set_text(t); });
        connect(view_->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
                [this](const QModelIndex& cur, const QModelIndex&) {
                    const QModelIndex src = filter_->mapToSource(cur);
                    selected_ = src.isValid() ? model_->rows()[static_cast<std::size_t>(src.row())].token : 0;
                    refresh_side();
                });
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
            if (++chain_tick_ % 2 == 0 && side_->currentWidget() == chain_ && isVisible()) chain_->refresh();
        });
        side_timer_.start();
        status_timer_.setInterval(2000);
        connect(&status_timer_, &QTimer::timeout, this, [this] { reload_universe(false); refresh_status(); });
        status_timer_.start();
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
    [[nodiscard]] QString status_text() const { return status_->text(); }
    void select_token(quint32 token) {
        const int r = model_->row_of(token);
        if (r < 0) return;
        view_->setCurrentIndex(filter_->mapFromSource(model_->index(r, 0)));
    }
    void set_rows_for_test(std::vector<LiveRow> rows) {
        model_->set_rows(rows);
        chain_->set_rows(rows);
    }

private:
    void reload_universe(bool force) {
        const QString path = root_ + QStringLiteral("/data/live/universe.csv");
        const QFileInfo fi(path);
        const qint64 stamp = fi.exists() ? fi.lastModified().toMSecsSinceEpoch() : 0;
        if (!force && stamp == universe_stamp_) return;
        universe_stamp_ = stamp;
        auto rows = load_live_universe(path);
        from_file_ = !rows.empty();
        if (rows.empty()) rows = default_live_universe();
        model_->set_rows(rows);
        chain_->set_rows(rows);
        if (selected_ == 0) {
            // Open on the near NIFTY future: the instrument with a book and a
            // tape, so depth and time and sales show something at once.
            for (const auto& r : rows)
                if (r.kind == QLatin1String("future") && r.underlying == QLatin1String("NIFTY")) { selected_ = r.token; break; }
            if (selected_ == 0 && !rows.empty()) selected_ = rows.front().token;
        }
        select_token(selected_);
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
            stream = QStringLiteral("<span style='color:%1'>● NOT CONNECTED</span> <span style='color:%2'>to the price "
                                    "service on 127.0.0.1:7421 — start a feed here, or run altair_price_service.</span>")
                         .arg(QStringLiteral("#8A93A2"), muted);
        } else {
            const LivePrice* n = client_->price(256265u);
            const bool sim = n != nullptr && n->simulated, rep = n != nullptr && n->replay;
            const QString tag = sim ? QStringLiteral("SIM — simulated, not market data")
                              : rep ? QStringLiteral("REPLAY") : QStringLiteral("LIVE");
            const QString col = sim || rep ? QStringLiteral("#F4C95D") : QStringLiteral("#7FD17F");
            stream = QStringLiteral("<span style='color:%1'>● %2</span> <span style='color:%3'>%4 instruments · "
                                    "%5 frames · %6 gaps (%7 missed) · engine %8 IST</span>")
                         .arg(col, tag, muted).arg(client_->instruments()).arg(client_->frames()).arg(client_->gaps())
                         .arg(client_->missed()).arg(live_detail::ist(n != nullptr ? n->exchange_ts_ns : 0, false));
        }
        QString feed;
        QFile f(root_ + QStringLiteral("/data/live/feed_status.json"));
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
            const qint64 age = QDateTime::currentSecsSinceEpoch() - o.value(QStringLiteral("written_unix")).toInteger();
            feed = QStringLiteral(" &nbsp; <span style='color:%1'>feed: %2 %3%4, %5 trades, %6 quotes, %7 books%8</span>")
                       .arg(muted, o.value(QStringLiteral("source")).toString(), o.value(QStringLiteral("state")).toString())
                       .arg(age > 10 ? QStringLiteral(" (status %1 s old)").arg(age) : QString())
                       .arg(o.value(QStringLiteral("trades")).toInteger()).arg(o.value(QStringLiteral("quotes")).toInteger())
                       .arg(o.value(QStringLiteral("books")).toInteger())
                       .arg(o.value(QStringLiteral("error")).toString().isEmpty()
                                ? QString() : QStringLiteral(" · ") + o.value(QStringLiteral("error")).toString().toHtmlEscaped());
        }
        const QString uni = from_file_ ? QString()
            : QStringLiteral(" &nbsp; <span style='color:%1'>(no data/live/universe.csv yet: indices only until a feed starts)</span>").arg(muted);
        status_->setText(stream + feed + uni);
    }

    void start_feed(bool sim) {
        if (feed_ != nullptr) return;
        const QString exe = live_detail::find_helper(QStringLiteral("altair_price_service"));
        if (exe.isEmpty()) {
            status_->setText(QStringLiteral("<span style='color:#F07A6A'>altair_price_service is not built "
                                            "(it needs Boost; build the net preset).</span>"));
            return;
        }
        feed_ = new QProcess(this);
        feed_->setWorkingDirectory(root_);
        feed_->setProcessChannelMode(QProcess::MergedChannels);
        connect(feed_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
            const QString tail = QString::fromLocal8Bit(feed_->readAll()).trimmed().section(QChar('\n'), -2);
            feed_->deleteLater();
            feed_ = nullptr;
            start_live_->setEnabled(true);
            start_sim_->setEnabled(true);
            stop_->setEnabled(false);
            if (code != 0)
                status_->setText(QStringLiteral("<span style='color:#F07A6A'>feed exited %1: %2</span>")
                                     .arg(code).arg(tail.toHtmlEscaped()));
        });
        const QStringList args = sim ? QStringList{QStringLiteral("--sim")} : QStringList{QStringLiteral("--fyers"), QStringLiteral("--go")};
        feed_->start(exe, args);
        start_live_->setEnabled(false);
        start_sim_->setEnabled(false);
        stop_->setEnabled(true);
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
    QTabWidget* side_ = nullptr;
    QLabel* sel_ = nullptr;
    QTableWidget* depth_ = nullptr;
    QTableWidget* tape_ = nullptr;
    LiveChainView* chain_ = nullptr;
    QComboBox* group_ = nullptr;
    QLineEdit* search_ = nullptr;
    QPushButton* start_live_ = nullptr;
    QPushButton* start_sim_ = nullptr;
    QPushButton* stop_ = nullptr;
    QLabel* status_ = nullptr;
    QProcess* feed_ = nullptr;
    QTimer side_timer_, status_timer_;
    quint32 selected_ = 0;
    bool side_dirty_ = false;
    bool labelled_ = false;
    bool from_file_ = false;
    qint64 universe_stamp_ = -1;
    int chain_tick_ = 0;
};

} // namespace altair::ui
