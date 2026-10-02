// desktop/paper_oms.hpp -- the Terminal's paper order book.
//
// A trader's terminal places orders with + and - (F1 / F2). This one places
// PAPER orders: nothing here links a broker or an OMS, and nothing can reach
// an exchange. What it does do is behave like the real thing against the live
// stream, so the book means something:
//
//   * MARKET fills at once, a buy at the ASK and a sell at the BID in force;
//     with no quote yet it waits for one (an index has none and is refused).
//   * LIMIT rests until the market crosses it -- a buy when the ask is at or
//     below the limit, a sell when the bid is at or above -- and fills at that
//     ask or bid (never at a price the book did not show).
//   * Modify (quantity, limit) and cancel work on a resting order only.
//   * Positions net per instrument and product; realised P&L on what closes,
//     the open part marked tick by tick: a long at the bid, a short at the ask.
//   * Expenses are charged on every fill by the function the owner supplies
//     (risk/cost.hpp under config/charges.toml); unpriced is said, not zero.
//
// Prices are paise; quantities are units (lots x lot size).

#pragma once

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QTextStream>

#include <cmath>
#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace altair::ui {

enum class PaperSide : std::uint8_t { Buy, Sell };
enum class PaperType : std::uint8_t { Market, Limit };
enum class PaperStatus : std::uint8_t { Open, Filled, Cancelled, Rejected };

[[nodiscard]] inline QString paper_status_text(PaperStatus s) {
    switch (s) {
    case PaperStatus::Open: return QStringLiteral("OPEN");
    case PaperStatus::Filled: return QStringLiteral("EXECUTED");
    case PaperStatus::Cancelled: return QStringLiteral("CANCELLED");
    case PaperStatus::Rejected: return QStringLiteral("REJECTED");
    }
    return QStringLiteral("?");
}

/// What an order is about: everything the window and the book need to know.
struct PaperInstrument {
    quint32 token = 0;
    QString symbol;
    QString exchange = QStringLiteral("NFO");   ///< NSE, NFO
    qint64 lot = 1;
    qint64 tick_paise = 5;
    bool tradable = true;                        ///< an index is not
};

struct PaperOrder {
    int id = 0;
    PaperInstrument inst;
    QString product = QStringLiteral("NRML");    ///< NRML, MIS, CNC
    PaperSide side = PaperSide::Buy;
    PaperType type = PaperType::Market;
    qint64 qty = 0;                              ///< units
    qint64 limit_paise = 0;                      ///< limit orders only
    PaperStatus status = PaperStatus::Open;
    std::int64_t placed_ns = 0, done_ns = 0;
    qint64 fill_paise = 0;
    QString note;
    bool sim = false;                            ///< filled against a SIM feed
    bool waiting_logged = false;                 ///< "waiting for a price" said once
};

struct PaperTrade {
    int id = 0, order_id = 0;
    PaperInstrument inst;
    QString product;
    PaperSide side = PaperSide::Buy;
    qint64 qty = 0;
    qint64 price_paise = 0;
    std::int64_t ns = 0;
    QString basis;                               ///< "at ask", "at bid", "at last trade (no quote)"
    double expenses = std::numeric_limits<double>::quiet_NaN();   ///< rupees; NaN = unpriced
    bool sim = false;
};

/// The quote an order is matched against.
struct PaperQuote {
    qint64 bid = 0, ask = 0, ltp = 0;
    std::int64_t ns = 0;
    bool sim = false;
};

struct PaperPosition {
    PaperInstrument inst;
    QString product;
    qint64 net = 0;                ///< units, + long
    qint64 buy_qty = 0, sell_qty = 0;
    double buy_value = 0.0, sell_value = 0.0;   ///< rupees
    double avg_open = 0.0;         ///< rupees, average price of the open quantity
    double realised = 0.0;         ///< rupees, gross
    double expenses = 0.0;         ///< rupees, priced fills only
    bool expenses_unpriced = false;
    qint64 mark_paise = 0;         ///< long at the bid, short at the ask; 0 = no quote
    double unrealised = 0.0;       ///< rupees
    [[nodiscard]] double net_pnl() const { return realised + unrealised - expenses; }
};

class PaperOms {
public:
    /// Rupees of expenses for one fill; nullopt when it cannot be priced.
    using ExpenseFn = std::function<std::optional<double>(const PaperTrade&)>;

    void set_expenses(ExpenseFn fn) { expenses_ = std::move(fn); }

    /// Place an order. Validation refusals come back as the reason, and are
    /// recorded as REJECTED so the order book shows them as a terminal would.
    std::expected<int, QString> place(PaperOrder o, const PaperQuote& q, std::int64_t now_ns) {
        o.id = next_order_++;
        o.placed_ns = now_ns;
        o.status = PaperStatus::Open;
        QString why;
        if (o.inst.token == 0) why = QStringLiteral("no instrument selected");
        else if (!o.inst.tradable) why = QStringLiteral("%1 is an index; it cannot be traded").arg(o.inst.symbol);
        else if (o.qty <= 0) why = QStringLiteral("quantity must be positive");
        else if (o.inst.lot > 1 && o.qty % o.inst.lot != 0)
            why = QStringLiteral("quantity %1 is not a multiple of the lot (%2)").arg(o.qty).arg(o.inst.lot);
        else if (o.type == PaperType::Limit && o.limit_paise <= 0) why = QStringLiteral("a limit order needs a price");
        else if (o.type == PaperType::Limit && o.inst.tick_paise > 0 && o.limit_paise % o.inst.tick_paise != 0)
            why = QStringLiteral("price is not a multiple of the tick (%1)").arg(static_cast<double>(o.inst.tick_paise) / 100.0, 0, 'f', 2);
        if (!why.isEmpty()) {
            o.status = PaperStatus::Rejected;
            o.done_ns = now_ns;
            o.note = why;
            orders_.push_back(o);
            log(QStringLiteral("REJECTED #%1 %2: %3").arg(o.id).arg(o.inst.symbol, why));
            if (on_order) on_order(o);
            return std::unexpected(why);
        }
        orders_.push_back(o);
        log(QStringLiteral("placed #%1 %2 %3 %4 %5%6")
                .arg(o.id)
                .arg(o.side == PaperSide::Buy ? QStringLiteral("BUY") : QStringLiteral("SELL"))
                .arg(o.qty)
                .arg(o.inst.symbol)
                .arg(o.type == PaperType::Market ? QStringLiteral("MKT") : QStringLiteral("LMT"))
                .arg(o.type == PaperType::Limit ? QStringLiteral(" @ %1").arg(rupees(o.limit_paise)) : QString()));
        try_fill(orders_.back(), q);
        if (orders_.back().status == PaperStatus::Open && on_order) on_order(orders_.back());
        return o.id;
    }

    /// Change a resting order's quantity and limit. False when it is not open.
    bool modify(int id, qint64 qty, qint64 limit_paise, const PaperQuote& q) {
        PaperOrder* o = find(id);
        if (o == nullptr || o->status != PaperStatus::Open || qty <= 0) return false;
        if (o->inst.lot > 1 && qty % o->inst.lot != 0) return false;
        if (o->type == PaperType::Limit && limit_paise <= 0) return false;
        o->qty = qty;
        if (o->type == PaperType::Limit) o->limit_paise = limit_paise;
        log(QStringLiteral("modified #%1 -> %2%3").arg(id).arg(qty)
                .arg(o->type == PaperType::Limit ? QStringLiteral(" @ %1").arg(rupees(limit_paise)) : QString()));
        try_fill(*o, q);
        if (o->status == PaperStatus::Open && on_order) on_order(*o);
        return true;
    }

    bool cancel(int id, std::int64_t now_ns) {
        PaperOrder* o = find(id);
        if (o == nullptr || o->status != PaperStatus::Open) return false;
        o->status = PaperStatus::Cancelled;
        o->done_ns = now_ns;
        log(QStringLiteral("cancelled #%1 %2").arg(id).arg(o->inst.symbol));
        if (on_order) on_order(*o);
        return true;
    }

    int cancel_all(std::int64_t now_ns) {
        int n = 0;
        for (auto& o : orders_) {
            if (o.status != PaperStatus::Open) continue;
            o.status = PaperStatus::Cancelled;
            o.done_ns = now_ns;
            if (on_order) on_order(o);
            ++n;
        }
        if (n > 0) log(QStringLiteral("cancelled all: %1 order(s)").arg(n));
        return n;
    }

    /// A new quote for `token`: resting orders that cross it fill; positions mark.
    void on_quote(quint32 token, const PaperQuote& q) {
        quotes_[token] = q;
        for (auto& o : orders_)
            if (o.status == PaperStatus::Open && o.inst.token == token) try_fill(o, q);
    }

    [[nodiscard]] const std::vector<PaperOrder>& orders() const noexcept { return orders_; }
    [[nodiscard]] const std::vector<PaperTrade>& trades() const noexcept { return trades_; }
    [[nodiscard]] const PaperOrder* order(int id) const {
        for (const auto& o : orders_) if (o.id == id) return &o;
        return nullptr;
    }
    [[nodiscard]] int open_orders() const {
        int n = 0;
        for (const auto& o : orders_) n += o.status == PaperStatus::Open ? 1 : 0;
        return n;
    }

    /// Net positions, per instrument and product, marked at the last quote.
    [[nodiscard]] std::vector<PaperPosition> positions() const {
        std::map<std::pair<quint32, QString>, PaperPosition> book;
        for (const auto& t : trades_) {
            auto& p = book[{t.inst.token, t.product}];
            p.inst = t.inst;
            p.product = t.product;
            const double px = static_cast<double>(t.price_paise) / 100.0;
            const qint64 signed_qty = t.side == PaperSide::Buy ? t.qty : -t.qty;
            if (t.side == PaperSide::Buy) { p.buy_qty += t.qty; p.buy_value += px * static_cast<double>(t.qty); }
            else { p.sell_qty += t.qty; p.sell_value += px * static_cast<double>(t.qty); }
            if (std::isnan(t.expenses)) p.expenses_unpriced = true;
            else p.expenses += t.expenses;
            // Closing part realises against the open average; the rest opens.
            if (p.net != 0 && (p.net > 0) != (signed_qty > 0)) {
                const qint64 closing = std::min(std::llabs(p.net), std::llabs(signed_qty));
                const double dir = p.net > 0 ? 1.0 : -1.0;
                p.realised += dir * (px - p.avg_open) * static_cast<double>(closing);
                const qint64 rest = std::llabs(signed_qty) - closing;
                p.net += signed_qty;
                if (p.net == 0) p.avg_open = 0.0;
                else if (rest > 0) p.avg_open = px;   // flipped: the remainder opened at this price
            } else {
                const double open_value = p.avg_open * static_cast<double>(std::llabs(p.net)) + px * static_cast<double>(t.qty);
                p.net += signed_qty;
                p.avg_open = p.net != 0 ? open_value / static_cast<double>(std::llabs(p.net)) : 0.0;
            }
        }
        std::vector<PaperPosition> out;
        for (auto& [key, p] : book) {
            const auto it = quotes_.find(key.first);
            if (it != quotes_.end()) {
                const PaperQuote& q = it->second;
                p.mark_paise = p.net > 0 ? (q.bid > 0 ? q.bid : q.ltp) : p.net < 0 ? (q.ask > 0 ? q.ask : q.ltp) : q.ltp;
            }
            if (p.net != 0 && p.mark_paise > 0)
                p.unrealised = (static_cast<double>(p.mark_paise) / 100.0 - p.avg_open) * static_cast<double>(p.net);
            out.push_back(p);
        }
        return out;
    }

    /// Lines for the message log since the last call.
    [[nodiscard]] QStringList take_messages() { return std::exchange(messages_, {}); }

    /// Restore a saved book (trades, and orders still resting today).
    void restore(std::vector<PaperOrder> orders, std::vector<PaperTrade> trades) {
        orders_ = std::move(orders);
        trades_ = std::move(trades);
        for (const auto& o : orders_) next_order_ = std::max(next_order_, o.id + 1);
        for (const auto& t : trades_) next_trade_ = std::max(next_trade_, t.id + 1);
    }

    /// Called with each new trade and each order whose state changed, for
    /// the owner to persist.
    std::function<void(const PaperTrade&)> on_trade;
    std::function<void(const PaperOrder&)> on_order;

    [[nodiscard]] static QString rupees(qint64 paise) {
        return QString::number(static_cast<double>(paise) / 100.0, 'f', 2);
    }

private:
    PaperOrder* find(int id) {
        for (auto& o : orders_) if (o.id == id) return &o;
        return nullptr;
    }

    void log(const QString& line) { messages_ << line; }

    void try_fill(PaperOrder& o, const PaperQuote& q) {
        if (o.status != PaperStatus::Open) return;
        const bool buy = o.side == PaperSide::Buy;
        const qint64 touch = buy ? q.ask : q.bid;
        qint64 px = 0;
        QString basis;
        if (o.type == PaperType::Market) {
            if (touch > 0) { px = touch; basis = buy ? QStringLiteral("at ask") : QStringLiteral("at bid"); }
            else if (q.ltp > 0) { px = q.ltp; basis = QStringLiteral("at last trade (no quote)"); }
        } else if (touch > 0 && (buy ? touch <= o.limit_paise : touch >= o.limit_paise)) {
            px = touch;
            basis = buy ? QStringLiteral("at ask (limit crossed)") : QStringLiteral("at bid (limit crossed)");
        }
        if (px <= 0) {
            if (!o.waiting_logged && o.type == PaperType::Market) {
                o.waiting_logged = true;
                log(QStringLiteral("#%1 %2: waiting for a price").arg(o.id).arg(o.inst.symbol));
            }
            return;
        }
        o.status = PaperStatus::Filled;
        o.fill_paise = px;
        o.done_ns = q.ns;
        o.sim = q.sim;
        PaperTrade t;
        t.id = next_trade_++;
        t.order_id = o.id;
        t.inst = o.inst;
        t.product = o.product;
        t.side = o.side;
        t.qty = o.qty;
        t.price_paise = px;
        t.ns = q.ns;
        t.basis = basis;
        t.sim = q.sim;
        if (expenses_) {
            if (const auto e = expenses_(t)) t.expenses = *e;
        }
        trades_.push_back(t);
        log(QStringLiteral("EXECUTED #%1 %2 %3 %4 @ %5 %6%7%8")
                .arg(o.id)
                .arg(buy ? QStringLiteral("BUY") : QStringLiteral("SELL"))
                .arg(o.qty)
                .arg(o.inst.symbol)
                .arg(rupees(px))
                .arg(basis)
                .arg(std::isnan(t.expenses) ? QStringLiteral(" · expenses unpriced")
                                            : QStringLiteral(" · expenses %1").arg(t.expenses, 0, 'f', 2))
                .arg(q.sim ? QStringLiteral(" · SIM") : QString()));
        if (on_trade) on_trade(t);
        if (on_order) on_order(o);
    }

    ExpenseFn expenses_;
    std::vector<PaperOrder> orders_;
    std::vector<PaperTrade> trades_;
    std::map<quint32, PaperQuote> quotes_;
    QStringList messages_;
    int next_order_ = 1, next_trade_ = 1;
};

// ---- persistence: data/live/paper/manual_orders.csv and manual_trades.csv ----
//
// Append-only. The order file is an event log (the last line for an order id
// is its state); the trade file is every fill. A restart rebuilds the book
// from both: positions from every trade, resting orders from today only (a
// day order does not survive the close).

namespace paper_store {

inline constexpr const char* kOrdersHeader =
    "id,ns,token,symbol,exchange,lot,tick,product,side,type,qty,limit,status,fill,done_ns,sim,note";
inline constexpr const char* kTradesHeader =
    "id,order_id,ns,token,symbol,exchange,lot,tick,product,side,qty,price,basis,expenses,sim";

[[nodiscard]] inline QString clean(QString s) { return s.replace(QLatin1Char(','), QLatin1Char(';')).replace(QLatin1Char('\n'), QLatin1Char(' ')); }

inline bool append(const QString& path, const char* header, const QString& line) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    const bool fresh = !QFileInfo::exists(path);
    QFile f(path);
    if (!f.open(QIODevice::Append | QIODevice::Text)) return false;
    QTextStream out(&f);
    if (fresh) out << header << '\n';
    out << line << '\n';
    return true;
}

inline bool append_order(const QString& path, const PaperOrder& o) {
    return append(path, kOrdersHeader, QStringLiteral("%1,%2,%3,%4,%5,%6,%7,%8,%9,%10,%11,%12,%13,%14,%15,%16,%17")
        .arg(o.id).arg(o.placed_ns).arg(o.inst.token).arg(clean(o.inst.symbol), clean(o.inst.exchange))
        .arg(o.inst.lot).arg(o.inst.tick_paise).arg(clean(o.product))
        .arg(o.side == PaperSide::Buy ? QStringLiteral("B") : QStringLiteral("S"))
        .arg(o.type == PaperType::Market ? QStringLiteral("MKT") : QStringLiteral("LMT"))
        .arg(o.qty).arg(o.limit_paise).arg(paper_status_text(o.status)).arg(o.fill_paise).arg(o.done_ns)
        .arg(o.sim ? 1 : 0).arg(clean(o.note)));
}

inline bool append_trade(const QString& path, const PaperTrade& t) {
    return append(path, kTradesHeader, QStringLiteral("%1,%2,%3,%4,%5,%6,%7,%8,%9,%10,%11,%12,%13,%14,%15")
        .arg(t.id).arg(t.order_id).arg(t.ns).arg(t.inst.token).arg(clean(t.inst.symbol), clean(t.inst.exchange))
        .arg(t.inst.lot).arg(t.inst.tick_paise).arg(clean(t.product))
        .arg(t.side == PaperSide::Buy ? QStringLiteral("B") : QStringLiteral("S"))
        .arg(t.qty).arg(t.price_paise).arg(clean(t.basis))
        .arg(std::isnan(t.expenses) ? QStringLiteral("") : QString::number(t.expenses, 'f', 2))
        .arg(t.sim ? 1 : 0));
}

[[nodiscard]] inline std::int64_t ist_day(std::int64_t ns) { return (ns / 1'000'000'000LL + 19800) / 86400; }

/// Rebuild a book: every trade, and the orders placed on `today_ns`'s IST day
/// (an order left open from an earlier day is shown as expired).
inline void load(const QString& orders_path, const QString& trades_path, std::int64_t today_ns,
                 std::vector<PaperOrder>& orders, std::vector<PaperTrade>& trades) {
    const auto rows = [](const QString& path) {
        std::vector<QStringList> out;
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return out;
        QTextStream in(&f);
        (void)in.readLine();
        while (!in.atEnd()) {
            const QString line = in.readLine();
            if (!line.isEmpty()) out.push_back(line.split(QLatin1Char(',')));
        }
        return out;
    };
    for (const auto& c : rows(trades_path)) {
        if (c.size() < 15) continue;
        PaperTrade t;
        t.id = c[0].toInt(); t.order_id = c[1].toInt(); t.ns = c[2].toLongLong();
        t.inst.token = c[3].toUInt(); t.inst.symbol = c[4]; t.inst.exchange = c[5];
        t.inst.lot = c[6].toLongLong(); t.inst.tick_paise = c[7].toLongLong(); t.product = c[8];
        t.side = c[9] == QLatin1String("B") ? PaperSide::Buy : PaperSide::Sell;
        t.qty = c[10].toLongLong(); t.price_paise = c[11].toLongLong(); t.basis = c[12];
        bool ok = false;
        const double e = c[13].toDouble(&ok);
        t.expenses = ok ? e : std::numeric_limits<double>::quiet_NaN();
        t.sim = c[14] == QLatin1String("1");
        if (t.id > 0 && t.inst.token != 0 && t.qty > 0) trades.push_back(t);
    }
    std::map<int, PaperOrder> last;
    for (const auto& c : rows(orders_path)) {
        if (c.size() < 17) continue;
        PaperOrder o;
        o.id = c[0].toInt(); o.placed_ns = c[1].toLongLong();
        o.inst.token = c[2].toUInt(); o.inst.symbol = c[3]; o.inst.exchange = c[4];
        o.inst.lot = c[5].toLongLong(); o.inst.tick_paise = c[6].toLongLong(); o.product = c[7];
        o.side = c[8] == QLatin1String("B") ? PaperSide::Buy : PaperSide::Sell;
        o.type = c[9] == QLatin1String("LMT") ? PaperType::Limit : PaperType::Market;
        o.qty = c[10].toLongLong(); o.limit_paise = c[11].toLongLong();
        const QString st = c[12];
        o.status = st == QLatin1String("EXECUTED") ? PaperStatus::Filled
                 : st == QLatin1String("CANCELLED") ? PaperStatus::Cancelled
                 : st == QLatin1String("REJECTED") ? PaperStatus::Rejected : PaperStatus::Open;
        o.fill_paise = c[13].toLongLong(); o.done_ns = c[14].toLongLong(); o.sim = c[15] == QLatin1String("1");
        o.note = c.mid(16).join(QLatin1Char(','));
        if (o.id > 0) last[o.id] = o;
    }
    const std::int64_t today = ist_day(today_ns);
    for (auto& [id, o] : last) {
        if (ist_day(o.placed_ns) != today) {
            if (o.status != PaperStatus::Open) continue;   // yesterday's history stays in the file
            o.status = PaperStatus::Cancelled;
            o.note = QStringLiteral("expired: a day order does not survive the close");
        }
        orders.push_back(o);
    }
}

} // namespace paper_store

} // namespace altair::ui
