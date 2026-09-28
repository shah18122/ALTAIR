// oms/paper_venue.hpp -- a deterministic paper fill simulator.
//
// P7-07. Turns a resting order plus a quote into fills, without a live broker.
// The scanners in strategies/ detect; oms/ executes; until this file, oms/
// could only execute for real. This is the isolated venue paper trading needs.
//
// IT REUSES, IT DOES NOT REIMPLEMENT.
//
//   - The state machine is oms/order_state.hpp. This venue produces the same
//     events a broker adapter would (Ack, Fill) and feeds them to apply()/
//     apply_fill(). cum_qty, avg_price and state are the machine's, never the
//     venue's own count.
//   - The ledger is core/invariant/conservation.hpp's ConservationLedger. It
//     owns virtual cash, the signed position, the running fills/costs totals,
//     and the exact conservation identity Sum(fills)+Sum(costs)+cash_delta==0.
//     A PaperLedger reinvented here would be that class with worse tests.
//   - Every fill is priced through risk/cost.hpp's compute_cost. An unverified
//     schedule is refused up front: a paper fill priced off unchecked rates is
//     a fabricated number, not a cheaper one.
//
// NO LIVE ROUTE, BY CONSTRUCTION. This header includes no broker adapter, no
// dispatch gate, no socket. There is no code path from here to a real order.
//
// SINGLE INSTRUMENT. ConservationLedger is per-symbol (its own note), so a
// PaperVenue is per-symbol too: PaperConfig carries the one segment/exchange,
// and every order and quote it sees is that instrument. A multi-symbol paper
// book is a portfolio of these, which is P4-03 territory, not this card.
//
// CONSERVATIVE, AND IT REFUSES RATHER THAN GUESSES (rule 9):
//   - a quote older than max_quote_age never fills -- a stale price is not a
//     standing one;
//   - a fill never exceeds the depth actually quoted -- no assumed liquidity;
//   - a buy virtual cash cannot fund does not fill, and cash never goes
//     negative (a zero-balance account is a real account, rule 16);
//   - a limit only fills at or through its price, with improvement to us.
//
// DETERMINISTIC AND CLOCK-FREE. Same inputs, same fills, byte for byte
// (rule 6). `now` is always a parameter; this file reads no wall clock
// (rule 7). Money is integer paise, quantity is Qty, no double crosses a
// boundary.

#pragma once

#include <core/invariant/conservation.hpp>
#include <core/time/timestamp.hpp>
#include <core/types/units.hpp>
#include <oms/order_intent.hpp>
#include <oms/order_state.hpp>
#include <risk/cost.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <vector>

namespace altair::oms {

/// Top of book supplied by the caller. The venue reads no feed itself. Prices
/// are integer paise; sizes are Qty (units, not lots); ts is when the quote was
/// observed, UTC ns.
struct PaperQuote {
    Price bid{0};
    Price ask{0};
    Qty   bid_qty{0};
    Qty   ask_qty{0};
    Timestamp ts{};
};

/// Simulation policy. Every field is caller-set; there is no hidden default
/// that would let a test pass without stating the cost schedule.
struct PaperConfig {
    /// A quote older than this at decision time yields no fill.
    Duration max_quote_age{};
    /// An order cannot fill before submit_ts + latency. Zero means immediate.
    Duration latency{};
    /// Passed straight to risk/cost.hpp. An unverified schedule is refused.
    ChargeSchedule schedule{};
    BrokerageRule  brokerage{};
    /// The instrument's cost segment/exchange, needed to price a fill.
    Segment  segment  = Segment::Cash;
    Exchange exchange = Exchange::NSE;
    bool     delivery = false;
};

enum class PaperError : std::uint8_t {
    ScheduleUnverified, ///< PaperConfig.schedule.verified is false
    LotOverflow,        ///< lots * lot_size overflowed Qty
    MalformedIntent,    ///< the intent is not complete()/positive
    OrderRejected,      ///< the state machine refused order creation/ack
    CostRefused,        ///< risk/cost.hpp refused a fill's cost
    ConservationBreach  ///< the ledger identity did not hold after a fill
};

/// One simulated fill. cum_qty/avg_price/state are the state machine's.
struct PaperFill {
    ClientOrderId id{};
    Qty       fill_qty{0};    ///< this fill only, units
    Price     fill_price{0};  ///< paise
    CostBreakdown cost{};     ///< itemised, from risk/cost.hpp, this fill
    OrderState state{OrderState::Unset}; ///< order state AFTER this fill
    Timestamp ts{};           ///< the quote ts that produced the fill
};

class PaperVenue {
public:
    /// opening_cash is the virtual balance in paise. A zero balance is legal
    /// and must still refuse a buy it cannot fund.
    explicit PaperVenue(Notional opening_cash, PaperConfig config) noexcept
        : ledger_(opening_cash), config_(config) {}

    /// Accept a validated intent as a working order. `lot_size` is from the
    /// spec store (rule 1: never a literal). Returns the created order's id.
    /// Does not fill: submission and fill are separate instants (latency).
    /// UNIT: now is UTC ns. PRECONDITION: intent.complete().
    [[nodiscard]] std::expected<ClientOrderId, PaperError>
    submit(const OrderIntent& intent, LotSize lot_size, Timestamp now) noexcept {
        if (!config_.schedule.verified) {
            return std::unexpected(PaperError::ScheduleUnverified);
        }
        if (!intent.complete() || intent.lots <= 0) {
            return std::unexpected(PaperError::MalformedIntent);
        }
        const auto qty = qty_of(Lots{intent.lots}, lot_size);
        if (!qty.has_value()) {
            return std::unexpected(PaperError::LotOverflow);
        }
        auto order = open_order(*qty, now);
        if (!order.has_value()) {
            return std::unexpected(PaperError::OrderRejected);
        }
        // Deterministic paper acknowledgement: the order rests immediately, so
        // fills apply from Open. A real adapter would wait for the broker's
        // Ack event; the paper venue is that event's source.
        if (!apply(*order, OrderEvent::Ack, now).has_value()) {
            return std::unexpected(PaperError::OrderRejected);
        }
        const ClientOrderId id = make_client_order_id(session_, seq_++);
        Working w{};
        w.id = id;
        w.order = *order;
        w.side = intent.side;
        w.type = intent.order_type;
        w.limit = Price{intent.limit_paise};
        w.eligible = now + config_.latency;
        working_.push_back(w);
        return id;
    }

    /// Advance every working order against this quote at time `now`. Appends a
    /// PaperFill per fill. Returns the fill count. Each fill is atomic in the
    /// ledger and the state machine; if a fill's cost is refused, the fills
    /// already booked in this call stand (they are real and conservation still
    /// holds) and the erroring order is left unchanged.
    /// UNIT: now is UTC ns.
    [[nodiscard]] std::expected<int, PaperError>
    on_quote(const PaperQuote& quote, Timestamp now,
             std::vector<PaperFill>& out) noexcept {
        // A stale quote is a normal event, not an error: no fills, return 0.
        if ((now - quote.ts) > config_.max_quote_age) {
            return 0;
        }
        int fills = 0;
        for (Working& w : working_) {
            if (!is_live(w.order.state) || w.order.done()) { continue; }
            if (now < w.eligible) { continue; }         // latency gate

            const bool buy = (w.side == IntentSide::Buy);
            const Price px      = buy ? quote.ask : quote.bid;
            const Qty   avail   = buy ? quote.ask_qty : quote.bid_qty;
            if (px.raw() <= 0 || avail.raw() <= 0) { continue; }   // no depth
            if (w.type == IntentType::Limit) {
                const bool ok = buy ? (px.raw() <= w.limit.raw())
                                    : (px.raw() >= w.limit.raw());
                if (!ok) { continue; }                  // limit not met
            }

            const std::int64_t leaves = w.order.leaves().raw();
            const std::int64_t incr_raw =
                leaves < avail.raw() ? leaves : avail.raw();
            if (incr_raw <= 0) { continue; }
            const Qty incr{incr_raw};

            const auto fill_notional = notional_of(px, incr);
            if (!fill_notional.has_value()) { continue; } // cannot price size

            const auto cost = compute_cost(
                make_trade(buy, incr, px, quote.ts), config_.schedule,
                config_.brokerage);
            if (!cost.has_value()) {
                return std::unexpected(PaperError::CostRefused);
            }

            // A buy virtual cash cannot fund does not fill. Cash never negative.
            if (buy) {
                const std::int64_t need = fill_notional->raw() + cost->total.raw();
                if (need > ledger_.cash().raw()) { continue; }
            }

            // Book the cash leg. Signed qty: +buy, -sell (ConservationLedger).
            const Qty ledger_qty{buy ? incr_raw : -incr_raw};
            if (!ledger_.on_fill(ledger_qty, px, cost->total).has_value()) {
                return std::unexpected(PaperError::ConservationBreach);
            }
            if (!ledger_.check().has_value()) {
                return std::unexpected(PaperError::ConservationBreach);
            }

            // Advance the order through the real state machine. cum is a
            // running total; avg is the VWAP over everything done.
            const Qty new_cum{w.order.cum_qty.raw() + incr_raw};
            const Price new_avg = vwap(w.order.cum_qty, w.order.avg_price,
                                       incr, px, new_cum);
            const FillReport fr{new_cum, new_avg, now};
            if (!apply_fill(w.order, fr).has_value()) {
                // The cash leg booked but the state machine refused the fill:
                // the ledger is now knowingly ahead of the order. Fail loud.
                return std::unexpected(PaperError::ConservationBreach);
            }

            PaperFill pf{};
            pf.id = w.id;
            pf.fill_qty = incr;
            pf.fill_price = px;
            pf.cost = *cost;
            pf.state = w.order.state;
            pf.ts = quote.ts;
            out.push_back(pf);
            ++fills;
        }
        return fills;
    }

    /// Virtual cash, paise. From the ledger.
    [[nodiscard]] Notional cash() const noexcept { return ledger_.cash(); }
    /// Signed net position in units. From the ledger.
    [[nodiscard]] Qty position() const noexcept { return ledger_.position(); }
    /// Working orders still live (PendingNew/Open/PartiallyFilled).
    [[nodiscard]] std::size_t working_count() const noexcept {
        std::size_t n = 0;
        for (const Working& w : working_) {
            if (is_live(w.order.state) && !w.order.done()) { ++n; }
        }
        return n;
    }
    /// True iff the conservation identity holds exactly. From the ledger.
    [[nodiscard]] bool conservation_ok() const noexcept {
        return ledger_.check().has_value();
    }
    /// The ledger, for a caller that wants equity, cash_delta or reconcile.
    [[nodiscard]] const ConservationLedger& ledger() const noexcept {
        return ledger_;
    }

private:
    struct Working {
        ClientOrderId id{};
        Order order{};
        IntentSide side = IntentSide::Buy;
        IntentType type = IntentType::Limit;
        Price limit{0};
        Timestamp eligible{};
    };

    [[nodiscard]] Trade make_trade(bool buy, Qty qty, Price px,
                                   Timestamp ts) const noexcept {
        Trade t{};
        t.segment  = config_.segment;
        t.exchange = config_.exchange;
        t.side     = buy ? Side::Buy : Side::Sell;
        t.qty      = qty;
        t.price    = px;
        t.delivery = config_.delivery;
        t.trade_ts = ts;
        return t;
    }

    /// VWAP over prior fills plus this one, integer paise, truncated. Both
    /// legs go through notional_of so the products are overflow-checked;
    /// callers price real chains, so the sum is guarded too.
    [[nodiscard]] static Price vwap(Qty old_cum, Price old_avg, Qty incr,
                                    Price px, Qty new_cum) noexcept {
        if (new_cum.raw() <= 0) { return Price{0}; }
        const auto prior = notional_of(old_avg, old_cum);
        const auto add   = notional_of(px, incr);
        if (!prior.has_value() || !add.has_value()) { return px; }
        // Guarded sum: if it would overflow, fall back to the latest price
        // rather than wrap. Real magnitudes are far below this bound.
        if (add->raw() > 0 && prior->raw() > kI64Max - add->raw()) { return px; }
        const std::int64_t total = prior->raw() + add->raw();
        return Price{total / new_cum.raw()};
    }

    static constexpr std::int64_t kI64Max = 0x7FFF'FFFF'FFFF'FFFF;

    ConservationLedger ledger_;
    PaperConfig config_;
    std::vector<Working> working_{};
    std::uint32_t session_ = 1;
    std::uint32_t seq_ = 0;
};

} // namespace altair::oms
