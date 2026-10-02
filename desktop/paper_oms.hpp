// desktop/paper_oms.hpp -- the Terminal's paper order book.
//
// A trader's terminal places orders with + and - (F1 / F2). This one places
// PAPER orders: nothing here links a broker or an OMS, and nothing can reach
// an exchange. What it does do is behave like the real thing against the live
// stream, so the book means something:
//
//   * An order meets the market no sooner than `latency_ns` after it is
//     placed, on the FEED's clock (a SIM session's clock, not the wall's).
//   * It fills only against a FRESH quote -- stamped within `max_quote_age_ns`
//     of the feed's clock, from a feed that is still updating -- and NEVER at
//     the last trade: a MARKET order with no fresh bid/ask is rejected, with
//     the quote's age as the reason (an index has none and is refused).
//   * A buy takes the ASK and a sell the BID, walking the five-level book
//     when it is fresh, for no more than the size shown. The rest keeps
//     working (a partial fill: the order shows filled/qty and its average).
//   * LIMIT rests until the market crosses it -- a buy when the ask is at or
//     below the limit, a sell when the bid is at or above -- and takes only
//     the levels inside the limit.
//   * Modify (quantity, limit) and cancel work on a working order only.
//   * Positions net per instrument and product; realised P&L on what closes,
//     the open part marked at a fresh bid (long) or ask (short) -- unmarked,
//     and said so, when there is none.
//   * Expenses are charged on every fill by the function the owner supplies
//     (risk/cost.hpp under config/charges.toml); unpriced is said, not zero,
//     and a net that would need an unpriced expense is not shown as a number.
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
    qint64 fill_paise = 0;                       ///< average over the fills so far
    qint64 filled = 0;                           ///< units filled so far
    std::int64_t due_ns = 0;                     ///< feed time it may first fill
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
    QString basis;                               ///< "at ask", "at bid", "at ask (part 75/150)"; never the last trade
    double expenses = std::numeric_limits<double>::quiet_NaN();   ///< rupees; NaN = unpriced
    bool sim = false;
};

/// One level of the book.
struct PaperLevel {
    qint64 px = 0, qty = 0;   ///< paise, units
};

/// The market an order is matched against, as the feed last showed it.
struct PaperQuote {
    qint64 bid = 0, ask = 0, ltp = 0;            ///< paise; the last trade is shown, never dealt at
    qint64 bid_qty = 0, ask_qty = 0;             ///< units at the touch; 0 = none shown
    std::int64_t ns = 0;                         ///< feed time of this update
    std::int64_t quote_ns = 0;                   ///< feed time the bid/ask was stamped; 0 = never
    std::int64_t book_ns = 0;                    ///< feed time of the five levels; 0 = none
    int levels = 0;
    PaperLevel bids[5]{}, asks[5]{};
    bool sim = false;
};

/// How orders meet the market (feed time).
struct PaperExecPolicy {
    std::int64_t latency_ns = 250'000'000;          ///< placed to at the market
    std::int64_t max_quote_age_ns = 10'000'000'000; ///< older, and a quote is not executable
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
    qint64 mark_paise = 0;         ///< long at a fresh bid, short at a fresh ask; 0 = no fresh quote
    double unrealised = 0.0;       ///< rupees; 0 while unmarked
    /// NaN when it cannot be known: an open quantity with no fresh mark, or
    /// a fill whose expenses are unpriced. Never a number that pretends.
    [[nodiscard]] double net_pnl() const {
        if (expenses_unpriced || (net != 0 && mark_paise <= 0)) return std::numeric_limits<double>::quiet_NaN();
        return realised + unrealised - expenses;
    }
};

class PaperOms {
public:
    /// Rupees of expenses for one fill; nullopt when it cannot be priced.
    using ExpenseFn = std::function<std::optional<double>(const PaperTrade&)>;

    void set_expenses(ExpenseFn fn) { expenses_ = std::move(fn); }
    void set_policy(const PaperExecPolicy& p) { pol_ = p; }
    [[nodiscard]] const PaperExecPolicy& policy() const noexcept { return pol_; }

    /// Place an order at wall time `wall_ns`, with `q` the market as it
    /// stands. Validation refusals come back as the reason, and are recorded
    /// as REJECTED so the order book shows them as a terminal would.
    std::expected<int, QString> place(PaperOrder o, const PaperQuote& q, std::int64_t wall_ns) {
        o.id = next_order_++;
        o.placed_ns = wall_ns;
        o.status = PaperStatus::Open;
        o.filled = 0;
        o.fill_paise = 0;
        if (o.inst.token != 0) see(o.inst.token, q, wall_ns);
        QString why;
        if (o.inst.token == 0) why = QStringLiteral("no instrument selected");
        else if (!o.inst.tradable) why = QStringLiteral("%1 is an index; it cannot be traded").arg(o.inst.symbol);
        else if (o.qty <= 0) why = QStringLiteral("quantity must be positive");
        else if (o.inst.lot > 1 && o.qty % o.inst.lot != 0)
            why = QStringLiteral("quantity %1 is not a multiple of the lot (%2)").arg(o.qty).arg(o.inst.lot);
        else if (o.type == PaperType::Limit && o.limit_paise <= 0) why = QStringLiteral("a limit order needs a price");
        else if (o.type == PaperType::Limit && o.inst.tick_paise > 0 && o.limit_paise % o.inst.tick_paise != 0)
            why = QStringLiteral("price is not a multiple of the tick (%1)").arg(static_cast<double>(o.inst.tick_paise) / 100.0, 0, 'f', 2);
        else if (o.type == PaperType::Market) {
            // A market order needs a price someone would deal at, now.
            const qint64 touch = o.side == PaperSide::Buy ? q.ask : q.bid;
            const qint64 size = o.side == PaperSide::Buy ? q.ask_qty : q.bid_qty;
            if (touch <= 0 || !fresh(q.quote_ns, wall_ns))
                why = QStringLiteral("no fresh %1 for %2%3 -- a market order is not filled at the last trade")
                          .arg(o.side == PaperSide::Buy ? QStringLiteral("ask") : QStringLiteral("bid"), o.inst.symbol,
                               age_text(q.quote_ns));
            else if (size <= 0 && !(q.levels > 0 && fresh(q.book_ns, wall_ns)))
                why = QStringLiteral("no size shown at the %1 for %2").arg(o.side == PaperSide::Buy ? QStringLiteral("ask") : QStringLiteral("bid"), o.inst.symbol);
        }
        if (!why.isEmpty()) {
            o.status = PaperStatus::Rejected;
            o.done_ns = wall_ns;
            o.note = why;
            orders_.push_back(o);
            log(QStringLiteral("REJECTED #%1 %2: %3").arg(o.id).arg(o.inst.symbol, why));
            if (on_order) on_order(o);
            return std::unexpected(why);
        }
        o.due_ns = feed_now_ + pol_.latency_ns;
        orders_.push_back(o);
        log(QStringLiteral("placed #%1 %2 %3 %4 %5%6")
                .arg(o.id)
                .arg(o.side == PaperSide::Buy ? QStringLiteral("BUY") : QStringLiteral("SELL"))
                .arg(o.qty)
                .arg(o.inst.symbol)
                .arg(o.type == PaperType::Market ? QStringLiteral("MKT") : QStringLiteral("LMT"))
                .arg(o.type == PaperType::Limit ? QStringLiteral(" @ %1").arg(rupees(o.limit_paise)) : QString()));
        if (pol_.latency_ns <= 0) try_fill(orders_.back(), wall_ns);
        if (orders_.back().status == PaperStatus::Open && on_order) on_order(orders_.back());
        return o.id;
    }

    /// Change a working order's quantity and limit. False when it is not
    /// working, or the new quantity is not above what has already filled.
    bool modify(int id, qint64 qty, qint64 limit_paise, const PaperQuote& q, std::int64_t wall_ns = 0) {
        PaperOrder* o = find(id);
        if (o == nullptr || o->status != PaperStatus::Open || qty <= 0 || qty <= o->filled) return false;
        if (o->inst.lot > 1 && qty % o->inst.lot != 0) return false;
        if (o->type == PaperType::Limit && limit_paise <= 0) return false;
        if (wall_ns > 0) see(o->inst.token, q, wall_ns);
        o->qty = qty;
        if (o->type == PaperType::Limit) o->limit_paise = limit_paise;
        log(QStringLiteral("modified #%1 -> %2%3").arg(id).arg(qty)
                .arg(o->type == PaperType::Limit ? QStringLiteral(" @ %1").arg(rupees(limit_paise)) : QString()));
        try_fill(*o, wall_ns > 0 ? wall_ns : feed_wall_);
        if (o->status == PaperStatus::Open && on_order) on_order(*o);
        return true;
    }

    bool cancel(int id, std::int64_t now_ns) {
        PaperOrder* o = find(id);
        if (o == nullptr || o->status != PaperStatus::Open) return false;
        o->status = PaperStatus::Cancelled;
        o->done_ns = now_ns;
        if (o->filled > 0) o->note = QStringLiteral("cancelled after %1 of %2 filled").arg(o->filled).arg(o->qty);
        log(QStringLiteral("cancelled #%1 %2%3").arg(id).arg(o->inst.symbol)
                .arg(o->filled > 0 ? QStringLiteral(" (%1 of %2 had filled)").arg(o->filled).arg(o->qty) : QString()));
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

    /// The feed updated `token` (received at wall time `wall_ns`): working
    /// orders that are due and can deal fill; positions mark.
    void on_quote(quint32 token, const PaperQuote& q, std::int64_t wall_ns) {
        see(token, q, wall_ns);
        for (auto& o : orders_)
            if (o.status == PaperStatus::Open && o.inst.token == token) try_fill(o, wall_ns);
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

    /// Net positions, per instrument and product, marked at a fresh quote.
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
            if (it != quotes_.end() && p.net != 0 && fresh(it->second.quote_ns, feed_wall_)) {
                const PaperQuote& q = it->second;
                p.mark_paise = p.net > 0 ? q.bid : q.ask;   // what closing would fetch; never the last trade
            }
            if (p.net != 0 && p.mark_paise > 0)
                p.unrealised = (static_cast<double>(p.mark_paise) / 100.0 - p.avg_open) * static_cast<double>(p.net);
            out.push_back(p);
        }
        return out;
    }

    /// Lines for the message log since the last call.
    [[nodiscard]] QStringList take_messages() { return std::exchange(messages_, {}); }

    /// Restore a saved book (trades, and orders still working today).
    void restore(std::vector<PaperOrder> orders, std::vector<PaperTrade> trades) {
        orders_ = std::move(orders);
        trades_ = std::move(trades);
        for (const auto& o : orders_) next_order_ = std::max(next_order_, o.id + 1);
        for (const auto& t : trades_) next_trade_ = std::max(next_trade_, t.id + 1);
        for (auto& o : orders_) o.due_ns = 0;   // a restored order works against the next fresh quote
    }

    /// The feed's clock: the newest update seen, and the wall time it arrived.
    [[nodiscard]] std::int64_t feed_now() const noexcept { return feed_now_; }

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

    void see(quint32 token, const PaperQuote& q, std::int64_t wall_ns) {
        if (q.ns > 0 || q.quote_ns > 0) quotes_[token] = q;
        // The feed's clock moves only when the feed does: seeing the same
        // quote again later (an order placed against it) does not refresh it.
        const std::int64_t t = std::max(q.ns, q.quote_ns);
        if (t > feed_now_) { feed_now_ = t; feed_wall_ = wall_ns; }
    }

    /// Fresh: stamped within the age limit of the feed's clock, and the feed
    /// itself has updated within that limit of the wall clock (a feed that
    /// stopped does not keep its last quotes executable).
    [[nodiscard]] bool fresh(std::int64_t stamp_ns, std::int64_t wall_ns) const noexcept {
        return stamp_ns > 0 && feed_now_ - stamp_ns <= pol_.max_quote_age_ns
            && wall_ns - feed_wall_ <= pol_.max_quote_age_ns;
    }

    [[nodiscard]] QString age_text(std::int64_t quote_ns) const {
        if (quote_ns <= 0) return QStringLiteral(" (never quoted)");
        return QStringLiteral(" (last quote %1 s old)").arg(static_cast<double>(feed_now_ - quote_ns) / 1e9, 0, 'f', 1);
    }

    void try_fill(PaperOrder& o, std::int64_t wall_ns) {
        if (o.status != PaperStatus::Open || feed_now_ < o.due_ns) return;
        const auto it = quotes_.find(o.inst.token);
        if (it == quotes_.end()) return;
        const PaperQuote& q = it->second;
        const bool buy = o.side == PaperSide::Buy;
        const bool limit = o.type == PaperType::Limit;
        const qint64 want = o.qty - o.filled;
        qint64 got = 0;
        double value = 0.0;
        const auto inside = [&](qint64 px) { return !limit || (buy ? px <= o.limit_paise : px >= o.limit_paise); };
        const auto eat = [&](qint64 px, qint64 qty) {
            if (px <= 0 || qty <= 0 || got >= want || !inside(px)) return;
            const qint64 n = std::min(qty, want - got);
            got += n;
            value += static_cast<double>(px) * static_cast<double>(n);
        };
        bool walked = false;
        if (q.levels > 0 && fresh(q.book_ns, wall_ns)) {
            for (int k = 0; k < q.levels && k < 5; ++k) eat(buy ? q.asks[k].px : q.bids[k].px, buy ? q.asks[k].qty : q.bids[k].qty);
            walked = got > 0;
        }
        if (!walked && fresh(q.quote_ns, wall_ns)) eat(buy ? q.ask : q.bid, buy ? q.ask_qty : q.bid_qty);
        if (got <= 0) {
            if (!o.waiting_logged && o.type == PaperType::Market) {
                o.waiting_logged = true;
                log(QStringLiteral("#%1 %2: working -- no fresh quote with size").arg(o.id).arg(o.inst.symbol));
            }
            return;
        }
        const qint64 px = std::llround(value / static_cast<double>(got));
        const double before = static_cast<double>(o.fill_paise) * static_cast<double>(o.filled);
        o.filled += got;
        o.fill_paise = std::llround((before + value) / static_cast<double>(o.filled));
        const bool done = o.filled >= o.qty;
        if (done) { o.status = PaperStatus::Filled; o.done_ns = std::max(q.ns, q.quote_ns); }
        o.sim = q.sim;
        PaperTrade t;
        t.id = next_trade_++;
        t.order_id = o.id;
        t.inst = o.inst;
        t.product = o.product;
        t.side = o.side;
        t.qty = got;
        t.price_paise = px;
        t.ns = std::max(q.ns, q.quote_ns);
        QString basis = buy ? QStringLiteral("at ask") : QStringLiteral("at bid");
        if (walked) basis += QStringLiteral(" (book)");
        if (limit) basis += QStringLiteral(" (limit crossed)");
        if (!done || o.filled != got) basis += QStringLiteral(" (part %1/%2)").arg(o.filled).arg(o.qty);
        t.basis = basis;
        t.sim = q.sim;
        if (expenses_) {
            if (const auto e = expenses_(t)) t.expenses = *e;
        }
        trades_.push_back(t);
        log(QStringLiteral("EXECUTED #%1 %2 %3 %4 @ %5 %6%7%8")
                .arg(o.id)
                .arg(buy ? QStringLiteral("BUY") : QStringLiteral("SELL"))
                .arg(got)
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
    PaperExecPolicy pol_;
    std::vector<PaperOrder> orders_;
    std::vector<PaperTrade> trades_;
    std::map<quint32, PaperQuote> quotes_;
    QStringList messages_;
    std::int64_t feed_now_ = 0, feed_wall_ = 0;
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
    "id,ns,token,symbol,exchange,lot,tick,product,side,type,qty,limit,status,fill,done_ns,sim,filled,note";
inline constexpr const char* kTradesHeader =
    "id,order_id,ns,token,symbol,exchange,lot,tick,product,side,qty,price,basis,expenses,sim";

[[nodiscard]] inline QString clean(QString s) { return s.replace(QLatin1Char(','), QLatin1Char(';')).replace(QLatin1Char('\n'), QLatin1Char(' ')); }

/// Append one line, flushed; true only when the file took it.
inline bool append(const QString& path, const char* header, const QString& line) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    const bool fresh = !QFileInfo::exists(path) || QFileInfo(path).size() == 0;
    QFile f(path);
    if (!f.open(QIODevice::Append | QIODevice::Text)) return false;
    QByteArray bytes;
    if (fresh) bytes += QByteArray(header) + '\n';
    bytes += line.toUtf8() + '\n';
    return f.write(bytes) == bytes.size() && f.flush();
}

inline bool append_order(const QString& path, const PaperOrder& o) {
    return append(path, kOrdersHeader, QStringLiteral("%1,%2,%3,%4,%5,%6,%7,%8,%9,%10,%11,%12,%13,%14,%15,%16,%17,%18")
        .arg(o.id).arg(o.placed_ns).arg(o.inst.token).arg(clean(o.inst.symbol), clean(o.inst.exchange))
        .arg(o.inst.lot).arg(o.inst.tick_paise).arg(clean(o.product))
        .arg(o.side == PaperSide::Buy ? QStringLiteral("B") : QStringLiteral("S"))
        .arg(o.type == PaperType::Market ? QStringLiteral("MKT") : QStringLiteral("LMT"))
        .arg(o.qty).arg(o.limit_paise).arg(paper_status_text(o.status)).arg(o.fill_paise).arg(o.done_ns)
        .arg(o.sim ? 1 : 0).arg(o.filled).arg(clean(o.note)));
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
    QString order_header;
    const auto rows = [](const QString& path, QString* header = nullptr) {
        std::vector<QStringList> out;
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return out;
        QTextStream in(&f);
        const QString h = in.readLine();
        if (header) *header = h;
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
    const auto order_rows = rows(orders_path, &order_header);
    // Files from before partial fills have no `filled` column: an EXECUTED
    // order there filled whole.
    const bool has_filled = order_header.contains(QLatin1String(",filled,"));
    for (const auto& c : order_rows) {
        if (c.size() < (has_filled ? 18 : 17)) continue;
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
        o.filled = has_filled ? c[16].toLongLong() : (o.status == PaperStatus::Filled ? o.qty : 0);
        o.note = c.mid(has_filled ? 17 : 16).join(QLatin1Char(','));
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
