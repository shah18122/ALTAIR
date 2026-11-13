// desktop/obi_page.hpp -- Order Book Imbalance: BANKNIFTY and NIFTY, every
// book and every print, the four trade types live,
// and a paper BUY / SELL one click away.
//
// WHAT IS ON IT (Smit, 2026-11-02: "purely order book")
//   * Buttons: BANKNIFTY, NIFTY and any F&O stock of config/obi_underlyings.csv
//     (none now). Nothing else -- the OBI trader trades only these.
//   * The ladder of the selected contract: up to 50 levels a side (the FYERS
//     50-level book while it holds a TBT slot, else 5), size bars, the sweep
//     total, the change at each level, touch, mid and microprice.
//   * THE FOUR TRADE TYPES on the near future, the trader's signal: A bid
//     pressure, B ask pressure, C absorption, D withdrawal -- each LONG, SHORT
//     or off, with the numbers, from live/obi_signals.hpp, the same function
//     the trader enters on.
//   * The strikes: the nearest expiry, ATM +/- 5 (11 strikes), call and put,
//     each with its own flow engine: OBI at 5 levels and weighted over the
//     book, the touch, and which way its own book leans. A click opens one on
//     the ladder.
//   * PUNCH (paper): the suggested trade of the first active type, armed --
//     the future BUY or SELL, or the put or call to SELL -- one click; and
//     BUY / SELL at the touch for whatever is open. Options are sold only: a
//     BUY on an option only closes a short. Positions mark live; every fill is
//     appended to data/live/punch/obi_punch.csv with the readings behind it.
//     It never reaches a broker (desktop links no OMS); for a real order the
//     Order Ticket's typed confirmation stays the only way.
//   * Tabs: how a trade happens (step by step, with this instrument's numbers),
//     the formulas worked with the live numbers (OBI, OBI5, weighted OBI_N and
//     its level table, microprice, persistence, S_t), and the order-book tape
//     (executed, taken, cancelled, hidden).
//   * The OBI trader's paper record (desktop/strategy_record.hpp): every model,
//     position and round trip with its reasons.
// No charts and no time frames: the page is the book, as it changes.
//
// EVERY FRAME, NOT EVERY BURST. The grid's priceUpdated is one notice per
// burst; a flow engine diffs consecutive books and must see each one, or an
// execution reads as a cancel. The page asks PriceClient::watch_frames for the
// future, the 22 strikes and the open contract, and runs one flow::FlowEngine
// each on frameApplied, on the GUI thread, synchronously: no queue, no lock,
// nothing shared. Painting is coalesced to 10 Hz; the engines are not.
//
// These engines and the trader's (altair_live_engine) run the same code on
// the same frames; they are separate instances, so this page's start of day
// is when it opened the underlying.

#pragma once

#include "live_market.hpp"
#include "price_client.hpp"
#include "strategy_record.hpp"
#include "theme.hpp"

#include <book/flow.hpp>
#include <live/obi_signals.hpp>
#include <live/obi_underlyings.hpp>

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QTabBar>
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
#include <limits>
#include <map>
#include <memory>
#include <vector>

namespace altair::ui {

namespace order_flow_detail {

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

/// The brief's reading of an OBI value.
[[nodiscard]] inline QString obi_reading(double v) {
    if (v >= 0.30) return QStringLiteral("stronger visible bid-side depth");
    if (v <= -0.30) return QStringLiteral("stronger visible ask-side depth");
    if (std::fabs(v) <= 0.10) return QStringLiteral("balanced visible depth");
    return v > 0 ? QStringLiteral("leaning to the bid") : QStringLiteral("leaning to the ask");
}

/// One level of the OBI_N table: its weight, sizes, and the OBI summed to it.
struct LevelRow {
    int i = 0;                 ///< 1 = the touch
    double w = 0.0;
    std::int64_t bid_px = 0, b = 0, ask_px = 0, a = 0;
    double wb = 0.0, wa = 0.0;          ///< w_i B_i, w_i A_i
    double obi_to = 0.0, wobi_to = 0.0; ///< plain and weighted OBI over levels 1..i
};

/// The table for the first n levels of a book, weights e^(-decay (i-1)) --
/// the engine's own weights (flow::LevelWeights::exponential), summed in the
/// same order, so its last row equals FlowSignals::obi_deep / obi_deep_w.
[[nodiscard]] inline std::vector<LevelRow> level_rows(const flow::Level* bids, const flow::Level* asks, std::size_t n, double decay) {
    std::vector<LevelRow> out;
    const flow::LevelWeights w = flow::LevelWeights::exponential(decay);
    std::int64_t cb = 0, ca = 0;
    double wb = 0.0, wa = 0.0;
    // RULE 11: proven -- n is the engine's level count, at most flow::kMaxLevels.
    n = n < flow::kMaxLevels ? n : flow::kMaxLevels;
    for (std::size_t i = 0; i < n; ++i) {
        LevelRow r;
        r.i = static_cast<int>(i) + 1;
        r.w = w.w[i];
        r.bid_px = bids[i].px; r.b = bids[i].qty; r.ask_px = asks[i].px; r.a = asks[i].qty;
        r.wb = r.w * static_cast<double>(r.b);
        r.wa = r.w * static_cast<double>(r.a);
        cb += r.b; ca += r.a;
        wb += r.wb; wa += r.wa;
        r.obi_to = flow::imbalance(cb, ca).value_or(0.0);
        r.wobi_to = wb + wa > 0.0 ? (wb - wa) / (wb + wa) : 0.0;
        out.push_back(r);
    }
    return out;
}

/// The OBI trader's numbers for one underlying (app/live_engine_main.cpp):
/// BANKNIFTY and NIFTY in index points, a stock of config/obi_underlyings.csv in rupees
/// a share. Shown on the page; the trader decides.
struct ObiRules {
    double obi_entry = 0.30;
    int min_persist = 3;
    std::int64_t min_persist_ns = 500'000'000;
    double target_pts = 25.0, stop_pts = 15.0, safety_pts = 2.0;
};
/// `root`, when given, is read for config/obi_underlyings.csv.
[[nodiscard]] inline ObiRules rules_for(const QString& underlying, const QString& root = {}) {
    ObiRules r;
    for (const auto& u : altair::live::kObiIndices)
        if (QString::fromStdString(u.symbol) == underlying) { r.target_pts = u.target_pts; r.stop_pts = u.stop_pts; r.safety_pts = u.safety_pts; }
    if (!root.isEmpty())
        for (const auto& u : altair::live::read_obi_underlyings((root + QStringLiteral("/config/obi_underlyings.csv")).toStdString()))
            if (QString::fromStdString(u.symbol) == underlying) { r.target_pts = u.target_pts; r.stop_pts = u.stop_pts; r.safety_pts = u.safety_pts; }
    return r;
}
[[nodiscard]] inline live::ObiSignalRules signal_rules(const ObiRules& r) { return {r.obi_entry, r.min_persist, r.min_persist_ns}; }

/// The underlyings the page offers: BANKNIFTY, NIFTY, then config/obi_underlyings.csv.
[[nodiscard]] inline QStringList obi_underlyings(const QString& root) {
    QStringList out;
    for (const auto& u : altair::live::kObiIndices) out << QString::fromStdString(u.symbol);
    for (const auto& u : altair::live::read_obi_underlyings((root + QStringLiteral("/config/obi_underlyings.csv")).toStdString()))
        if (!out.contains(QString::fromStdString(u.symbol))) out << QString::fromStdString(u.symbol);
    return out;
}

/// One row of the strike grid: a strike and its call and put (0 if absent).
struct StrikeRow { double strike = 0.0; quint32 ce = 0, pe = 0; };

/// The nearest expiry's strikes of `rows` (one underlying's options), the `each`
/// either side of the one nearest `atm` (the middle of the chain when atm <= 0).
[[nodiscard]] inline std::vector<StrikeRow> strike_rows(const std::vector<const LiveRow*>& opts, double atm, int each) {
    qint64 expiry = 0;
    for (const LiveRow* r : opts) if (expiry == 0 || r->expiry_day < expiry) expiry = r->expiry_day;
    std::map<double, StrikeRow> by;
    for (const LiveRow* r : opts) {
        if (r->expiry_day != expiry) continue;
        auto& s = by[r->strike];
        s.strike = r->strike;
        (r->kind == QLatin1String("call") ? s.ce : s.pe) = r->token;
    }
    std::vector<StrikeRow> all;
    for (const auto& [k, s] : by) all.push_back(s);
    if (all.empty()) return all;
    std::size_t at = all.size() / 2;
    if (atm > 0.0)
        for (std::size_t i = 0; i < all.size(); ++i)
            if (std::fabs(all[i].strike - atm) < std::fabs(all[at].strike - atm)) at = i;
    const std::size_t lo = at >= static_cast<std::size_t>(each) ? at - static_cast<std::size_t>(each) : 0;
    const std::size_t hi = std::min(all.size(), at + static_cast<std::size_t>(each) + 1);
    return {all.begin() + static_cast<std::ptrdiff_t>(lo), all.begin() + static_cast<std::ptrdiff_t>(hi)};
}

/// A paper position of the punch panel: signed units, average price.
struct PunchPosition {
    quint32 token = 0;
    QString symbol;
    bool option = false;
    qint64 qty = 0;          ///< units, + long, - short
    double avg = 0.0;        ///< rupees
    qint64 lot = 1;
};

/// What a fill does to a position: the new position, and the rupees realised
/// by the part of it that closed. `qty` is signed (+ buy, - sell).
[[nodiscard]] inline double apply_fill(PunchPosition& p, qint64 qty, double px) {
    double realised = 0.0;
    if (p.qty == 0 || (p.qty > 0) == (qty > 0)) {   // opening or adding
        const double cost = p.avg * static_cast<double>(std::llabs(p.qty)) + px * static_cast<double>(std::llabs(qty));
        p.qty += qty;
        p.avg = p.qty != 0 ? cost / static_cast<double>(std::llabs(p.qty)) : 0.0;
        return 0.0;
    }
    const qint64 closing = std::min(std::llabs(qty), std::llabs(p.qty));
    realised = (px - p.avg) * static_cast<double>(closing) * (p.qty > 0 ? 1.0 : -1.0);
    const qint64 left = qty + p.qty;
    if (left == 0) { p.qty = 0; p.avg = 0.0; }
    else if ((left > 0) == (p.qty > 0)) p.qty = left;   // partly closed, same side
    else { p.qty = left; p.avg = px; }                    // closed and reversed
    return realised;
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
// The page
// ---------------------------------------------------------------------------

class OrderBookImbalancePage final : public QWidget {
public:
    OrderBookImbalancePage(PriceClient* client, QString root, QWidget* parent = nullptr)
        : QWidget(parent), client_(client), root_(std::move(root)) {
#ifdef ALTAIR_SOURCE_DIR
        if (root_.isEmpty()) root_ = QStringLiteral(ALTAIR_SOURCE_DIR);
#endif
        setObjectName(QStringLiteral("obiPage"));
        const QString muted = QString::fromLatin1(theme_token::kTextMuted);
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(10, 8, 10, 8);
        v->setSpacing(6);

        // The bar: the two underlyings, then the book's options.
        auto* bar = new QHBoxLayout;
        bar->setSpacing(6);
        bar->addWidget(new QLabel(QStringLiteral("<b>ORDER BOOK IMBALANCE</b>"), this));
        bar->addSpacing(10);
        group_ = new QButtonGroup(this);
        group_->setExclusive(true);
        for (const QString& u : order_flow_detail::obi_underlyings(root_)) {
            auto* b = new QPushButton(u, this);
            b->setObjectName(QStringLiteral("obiUnder_") + u);
            b->setCheckable(true);
            b->setMinimumWidth(110);
            group_->addButton(b);
            bar->addWidget(b);
            QObject::connect(b, &QPushButton::clicked, this, [this, u] { select_underlying(u); });
        }
        bar->addSpacing(12);
        rows_ = new QComboBox(this);
        rows_->setObjectName(QStringLiteral("flowRows"));
        for (const int n : {5, 10, 20, 50}) rows_->addItem(QStringLiteral("%1 levels").arg(n), n);
        rows_->setCurrentIndex(2);
        deep_ = new QCheckBox(QStringLiteral("50-level book for this contract"), this);
        deep_->setObjectName(QStringLiteral("flowDeep"));
        deep_->setToolTip(QStringLiteral(
            "Asks the feed for this contract's 50-level book (data/live/obi_watch.csv, also read by the OBI trader).\n"
            "FYERS allows 1 symbol on the Standard plan, 5 a connection and 3 connections (15) on Prime:\n"
            "start the price service with --depth50 15. Everything else keeps the 5-level book."));
        bar->addWidget(rows_);
        bar->addWidget(deep_);
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

        // Left: the ladder of the open contract.
        ladder_ = new FlowLadder;
        auto* scroll = new QScrollArea(split);
        scroll->setObjectName(QStringLiteral("flowLadderScroll"));
        scroll->setWidget(ladder_);
        scroll->setWidgetResizable(true);
        scroll->setMinimumWidth(380);
        split->addWidget(scroll);

        // Middle: the four types, the strikes, then the tabs.
        auto* mid = new QWidget(split);
        auto* mv = new QVBoxLayout(mid);
        mv->setContentsMargins(0, 0, 0, 0);
        mv->setSpacing(6);
        auto* cards = new QGridLayout;
        cards->setSpacing(6);
        for (std::size_t k = 0; k < 4; ++k) {
            auto* c = new QLabel(mid);
            c->setObjectName(QStringLiteral("obiSignal") + QString::fromLatin1(live::obi_type_letter(live::kObiTypes[k])));
            c->setTextFormat(Qt::RichText);
            c->setWordWrap(true);
            c->setMinimumHeight(58);
            c->setAlignment(Qt::AlignTop | Qt::AlignLeft);
            c->setStyleSheet(QStringLiteral("QLabel{border:1px solid %1;border-radius:4px;padding:5px;}").arg(QString::fromLatin1(theme_token::kBorder)));
            cards_[k] = c;
            cards->addWidget(c, static_cast<int>(k / 2), static_cast<int>(k % 2));
        }
        mv->addLayout(cards);
        strikes_ = make_live_table(9, {QStringLiteral("CE OBI5"), QStringLiteral("CE OBI-N w"), QStringLiteral("CE bid × ask"), QStringLiteral("CE book"),
                                       QStringLiteral("Strike"), QStringLiteral("PE book"), QStringLiteral("PE bid × ask"), QStringLiteral("PE OBI5"),
                                       QStringLiteral("PE OBI-N w")}, mid);
        strikes_->setObjectName(QStringLiteral("obiStrikes"));
        strikes_->setToolTip(QStringLiteral("Click a call or put to open it on the ladder; click the strike column for the future."));
        strikes_->setMinimumHeight(12 * 22);
        mv->addWidget(strikes_, 3);
        QObject::connect(strikes_, &QTableWidget::cellClicked, this, [this](int row, int col) { strike_clicked(row, col); });

        tabs_ = new QTabWidget(mid);
        tabs_->setObjectName(QStringLiteral("obiTabs"));
        how_ = new QLabel;
        how_->setObjectName(QStringLiteral("obiHow"));
        how_->setTextFormat(Qt::RichText);
        how_->setWordWrap(true);
        how_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        how_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        tabs_->addTab(scrolled(how_), QStringLiteral("How a trade happens"));
        {
            auto* w = new QWidget;
            auto* iv = new QVBoxLayout(w);
            iv->setContentsMargins(4, 4, 4, 4);
            calc_ = new QLabel(w);
            calc_->setObjectName(QStringLiteral("obiCalc"));
            calc_->setTextFormat(Qt::RichText);
            calc_->setWordWrap(true);
            calc_->setTextInteractionFlags(Qt::TextSelectableByMouse);
            iv->addWidget(calc_);
            levels_ = make_live_table(9, {QStringLiteral("i"), QStringLiteral("w_i"), QStringLiteral("Bid"), QStringLiteral("B_i"),
                                          QStringLiteral("w_i·B_i"), QStringLiteral("Ask"), QStringLiteral("A_i"), QStringLiteral("w_i·A_i"),
                                          QStringLiteral("OBI to i (plain / weighted)")}, w);
            levels_->setObjectName(QStringLiteral("obiLevels"));
            for (const int c : {0, 2, 5, 8}) levels_->horizontalHeader()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
            levels_->setMinimumHeight(180);
            iv->addWidget(levels_, 1);
            tabs_->addTab(scrolled(w), QStringLiteral("Formulas, live"));
        }
        {
            auto* w = new QWidget;
            auto* iv = new QVBoxLayout(w);
            iv->setContentsMargins(4, 4, 4, 4);
            auto* row = new QHBoxLayout;
            counts_ = new QLabel(w);
            counts_->setObjectName(QStringLiteral("flowCounts"));
            counts_->setTextFormat(Qt::RichText);
            counts_->setWordWrap(true);
            row->addWidget(counts_, 1);
            show_adds_ = new QCheckBox(QStringLiteral("show adds"), w);
            show_adds_->setObjectName(QStringLiteral("flowShowAdds"));
            row->addWidget(show_adds_, 0, Qt::AlignTop);
            iv->addLayout(row);
            tape_ = make_live_table(5, {QStringLiteral("Time"), QStringLiteral("Event"), QStringLiteral("Size"), QStringLiteral("Bid"), QStringLiteral("Ask")}, w);
            tape_->setObjectName(QStringLiteral("flowTape"));
            tape_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
            iv->addWidget(tape_, 1);
            tabs_->addTab(w, QStringLiteral("Order-book tape"));
        }
        // Numbers first (roadmap section 7): the tape opens, the explanation
        // is the last tab.
        tabs_->tabBar()->moveTab(2, 0);
        tabs_->tabBar()->moveTab(1, 2);
        tabs_->setCurrentIndex(0);
        mv->addWidget(tabs_, 4);
        split->addWidget(mid);

        // Right: punch, then the trader's record.
        auto* right = new QWidget(split);
        auto* rv = new QVBoxLayout(right);
        rv->setContentsMargins(0, 0, 0, 0);
        rv->setSpacing(6);
        rv->addWidget(new QLabel(QStringLiteral("<b>PUNCH</b> &nbsp;<span style='color:%1'>paper · fills at the touch · options are sold only</span>").arg(muted), right));
        suggest_ = new QLabel(right);
        suggest_->setObjectName(QStringLiteral("obiSuggest"));
        suggest_->setTextFormat(Qt::RichText);
        suggest_->setWordWrap(true);
        rv->addWidget(suggest_);
        auto* ready = new QHBoxLayout;
        ready_fut_ = new QPushButton(right);
        ready_fut_->setObjectName(QStringLiteral("obiReadyFuture"));
        ready_opt_ = new QPushButton(right);
        ready_opt_->setObjectName(QStringLiteral("obiReadyOption"));
        for (auto* b : {ready_fut_, ready_opt_}) { b->setMinimumHeight(34); ready->addWidget(b); }
        rv->addLayout(ready);
        QObject::connect(ready_fut_, &QPushButton::clicked, this, [this] { if (ready_fut_tok_ != 0) (void)punch(ready_fut_tok_, ready_fut_side_, lots_->value()); });
        QObject::connect(ready_opt_, &QPushButton::clicked, this, [this] { if (ready_opt_tok_ != 0) (void)punch(ready_opt_tok_, -1, lots_->value()); });
        auto* manual = new QHBoxLayout;
        manual->addWidget(new QLabel(QStringLiteral("Lots"), right));
        lots_ = new QSpinBox(right);
        lots_->setObjectName(QStringLiteral("obiLots"));
        lots_->setRange(1, 20);
        manual->addWidget(lots_);
        buy_ = new QPushButton(right);
        buy_->setObjectName(QStringLiteral("obiPunchBuy"));
        sell_ = new QPushButton(right);
        sell_->setObjectName(QStringLiteral("obiPunchSell"));
        buy_->setShortcut(QKeySequence(Qt::Key_F2));
        sell_->setShortcut(QKeySequence(Qt::Key_F3));
        buy_->setStyleSheet(QStringLiteral("QPushButton{background:%1;color:white;font-weight:bold;padding:6px;}").arg(QString::fromLatin1(theme_token::kUp)));
        sell_->setStyleSheet(QStringLiteral("QPushButton{background:%1;color:white;font-weight:bold;padding:6px;}").arg(QString::fromLatin1(theme_token::kDown)));
        manual->addWidget(buy_, 1);
        manual->addWidget(sell_, 1);
        rv->addLayout(manual);
        QObject::connect(buy_, &QPushButton::clicked, this, [this] { (void)punch(token_, 1, lots_->value()); });
        QObject::connect(sell_, &QPushButton::clicked, this, [this] { (void)punch(token_, -1, lots_->value()); });
        punch_status_ = new QLabel(right);
        punch_status_->setObjectName(QStringLiteral("obiPunchStatus"));
        punch_status_->setWordWrap(true);
        punch_status_->setTextFormat(Qt::RichText);
        rv->addWidget(punch_status_);
        positions_ = make_live_table(6, {QStringLiteral("Contract"), QStringLiteral("Qty"), QStringLiteral("Avg"), QStringLiteral("Mark"),
                                         QStringLiteral("P&L Rs"), QStringLiteral("")}, right);
        positions_->setObjectName(QStringLiteral("obiPunchPositions"));
        positions_->setMaximumHeight(150);
        rv->addWidget(positions_);
        auto* close_row = new QHBoxLayout;
        close_all_ = new QPushButton(QStringLiteral("Close all at the touch"), right);
        close_all_->setObjectName(QStringLiteral("obiCloseAll"));
        close_row->addWidget(close_all_);
        close_row->addStretch(1);
        rv->addLayout(close_row);
        QObject::connect(close_all_, &QPushButton::clicked, this, [this] { close_all(); });
        rv->addWidget(new QLabel(QStringLiteral("<b>OBI trader</b> &nbsp;<span style='color:%1'>paper · 4 types × futures / option-sell · live/obi_trader.hpp</span>").arg(muted), right));
        record_ = new StrategyRecordPanel(QStringLiteral("OBI"), root_, right);
        rv->addWidget(record_, 1);
        split->addWidget(right);
        split->setStretchFactor(0, 3);
        split->setStretchFactor(1, 5);
        split->setStretchFactor(2, 4);
        v->addWidget(split, 1);

        QObject::connect(rows_, &QComboBox::currentIndexChanged, this, [this] { dirty_ = true; });
        QObject::connect(show_adds_, &QCheckBox::toggled, this, [this] { tape_dirty_ = true; dirty_ = true; });
        QObject::connect(deep_, &QCheckBox::clicked, this, [this](bool on) {
            const LiveRow* r = row_of(token_);
            if (r == nullptr || r->fyers.isEmpty()) return;
            if (!order_flow_detail::set_watch(root_, r->fyers, on)) deep_->setChecked(!on);
            dirty_ = true;
        });
        if (client_ != nullptr)
            QObject::connect(client_, &PriceClient::frameApplied, this, [this](unsigned tok, int kind, qint64 ns) { on_frame(tok, kind, ns); });

        paint_.setInterval(100);
        QObject::connect(&paint_, &QTimer::timeout, this, [this] { if (dirty_ && isVisible()) repaint_all(); });
        paint_.start();
        reload_.setInterval(30'000);
        QObject::connect(&reload_, &QTimer::timeout, this, [this] { if (isVisible()) { reload_universe(); recentre(); } });
        reload_.start();
        reload_universe();
    }
    ~OrderBookImbalancePage() override { unwatch_all(); }

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
        for (auto* b : group_->buttons()) b->setEnabled(future_of(b->text()) != nullptr);
        QString want = under_;
        if (want.isEmpty() || future_of(want) == nullptr)
            for (auto* b : group_->buttons()) if (b->isEnabled()) { want = b->text(); break; }
        under_.clear();
        if (!want.isEmpty()) select_underlying(want);
    }

    /// Opens one underlying: its near future on the ladder, its strikes in the grid.
    bool select_underlying(const QString& u) {
        const LiveRow* fut = future_of(u);
        if (fut == nullptr) return false;
        for (auto* b : group_->buttons()) if (b->text() == u) b->setChecked(true);
        if (u == under_ && token_ != 0) return true;
        under_ = u;
        rules_ = order_flow_detail::rules_for(u, root_);
        unwatch_all();
        engines_.clear();
        fut_ = fut->token;
        build_strikes(0.0);
        token_ = 0;
        (void)select(fut_);
        return true;
    }

    /// Opens one contract of the underlying on the ladder; false if unknown.
    bool select(quint32 token) {
        const LiveRow* r = row_of(token);
        if (r == nullptr) return false;
        if (token == token_) return true;
        token_ = token;
        watch(token);
        lot_ = std::max<qint64>(1, r->lot);
        events_.clear();
        const flow::FlowEngine* e = engine_of(token);
        last_total_ = e != nullptr ? e->signals().total : flow::FlowTotals{};
        tape_dirty_ = true;
        ladder_->clear_history();
        deep_->setChecked(!r->fyers.isEmpty() && order_flow_detail::read_watch(root_).contains(r->fyers));
        deep_->setEnabled(!r->fyers.isEmpty());
        dirty_ = true;
        repaint_all();
        return true;
    }

    /// A paper fill at the touch: `side` +1 buy, -1 sell, `lots` lots. Returns
    /// what happened (also shown under the buttons).
    QString punch(quint32 token, int side, int lots) {
        using namespace order_flow_detail;
        const LiveRow* r = row_of(token);
        QString msg;
        const auto say = [&](const QString& t, bool ok) {
            msg = t;
            punch_status_->setText(QStringLiteral("<span style='color:%1'>%2</span>").arg(QString::fromLatin1(ok ? theme_token::kText : theme_token::kWarn), t.toHtmlEscaped()));
            return msg;
        };
        if (r == nullptr || lots <= 0) return say(QStringLiteral("nothing to trade"), false);
        const bool option = r->kind == QLatin1String("call") || r->kind == QLatin1String("put");
        auto& p = punch_[token];
        p.token = token; p.symbol = r->symbol; p.option = option; p.lot = std::max<qint64>(1, r->lot);
        const qint64 qty = side * lots * p.lot;
        // RULE: options are sold only -- a buy may only close (part of) a short.
        if (option && side > 0 && (p.qty >= 0 || qty > -p.qty)) {
            if (p.qty == 0) punch_.erase(token);
            return say(QStringLiteral("refused: BUY %1 would open a long option (options are sold only; a buy only closes a short)").arg(r->symbol), false);
        }
        const auto [bid, ask] = touch(token);
        const double px = side > 0 ? ask : bid;
        if (!(px > 0.0)) {
            if (p.qty == 0) punch_.erase(token);
            return say(QStringLiteral("no %1 for %2 yet").arg(side > 0 ? QStringLiteral("ask") : QStringLiteral("bid"), r->symbol), false);
        }
        const double realised = apply_fill(p, qty, px);
        realised_ += realised;
        const QString reason = readings_now();
        log_fill(*r, side, lots, qty, px, realised, reason);
        if (p.qty == 0) punch_.erase(token);
        dirty_ = true;
        repaint_punch();
        return say(QStringLiteral("%1 %2 lot(s) %3 at %4%5").arg(side > 0 ? QStringLiteral("BOUGHT") : QStringLiteral("SOLD")).arg(lots).arg(r->symbol, QString::number(px, 'f', 2),
                       realised != 0.0 ? QStringLiteral(" · realised Rs %1").arg(QString::number(realised, 'f', 0)) : QString()), true);
    }
    void close_all() {
        std::vector<std::pair<quint32, qint64>> open;
        for (const auto& [tok, p] : punch_) open.emplace_back(tok, p.qty);
        for (const auto& [tok, q] : open) {
            const auto it = punch_.find(tok);
            if (it == punch_.end()) continue;
            (void)punch(tok, q > 0 ? -1 : 1, static_cast<int>(std::llabs(q) / std::max<qint64>(1, it->second.lot)));
        }
    }

    // For the tests and the main window.
    [[nodiscard]] QString underlying() const { return under_; }
    [[nodiscard]] quint32 token() const noexcept { return token_; }
    [[nodiscard]] quint32 future_token() const noexcept { return fut_; }
    [[nodiscard]] const flow::FlowEngine* engine() const { return engine_of(token_); }
    [[nodiscard]] const flow::FlowEngine* engine_of(quint32 tok) const {
        const auto it = engines_.find(tok);
        return it == engines_.end() ? nullptr : it->second.get();
    }
    [[nodiscard]] std::size_t watched() const noexcept { return watched_.size(); }
    [[nodiscard]] const std::vector<order_flow_detail::StrikeRow>& strike_list() const noexcept { return strikes_list_; }
    [[nodiscard]] const std::deque<order_flow_detail::FlowEvent>& events() const noexcept { return events_; }
    [[nodiscard]] QTableWidget* tape() const noexcept { return tape_; }
    [[nodiscard]] QTableWidget* strikes_table() const noexcept { return strikes_; }
    [[nodiscard]] QTableWidget* positions_table() const noexcept { return positions_; }
    [[nodiscard]] FlowLadder* ladder() const noexcept { return ladder_; }
    [[nodiscard]] QString head_text() const { return head_->text(); }
    [[nodiscard]] QString counts_text() const { return counts_->text(); }
    [[nodiscard]] QString imbalance_text() const { return calc_->text(); }
    [[nodiscard]] QString how_text() const { return how_->text(); }
    [[nodiscard]] QString card_text(std::size_t k) const { return k < cards_.size() ? cards_[k]->text() : QString(); }
    [[nodiscard]] QString suggest_text() const { return suggest_->text(); }
    [[nodiscard]] QPushButton* ready_future() const noexcept { return ready_fut_; }
    [[nodiscard]] QPushButton* ready_option() const noexcept { return ready_opt_; }
    [[nodiscard]] const std::map<quint32, order_flow_detail::PunchPosition>& punch_positions() const noexcept { return punch_; }
    [[nodiscard]] double realised() const noexcept { return realised_; }
    [[nodiscard]] QTableWidget* levels_table() const noexcept { return levels_; }
    [[nodiscard]] QTabWidget* tabs() const noexcept { return tabs_; }
    void repaint_now() { dirty_ = true; repaint_all(); }

private:
    [[nodiscard]] static QScrollArea* scrolled(QWidget* w) {
        auto* sc = new QScrollArea;
        sc->setWidgetResizable(true);
        sc->setFrameShape(QFrame::NoFrame);
        sc->setWidget(w);
        return sc;
    }
    [[nodiscard]] const LiveRow* row_of(quint32 tok) const {
        for (const auto& r : universe_) if (r.token == tok) return &r;
        return nullptr;
    }
    /// The nearest future of `u` in the universe.
    [[nodiscard]] const LiveRow* future_of(const QString& u) const {
        const LiveRow* best = nullptr;
        for (const auto& r : universe_)
            if (r.kind == QLatin1String("future") && r.underlying == u && (best == nullptr || r.expiry_day < best->expiry_day)) best = &r;
        return best;
    }
    [[nodiscard]] bool is_option(quint32 tok) const {
        const LiveRow* r = row_of(tok);
        return r != nullptr && (r->kind == QLatin1String("call") || r->kind == QLatin1String("put"));
    }
    /// The underlying's level: the future's mid.
    [[nodiscard]] double atm_level() const {
        const flow::FlowEngine* f = engine_of(fut_);
        if (f != nullptr && f->signals().valid) return f->signals().mid / 100.0;
        if (client_ != nullptr)
            if (const LivePrice* p = client_->price(fut_); p != nullptr && p->last_paise > 0) return static_cast<double>(p->last_paise) / 100.0;
        return 0.0;
    }
    /// The touch of `tok` in rupees: the engine's, else the client's quote.
    [[nodiscard]] std::pair<double, double> touch(quint32 tok) const {
        if (const flow::FlowEngine* e = engine_of(tok); e != nullptr && e->signals().valid)
            return {static_cast<double>(e->signals().bid) / 100.0, static_cast<double>(e->signals().ask) / 100.0};
        if (client_ != nullptr)
            if (const LivePrice* p = client_->price(tok); p != nullptr) {
                if (p->levels > 0) return {static_cast<double>(p->bids[0].price_paise) / 100.0, static_cast<double>(p->asks[0].price_paise) / 100.0};
                if (p->has_quote) return {static_cast<double>(p->quote.bid) / 100.0, static_cast<double>(p->quote.ask) / 100.0};
            }
        return {0.0, 0.0};
    }

    void build_strikes(double atm) {
        std::vector<const LiveRow*> opts;
        for (const auto& r : universe_)
            if (r.underlying == under_ && (r.kind == QLatin1String("call") || r.kind == QLatin1String("put"))) opts.push_back(&r);
        strikes_list_ = order_flow_detail::strike_rows(opts, atm, kStrikesEach);
        watch(fut_);
        for (const auto& s : strikes_list_) { if (s.ce != 0) watch(s.ce); if (s.pe != 0) watch(s.pe); }
        strikes_->setRowCount(static_cast<int>(strikes_list_.size()));
        atm_strike_ = 0.0;
    }
    /// Every 30 s: the strikes follow the money (contracts that leave stay watched while open).
    void recentre() {
        if (strikes_list_.empty()) return;
        const double atm = atm_level();
        if (!(atm > 0.0)) return;
        const auto& mid = strikes_list_[strikes_list_.size() / 2];
        const double step = strikes_list_.size() > 1 ? strikes_list_[1].strike - strikes_list_[0].strike : 0.0;
        if (step > 0.0 && std::fabs(atm - mid.strike) > 0.75 * step) {
            build_strikes(atm);
            dirty_ = true;
        }
    }

    /// Starts `tok`'s frames and its engine, primed with the book as it stands.
    void watch(quint32 tok) {
        if (tok == 0 || engines_.count(tok) != 0) return;
        const LiveRow* r = row_of(tok);
        flow::FlowConfig c;
        c.tick = std::max<std::int64_t>(1, std::llround((r != nullptr ? r->tick : 0.05) * 100.0));
        auto& e = engines_[tok];
        e = std::make_unique<flow::FlowEngine>(c);
        if (client_ == nullptr) return;
        client_->watch_frames(tok, true);
        watched_.push_back(tok);
        if (const LivePrice* p = client_->price(tok)) {
            if (p->levels > 0) on_frame(tok, PriceClient::AppliedBook, p->book_ns);
            else if (p->has_quote) on_frame(tok, PriceClient::AppliedQuote, p->quote_ns);
            // The day's volume so far is the baseline the next print is measured from.
            if (p->has_volume && p->volume > 0) e->on_trade(p->last_paise, p->volume, 0, p->last_ns);
        }
    }
    void unwatch_all() {
        if (client_ != nullptr)
            for (const quint32 t : watched_) client_->watch_frames(t, false);
        watched_.clear();
    }

    void strike_clicked(int row, int col) {
        if (row < 0 || row >= static_cast<int>(strikes_list_.size())) return;
        const auto& s = strikes_list_[static_cast<std::size_t>(row)];
        const quint32 tok = col < 4 ? s.ce : col > 4 ? s.pe : fut_;
        if (tok != 0) (void)select(tok);
    }

    /// Every frame of a watched contract: into its engine, at once.
    void on_frame(unsigned tok, int kind, qint64 ns) {
        const auto it = engines_.find(tok);
        if (it == engines_.end() || client_ == nullptr) return;
        const LivePrice* p = client_->price(tok);
        if (p == nullptr) return;
        flow::FlowEngine& e = *it->second;
        const auto t0 = std::chrono::steady_clock::now();
        if (kind == PriceClient::AppliedTrade) {
            e.on_trade(p->last_paise, p->has_volume ? p->volume : -1, p->last_qty, ns);
        } else if (kind == PriceClient::AppliedBook) {
            if (p->levels == 0) return;
            // RULE 11: proven -- PriceClient keeps kMaxDepthLevels (50) a side and
            // depth_levels never exceeds it (server/price_payload.hpp).
            const std::size_t n = p->levels < kMaxDepthLevels ? p->levels : kMaxDepthLevels;
            for (std::size_t i = 0; i < n; ++i) {
                bids_[i] = flow::Level{p->bids[i].price_paise, p->bids[i].qty};
                asks_[i] = flow::Level{p->asks[i].price_paise, p->asks[i].qty};
            }
            e.on_book(bids_.data(), n, asks_.data(), n, ns);
            if (tok == token_) { book_n_ = n; sel_bids_ = bids_; sel_asks_ = asks_; }
        } else {
            if (p->levels > 0) return;   // the depth frames are the book; a quote between them is not a new one
            const flow::Level b{p->quote.bid, p->quote.bid_qty}, a{p->quote.ask, p->quote.ask_qty};
            e.on_book(&b, 1, &a, 1, ns);
            if (tok == token_) { book_n_ = 1; sel_bids_[0] = b; sel_asks_[0] = a; }
        }
        const auto spent = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
        work_ns_ += spent;
        work_max_ns_ = std::max<qint64>(work_max_ns_, spent);
        ++updates_;
        if (tok == token_) {
            const flow::FlowSignals& s = e.signals();
            for (const auto& ev : order_flow_detail::events_between(last_total_, s.total, ns, s.bid, s.ask)) {
                events_.push_front(ev);
                tape_dirty_ = true;
            }
            last_total_ = s.total;
            // RULE 11: counted -- the tape keeps the newest kTapeKeep events; the counters keep every one.
            while (events_.size() > kTapeKeep) events_.pop_back();
        }
        dirty_ = true;
    }

    /// The readings behind a punch, for its log line.
    [[nodiscard]] QString readings_now() const {
        QStringList out;
        const flow::FlowEngine* f = engine_of(fut_);
        if (f == nullptr || !f->signals().valid) return QStringLiteral("no future book");
        const auto& s = f->signals();
        out << QStringLiteral("fut OBI5 %1 OBI-N w %2 micro %3 t").arg(order_flow_detail::signed2(s.obi_short), order_flow_detail::signed2(s.obi_deep_w),
                                                                       order_flow_detail::signed2(s.micro_offset_ticks));
        for (const auto t : live::kObiTypes) {
            std::string why;
            const int d = live::obi_signal(t, s, order_flow_detail::signal_rules(rules_), why);
            if (d != 0) out << QStringLiteral("%1 %2: %3").arg(QString::fromLatin1(live::obi_type_letter(t)), d > 0 ? QStringLiteral("LONG") : QStringLiteral("SHORT"),
                                                                QString::fromStdString(why));
        }
        if (out.size() == 1) out << QStringLiteral("no trade type active (manual)");
        return out.join(QStringLiteral("; "));
    }
    void log_fill(const LiveRow& r, int side, int lots, qint64 qty, double px, double realised, const QString& reason) {
        const QString dir = root_ + QStringLiteral("/data/live/punch");
        QDir().mkpath(dir);
        QFile f(dir + QStringLiteral("/obi_punch.csv"));
        const bool fresh = !f.exists();
        if (!f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) return;
        QTextStream out(&f);
        if (fresh) out << "time,symbol,side,lots,qty,price,realised,reason\n";
        QString why = reason;
        why.replace(QLatin1Char('"'), QLatin1Char('\''));
        out << QDateTime::currentDateTime().toString(Qt::ISODate) << ',' << r.symbol << ',' << (side > 0 ? "BUY" : "SELL") << ',' << lots << ','
            << std::llabs(qty) << ',' << QString::number(px, 'f', 2) << ',' << QString::number(realised, 'f', 2) << ",\"" << why << "\"\n";
    }

    void repaint_punch() {
        using namespace order_flow_detail;
        double open = 0.0;
        int row = 0;
        positions_->setRowCount(static_cast<int>(punch_.size()));
        for (const auto& [tok, p] : punch_) {
            const auto [bid, ask] = touch(tok);
            const double mark = bid > 0.0 && ask > 0.0 ? 0.5 * (bid + ask) : 0.0;
            const double pnl = mark > 0.0 ? (mark - p.avg) * static_cast<double>(p.qty) : 0.0;
            open += pnl;
            live_set(positions_, row, 0, p.symbol);
            live_set(positions_, row, 1, QString::number(p.qty), QColor(p.qty > 0 ? theme_token::kUp : theme_token::kDown));
            live_set(positions_, row, 2, QString::number(p.avg, 'f', 2));
            live_set(positions_, row, 3, mark > 0.0 ? QString::number(mark, 'f', 2) : QStringLiteral("—"));
            live_set(positions_, row, 4, QString::number(pnl, 'f', 0), QColor(pnl >= 0 ? theme_token::kUp : theme_token::kDown));
            live_set(positions_, row, 5, p.option ? QStringLiteral("sold option") : QStringLiteral("future"));
            ++row;
        }
        close_all_->setEnabled(!punch_.empty());
        close_all_->setText(QStringLiteral("Close all at the touch · open Rs %1 · realised Rs %2 (gross, before charges)")
                                .arg(QString::number(open, 'f', 0), QString::number(realised_, 'f', 0)));
    }

    void repaint_all() {
        using namespace order_flow_detail;
        dirty_ = false;
        const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
        const LivePrice* p = client_ != nullptr && token_ != 0 ? client_->price(token_) : nullptr;
        const LiveRow* r = row_of(token_);
        const flow::FlowEngine* eng = engine_of(token_);
        const flow::FlowSignals* s = eng != nullptr ? &eng->signals() : nullptr;
        const bool valid = s != nullptr && s->valid;
        ladder_->set_book(p, rows_->currentData().toInt(), valid ? s->microprice.value() : 0.0, now_ms);
        const QString up = QString::fromLatin1(theme_token::kUp), down = QString::fromLatin1(theme_token::kDown),
                      muted = QString::fromLatin1(theme_token::kTextMuted), faint = QString::fromLatin1(theme_token::kTextFaint);

        // The headline.
        {
            QString h;
            const QString sym = r != nullptr ? r->symbol : QStringLiteral("—");
            if (!valid) {
                h = QStringLiteral("<b>%1</b> &nbsp; <span style='color:%2'>%3</span>")
                        .arg(sym.toHtmlEscaped(), faint,
                             client_ == nullptr || !client_->connected() ? QStringLiteral("the feed is not connected (Terminal starts it)") : QStringLiteral("no tradable book yet"));
            } else {
                const int lv = static_cast<int>(std::min(s->bid_levels, s->ask_levels));
                const QString src = lv > 5 ? QStringLiteral("%1 levels (TBT)").arg(lv) : lv <= 1 ? QStringLiteral("touch only") : QStringLiteral("%1 levels").arg(lv);
                const qint64 age_ms = p != nullptr && p->book_ns > 0 ? now_ms - p->book_ns / 1'000'000 : -1;
                QString flag;
                if (p != nullptr && p->simulated) flag = QStringLiteral(" &nbsp;<b style='color:%1'>SIM</b>").arg(QString::fromLatin1(theme_token::kWarn));
                else if (p != nullptr && p->replay) flag = QStringLiteral(" &nbsp;<b style='color:%1'>REPLAY</b>").arg(QString::fromLatin1(theme_token::kWarn));
                h = QStringLiteral("<b>%1</b>%2 &nbsp; <span style='color:%3'>bid</span> <b>%4</b> × %5 &nbsp; <span style='color:%6'>ask</span> <b>%7</b> × %8 &nbsp; "
                                   "mid %9 &nbsp; <span style='color:%10'>micro %11 (%12 t)</span> &nbsp; spread %13 t &nbsp; · %14%15")
                        .arg(sym.toHtmlEscaped(), flag, up, rupees(static_cast<double>(s->bid)), units(s->bid_qty), down, rupees(static_cast<double>(s->ask)),
                             units(s->ask_qty), rupees(s->mid, 3))
                        .arg(QString::fromLatin1(theme_token::kAccent), rupees(s->microprice.value(), 3), signed2(s->micro_offset_ticks),
                             QString::number(s->spread_ticks, 'f', 0), src,
                             age_ms < 0 ? QString()
                             : age_ms <= 60'000 ? QStringLiteral(" · book %1 s ago").arg(QString::number(static_cast<double>(age_ms) / 1000.0, 'f', 1))
                                                : QStringLiteral(" · <b style='color:%1'>book from %2</b>").arg(QString::fromLatin1(theme_token::kWarn), live_detail::ist(p->book_ns, false)));
            }
            head_->setText(h);
        }

        // The four types, on the future.
        const flow::FlowEngine* fe = engine_of(fut_);
        const flow::FlowSignals* fs = fe != nullptr ? &fe->signals() : nullptr;
        const LiveRow* fr = row_of(fut_);
        int first_dir = 0;
        live::ObiType first_type = live::ObiType::A;
        for (std::size_t k = 0; k < 4; ++k) {
            const auto t = live::kObiTypes[k];
            std::string why;
            const int d = fs != nullptr ? live::obi_signal(t, *fs, signal_rules(rules_), why) : 0;
            if (fs == nullptr) why = "no book for the future yet";
            if (d != 0 && first_dir == 0) { first_dir = d; first_type = t; }
            cards_[k]->setText(QStringLiteral("<b>%1 · %2</b> &nbsp; <b style='color:%3'>%4</b><br><span style='color:%5'>%6</span>")
                                   .arg(QString::fromLatin1(live::obi_type_letter(t)), QString::fromLatin1(live::obi_type_name(t)),
                                        d > 0 ? up : d < 0 ? down : faint, d > 0 ? QStringLiteral("LONG ▲") : d < 0 ? QStringLiteral("SHORT ▼") : QStringLiteral("off"),
                                        d != 0 ? QString::fromLatin1(theme_token::kText) : muted, QString::fromStdString(why).toHtmlEscaped()));
        }

        // The strikes.
        for (std::size_t i = 0; i < strikes_list_.size(); ++i) {
            const auto& sr = strikes_list_[i];
            const int k = static_cast<int>(i);
            const auto side = [&](quint32 tok, int c_obi, int c_w, int c_px, int c_lean) {
                const flow::FlowEngine* e = engine_of(tok);
                const flow::FlowSignals* x = e != nullptr ? &e->signals() : nullptr;
                const bool ok = x != nullptr && x->valid;
                const auto colour = [&](double v) { return QColor(v >= rules_.obi_entry ? theme_token::kUp : v <= -rules_.obi_entry ? theme_token::kDown : theme_token::kTextMuted); };
                live_set(strikes_, k, c_obi, ok ? signed2(x->obi_short) : QStringLiteral("—"), ok ? colour(x->obi_short) : QColor(theme_token::kTextFaint));
                live_set(strikes_, k, c_w, ok ? signed2(x->obi_deep_w) : QStringLiteral("—"), ok ? colour(x->obi_deep_w) : QColor(theme_token::kTextFaint));
                live_set(strikes_, k, c_px, ok ? QStringLiteral("%1 × %2").arg(rupees(static_cast<double>(x->bid)), rupees(static_cast<double>(x->ask))) : QStringLiteral("—"));
                live_set(strikes_, k, c_lean, !ok ? QString() : x->obi_short >= rules_.obi_entry ? QStringLiteral("bid-heavy") : x->obi_short <= -rules_.obi_entry ? QStringLiteral("ask-heavy") : QStringLiteral("balanced"),
                         ok ? colour(x->obi_short) : QColor(theme_token::kTextFaint));
                if (tok == token_ && tok != 0)
                    for (const int c : {c_obi, c_w, c_px, c_lean}) if (auto* it = strikes_->item(k, c)) it->setBackground(tint(theme_token::kAccent, 60));
            };
            side(sr.ce, 0, 1, 2, 3);
            live_set(strikes_, k, 4, QString::number(sr.strike, 'f', sr.strike == std::floor(sr.strike) ? 0 : 1), QColor(theme_token::kText));
            side(sr.pe, 7, 8, 6, 5);
        }

        // Punch: the suggestion, armed.
        {
            ready_fut_tok_ = ready_opt_tok_ = 0;
            const QString fsym = fr != nullptr ? fr->symbol : QStringLiteral("the future");
            if (first_dir != 0 && fr != nullptr) {
                ready_fut_tok_ = fut_;
                ready_fut_side_ = first_dir;
                const auto [fb, fa] = touch(fut_);
                ready_fut_->setText(QStringLiteral("%1 %2 @ %3").arg(first_dir > 0 ? QStringLiteral("BUY") : QStringLiteral("SELL"), fsym,
                                                                    QString::number(first_dir > 0 ? fa : fb, 'f', 2)));
                // A long view sells a put one strike below the money, a short view a call one above.
                const double atm = atm_level();
                const order_flow_detail::StrikeRow* pick = nullptr;
                double want = atm;
                if (strikes_list_.size() > 1) want += (first_dir > 0 ? -1.0 : 1.0) * (strikes_list_[1].strike - strikes_list_[0].strike);
                for (const auto& sr : strikes_list_)
                    if ((first_dir > 0 ? sr.pe : sr.ce) != 0 && (pick == nullptr || std::fabs(sr.strike - want) < std::fabs(pick->strike - want))) pick = &sr;
                if (pick != nullptr) {
                    ready_opt_tok_ = first_dir > 0 ? pick->pe : pick->ce;
                    const LiveRow* orow = row_of(ready_opt_tok_);
                    ready_opt_->setText(QStringLiteral("SELL %1 @ %2").arg(orow != nullptr ? orow->symbol : QStringLiteral("?"), QString::number(touch(ready_opt_tok_).first, 'f', 2)));
                }
                suggest_->setText(QStringLiteral("<b style='color:%1'>%2 %3</b> on %4 (%5). Armed — one click:")
                                      .arg(first_dir > 0 ? up : down, QString::fromLatin1(live::obi_type_letter(first_type)), first_dir > 0 ? QStringLiteral("LONG") : QStringLiteral("SHORT"),
                                           fsym, QString::fromLatin1(live::obi_type_name(first_type))));
            } else {
                suggest_->setText(QStringLiteral("<span style='color:%1'>No trade type is active on %2. The buttons arm themselves when one is.</span>").arg(muted, fsym));
            }
            if (ready_fut_tok_ == 0) ready_fut_->setText(QStringLiteral("future: nothing armed"));
            if (ready_opt_tok_ == 0) ready_opt_->setText(QStringLiteral("option to sell: nothing armed"));
            ready_fut_->setEnabled(ready_fut_tok_ != 0);
            ready_opt_->setEnabled(ready_opt_tok_ != 0);
            const auto [b, a] = touch(token_);
            const QString sym = r != nullptr ? r->symbol : QString();
            const bool opt = is_option(token_);
            const auto pit = punch_.find(token_);
            const bool can_buy = !opt || (pit != punch_.end() && pit->second.qty < 0);
            buy_->setText(QStringLiteral("BUY  %1 @ %2  (F2)").arg(sym, a > 0.0 ? QString::number(a, 'f', 2) : QStringLiteral("—")));
            sell_->setText(QStringLiteral("SELL  %1 @ %2  (F3)").arg(sym, b > 0.0 ? QString::number(b, 'f', 2) : QStringLiteral("—")));
            buy_->setEnabled(token_ != 0 && can_buy);
            buy_->setToolTip(can_buy ? QString() : QStringLiteral("Options are sold only: BUY is for closing a short."));
            sell_->setEnabled(token_ != 0);
            repaint_punch();
        }

        fill_how(fs, fr);
        fill_imbalance(s, valid);

        // The tape and its counters.
        if (s != nullptr) {
            const auto& t = s->total;
            counts_->setText(QStringLiteral("<b>Since opened</b> &nbsp; executed: <span style='color:%1'>buy %2</span> · <span style='color:%3'>sell %4</span> &nbsp;|&nbsp; "
                                            "taken: ask %5 · bid %6 &nbsp;|&nbsp; cancelled: ask %7 · bid %8 &nbsp;|&nbsp; hidden: buy %9 · sell %10<br>"
                                            "<span style='color:%11'>engines: %12 contracts · %13 µs an update (max %14 µs)</span>")
                                 .arg(up, units(t.buy), down, units(t.sell), units(t.ask_exec_drop), units(t.bid_exec_drop), units(t.ask_cancel), units(t.bid_cancel))
                                 .arg(units(t.hidden_buy), units(t.hidden_sell), faint)
                                 .arg(static_cast<int>(engines_.size()))
                                 .arg(QString::number(updates_ > 0 ? static_cast<double>(work_ns_) / static_cast<double>(updates_) / 1000.0 : 0.0, 'f', 2),
                                      QString::number(static_cast<double>(work_max_ns_) / 1000.0, 'f', 1)));
        } else {
            counts_->setText(QString());
        }
        if (tape_dirty_) {
            tape_dirty_ = false;
            const bool adds = show_adds_->isChecked();
            int row = 0;
            for (const auto& e : events_) {
                if (!adds && (e.kind == AskAdd || e.kind == BidAdd)) continue;
                if (row >= kTapeRows) break;
                if (row >= tape_->rowCount()) tape_->insertRow(row);
                const int lean = event_lean(e.kind);
                const bool cancel = e.kind == AskCancel || e.kind == BidCancel;
                const QColor c = cancel ? QColor(theme_token::kWarn) : lean > 0 ? QColor(theme_token::kUp) : lean < 0 ? QColor(theme_token::kDown) : QColor(theme_token::kTextMuted);
                live_set(tape_, row, 0, live_detail::ist(e.ns, true));
                live_set(tape_, row, 1, event_name(e.kind), c);
                live_set(tape_, row, 2, units(e.qty), c);
                live_set(tape_, row, 3, rupees(static_cast<double>(e.bid)));
                live_set(tape_, row, 4, rupees(static_cast<double>(e.ask)));
                if (auto* it = tape_->item(row, 1)) it->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
                ++row;
            }
            tape_->setRowCount(row);
        }
    }

    /// How a trade happens, with this underlying's numbers.
    void fill_how(const flow::FlowSignals* fs, const LiveRow* fr) {
        using namespace order_flow_detail;
        const QString acc = QString::fromLatin1(theme_token::kAccent), muted = QString::fromLatin1(theme_token::kTextMuted);
        const bool stock = !altair::live::obi_is_index(under_.toStdString());
        const QString unit = stock ? QStringLiteral("rupees a share") : QStringLiteral("index points");
        const qint64 lot = fr != nullptr ? fr->lot : 1;
        const QString fsym = fr != nullptr ? fr->symbol : QStringLiteral("the near future");
        const auto step = [&](int n, const QString& head, const QString& body) {
            return QStringLiteral("<div style='margin-top:8px'><b style='color:%1'>%2. %3</b><br>%4</div>").arg(acc).arg(n).arg(head, body);
        };
        QStringList h;
        h << QStringLiteral("<b>How a trade happens</b> — %1, paper only. The OBI trader (in altair_live_engine) does this on every book and print; "
                            "the PUNCH panel lets you do it by hand.").arg(under_);
        h << step(1, QStringLiteral("The book arrives"),
                  QStringLiteral("FYERS sends %1's book — 50 levels while it holds a TBT slot (Prime: 15 slots, --depth50 15), else 5 — and every trade print. "
                                 "One flow engine per contract (book/flow.hpp) turns it into OBI, OBI-N, microprice, OFI, executed vs cancelled vs hidden size. "
                                 "On this page: %2 µs an update.")
                      .arg(fsym, QString::number(updates_ > 0 ? static_cast<double>(work_ns_) / static_cast<double>(updates_) / 1000.0 : 0.0, 'f', 2)));
        QString types;
        for (const auto t : live::kObiTypes) {
            std::string why;
            const int d = fs != nullptr ? live::obi_signal(t, *fs, signal_rules(rules_), why) : 0;
            types += QStringLiteral("<br>&nbsp;&nbsp;<b>%1 %2</b>: %3 — now <b>%4</b>").arg(QString::fromLatin1(live::obi_type_letter(t)), QString::fromLatin1(live::obi_type_name(t)),
                         t == live::ObiType::A ? QStringLiteral("OBI5 ≥ +%1 held %2 updates and %3 s, more buying than selling → LONG").arg(QString::number(rules_.obi_entry, 'f', 2)).arg(rules_.min_persist).arg(QString::number(static_cast<double>(rules_.min_persist_ns) / 1e9, 'f', 1))
                         : t == live::ObiType::B ? QStringLiteral("the mirror: OBI5 ≤ −%1 held, more selling → SHORT").arg(QString::number(rules_.obi_entry, 'f', 2))
                         : t == live::ObiType::C ? QStringLiteral("heavy selling into bids that refill while the bid holds → LONG (mirror → SHORT): the reversal")
                                                 : QStringLiteral("asks pulled while buyers lift them → LONG (mirror → SHORT): the breakout"),
                         d > 0 ? QStringLiteral("LONG") : d < 0 ? QStringLiteral("SHORT") : QStringLiteral("off"));
        }
        h << step(2, QStringLiteral("One of four trade types fires, on the future (futures lead options)"),
                  QStringLiteral("Each type is its own model with its own paper record:") + types);
        h << step(3, QStringLiteral("One check: the target pays for the trip"),
                  QStringLiteral("target %1 %2 − round-trip charges (config/charges.toml) − one spread − safety %3 must stay above 0. Nothing else filters the signal.")
                      .arg(QString::number(rules_.target_pts, 'f', 2), unit, QString::number(rules_.safety_pts, 'f', 2)));
        h << step(4, QStringLiteral("The order"),
                  QStringLiteral("<b>Futures model</b>: BUY (long) or SELL (short) one lot of %1 (%2 units) at the touch.<br>"
                                 "<b>Option model</b>: a LONG view SELLS a put one strike below the money; a SHORT view SELLS a call one strike above. "
                                 "Never a bought option. Skipped if that option's own book is bid-heavy (OBI5 ≥ +%3) or its spread is over 4 ticks.")
                      .arg(fsym).arg(lot).arg(QString::number(rules_.obi_entry, 'f', 2)));
        h << step(5, QStringLiteral("The exit, whichever comes first"),
                  QStringLiteral("target +%1 / stop −%2 %3 (options: premium −15 %% / +25 %%) · a loss of Rs 2,000 · for A and B, the pressure reversing "
                                 "(OBI5 held the other way) · 5 minutes · 15:20 square-off.")
                      .arg(QString::number(rules_.target_pts, 'f', 2), QString::number(rules_.stop_pts, 'f', 2), unit));
        h << step(6, QStringLiteral("The limits"),
                  QStringLiteral("each model: Rs 5,000 a day, then it stops; 30 s pause after a loss; 40 trades a day; entries 09:16–15:00. Paper: no real order is ever sent."));
        h << step(7, QStringLiteral("Where you see it"),
                  QStringLiteral("right column: the OBI trader's models, open positions and every round trip with its reason. PUNCH: your own paper trades, "
                                 "logged to data/live/punch/obi_punch.csv with the readings behind each."));
        h << QStringLiteral("<div style='color:%1;margin-top:8px'>The thresholds are a starting framework, not validated: the paper record of each type is the test.</div>").arg(muted);
        how_->setText(h.join(QString()));
    }

    /// The formulas tab: each formula with the numbers of the book the engine read.
    void fill_imbalance(const flow::FlowSignals* s, bool valid) {
        using namespace order_flow_detail;
        const flow::FlowEngine* eng = engine_of(token_);
        if (!valid || eng == nullptr) {
            calc_->setText(QStringLiteral("<span style='color:%1'>Waiting for a tradable book. The formulas fill in on the first one.</span>").arg(QString::fromLatin1(theme_token::kTextFaint)));
            levels_->setRowCount(0);
            return;
        }
        const auto& cfg = eng->config();
        const QString up = QString::fromLatin1(theme_token::kUp), down = QString::fromLatin1(theme_token::kDown),
                      muted = QString::fromLatin1(theme_token::kTextMuted), acc = QString::fromLatin1(theme_token::kAccent);
        const auto val = [&](double v) {
            if (v >= cfg.threshold || v <= -cfg.threshold) return QStringLiteral("<b style='color:%1'>%2</b>").arg(v > 0 ? up : down, signed2(v));
            return QStringLiteral("<b>%1</b>").arg(signed2(v));
        };
        const std::size_t n_deep = std::min<std::size_t>({cfg.deep_levels, s->bid_levels, s->ask_levels, book_n_});
        const auto rows = level_rows(sel_bids_.data(), sel_asks_.data(), n_deep, cfg.decay);
        const std::size_t n5 = std::min<std::size_t>(cfg.short_levels, rows.size());
        std::int64_t b5 = 0, a5 = 0;
        for (std::size_t i = 0; i < n5; ++i) { b5 += rows[i].b; a5 += rows[i].a; }
        double swb = 0.0, swa = 0.0;
        std::int64_t sb = 0, sa = 0;
        for (const auto& r : rows) { swb += r.wb; swa += r.wa; sb += r.b; sa += r.a; }
        QStringList h;
        const QString sec = QStringLiteral("<div style='margin-top:6px'><b style='color:%1'>%2</b></div>").arg(acc);
        h << sec.arg(QStringLiteral("1. Order Book Imbalance — at the touch"));
        h << QStringLiteral("OBI = (Q<sub>bid</sub> − Q<sub>ask</sub>) / (Q<sub>bid</sub> + Q<sub>ask</sub>) = (%1 − %2) / (%1 + %2) = %3 &nbsp; → %4")
                 .arg(units(s->bid_qty), units(s->ask_qty), val(s->obi1), obi_reading(s->obi1));
        h << sec.arg(QStringLiteral("2. Five-level OBI (types A and B)"));
        h << QStringLiteral("OBI<sub>5</sub> = (ΣB<sub>1..%1</sub> − ΣA<sub>1..%1</sub>) / (ΣB + ΣA) = (%2 − %3) / (%2 + %3) = %4 &nbsp; → %5")
                 .arg(static_cast<int>(n5)).arg(units(b5), units(a5), val(s->obi_short), obi_reading(s->obi_short));
        h << sec.arg(QStringLiteral("3. Multi-level weighted OBI<sub>N</sub> — N = %1, w<sub>i</sub> = e<sup>−λ(i−1)</sup>, λ = %2")
                         .arg(static_cast<int>(rows.size())).arg(QString::number(cfg.decay, 'f', 2)));
        h << QStringLiteral("OBI<sub>N</sub> = (Σw<sub>i</sub>B<sub>i</sub> − Σw<sub>i</sub>A<sub>i</sub>) / (Σw<sub>i</sub>B<sub>i</sub> + Σw<sub>i</sub>A<sub>i</sub>) = "
                            "(%1 − %2) / (%1 + %2) = %3 &nbsp; → %4<br><span style='color:%5'>Unweighted over the same %6 levels: (%7 − %8) / (%7 + %8) = %9 "
                            "— the weights keep far-away size from dominating. Table below, level by level.</span>")
                 .arg(QString::number(swb, 'f', 1), QString::number(swa, 'f', 1), val(s->obi_deep_w), obi_reading(s->obi_deep_w), muted)
                 .arg(static_cast<int>(rows.size())).arg(units(sb), units(sa), signed2(s->obi_deep));
        h << sec.arg(QStringLiteral("4. Microprice"));
        const double mp = s->microprice.value(), mid = s->mid;
        h << QStringLiteral("P<sub>micro</sub> = (A<sub>1</sub>·P<sub>bid</sub> + B<sub>1</sub>·P<sub>ask</sub>) / (B<sub>1</sub> + A<sub>1</sub>) = "
                            "(%1 × %2 + %3 × %4) / (%3 + %1) = <b style='color:%5'>%6</b><br>mid = (%2 + %4) / 2 = %7 &nbsp; → microprice %8 the mid by %9 ticks: %10")
                 .arg(units(s->ask_qty), rupees(static_cast<double>(s->bid)), units(s->bid_qty), rupees(static_cast<double>(s->ask)), acc, rupees(mp, 4), rupees(mid, 3))
                 .arg(mp > mid ? QStringLiteral("above") : mp < mid ? QStringLiteral("below") : QStringLiteral("at"), signed2(s->micro_offset_ticks),
                      mp > mid ? QStringLiteral("short-term upward bias (more size behind the bid pulls the fair price toward the ask)")
                      : mp < mid ? QStringLiteral("short-term downward bias (more size behind the ask pulls it toward the bid)") : QStringLiteral("no bias"));
        h << sec.arg(QStringLiteral("5. Imbalance persistence"));
        h << (s->persist != 0
                  ? QStringLiteral("OBI<sub>5</sub> has stayed %1 %2 for <b>%3 updates</b> over %4 s (EWMA %5).")
                        .arg(s->persist > 0 ? QStringLiteral("above +") : QStringLiteral("below −"), QString::number(cfg.threshold, 'f', 2))
                        .arg(std::abs(s->persist)).arg(QString::number(static_cast<double>(s->persist_ns) / 1e9, 'f', 1), signed2(s->obi_ewma))
                  : QStringLiteral("OBI<sub>5</sub> is inside ±%1: no persistent imbalance (EWMA %2).").arg(QString::number(cfg.threshold, 'f', 2), signed2(s->obi_ewma)));
        h << sec.arg(QStringLiteral("6. Signal S<sub>t</sub> = α·OBI<sub>t</sub> + β·OFI<sub>t</sub> + γ·M<sub>t</sub>"));
        h << QStringLiteral("S = %1 × %2 (weighted OBI<sub>N</sub>) + %3 × %4 (OFI, last %5 s) + %6 × %7 (momentum, %8 ticks) = %9"
                            "<br><span style='color:%10'>α, β, γ are placeholders until calibrated out of sample.</span>")
                 .arg(QString::number(cfg.w_obi, 'f', 1), signed2(s->obi_deep_w), QString::number(cfg.w_ofi, 'f', 1), signed2(s->ofi_norm))
                 .arg(static_cast<qlonglong>(cfg.window_ns / 1'000'000'000))
                 .arg(QString::number(cfg.w_mom, 'f', 1), signed2(s->mom_norm), QString::number(s->momentum_ticks, 'f', 1), val(s->score), muted);
        calc_->setText(h.join(QStringLiteral("<br>")));
        levels_->setRowCount(static_cast<int>(rows.size()));
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const auto& r = rows[i];
            const int k = static_cast<int>(i);
            live_set(levels_, k, 0, QString::number(r.i));
            live_set(levels_, k, 1, QString::number(r.w, 'f', 3));
            live_set(levels_, k, 2, rupees(static_cast<double>(r.bid_px)));
            live_set(levels_, k, 3, units(r.b), QColor(theme_token::kUp));
            live_set(levels_, k, 4, QString::number(r.wb, 'f', 1));
            live_set(levels_, k, 5, rupees(static_cast<double>(r.ask_px)));
            live_set(levels_, k, 6, units(r.a), QColor(theme_token::kDown));
            live_set(levels_, k, 7, QString::number(r.wa, 'f', 1));
            live_set(levels_, k, 8, QStringLiteral("%1 / %2").arg(signed2(r.obi_to), signed2(r.wobi_to)),
                     r.wobi_to >= cfg.threshold ? QColor(theme_token::kUp) : r.wobi_to <= -cfg.threshold ? QColor(theme_token::kDown) : levels_->palette().color(QPalette::Text));
        }
    }

    static constexpr int kStrikesEach = 5;                 ///< ATM +/- 5: 11 strikes
    static constexpr std::size_t kTapeKeep = 2'000;        ///< events kept (the tape shows the newest kTapeRows)
    static constexpr int kTapeRows = 300;

    PriceClient* client_ = nullptr;
    QString root_;
    std::vector<LiveRow> universe_;
    QString under_;
    quint32 fut_ = 0, token_ = 0;
    std::map<quint32, std::unique_ptr<flow::FlowEngine>> engines_;
    std::vector<quint32> watched_;
    std::vector<order_flow_detail::StrikeRow> strikes_list_;
    double atm_strike_ = 0.0;
    std::array<flow::Level, flow::kMaxLevels> bids_{}, asks_{};          ///< scratch for each frame
    std::array<flow::Level, flow::kMaxLevels> sel_bids_{}, sel_asks_{};  ///< the open contract's last book
    std::size_t book_n_ = 0;
    flow::FlowTotals last_total_{};
    std::deque<order_flow_detail::FlowEvent> events_;
    std::uint64_t updates_ = 0;
    qint64 work_ns_ = 0, work_max_ns_ = 0;
    bool dirty_ = true, tape_dirty_ = true;
    order_flow_detail::ObiRules rules_{};
    qint64 lot_ = 1;
    std::map<quint32, order_flow_detail::PunchPosition> punch_;
    double realised_ = 0.0;
    quint32 ready_fut_tok_ = 0, ready_opt_tok_ = 0;
    int ready_fut_side_ = 0;

    QButtonGroup* group_ = nullptr;
    QComboBox* rows_ = nullptr;
    QCheckBox* deep_ = nullptr;
    QCheckBox* show_adds_ = nullptr;
    QLabel* head_ = nullptr;
    FlowLadder* ladder_ = nullptr;
    std::array<QLabel*, 4> cards_{};
    QTableWidget* strikes_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QLabel* how_ = nullptr;
    QLabel* calc_ = nullptr;
    QTableWidget* levels_ = nullptr;
    QLabel* counts_ = nullptr;
    QTableWidget* tape_ = nullptr;
    QLabel* suggest_ = nullptr;
    QPushButton* ready_fut_ = nullptr;
    QPushButton* ready_opt_ = nullptr;
    QSpinBox* lots_ = nullptr;
    QPushButton* buy_ = nullptr;
    QPushButton* sell_ = nullptr;
    QLabel* punch_status_ = nullptr;
    QTableWidget* positions_ = nullptr;
    QPushButton* close_all_ = nullptr;
    StrategyRecordPanel* record_ = nullptr;
    QTimer paint_, reload_;
};

} // namespace altair::ui
