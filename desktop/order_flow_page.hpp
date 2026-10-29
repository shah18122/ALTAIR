// desktop/order_flow_page.hpp -- the order book, read as flow: one instrument,
// every book and every print, as book/flow.hpp reads them.
//
// WHAT IS ON IT
//   * Any instrument the feed streams (data/live/universe.csv): an underlying,
//     then its future or any strike's call or put, or a search over all of
//     them. A box asks the feed for that instrument's 50-level book (the TBT
//     slots, data/live/obi_watch.csv -- the same list Market watch's "Watch
//     for OBI" writes); without it the feed sends five levels.
//   * The ladder: up to 50 levels a side, size bars, the running total a
//     market order would sweep, the change at each level since the last book,
//     and the touch, mid and microprice in the spread row.
//   * The readings: OBI at the touch, 5 levels and the deep book (plain and
//     weighted), OFI, the trade imbalance, momentum and the score S, each as a
//     bar from -1 to +1 with the +/-threshold marked; persistence; the
//     absorption and withdrawal flags; executed, cancelled, added and hidden
//     size over the window and since the page opened the instrument.
//   * Two charts over the last minutes: bid, ask, mid and microprice; OBI5,
//     OFI and S against the threshold.
//   * The flow tape: every execution, cancel and hidden print as the engine
//     classified it, newest first (adds on request -- there are many).
//   * The OBI trader (live/obi_trader.hpp, paper): what each of its models is
//     doing and why, its open positions and every round trip with reasons
//     (desktop/strategy_record.hpp).
//
// EVERY FRAME, NOT EVERY BURST. The grid's priceUpdated is one notice per
// burst; an order-flow engine diffs consecutive books and must see each one,
// or an execution reads as a cancel. The page asks PriceClient::watch_frames
// for its one instrument and runs its own flow::FlowEngine on frameApplied,
// on the GUI thread, synchronously -- so there is no queue, no lock and
// nothing shared. Painting is coalesced to 10 Hz; the engine is not.
//
// The engine here and the trader's engines (in altair_live_engine) read the
// same frames with the same code, so the numbers agree; they are separate
// instances, so this page's start-of-day is when it opened the instrument.
//
// IT CANNOT TRADE. It reads the bus and the engine's files; it links no
// broker and no OMS.

#pragma once

#include "live_market.hpp"
#include "price_client.hpp"
#include "strategy_record.hpp"
#include "theme.hpp"

#include <book/flow.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QSaveFile>
#include <QScrollArea>
#include <QSplitter>
#include <QTableWidget>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <vector>

namespace altair::ui {

namespace order_flow_detail {

/// One point of the charts.
struct FlowSample {
    qint64 ns = 0;
    double bid = 0.0, ask = 0.0, mid = 0.0, micro = 0.0;   ///< rupees
    double obi5 = 0.0, ofi = 0.0, score = 0.0;
};

/// One line of the flow tape: what the engine classified in one update.
struct FlowEvent {
    qint64 ns = 0;
    int kind = 0;   ///< FlowEventKind
    qint64 qty = 0;
    qint64 bid = 0, ask = 0;   ///< the touch after it, paise
};

enum FlowEventKind : int {
    BuyExec, SellExec, UnknownExec, AskTaken, BidTaken, AskCancel, BidCancel, AskAdd, BidAdd, HiddenBuy, HiddenSell, kKinds
};

[[nodiscard]] inline QString event_name(int k) {
    switch (k) {
    case BuyExec: return QStringLiteral("Buy executed");
    case SellExec: return QStringLiteral("Sell executed");
    case UnknownExec: return QStringLiteral("Print, side unknown");
    case AskTaken: return QStringLiteral("Ask taken by buyers");
    case BidTaken: return QStringLiteral("Bid taken by sellers");
    case AskCancel: return QStringLiteral("Ask cancelled");
    case BidCancel: return QStringLiteral("Bid cancelled");
    case AskAdd: return QStringLiteral("Ask added");
    case BidAdd: return QStringLiteral("Bid added");
    case HiddenBuy: return QStringLiteral("Hidden buy (no ask drop)");
    case HiddenSell: return QStringLiteral("Hidden sell (no bid drop)");
    default: return QStringLiteral("?");
    }
}

/// +1 reads as buying pressure, -1 as selling, 0 neither (adds, unknown).
[[nodiscard]] inline int event_lean(int k) {
    switch (k) {
    case BuyExec: case AskTaken: case HiddenBuy: case AskCancel: return 1;
    case SellExec: case BidTaken: case HiddenSell: case BidCancel: return -1;
    default: return 0;
    }
}

/// The events between two snapshots: what each session total grew by.
/// Totals never shrink between resets, so a negative difference means a
/// reset happened in between and nothing is reported for it.
[[nodiscard]] inline std::vector<FlowEvent> events_between(const flow::FlowTotals& a, const flow::FlowTotals& b, qint64 ns,
                                                         qint64 bid, qint64 ask) {
    std::vector<FlowEvent> out;
    const std::int64_t d[kKinds] = {b.buy - a.buy, b.sell - a.sell, b.unknown - a.unknown,
                                    b.ask_exec_drop - a.ask_exec_drop, b.bid_exec_drop - a.bid_exec_drop,
                                    b.ask_cancel - a.ask_cancel, b.bid_cancel - a.bid_cancel,
                                    b.ask_add - a.ask_add, b.bid_add - a.bid_add,
                                    b.hidden_buy - a.hidden_buy, b.hidden_sell - a.hidden_sell};
    for (int k = 0; k < kKinds; ++k)
        if (d[k] > 0) out.push_back(FlowEvent{ns, k, d[k], bid, ask});
    return out;
}

/// data/live/obi_watch.csv (live/universe.hpp read_obi_watch): one FYERS
/// symbol a line; # comments; text after a comma ignored.
[[nodiscard]] inline QStringList read_watch(const QString& root) {
    QStringList out;
    QFile f(root + QStringLiteral("/data/live/obi_watch.csv"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return out;
    QTextStream in(&f);
    while (!in.atEnd()) {
        const QString line = in.readLine().section(QLatin1Char(','), 0, 0).trimmed();
        if (!line.isEmpty() && !line.startsWith(QLatin1Char('#'))) out << line;
    }
    return out;
}

/// Adds or removes one symbol; written whole, atomically. False if it could not be written.
inline bool set_watch(const QString& root, const QString& fyers, bool on) {
    QStringList list = read_watch(root);
    list.removeAll(fyers);
    if (on) list << fyers;
    QDir().mkpath(root + QStringLiteral("/data/live"));
    QSaveFile f(root + QStringLiteral("/data/live/obi_watch.csv"));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    QTextStream out(&f);
    out << "# instruments the OBI trader watches (Market watch: right-click, Watch for OBI; or the Order Flow page)\n";
    for (const auto& s : list) out << QString(s).remove(QLatin1Char(',')) << '\n';
    out.flush();
    return f.commit();
}

[[nodiscard]] inline QString rupees(double paise, int dp = 2) { return QString::number(paise / 100.0, 'f', dp); }
[[nodiscard]] inline QString units(qint64 q) { return QLocale(QLocale::English, QLocale::India).toString(q); }
[[nodiscard]] inline QString signed2(double v) { return (v >= 0 ? QStringLiteral("+") : QString()) + QString::number(v, 'f', 2); }

[[nodiscard]] inline QColor tint(const char* c, int alpha) {
    QColor q(QString::fromLatin1(c));
    q.setAlpha(alpha);
    return q;
}

/// The order a contract is listed in: futures by expiry, then options by
/// expiry, strike, call before put.
[[nodiscard]] inline bool contract_before(const LiveRow& a, const LiveRow& b) {
    const bool fa = a.kind == QLatin1String("future"), fb = b.kind == QLatin1String("future");
    if (fa != fb) return fa;
    if (a.expiry_day != b.expiry_day) return a.expiry_day < b.expiry_day;
    if (a.strike != b.strike) return a.strike < b.strike;
    return a.kind == QLatin1String("call") && b.kind != QLatin1String("call");
}

[[nodiscard]] inline QString contract_label(const LiveRow& r) {
    const QString exp = r.expiry_day > 0 ? QDate(1970, 1, 1).addDays(r.expiry_day).toString(QStringLiteral("dd MMM")) : QString();
    if (r.kind == QLatin1String("future")) return QStringLiteral("FUT  ") + exp;
    if (r.kind == QLatin1String("call") || r.kind == QLatin1String("put"))
        return QStringLiteral("%1 %2  %3").arg(QString::number(r.strike, 'f', r.strike == std::floor(r.strike) ? 0 : 1),
                                               r.kind == QLatin1String("call") ? QStringLiteral("CE") : QStringLiteral("PE"), exp);
    return r.symbol;
}

} // namespace order_flow_detail

// ---------------------------------------------------------------------------
// The ladder
// ---------------------------------------------------------------------------

/// Asks worst-to-best above the spread row, bids best-to-worst below it.
/// Each level: orders, size with a bar, the running total from the touch,
/// and the change in size since the previous book (shown for 1.5 s).
class FlowLadder final : public QWidget {
public:
    explicit FlowLadder(QWidget* parent = nullptr) : QWidget(parent) {
        setObjectName(QStringLiteral("flowLadder"));
        setMinimumWidth(360);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    /// The book to draw, `rows` levels a side at most. Null: never delivered.
    void set_book(const LivePrice* p, int rows, double micro_paise, qint64 now_ms) {
        never_ = p == nullptr || (p->levels == 0 && !p->has_quote);
        rows_ = std::max(1, rows);
        bids_.clear(); asks_.clear();
        if (p != nullptr && p->levels > 0) {
            for (int i = 0; i < p->levels; ++i) {
                if (p->bids[i].price_paise > 0 && p->bids[i].qty > 0) bids_.push_back(p->bids[i]);
                if (p->asks[i].price_paise > 0 && p->asks[i].qty > 0) asks_.push_back(p->asks[i]);
            }
        } else if (p != nullptr && p->has_quote && p->quote.bid > 0 && p->quote.ask > 0) {
            PriceLevel b{}, a{};
            b.price_paise = p->quote.bid; b.qty = p->quote.bid_qty;
            a.price_paise = p->quote.ask; a.qty = p->quote.ask_qty;
            bids_.push_back(b); asks_.push_back(a);
        }
        levels_ = p != nullptr ? p->levels : 0;
        micro_ = micro_paise;
        // The change at each price since the last book, kept for 1.5 s.
        const auto mark = [&](const std::vector<PriceLevel>& side, std::map<qint64, qint64>& last, std::map<qint64, Change>& ch) {
            std::map<qint64, qint64> now;
            for (const auto& l : side) now[l.price_paise] = l.qty;
            if (!last.empty()) {
                for (const auto& [px, q] : now) {
                    const auto it = last.find(px);
                    const qint64 d = q - (it == last.end() ? 0 : it->second);
                    if (d != 0) ch[px] = Change{d, now_ms};
                }
                for (const auto& [px, q] : last)
                    if (now.count(px) == 0 && px >= (side.empty() ? 0 : std::min(side.front().price_paise, side.back().price_paise))
                        && px <= (side.empty() ? 0 : std::max(side.front().price_paise, side.back().price_paise)))
                        ch[px] = Change{-q, now_ms};
            }
            for (auto it = ch.begin(); it != ch.end();) it = now_ms - it->second.ms > 1500 ? ch.erase(it) : std::next(it);
            last = std::move(now);
        };
        mark(bids_, last_bid_, bid_change_);
        mark(asks_, last_ask_, ask_change_);
        const int shown = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(rows_), std::max(bids_.size(), asks_.size())));
        setFixedHeight(kHead + (2 * std::max(shown, 1) + 1) * kRow + 4);
        update();
    }
    void clear_history() { last_bid_.clear(); last_ask_.clear(); bid_change_.clear(); ask_change_.clear(); }
    [[nodiscard]] int bid_rows() const noexcept { return static_cast<int>(std::min<std::size_t>(bids_.size(), static_cast<std::size_t>(rows_))); }
    [[nodiscard]] int ask_rows() const noexcept { return static_cast<int>(std::min<std::size_t>(asks_.size(), static_cast<std::size_t>(rows_))); }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter g(this);
        g.fillRect(rect(), QColor(theme_token::kCanvas));
        QFont f = font();
        f.setStyleHint(QFont::Monospace);
        f.setFamily(QStringLiteral("Consolas"));
        f.setPointSizeF(std::max(7.5, f.pointSizeF() - 0.5));
        g.setFont(f);
        const int w = width();
        // Columns: orders | change | bid qty (bar) | price | ask qty (bar) | change | orders | cumulative
        const int c_ord = 34, c_chg = 52, c_cum = 70;
        const int c_px = 84;
        const int c_qty = std::max(60, (w - 2 * c_ord - 2 * c_chg - c_px - c_cum) / 2);
        const int x_bord = 0, x_bchg = x_bord + c_ord, x_bq = x_bchg + c_chg, x_px = x_bq + c_qty, x_aq = x_px + c_px,
                  x_achg = x_aq + c_qty, x_aord = x_achg + c_chg, x_cum = x_aord + c_ord;
        g.setPen(QColor(theme_token::kTextMuted));
        const auto head = [&](int x, int cw, const char* t, int align) {
            g.drawText(QRect(x, 0, cw, kHead), align | Qt::AlignVCenter, QString::fromUtf8(t));
        };
        head(x_bord, c_ord, "ord", Qt::AlignRight);
        head(x_bchg, c_chg, "Δ", Qt::AlignRight);
        head(x_bq, c_qty, "bid size", Qt::AlignRight);
        head(x_px, c_px, "price", Qt::AlignHCenter);
        head(x_aq, c_qty, "ask size", Qt::AlignLeft);
        head(x_achg, c_chg, "Δ", Qt::AlignLeft);
        head(x_aord, c_ord, "ord", Qt::AlignLeft);
        head(x_cum, c_cum, "sweep", Qt::AlignRight);
        if (never_) {
            g.setPen(QColor(theme_token::kTextFaint));
            g.drawText(rect().adjusted(0, kHead, 0, 0), Qt::AlignHCenter | Qt::AlignTop,
                       QStringLiteral("\nNo book yet for this instrument.\nIs the feed streaming it?"));
            return;
        }
        const int na = ask_rows(), nb = bid_rows();
        const int shown = std::max({na, nb, 1});
        qint64 maxq = 1;
        for (int i = 0; i < na; ++i) maxq = std::max<qint64>(maxq, asks_[static_cast<std::size_t>(i)].qty);
        for (int i = 0; i < nb; ++i) maxq = std::max<qint64>(maxq, bids_[static_cast<std::size_t>(i)].qty);
        const auto chg_text = [](const std::map<qint64, Change>& ch, qint64 px, QColor& col) -> QString {
            const auto it = ch.find(px);
            if (it == ch.end()) return {};
            col = it->second.d > 0 ? QColor(theme_token::kUp) : QColor(theme_token::kDown);
            return (it->second.d > 0 ? QStringLiteral("+") : QStringLiteral("−")) + order_flow_detail::units(std::llabs(it->second.d));
        };
        // Asks: the worst at the top, the best just above the spread row.
        qint64 cum = 0;
        std::vector<qint64> ask_cum(static_cast<std::size_t>(na));
        for (int i = 0; i < na; ++i) { cum += asks_[static_cast<std::size_t>(i)].qty; ask_cum[static_cast<std::size_t>(i)] = cum; }
        for (int i = 0; i < na; ++i) {
            const int y = kHead + (shown - 1 - i) * kRow;
            const auto& l = asks_[static_cast<std::size_t>(i)];
            if (i % 2 == 1) g.fillRect(QRect(0, y, w, kRow), QColor(theme_token::kSurfaceAlt));
            const int bar = static_cast<int>(static_cast<double>(c_qty) * static_cast<double>(l.qty) / static_cast<double>(maxq));
            g.fillRect(QRect(x_aq, y + 2, bar, kRow - 4), order_flow_detail::tint(theme_token::kDown, i == 0 ? 110 : 60));
            g.setPen(QColor(i == 0 ? theme_token::kText : theme_token::kTextMuted));
            g.drawText(QRect(x_px, y, c_px, kRow), Qt::AlignHCenter | Qt::AlignVCenter, order_flow_detail::rupees(static_cast<double>(l.price_paise)));
            g.setPen(QColor(theme_token::kText));
            g.drawText(QRect(x_aq + 3, y, c_qty - 3, kRow), Qt::AlignLeft | Qt::AlignVCenter, order_flow_detail::units(l.qty));
            g.setPen(QColor(theme_token::kTextFaint));
            if (l.orders > 0) g.drawText(QRect(x_aord, y, c_ord, kRow), Qt::AlignLeft | Qt::AlignVCenter, QString::number(l.orders));
            g.drawText(QRect(x_cum, y, c_cum - 4, kRow), Qt::AlignRight | Qt::AlignVCenter, order_flow_detail::units(ask_cum[static_cast<std::size_t>(i)]));
            QColor col;
            const QString c = chg_text(ask_change_, l.price_paise, col);
            if (!c.isEmpty()) { g.setPen(col); g.drawText(QRect(x_achg + 2, y, c_chg - 2, kRow), Qt::AlignLeft | Qt::AlignVCenter, c); }
        }
        // The spread row: touch, mid, microprice.
        {
            const int y = kHead + shown * kRow;
            g.fillRect(QRect(0, y, w, kRow), QColor(theme_token::kSurface));
            g.setPen(QColor(theme_token::kBorderHi));
            g.drawLine(0, y, w, y);
            g.drawLine(0, y + kRow - 1, w, y + kRow - 1);
            QString t;
            if (!bids_.empty() && !asks_.empty()) {
                const double bp = static_cast<double>(bids_.front().price_paise), ap = static_cast<double>(asks_.front().price_paise);
                t = ap <= bp ? QStringLiteral("CROSSED %1 / %2").arg(order_flow_detail::rupees(bp), order_flow_detail::rupees(ap))
                             : QStringLiteral("spread %1 · mid %2 · micro %3").arg(order_flow_detail::rupees(ap - bp),
                                                                             order_flow_detail::rupees(0.5 * (ap + bp), 3),
                                                                             micro_ > 0 ? order_flow_detail::rupees(micro_, 3) : QStringLiteral("—"));
                g.setPen(QColor(ap <= bp ? theme_token::kWarn : theme_token::kAccent));
            } else {
                g.setPen(QColor(theme_token::kTextFaint));
                t = QStringLiteral("one side of the book is empty");
            }
            g.drawText(QRect(0, y, w, kRow), Qt::AlignHCenter | Qt::AlignVCenter, t);
        }
        cum = 0;
        for (int i = 0; i < nb; ++i) {
            const int y = kHead + (shown + 1 + i) * kRow;
            const auto& l = bids_[static_cast<std::size_t>(i)];
            cum += l.qty;
            if (i % 2 == 1) g.fillRect(QRect(0, y, w, kRow), QColor(theme_token::kSurfaceAlt));
            const int bar = static_cast<int>(static_cast<double>(c_qty) * static_cast<double>(l.qty) / static_cast<double>(maxq));
            g.fillRect(QRect(x_px - bar, y + 2, bar, kRow - 4), order_flow_detail::tint(theme_token::kUp, i == 0 ? 110 : 60));
            g.setPen(QColor(i == 0 ? theme_token::kText : theme_token::kTextMuted));
            g.drawText(QRect(x_px, y, c_px, kRow), Qt::AlignHCenter | Qt::AlignVCenter, order_flow_detail::rupees(static_cast<double>(l.price_paise)));
            g.setPen(QColor(theme_token::kText));
            g.drawText(QRect(x_bq, y, c_qty - 3, kRow), Qt::AlignRight | Qt::AlignVCenter, order_flow_detail::units(l.qty));
            g.setPen(QColor(theme_token::kTextFaint));
            if (l.orders > 0) g.drawText(QRect(x_bord, y, c_ord - 2, kRow), Qt::AlignRight | Qt::AlignVCenter, QString::number(l.orders));
            g.drawText(QRect(x_cum, y, c_cum - 4, kRow), Qt::AlignRight | Qt::AlignVCenter, order_flow_detail::units(cum));
            QColor col;
            const QString c = chg_text(bid_change_, l.price_paise, col);
            if (!c.isEmpty()) { g.setPen(col); g.drawText(QRect(x_bchg, y, c_chg - 2, kRow), Qt::AlignRight | Qt::AlignVCenter, c); }
        }
    }

private:
    struct Change { qint64 d = 0; qint64 ms = 0; };
    static constexpr int kRow = 17, kHead = 18;
    bool never_ = true;
    int rows_ = 20;
    int levels_ = 0;
    double micro_ = 0.0;
    std::vector<PriceLevel> bids_, asks_;
    std::map<qint64, qint64> last_bid_, last_ask_;
    std::map<qint64, Change> bid_change_, ask_change_;
};

// ---------------------------------------------------------------------------
// The readings, as bars from -1 to +1
// ---------------------------------------------------------------------------

class FlowGauges final : public QWidget {
public:
    struct Gauge { QString name; double v = 0.0; bool known = false; QString note; };

    explicit FlowGauges(QWidget* parent = nullptr) : QWidget(parent) {
        setObjectName(QStringLiteral("flowGauges"));
        setMinimumHeight(8 * kRow + 6);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
    void set(std::vector<Gauge> g, double threshold) {
        g_ = std::move(g);
        threshold_ = threshold;
        setFixedHeight(static_cast<int>(g_.size()) * kRow + 6);
        update();
    }
    [[nodiscard]] const std::vector<Gauge>& gauges() const noexcept { return g_; }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), QColor(theme_token::kCanvas));
        const int label = 112, value = 56, note = std::min(190, width() / 4);
        const int x0 = label + value, bw = std::max(40, width() - x0 - note - 8);
        const int mid = x0 + bw / 2;
        for (std::size_t i = 0; i < g_.size(); ++i) {
            const auto& g = g_[i];
            const int y = 3 + static_cast<int>(i) * kRow;
            p.setPen(QColor(theme_token::kTextMuted));
            p.drawText(QRect(0, y, label, kRow), Qt::AlignLeft | Qt::AlignVCenter, g.name);
            p.fillRect(QRect(x0, y + 4, bw, kRow - 8), QColor(theme_token::kSurface));
            // The threshold marks.
            p.setPen(QPen(QColor(theme_token::kBorderHi), 1, Qt::DashLine));
            const int t = static_cast<int>(threshold_ * bw / 2.0);
            p.drawLine(mid - t, y + 2, mid - t, y + kRow - 2);
            p.drawLine(mid + t, y + 2, mid + t, y + kRow - 2);
            p.setPen(QColor(theme_token::kBorderHi));
            p.drawLine(mid, y + 2, mid, y + kRow - 2);
            if (g.known) {
                const double v = std::isfinite(g.v) ? std::max(-1.0, std::min(1.0, g.v)) : 0.0;
                const int len = static_cast<int>(std::lround(v * bw / 2.0));
                const bool strong = std::fabs(v) >= threshold_;
                const QColor c = order_flow_detail::tint(v >= 0 ? theme_token::kUp : theme_token::kDown, strong ? 230 : 120);
                p.fillRect(len >= 0 ? QRect(mid, y + 4, len, kRow - 8) : QRect(mid + len, y + 4, -len, kRow - 8), c);
                p.setPen(QColor(strong ? (v >= 0 ? theme_token::kUp : theme_token::kDown) : theme_token::kText));
                p.drawText(QRect(label, y, value - 6, kRow), Qt::AlignRight | Qt::AlignVCenter, order_flow_detail::signed2(g.v));
            } else {
                p.setPen(QColor(theme_token::kTextFaint));
                p.drawText(QRect(label, y, value - 6, kRow), Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("—"));
            }
            p.setPen(QColor(theme_token::kTextFaint));
            p.drawText(QRect(x0 + bw + 8, y, note, kRow), Qt::AlignLeft | Qt::AlignVCenter, g.note);
        }
    }

private:
    static constexpr int kRow = 18;
    std::vector<Gauge> g_;
    double threshold_ = 0.30;
};

// ---------------------------------------------------------------------------
// A small time chart
// ---------------------------------------------------------------------------

class FlowChart final : public QWidget {
public:
    struct Series { QString name; const char* color; std::function<double(const order_flow_detail::FlowSample&)> get; bool bold = false; };

    FlowChart(std::vector<Series> s, bool fixed_unit, QWidget* parent = nullptr)
        : QWidget(parent), s_(std::move(s)), fixed_(fixed_unit) {
        setMinimumHeight(120);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }
    void set(const std::deque<order_flow_detail::FlowSample>* h, qint64 now_ns, qint64 span_ns, double threshold) {
        h_ = h; now_ = now_ns; span_ = span_ns; threshold_ = threshold;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.fillRect(rect(), QColor(theme_token::kCanvas));
        const QRect area = rect().adjusted(66, 18, -8, -14);
        p.setPen(QColor(theme_token::kBorder));
        p.drawRect(area);
        // The legend.
        int lx = area.left();
        for (const auto& s : s_) {
            p.setPen(QColor(QString::fromLatin1(s.color)));
            p.drawText(QRect(lx, 0, 140, 16), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("— ") + s.name);
            lx += p.fontMetrics().horizontalAdvance(QStringLiteral("— ") + s.name) + 14;
        }
        if (h_ == nullptr || h_->empty() || span_ <= 0) {
            p.setPen(QColor(theme_token::kTextFaint));
            p.drawText(area, Qt::AlignCenter, QStringLiteral("waiting for the book"));
            return;
        }
        const qint64 t0 = now_ - span_;
        double lo = fixed_ ? -1.0 : std::numeric_limits<double>::infinity(), hi = fixed_ ? 1.0 : -std::numeric_limits<double>::infinity();
        if (!fixed_) {
            for (const auto& x : *h_) {
                if (x.ns < t0) continue;
                for (const auto& s : s_) {
                    const double v = s.get(x);
                    if (v > 0 && std::isfinite(v)) { lo = std::min(lo, v); hi = std::max(hi, v); }
                }
            }
            if (!std::isfinite(lo) || !std::isfinite(hi)) return;
            if (hi - lo < 0.10) { const double m = 0.5 * (hi + lo); lo = m - 0.05; hi = m + 0.05; }
            const double pad = 0.08 * (hi - lo);
            lo -= pad; hi += pad;
        }
        const auto X = [&](qint64 ns) { return area.left() + static_cast<double>(ns - t0) / static_cast<double>(span_) * area.width(); };
        const auto Y = [&](double v) { return area.bottom() - (v - lo) / (hi - lo) * area.height(); };
        // The scale.
        p.setPen(QColor(theme_token::kTextFaint));
        for (int k = 0; k <= 4; ++k) {
            const double v = lo + (hi - lo) * k / 4.0;
            const int y = static_cast<int>(Y(v));
            p.drawText(QRect(0, y - 8, 62, 16), Qt::AlignRight | Qt::AlignVCenter,
                       QString::number(v, 'f', 2));
        }
        if (fixed_) {
            p.setPen(QPen(QColor(theme_token::kBorderHi), 1, Qt::DashLine));
            for (const double v : {threshold_, -threshold_}) p.drawLine(QPointF(area.left(), Y(v)), QPointF(area.right(), Y(v)));
            p.setPen(QColor(theme_token::kBorder));
            p.drawLine(QPointF(area.left(), Y(0.0)), QPointF(area.right(), Y(0.0)));
        }
        p.setPen(QColor(theme_token::kTextFaint));
        p.drawText(QRect(area.left(), area.bottom() + 1, area.width(), 13), Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("-%1 s").arg(span_ / 1'000'000'000));
        p.drawText(QRect(area.left(), area.bottom() + 1, area.width(), 13), Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("now"));
        p.setClipRect(area);
        for (const auto& s : s_) {
            QPainterPath path;
            bool started = false;
            for (const auto& x : *h_) {
                if (x.ns < t0) continue;
                const double v = s.get(x);
                if (!std::isfinite(v) || (!fixed_ && v <= 0)) { started = false; continue; }
                const QPointF pt(X(x.ns), Y(v));
                if (!started) { path.moveTo(pt); started = true; }
                else path.lineTo(pt);
            }
            p.setPen(QPen(QColor(QString::fromLatin1(s.color)), s.bold ? 1.8 : 1.1));
            p.drawPath(path);
        }
    }

private:
    std::vector<Series> s_;
    bool fixed_ = false;
    const std::deque<order_flow_detail::FlowSample>* h_ = nullptr;
    qint64 now_ = 0, span_ = 300'000'000'000LL;
    double threshold_ = 0.30;
};

// ---------------------------------------------------------------------------
// The page
// ---------------------------------------------------------------------------

class OrderFlowPage final : public QWidget {
public:
    OrderFlowPage(PriceClient* client, QString root, QWidget* parent = nullptr)
        : QWidget(parent), client_(client), root_(std::move(root)) {
#ifdef ALTAIR_SOURCE_DIR
        if (root_.isEmpty()) root_ = QStringLiteral(ALTAIR_SOURCE_DIR);
#endif
        setObjectName(QStringLiteral("orderFlowPage"));
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(10, 8, 10, 8);
        v->setSpacing(6);

        auto* title = new QLabel(QStringLiteral("<b>ORDER FLOW</b> &nbsp;<span style='color:%1'>imbalance, microprice, OFI, "
                                                "executed vs cancelled — tick by tick (book/flow.hpp)</span>")
                                     .arg(QString::fromLatin1(theme_token::kTextMuted)), this);
        v->addWidget(title);

        auto* bar = new QHBoxLayout;
        bar->setSpacing(6);
        under_ = new QComboBox(this);
        under_->setObjectName(QStringLiteral("flowUnderlying"));
        under_->setMinimumWidth(120);
        contract_ = new QComboBox(this);
        contract_->setObjectName(QStringLiteral("flowContract"));
        contract_->setMinimumWidth(170);
        search_ = new QLineEdit(this);
        search_->setObjectName(QStringLiteral("flowSearch"));
        search_->setPlaceholderText(QStringLiteral("Search any symbol…"));
        search_->setClearButtonEnabled(true);
        search_->setMaximumWidth(240);
        rows_ = new QComboBox(this);
        rows_->setObjectName(QStringLiteral("flowRows"));
        for (const int n : {5, 10, 20, 50}) rows_->addItem(QStringLiteral("%1 levels").arg(n), n);
        rows_->setCurrentIndex(2);
        span_ = new QComboBox(this);
        span_->setObjectName(QStringLiteral("flowSpan"));
        for (const int s : {60, 300, 900}) span_->addItem(QStringLiteral("chart %1 min").arg(s / 60), s);
        span_->setCurrentIndex(1);
        deep_ = new QCheckBox(QStringLiteral("50-level book (TBT)"), this);
        deep_->setObjectName(QStringLiteral("flowDeep"));
        deep_->setToolTip(QStringLiteral(
            "Asks the feed for this instrument's 50-level book (data/live/obi_watch.csv, also used by the OBI trader).\n"
            "FYERS TBT slots are limited: 1 symbol on the Standard plan, 5 per connection on Prime."));
        show_adds_ = new QCheckBox(QStringLiteral("Tape: adds"), this);
        show_adds_->setObjectName(QStringLiteral("flowShowAdds"));
        bar->addWidget(new QLabel(QStringLiteral("Instrument"), this));
        bar->addWidget(under_);
        bar->addWidget(contract_);
        bar->addWidget(search_);
        bar->addSpacing(8);
        bar->addWidget(rows_);
        bar->addWidget(span_);
        bar->addWidget(deep_);
        bar->addWidget(show_adds_);
        bar->addStretch(1);
        v->addLayout(bar);

        head_ = new QLabel(this);
        head_->setObjectName(QStringLiteral("flowHead"));
        head_->setTextFormat(Qt::RichText);
        head_->setWordWrap(true);
        v->addWidget(head_);

        auto* split = new QSplitter(Qt::Horizontal, this);
        split->setObjectName(QStringLiteral("flowSplit"));
        split->setChildrenCollapsible(false);

        // Left: the ladder.
        ladder_ = new FlowLadder;
        auto* scroll = new QScrollArea(split);
        scroll->setObjectName(QStringLiteral("flowLadderScroll"));
        scroll->setWidget(ladder_);
        scroll->setWidgetResizable(true);
        scroll->setMinimumWidth(380);
        split->addWidget(scroll);

        // Middle: readings, charts and the tape.
        auto* mid = new QWidget(split);
        auto* mv = new QVBoxLayout(mid);
        mv->setContentsMargins(0, 0, 0, 0);
        mv->setSpacing(4);
        gauges_ = new FlowGauges(mid);
        mv->addWidget(gauges_);
        flows_ = new QLabel(mid);
        flows_->setObjectName(QStringLiteral("flowCounts"));
        flows_->setTextFormat(Qt::RichText);
        flows_->setWordWrap(true);
        mv->addWidget(flows_);
        using order_flow_detail::FlowSample;
        price_chart_ = new FlowChart({{QStringLiteral("bid"), theme_token::kUp, [](const FlowSample& s) { return s.bid; }},
                                      {QStringLiteral("ask"), theme_token::kDown, [](const FlowSample& s) { return s.ask; }},
                                      {QStringLiteral("mid"), theme_token::kTextMuted, [](const FlowSample& s) { return s.mid; }},
                                      {QStringLiteral("microprice"), theme_token::kAccent, [](const FlowSample& s) { return s.micro; }, true}},
                                     false, mid);
        price_chart_->setObjectName(QStringLiteral("flowPriceChart"));
        signal_chart_ = new FlowChart({{QStringLiteral("OBI 5"), theme_token::kAccent, [](const FlowSample& s) { return s.obi5; }, true},
                                       {QStringLiteral("OFI"), theme_token::kInfo, [](const FlowSample& s) { return s.ofi; }},
                                       {QStringLiteral("score S"), theme_token::kUp, [](const FlowSample& s) { return s.score; }}},
                                      true, mid);
        signal_chart_->setObjectName(QStringLiteral("flowSignalChart"));
        mv->addWidget(price_chart_, 2);
        mv->addWidget(signal_chart_, 2);
        tape_ = make_live_table(5, {QStringLiteral("Time"), QStringLiteral("Event"), QStringLiteral("Size"),
                                    QStringLiteral("Bid"), QStringLiteral("Ask")}, mid);
        tape_->setObjectName(QStringLiteral("flowTape"));
        tape_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        mv->addWidget(tape_, 3);
        split->addWidget(mid);

        // Right: the OBI trader.
        auto* right = new QWidget(split);
        auto* rv = new QVBoxLayout(right);
        rv->setContentsMargins(0, 0, 0, 0);
        rv->addWidget(new QLabel(QStringLiteral("<b>OBI trader</b> &nbsp;<span style='color:%1'>paper · live/obi_trader.hpp</span>")
                                     .arg(QString::fromLatin1(theme_token::kTextMuted)), right));
        record_ = new StrategyRecordPanel(QStringLiteral("OBI"), root_, right);
        rv->addWidget(record_, 1);
        split->addWidget(right);
        split->setStretchFactor(0, 3);
        split->setStretchFactor(1, 5);
        split->setStretchFactor(2, 4);
        v->addWidget(split, 1);

        QObject::connect(under_, &QComboBox::currentIndexChanged, this, [this] { fill_contracts(); });
        QObject::connect(contract_, &QComboBox::currentIndexChanged, this, [this] {
            const quint32 tok = contract_->currentData().toUInt();
            if (tok != 0) select(tok);
        });
        QObject::connect(rows_, &QComboBox::currentIndexChanged, this, [this] { dirty_ = true; });
        QObject::connect(span_, &QComboBox::currentIndexChanged, this, [this] { dirty_ = true; });
        QObject::connect(show_adds_, &QCheckBox::toggled, this, [this] { tape_dirty_ = true; dirty_ = true; });
        QObject::connect(deep_, &QCheckBox::clicked, this, [this](bool on) {
            const LiveRow* r = row_of(token_);
            if (r == nullptr || r->fyers.isEmpty()) return;
            if (!order_flow_detail::set_watch(root_, r->fyers, on)) deep_->setChecked(!on);
            dirty_ = true;
        });
        QObject::connect(search_, &QLineEdit::returnPressed, this, [this] { pick_search(search_->text()); });
        if (client_ != nullptr)
            QObject::connect(client_, &PriceClient::frameApplied, this,
                             [this](unsigned tok, int kind, qint64 ns) { on_frame(tok, kind, ns); });

        paint_.setInterval(100);
        QObject::connect(&paint_, &QTimer::timeout, this, [this] { if (dirty_ && isVisible()) repaint_all(); });
        paint_.start();
        reload_.setInterval(30'000);
        QObject::connect(&reload_, &QTimer::timeout, this, [this] { if (isVisible()) reload_universe(); });
        reload_.start();
        reload_universe();
    }
    ~OrderFlowPage() override {
        if (client_ != nullptr && token_ != 0) client_->watch_frames(token_, false);
    }

    /// Reads data/live/universe.csv again (the feed rewrites it each session).
    void reload_universe() {
        auto rows = load_live_universe(root_ + QStringLiteral("/data/live/universe.csv"));
        if (rows.empty()) rows = default_live_universe();
        std::vector<LiveRow> keep;
        for (auto& r : rows)
            if (r.kind != QLatin1String("index")) keep.push_back(std::move(r));   // an index has no order book
        if (keep.size() == universe_.size()
            && std::equal(keep.begin(), keep.end(), universe_.begin(), [](const LiveRow& a, const LiveRow& b) { return a.token == b.token; }))
            return;
        universe_ = std::move(keep);
        QStringList unders, symbols;
        for (const auto& r : universe_) {
            const QString u = r.underlying.isEmpty() ? r.symbol : r.underlying;
            if (!unders.contains(u)) unders << u;
            symbols << r.symbol;
        }
        std::stable_sort(unders.begin(), unders.end(), [](const QString& a, const QString& b) {
            const auto rank = [](const QString& s) { return s == QLatin1String("NIFTY") ? 0 : s == QLatin1String("BANKNIFTY") ? 1 : 2; };
            return rank(a) != rank(b) ? rank(a) < rank(b) : a < b;
        });
        auto* comp = new QCompleter(symbols, search_);
        comp->setCaseSensitivity(Qt::CaseInsensitive);
        comp->setFilterMode(Qt::MatchContains);
        QObject::connect(comp, qOverload<const QString&>(&QCompleter::activated), this, [this](const QString& s) { pick_search(s); });
        if (search_->completer() != nullptr) search_->completer()->deleteLater();
        search_->setCompleter(comp);
        const QString was = under_->currentText();
        {
            const QSignalBlocker b(under_);
            under_->clear();
            under_->addItems(unders);
            const int at = under_->findText(was);
            under_->setCurrentIndex(at >= 0 ? at : 0);
        }
        fill_contracts();
    }

    /// Opens one instrument by token; false if the feed's universe lacks it.
    bool select(quint32 token) {
        const LiveRow* r = row_of(token);
        if (r == nullptr) return false;
        if (token == token_) return true;
        if (client_ != nullptr && token_ != 0) client_->watch_frames(token_, false);
        token_ = token;
        flow::FlowConfig c;
        c.tick = std::max<std::int64_t>(1, std::llround(r->tick * 100.0));
        engine_ = std::make_unique<flow::FlowEngine>(c);
        history_.clear();
        events_.clear();
        last_total_ = {};
        tape_dirty_ = true;
        books_ = prints_ = 0;
        work_ns_ = work_max_ns_ = 0;
        ladder_->clear_history();
        deep_->setChecked(!r->fyers.isEmpty() && order_flow_detail::read_watch(root_).contains(r->fyers));
        deep_->setEnabled(!r->fyers.isEmpty());
        {
            const QSignalBlocker b1(under_), b2(contract_);
            const QString u = r->underlying.isEmpty() ? r->symbol : r->underlying;
            if (under_->currentText() != u) { under_->setCurrentIndex(under_->findText(u)); fill_contracts(false); }
            const int at = contract_->findData(token);
            if (at >= 0) contract_->setCurrentIndex(at);
        }
        if (client_ != nullptr) {
            client_->watch_frames(token, true);
            // The book as it stands now, so the page has a reading at once.
            if (const LivePrice* p = client_->price(token)) {
                if (p->levels > 0) on_frame(token, PriceClient::AppliedBook, p->book_ns);
                else if (p->has_quote) on_frame(token, PriceClient::AppliedQuote, p->quote_ns);
                // The day's volume so far is the baseline the next print is measured from
                // (the engine's first volume is never a print), so that print is not lost.
                if (p->has_volume && p->volume > 0) engine_->on_trade(p->last_paise, p->volume, 0, p->last_ns);
            }
        }
        dirty_ = true;
        repaint_all();
        return true;
    }
    [[nodiscard]] quint32 token() const noexcept { return token_; }
    [[nodiscard]] const flow::FlowEngine* engine() const noexcept { return engine_.get(); }
    [[nodiscard]] const std::deque<order_flow_detail::FlowEvent>& events() const noexcept { return events_; }
    [[nodiscard]] QTableWidget* tape() const noexcept { return tape_; }
    [[nodiscard]] FlowLadder* ladder() const noexcept { return ladder_; }
    [[nodiscard]] FlowGauges* gauges() const noexcept { return gauges_; }
    [[nodiscard]] QString head_text() const { return head_->text(); }
    [[nodiscard]] QString counts_text() const { return flows_->text(); }
    [[nodiscard]] std::size_t samples() const noexcept { return history_.size(); }
    void repaint_now() { dirty_ = true; repaint_all(); }

private:
    [[nodiscard]] const LiveRow* row_of(quint32 tok) const {
        for (const auto& r : universe_) if (r.token == tok) return &r;
        return nullptr;
    }

    void fill_contracts(bool choose = true) {
        const QString u = under_->currentText();
        std::vector<const LiveRow*> list;
        for (const auto& r : universe_)
            if ((r.underlying.isEmpty() ? r.symbol : r.underlying) == u) list.push_back(&r);
        std::sort(list.begin(), list.end(), [](const LiveRow* a, const LiveRow* b) { return order_flow_detail::contract_before(*a, *b); });
        {
            const QSignalBlocker b(contract_);
            contract_->clear();
            for (const LiveRow* r : list) contract_->addItem(order_flow_detail::contract_label(*r), r->token);
            const int at = contract_->findData(token_);
            contract_->setCurrentIndex(at >= 0 ? at : (contract_->count() > 0 ? 0 : -1));
        }
        if (choose && contract_->currentIndex() >= 0) (void)select(contract_->currentData().toUInt());
    }

    void pick_search(const QString& text) {
        const QString want = text.trimmed();
        if (want.isEmpty()) return;
        for (const auto& r : universe_)
            if (r.symbol.compare(want, Qt::CaseInsensitive) == 0 || r.fyers.compare(want, Qt::CaseInsensitive) == 0) {
                (void)select(r.token);
                return;
            }
        for (const auto& r : universe_)
            if (r.symbol.contains(want, Qt::CaseInsensitive)) { (void)select(r.token); return; }
    }

    /// Every frame of the instrument: into the engine, at once.
    void on_frame(unsigned tok, int kind, qint64 ns) {
        if (tok != token_ || engine_ == nullptr || client_ == nullptr) return;
        const LivePrice* p = client_->price(tok);
        if (p == nullptr) return;
        const auto t0 = std::chrono::steady_clock::now();
        if (kind == PriceClient::AppliedTrade) {
            engine_->on_trade(p->last_paise, p->has_volume ? p->volume : -1, p->last_qty, ns);
            ++prints_;
        } else if (kind == PriceClient::AppliedBook) {
            if (p->levels == 0) return;
            // RULE 11: proven -- PriceClient keeps kMaxDepthLevels (50) a side and
            // depth_levels never exceeds it (server/price_payload.hpp).
            const std::size_t n = p->levels < kMaxDepthLevels ? p->levels : kMaxDepthLevels;
            for (std::size_t i = 0; i < n; ++i) {
                bids_[i] = flow::Level{p->bids[i].price_paise, p->bids[i].qty};
                asks_[i] = flow::Level{p->asks[i].price_paise, p->asks[i].qty};
            }
            engine_->on_book(bids_.data(), n, asks_.data(), n, ns);
            ++books_;
        } else {
            if (p->levels > 0) return;   // the depth frames are the book; a quote between them is not a new one
            const flow::Level b{p->quote.bid, p->quote.bid_qty}, a{p->quote.ask, p->quote.ask_qty};
            engine_->on_book(&b, 1, &a, 1, ns);
            ++books_;
        }
        const auto spent = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
        work_ns_ += spent;
        work_max_ns_ = std::max<qint64>(work_max_ns_, spent);
        const flow::FlowSignals& s = engine_->signals();
        for (const auto& e : order_flow_detail::events_between(last_total_, s.total, ns, s.bid, s.ask)) {
            events_.push_front(e);
            tape_dirty_ = true;
        }
        last_total_ = s.total;
        // RULE 11: counted -- the tape is a window of the newest kTapeKeep
        // events and says so in its title; the counters keep every one.
        while (events_.size() > kTapeKeep) events_.pop_back();
        if (s.valid && (history_.empty() || ns - history_.back().ns >= kSampleNs)) {
            order_flow_detail::FlowSample x;
            x.ns = ns;
            x.bid = static_cast<double>(s.bid) / 100.0;
            x.ask = static_cast<double>(s.ask) / 100.0;
            x.mid = s.mid / 100.0;
            x.micro = s.microprice.value() / 100.0;
            x.obi5 = s.obi_short;
            x.ofi = s.ofi_norm;
            x.score = s.score;
            history_.push_back(x);
            while (!history_.empty() && history_.front().ns < ns - kKeepNs) history_.pop_front();
        }
        last_ns_ = ns;
        dirty_ = true;
    }

    void repaint_all() {
        dirty_ = false;
        const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
        const LivePrice* p = client_ != nullptr && token_ != 0 ? client_->price(token_) : nullptr;
        const LiveRow* r = row_of(token_);
        const flow::FlowSignals* s = engine_ != nullptr ? &engine_->signals() : nullptr;
        const bool valid = s != nullptr && s->valid;
        ladder_->set_book(p, rows_->currentData().toInt(), valid ? s->microprice.value() : 0.0, now_ms);

        // The headline.
        {
            QString h;
            const QString sym = r != nullptr ? r->symbol : QStringLiteral("—");
            if (!valid) {
                h = QStringLiteral("<b>%1</b> &nbsp; <span style='color:%2'>%3</span>")
                        .arg(sym.toHtmlEscaped(), QString::fromLatin1(theme_token::kTextFaint),
                             client_ == nullptr || !client_->connected() ? QStringLiteral("the feed is not connected (Terminal starts it)")
                                                                         : QStringLiteral("no tradable book yet"));
            } else {
                const QString src = s->bid_levels > 5 || s->ask_levels > 5 ? QStringLiteral("%1 levels (TBT)").arg(static_cast<int>(std::min(s->bid_levels, s->ask_levels)))
                                    : s->bid_levels <= 1 ? QStringLiteral("touch only")
                                                         : QStringLiteral("%1 levels").arg(static_cast<int>(std::min(s->bid_levels, s->ask_levels)));
                const qint64 age_ms = p != nullptr && p->book_ns > 0 ? now_ms - p->book_ns / 1'000'000 : -1;
                QString flag;
                if (p != nullptr && p->simulated) flag = QStringLiteral(" &nbsp;<b style='color:%1'>SIM</b>").arg(QString::fromLatin1(theme_token::kWarn));
                else if (p != nullptr && p->replay) flag = QStringLiteral(" &nbsp;<b style='color:%1'>REPLAY</b>").arg(QString::fromLatin1(theme_token::kWarn));
                h = QStringLiteral("<b>%1</b>%2 &nbsp; <span style='color:%3'>bid</span> <b>%4</b> × %5 &nbsp; "
                                   "<span style='color:%6'>ask</span> <b>%7</b> × %8 &nbsp; mid %9 &nbsp; "
                                   "<span style='color:%10'>micro %11 (%12 t)</span> &nbsp; spread %13 t &nbsp; · %14%15")
                        .arg(sym.toHtmlEscaped(), flag, QString::fromLatin1(theme_token::kUp), order_flow_detail::rupees(static_cast<double>(s->bid)),
                             order_flow_detail::units(s->bid_qty), QString::fromLatin1(theme_token::kDown),
                             order_flow_detail::rupees(static_cast<double>(s->ask)), order_flow_detail::units(s->ask_qty),
                             order_flow_detail::rupees(s->mid, 3))
                        .arg(QString::fromLatin1(theme_token::kAccent), order_flow_detail::rupees(s->microprice.value(), 3),
                             order_flow_detail::signed2(s->micro_offset_ticks), QString::number(s->spread_ticks, 'f', 0), src,
                             age_ms < 0 ? QString()
                             : age_ms <= 60'000 ? QStringLiteral(" · book %1 s ago").arg(QString::number(static_cast<double>(age_ms) / 1000.0, 'f', 1))
                                                : QStringLiteral(" · <b style='color:%1'>book from %2</b>")
                                                      .arg(QString::fromLatin1(theme_token::kWarn), live_detail::ist(p->book_ns, false)));
            }
            head_->setText(h);
        }

        // The bars.
        {
            const double th = engine_ != nullptr ? engine_->config().threshold : 0.30;
            std::vector<FlowGauges::Gauge> g;
            const auto add = [&](const QString& name, double v, const QString& note) { g.push_back({name, v, valid, note}); };
            if (s != nullptr) {
                add(QStringLiteral("OBI touch"), s->obi1, QStringLiteral("best bid vs best ask"));
                add(QStringLiteral("OBI 5"), s->obi_short, QStringLiteral("5 levels"));
                add(QStringLiteral("OBI %1").arg(static_cast<int>(s->deep_used)), s->obi_deep, QStringLiteral("all shown levels"));
                add(QStringLiteral("OBI %1 weighted").arg(static_cast<int>(s->deep_used)), s->obi_deep_w, QStringLiteral("e^-0.25·i toward the touch"));
                add(QStringLiteral("OFI"), s->ofi_norm, QStringLiteral("touch flow, last %1 s").arg(static_cast<qlonglong>(engine_->config().window_ns / 1'000'000'000)));
                add(QStringLiteral("Trades"), s->trade_imbalance, QStringLiteral("buy vs sell prints"));
                add(QStringLiteral("Momentum"), s->mom_norm, QStringLiteral("%1 ticks").arg(QString::number(s->momentum_ticks, 'f', 1)));
                add(QStringLiteral("Score S"), s->score, QStringLiteral("0.4 OBI + 0.4 OFI + 0.2 M"));
            }
            gauges_->set(std::move(g), th);
        }

        // The counters.
        if (s != nullptr && engine_ != nullptr) {
            const auto line = [](const QString& head, qint64 buy, qint64 sell, qint64 ask_take, qint64 bid_take, qint64 ask_cxl,
                                 qint64 bid_cxl, qint64 ask_add, qint64 bid_add, qint64 hb, qint64 hs) {
                using order_flow_detail::units;
                return QStringLiteral("<b>%1</b> &nbsp; executed: <span style='color:%2'>buy %3</span> · <span style='color:%4'>sell %5</span>"
                                      " &nbsp;|&nbsp; taken off the book: ask %6 · bid %7 &nbsp;|&nbsp; cancelled: ask %8 · bid %9"
                                      " &nbsp;|&nbsp; added: ask %10 · bid %11 &nbsp;|&nbsp; hidden: buy %12 · sell %13")
                    .arg(head, QString::fromLatin1(theme_token::kUp), units(buy), QString::fromLatin1(theme_token::kDown), units(sell), units(ask_take), units(bid_take),
                         units(ask_cxl), units(bid_cxl))
                    .arg(units(ask_add), units(bid_add), units(hb), units(hs));
            };
            const auto& t = s->total;
            QStringList out;
            out << line(QStringLiteral("Last %1 s").arg(static_cast<qlonglong>(engine_->config().window_ns / 1'000'000'000)), s->buy_exec, s->sell_exec,
                        s->ask_exec_drop, s->bid_exec_drop, s->ask_cancel, s->bid_cancel, s->ask_add, s->bid_add, s->hidden_buy, s->hidden_sell);
            out << line(QStringLiteral("Since opened"), t.buy, t.sell, t.ask_exec_drop, t.bid_exec_drop, t.ask_cancel, t.bid_cancel,
                        t.ask_add, t.bid_add, t.hidden_buy, t.hidden_sell);
            QStringList flags;
            const double th = engine_->config().threshold;
            if (s->persist != 0)
                flags << QStringLiteral("<b style='color:%1'>%2 %3 updates</b> beyond %4 OBI5 for %5 s")
                             .arg(QString::fromLatin1(s->persist > 0 ? theme_token::kUp : theme_token::kDown), s->persist > 0 ? QStringLiteral("BID PRESSURE") : QStringLiteral("ASK PRESSURE"))
                             .arg(std::abs(s->persist))
                             .arg(QString::number(th, 'f', 2), QString::number(static_cast<double>(s->persist_ns) / 1e9, 'f', 1));
            else
                flags << QStringLiteral("<span style='color:%1'>no persistent imbalance</span>").arg(QString::fromLatin1(theme_token::kTextFaint));
            if (s->absorption != 0)
                flags << QStringLiteral("<b style='color:%1'>ABSORPTION</b>: %2").arg(QString::fromLatin1(theme_token::kWarn),
                             s->absorption > 0 ? QStringLiteral("buyers absorb the selling") : QStringLiteral("sellers absorb the buying"));
            if (s->withdrawal != 0)
                flags << QStringLiteral("<b style='color:%1'>WITHDRAWAL</b>: %2").arg(QString::fromLatin1(theme_token::kWarn),
                             s->withdrawal > 0 ? QStringLiteral("ask liquidity pulled while lifted") : QStringLiteral("bid liquidity pulled while hit"));
            if (s->prior_high > 0 && s->mid > s->prior_high) flags << QStringLiteral("mid above the 30 s high");
            else if (s->prior_low > 0 && s->mid < s->prior_low) flags << QStringLiteral("mid below the 30 s low");
            out << flags.join(QStringLiteral(" &nbsp;·&nbsp; "));
            const std::uint64_t n = books_ + prints_;
            out << QStringLiteral("<span style='color:%1'>engine %2 µs an update (max %3 µs) · %4 books · %5 prints · %6 malformed · %7 evicted</span>")
                       .arg(QString::fromLatin1(theme_token::kTextFaint),
                            QString::number(n > 0 ? static_cast<double>(work_ns_) / static_cast<double>(n) / 1000.0 : 0.0, 'f', 2),
                            QString::number(static_cast<double>(work_max_ns_) / 1000.0, 'f', 1))
                       .arg(static_cast<qulonglong>(books_)).arg(static_cast<qulonglong>(prints_))
                       .arg(static_cast<qulonglong>(s->malformed)).arg(static_cast<qulonglong>(s->evicted));
            flows_->setText(out.join(QStringLiteral("<br>")));
        } else {
            flows_->setText(QString());
        }

        // The charts.
        const qint64 span = static_cast<qint64>(span_->currentData().toInt()) * 1'000'000'000LL;
        const double th = engine_ != nullptr ? engine_->config().threshold : 0.30;
        price_chart_->set(&history_, last_ns_, span, th);
        signal_chart_->set(&history_, last_ns_, span, th);

        // The tape.
        if (tape_dirty_) {
            tape_dirty_ = false;
            const bool adds = show_adds_->isChecked();
            int row = 0;
            for (const auto& e : events_) {
                if (!adds && (e.kind == order_flow_detail::AskAdd || e.kind == order_flow_detail::BidAdd)) continue;
                if (row >= kTapeRows) break;
                if (row >= tape_->rowCount()) tape_->insertRow(row);
                const int lean = order_flow_detail::event_lean(e.kind);
                const bool cancel = e.kind == order_flow_detail::AskCancel || e.kind == order_flow_detail::BidCancel;
                const QColor c = cancel ? QColor(theme_token::kWarn) : lean > 0 ? QColor(theme_token::kUp) : lean < 0 ? QColor(theme_token::kDown) : QColor(theme_token::kTextMuted);
                live_set(tape_, row, 0, live_detail::ist(e.ns, true));
                live_set(tape_, row, 1, order_flow_detail::event_name(e.kind), c);
                live_set(tape_, row, 2, order_flow_detail::units(e.qty), c);
                live_set(tape_, row, 3, order_flow_detail::rupees(static_cast<double>(e.bid)));
                live_set(tape_, row, 4, order_flow_detail::rupees(static_cast<double>(e.ask)));
                if (auto* it = tape_->item(row, 1)) it->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
                ++row;
            }
            tape_->setRowCount(row);
        }
    }

    static constexpr std::size_t kTapeKeep = 2'000;   ///< events kept (the tape shows the newest kTapeRows)
    static constexpr int kTapeRows = 300;
    static constexpr qint64 kSampleNs = 100'000'000;        ///< one chart point per 100 ms at most
    static constexpr qint64 kKeepNs = 900'000'000'000LL;    ///< 15 minutes of chart

    PriceClient* client_ = nullptr;
    QString root_;
    std::vector<LiveRow> universe_;
    quint32 token_ = 0;
    std::unique_ptr<flow::FlowEngine> engine_;
    std::array<flow::Level, flow::kMaxLevels> bids_{}, asks_{};
    flow::FlowTotals last_total_{};
    std::deque<order_flow_detail::FlowEvent> events_;
    std::deque<order_flow_detail::FlowSample> history_;
    std::uint64_t books_ = 0, prints_ = 0;
    qint64 work_ns_ = 0, work_max_ns_ = 0, last_ns_ = 0;
    bool dirty_ = true, tape_dirty_ = true;

    QComboBox* under_ = nullptr;
    QComboBox* contract_ = nullptr;
    QLineEdit* search_ = nullptr;
    QComboBox* rows_ = nullptr;
    QComboBox* span_ = nullptr;
    QCheckBox* deep_ = nullptr;
    QCheckBox* show_adds_ = nullptr;
    QLabel* head_ = nullptr;
    QLabel* flows_ = nullptr;
    FlowLadder* ladder_ = nullptr;
    FlowGauges* gauges_ = nullptr;
    FlowChart* price_chart_ = nullptr;
    FlowChart* signal_chart_ = nullptr;
    QTableWidget* tape_ = nullptr;
    StrategyRecordPanel* record_ = nullptr;
    QTimer paint_, reload_;
};

} // namespace altair::ui
