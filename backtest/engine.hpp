// backtest/engine.hpp -- the tick-level, cost-aware replay engine.
//
// P6-05.
//
// THE FILL MODEL IS THE BACKTEST.
//
// This is the card. Everything else in a backtester -- the signal, the sizing,
// the reporting -- is arithmetic that can be checked. The fill model is an
// ASSUMPTION about a counterparty that was not asked, and it is where a
// backtest goes from a measurement to a story.
//
// Two assumptions do almost all the damage:
//
//   FILLING AT THE MID. A market order does not get the mid; it crosses the
//   spread. `FillModel::Mid` exists in this file ONLY so the gift can be
//   measured: the same edgeless strategy over the same 20,000 ticks, 400 fills
//   either way, on a market quoted just 10 paise wide, is Rs 1,499.82 better
//   filled at the mid -- Rs 3.75 a fill, which is the 5-paise half-spread
//   times the 75-unit lot, to the paisa. The model hands back exactly the
//   spread it declined to cross, and the gift scales with the spread and the
//   turnover, so the strategies most flattered by it are the high-frequency
//   ones that look best on a mid-filled backtest.
//
//   FILLING A LIMIT ORDER BECAUSE THE PRICE TOUCHED IT. A resting order does
//   not trade because the market printed at its price; it trades when the
//   queue ahead of it has been consumed.
//
//   Measured on a visit to the level that reverses without clearing it --
//   twelve prints of 25 units at the limit, 300 units traded: a 75-unit order
//   at the FRONT of the queue is done after three prints, and the identical
//   order with 600 units ahead of it fills NOTHING. Every one of those 300
//   units belonged to somebody who was there first. The naive rule books a
//   fill on the very first print, and over a mean-reverting session it books
//   one on 399 separate ticks -- one order, filled 399 times.
//
//   When the price trades THROUGH the level the queue is correctly
//   irrelevant, because the book at that level cleared. Touch-and-reverse is
//   the case queue position decides, and it is the common one for anything
//   resting passively.
//
// THE REPLAYER PHYSICALLY CANNOT EXPOSE A FUTURE TICK (rule 7).
//
// The engine drives the tape and hands the strategy ONE tick at a time. There
// is no accessor for the tape, no index, no peek, and no length. A strategy
// cannot look ahead because there is nothing to look ahead with -- which is a
// stronger guarantee than a convention that says not to.
//
// BACKTEST AND LIVE SHARE ONE PATH (rule 6).
//
// The engine consumes `Tick`, which is what the live normaliser emits. There
// is no backtest-only tick type and no branch on whether this is a replay. If
// the two diverged, the backtest would be measuring something that will never
// trade.
//
// CONSERVATION IS CHECKED, NOT ASSUMED.
//
// CLAUDE.md: Sigma(fills) + Sigma(costs) + cash_delta == 0 exactly, in paise,
// checked every tick. The ledger below maintains cash incrementally and
// verifies it against independently accumulated gross bought, gross sold and
// total costs. Two routes to the same number; a sign error, a double count, or
// a cost applied to the wrong side breaks the identity rather than quietly
// improving the result.

#pragma once

#include <feed/tick.hpp>
#include <risk/cost.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

enum class FillModel : std::uint8_t {
    /// Ordinal 0 is Unspecified. A backtest whose fill model was never chosen
    /// must not run -- it is the single largest determinant of the result.
    Unspecified = 0,
    /// Fill at the midpoint. WRONG, and present only so the error it makes can
    /// be measured against the alternatives.
    Mid,
    /// Cross the spread: buys lift the ask, sells hit the bid.
    Touch,
    /// Cross the spread and pay impact on top, from the P3-10 slippage model.
    TouchPlusImpact
};

enum class BacktestError : std::uint8_t {
    /// The fill model was not chosen.
    NoFillModel,
    /// No two-sided quote at this instant.
    NoTouch,
    /// The cost calculator refused.
    CostUnavailable,
    /// The conservation identity failed. Nothing continues after this.
    ConservationBreach,
    /// The order was for a non-positive quantity.
    BadQuantity
};

/// The two-sided quote in force when an order is sent.
struct Quote {
    Price bid{0};
    Price ask{0};
    Qty bid_qty{0};
    Qty ask_qty{0};
    [[nodiscard]] bool live() const noexcept {
        return bid.raw() > 0 && ask.raw() >= bid.raw();
    }
};

struct Fill {
    Timestamp ts{};
    Side side = Side::Buy;
    Qty qty{0};
    Price price{0};
    Notional cost{};
};

// ---------------------------------------------------------------------------
// The ledger and its invariant
// ---------------------------------------------------------------------------

/// Cash, position, and the conservation check.
///
/// Everything is integer paise (rule 3). `cash` is maintained incrementally as
/// fills arrive; `residual` recomputes it from independently accumulated
/// totals. The two agreeing is the invariant, and they are maintained by
/// separate statements so one bug does not move both.
class Ledger {
public:
    explicit Ledger(Notional opening_cash) noexcept
        : cash_(opening_cash), opening_(opening_cash) {}

    [[nodiscard]] std::expected<void, BacktestError>
    apply(const Fill& f) noexcept {
        if (f.qty.raw() <= 0) {
            return std::unexpected(BacktestError::BadQuantity);
        }
        const std::int64_t notional = f.price.raw() * f.qty.raw();
        if (f.side == Side::Buy) {
            cash_ = Notional{cash_.raw() - notional - f.cost.raw()};
            position_ = Qty{position_.raw() + f.qty.raw()};
            gross_bought_ += notional;
            qty_bought_ += f.qty.raw();
        } else {
            cash_ = Notional{cash_.raw() + notional - f.cost.raw()};
            position_ = Qty{position_.raw() - f.qty.raw()};
            gross_sold_ += notional;
            qty_sold_ += f.qty.raw();
        }
        costs_ += f.cost.raw();
        ++fills_;
        if (residual() != 0 || position_residual() != 0) {
            return std::unexpected(BacktestError::ConservationBreach);
        }
        return {};
    }

    /// opening - bought + sold - costs - cash. Exactly zero, in paise, always.
    [[nodiscard]] std::int64_t residual() const noexcept {
        return opening_.raw() - gross_bought_ + gross_sold_ - costs_
             - cash_.raw();
    }
    /// bought - sold - position. Same idea for quantity.
    [[nodiscard]] std::int64_t position_residual() const noexcept {
        return qty_bought_ - qty_sold_ - position_.raw();
    }

    /// Equity marked at a price. The only place a double would be tempting and
    /// is not used: mark is a Price and the product is exact.
    [[nodiscard]] Notional equity(Price mark) const noexcept {
        return Notional{cash_.raw() + position_.raw() * mark.raw()};
    }
    [[nodiscard]] Notional pnl(Price mark) const noexcept {
        return Notional{equity(mark).raw() - opening_.raw()};
    }

    [[nodiscard]] Notional cash() const noexcept { return cash_; }
    [[nodiscard]] Qty position() const noexcept { return position_; }
    [[nodiscard]] Notional costs() const noexcept { return Notional{costs_}; }
    [[nodiscard]] std::size_t fills() const noexcept { return fills_; }

private:
    Notional cash_;
    Notional opening_;
    Qty position_{0};
    std::int64_t gross_bought_ = 0;
    std::int64_t gross_sold_ = 0;
    std::int64_t costs_ = 0;
    std::int64_t qty_bought_ = 0;
    std::int64_t qty_sold_ = 0;
    std::size_t fills_ = 0;
};

// ---------------------------------------------------------------------------
// Fills
// ---------------------------------------------------------------------------

/// Where a MARKET order fills, under a given model.
///
/// `impact_paise` comes from P3-10 and is added only under TouchPlusImpact; it
/// is a parameter rather than something computed here, because impact depends
/// on size against depth and this file does not know the size.
[[nodiscard]] inline std::expected<Price, BacktestError>
market_fill_price(const Quote& q, Side side, FillModel m,
                  double impact_paise) noexcept {
    if (m == FillModel::Unspecified) {
        return std::unexpected(BacktestError::NoFillModel);
    }
    if (!q.live()) { return std::unexpected(BacktestError::NoTouch); }
    switch (m) {
        case FillModel::Mid: {
            // Rounded to a whole paisa, in the direction that does NOT favour
            // the trade -- so even the wrong model is not made wronger by its
            // rounding.
            const std::int64_t sum = q.bid.raw() + q.ask.raw();
            return Price{side == Side::Buy ? (sum + 1) / 2 : sum / 2};
        }
        case FillModel::Touch:
            return side == Side::Buy ? q.ask : q.bid;
        case FillModel::TouchPlusImpact: {
            const std::int64_t imp =
                static_cast<std::int64_t>(std::fabs(impact_paise) + 0.5);
            return side == Side::Buy ? Price{q.ask.raw() + imp}
                                     : Price{q.bid.raw() - imp};
        }
        case FillModel::Unspecified:
            break;
    }
    return std::unexpected(BacktestError::NoFillModel);
}

/// A resting limit order, with its place in the queue.
///
/// `queue_ahead` is the volume resting in front of it at its price when it was
/// placed. That number is the difference between this model and the naive one,
/// and it is not optional: zero means "front of queue", which is a claim, so
/// it is only ever zero when a caller says so.
struct LimitOrder {
    Side side = Side::Buy;
    Price limit{0};
    Qty qty{0};
    Qty queue_ahead{0};
    Qty filled{0};
    bool active = false;
};

/// Feed one TRADE print to a resting limit order.
///
/// A trade at a price BETTER than the limit (below a buy limit, above a sell
/// limit) trades through it and fills it regardless of queue -- the book at
/// that level cleared. A trade AT the limit consumes the queue first and only
/// then the order.
///
/// Returns the quantity filled by this print.
[[nodiscard]] inline Qty
apply_print_to_limit(LimitOrder& o, Price px, Qty size) noexcept {
    if (!o.active || o.qty.raw() <= o.filled.raw() || size.raw() <= 0) {
        return Qty{0};
    }
    const bool through = o.side == Side::Buy ? px.raw() < o.limit.raw()
                                             : px.raw() > o.limit.raw();
    const bool at = px.raw() == o.limit.raw();
    if (!through && !at) { return Qty{0}; }

    std::int64_t available = size.raw();
    if (at && o.queue_ahead.raw() > 0) {
        // THE QUEUE. Everything resting ahead trades first.
        const std::int64_t eaten = available < o.queue_ahead.raw()
                                     ? available : o.queue_ahead.raw();
        o.queue_ahead = Qty{o.queue_ahead.raw() - eaten};
        available -= eaten;
        if (available <= 0) { return Qty{0}; }
    }
    const std::int64_t want = o.qty.raw() - o.filled.raw();
    const std::int64_t got = available < want ? available : want;
    o.filled = Qty{o.filled.raw() + got};
    if (o.filled.raw() >= o.qty.raw()) { o.active = false; }
    return Qty{got};
}

/// The naive rule, kept so the difference can be measured: a limit order fills
/// the moment the price touches it, in full, regardless of queue or size.
[[nodiscard]] inline bool naive_limit_touched(const LimitOrder& o,
                                              Price px) noexcept {
    if (!o.active) { return false; }
    return o.side == Side::Buy ? px.raw() <= o.limit.raw()
                               : px.raw() >= o.limit.raw();
}

// ---------------------------------------------------------------------------
// The engine
// ---------------------------------------------------------------------------

/// Result of one replay.
struct BacktestResult {
    Notional pnl{};
    Notional costs{};
    std::size_t fills = 0;
    std::size_t ticks = 0;
    FillModel model = FillModel::Unspecified;
    /// The largest conservation residual seen. Zero on a sound run; anything
    /// else and the run is void rather than merely inaccurate.
    std::int64_t worst_residual = 0;
};

/// Drive a strategy over a tape, one tick at a time.
///
/// `Strategy` must expose:
///     Intent on_tick(const Tick&, const Quote&, const Ledger&)
/// where Intent is {Side side; Qty qty; bool act;}.
///
/// The strategy receives the tick, the quote and the LEDGER -- never the tape.
/// There is no argument through which a future tick could arrive, which is
/// what makes rule 7 structural here rather than advisory.
struct Intent {
    Side side = Side::Buy;
    Qty qty{0};
    bool act = false;
};

template <class Strategy>
[[nodiscard]] inline std::expected<BacktestResult, BacktestError>
run(const Tick* tape, const Quote* quotes, std::size_t n, Strategy& strat,
    Ledger& ledger, FillModel model, const ChargeSchedule& sch,
    const BrokerageRule& br, double impact_paise, Segment segment,
    Exchange ex) noexcept {
    if (model == FillModel::Unspecified) {
        return std::unexpected(BacktestError::NoFillModel);
    }
    BacktestResult out{};
    out.model = model;

    for (std::size_t i = 0; i < n; ++i) {
        // ONE tick. The strategy never sees the array, its length, or its
        // cursor.
        const Tick& t = tape[i];
        const Quote& q = quotes[i];
        ++out.ticks;

        const Intent in = strat.on_tick(t, q, ledger);
        if (!in.act || in.qty.raw() <= 0) { continue; }

        const auto px = market_fill_price(q, in.side, model, impact_paise);
        if (!px) { continue; }          // no quote: no fill, not a mid guess

        Trade tr{};
        tr.segment = segment;
        tr.exchange = ex;
        tr.side = in.side;
        tr.qty = in.qty;
        tr.price = *px;
        tr.trade_ts = t.exchange_ts;    // time off the TICK (rule 7)
        const auto c = compute_cost(tr, sch, br);
        if (!c) { return std::unexpected(BacktestError::CostUnavailable); }

        Fill f{};
        f.ts = t.exchange_ts;
        f.side = in.side;
        f.qty = in.qty;
        f.price = *px;
        f.cost = c->total;
        if (const auto ok = ledger.apply(f); !ok) { return std::unexpected(ok.error()); }

        const std::int64_t r = ledger.residual();
        const std::int64_t a = r < 0 ? -r : r;
        if (a > out.worst_residual) { out.worst_residual = a; }
    }

    // Mark at the last quote's mid, which is the only defensible mark: the
    // last TRADE could be a print on either side of a wide spread.
    if (n > 0 && quotes[n - 1].live()) {
        const Price mark{(quotes[n - 1].bid.raw() + quotes[n - 1].ask.raw()) / 2};
        out.pnl = ledger.pnl(mark);
    }
    out.costs = ledger.costs();
    out.fills = ledger.fills();
    return out;
}

} // namespace altair
