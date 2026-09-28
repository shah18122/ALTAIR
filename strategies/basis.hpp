// strategies/basis.hpp -- the cash-futures basis and the cross-venue scanner.
//
// P5-05. The first of Phase 5's four arbitrage scanners.
//
// THIS DIRECTORY DETECTS. IT DOES NOT TRADE.
//
// A scanner produces a priced, executability-tagged observation and writes it
// to a log. Deciding what to do with one -- whether to take it, at what size,
// against what limits -- belongs to the trade handler in oms/, which is the
// only thing in this tree that can place an order. Nothing here includes an
// oms/ header, and it never will (CLAUDE.md, one component one directory).
//
// HALF THE OPPORTUNITIES A NAIVE BASIS SCANNER REPORTS CANNOT BE TRADED.
//
// This is the card. The basis goes both ways -- the future can be rich or
// cheap against cash -- and the arithmetic is symmetric, so a scanner written
// from the formula reports both directions and looks twice as productive as it
// is.
//
// It is not symmetric in India. Naked short-selling in the cash segment is not
// permitted, and borrowing through SLB is thin, expensive and unavailable in
// size for most of the list. So:
//
//   FUTURE RICH  -> buy cash, sell future. Cash-and-carry. EXECUTABLE.
//   FUTURE CHEAP -> buy future, SHORT CASH. Reverse. NOT EXECUTABLE if the
//                   position must be carried, because carrying a short cash
//                   position means delivering stock you do not have.
//
// The one exception is a position opened and closed inside the same session:
// intraday short IS permitted in the cash segment. So a cross-venue scan
// (buy NSE, sell BSE, both squared off before the close) is executable in
// both directions, while a cash-futures carry to expiry is executable in one.
// The difference is not a detail -- it is the difference between a scanner
// that finds twenty opportunities a day and one that finds ten.
//
// `Executability` therefore travels with every result, its ordinal 0 is
// `Unknown`, and `net_edge()` refuses to report an edge on a direction it has
// not been told is reachable. A zeroed result is not a tradable one.
//
// EVERY EDGE IS NET OF FULL COST BEFORE IT EXISTS (rule 5).
//
// There is no gross-edge field to read by mistake. The cash leg of a carry is
// a DELIVERY trade, and delivery STT is 0.1% on BOTH sides -- 0.2% for the
// round trip, against 0.05% sell-side on the futures leg.
//
// Measured, on a 500-share RELIANCE carry at 2950 over 30 days: Rs 4,235
// across the four legs, 0.287% of turnover, of which Rs 3,697 is STT. So the
// future must be rich by Rs 8.47 per share ABOVE fair before the trade breaks
// even -- a total basis of Rs 22.32, an implied financing rate of 9.2%
// against a real rate of 6.5%. A 270 basis point hurdle before the first
// rupee of profit.
//
// The cross-venue scan lands in the same place: a 60-paise NSE/BSE cross on
// 500 shares is Rs 300 gross and Rs 584 of cost, so it nets MINUS Rs 284.
// This is what CLAUDE.md means by Phase 5's most valuable output possibly
// being the proof that the edge is gone.
//
// A SCREEN BASIS AND A TRADABLE BASIS CAN HAVE OPPOSITE SIGNS. Rs 12.00 over
// spot on a 2950 stock looks like a rich future; against Rs 13.85 of carry it
// is Rs 1.85 cheap. Nothing downstream is ever handed the raw basis for that
// reason.
//
// THE TWO LEGS MUST BE OBSERVED AT THE SAME INSTANT.
//
// A basis computed from a fresh future and a cash print from nine seconds ago
// is not a measurement of the basis; it is a measurement of how much the
// market moved in nine seconds, and it will show its largest "opportunities"
// exactly when the market is moving fastest and the fill is least likely.
// `max_skew` is required and has no default.

#pragma once

#include <core/types/units.hpp>
#include <risk/cost.hpp>

#include <cmath>
#include <cstdint>
#include <expected>

namespace altair {

/// Which way the mispricing points, and therefore which trade it implies.
enum class BasisDirection : std::uint8_t {
    /// Nothing established yet. Ordinal 0 is NOT a tradable direction.
    Unknown = 0,
    /// The future is rich against carry: buy cash, sell the future.
    CashAndCarry,
    /// The future is cheap against carry: buy the future, short cash.
    ReverseCashAndCarry
};

/// Screen classification of futures relative to cash. This is deliberately
/// separate from executable direction: a premium can still be untradeable
/// after costs, borrow or stale-touch checks.
enum class PremiumDiscount : std::uint8_t {
    Unknown = 0,
    Premium,
    Discount,
    Fair
};

/// Whether the trade this mispricing implies can actually be placed.
///
/// Ordinal 0 is `Unknown` on purpose. A zeroed struct must not read as
/// "executable" -- that is the phantom-default-state bug, and it would turn
/// every uninitialised scan result into a tradable-looking signal.
enum class Executability : std::uint8_t {
    Unknown = 0,
    Executable,
    /// The trade needs a short cash position carried past the session, which
    /// requires stock borrow. Not a rejection of the arithmetic -- the
    /// mispricing is real, we simply cannot reach it.
    ShortCashUnavailable,
    /// The two legs were observed too far apart to be compared.
    QuotesTooFarApart,
    /// One of the legs had no quote.
    NoQuote,
    /// The scanned size exceeds the size actually resting at the touched
    /// prices. A two-leg arbitrage cannot be half-filled: doing one leg and
    /// not the other leaves a naked position rather than a smaller arbitrage.
    /// This therefore REFUSES rather than shrinking quietly. Appended last so
    /// the existing ordinals -- and ordinal 0 = Unknown -- do not move.
    InsufficientSize,
    /// The account-side inventory plus permitted short capacity cannot cover
    /// the requested sell leg. Refuses rather than shrinking the pair.
    InventoryUnavailable,
    /// Settlement eligibility was not proven for the requested same-session
    /// cross. Unknown is conservative and refuses exactly like ineligible.
    SettlementUnavailable
};

enum class BasisError : std::uint8_t {
    /// A price was zero or negative.
    BadPrice,
    /// The scanned quantity was zero or negative.
    BadQuantity,
    /// Time to expiry was zero or negative -- there is no carry to price.
    BadTenor,
    /// The two legs were observed too far apart.
    StaleLeg,
    /// The cost calculator refused.
    CostUnavailable,
    /// The caller did not say how stale is too stale.
    NoSkewLimit,
    /// The execution allowance was negative or non-finite.
    BadAllowance
};

/// The two legs of a basis observation, each with the instant it was seen.
struct BasisQuote {
    Price spot{0};
    Timestamp spot_ts{};
    Price future{0};
    Timestamp future_ts{};
    /// Time to the future's expiry. From the spec store, never a literal
    /// (rule 1).
    Years t{0.0};
    /// Continuously-compounded financing rate, per year.
    double rate = 0.0;
    /// Continuous dividend yield over the life of the future, per year. NIFTY
    /// pays a meaningful one and ignoring it makes every future look cheap.
    double dividend_yield = 0.0;
};

/// What the basis is, before any question of whether it can be traded.
struct Basis {
    /// future - spot, in paise. The raw number a screen shows.
    std::int64_t basis{0};
    /// The basis carry alone accounts for: S*(exp((r-q)T) - 1), in paise.
    double fair_basis = 0.0;
    /// future - fair, in paise. THE mispricing. Signed: positive is rich.
    double mispricing = 0.0;
    /// The financing rate the future is actually quoting, per year:
    /// ln(F/S)/T + q. Comparable across tenors in a way the basis is not --
    /// a 40-point basis on a one-week future and on a three-month future are
    /// wildly different rates.
    double implied_repo = 0.0;
    /// How far apart the two legs were observed.
    Duration skew{0};
};

/// Measure the basis. Says nothing about whether it can be traded.
[[nodiscard]] inline std::expected<Basis, BasisError>
measure_basis(const BasisQuote& q, Duration max_skew) noexcept {
    if (max_skew.raw() <= 0) {
        return std::unexpected(BasisError::NoSkewLimit);
    }
    if (q.spot.raw() <= 0 || q.future.raw() <= 0) {
        return std::unexpected(BasisError::BadPrice);
    }
    if (!(q.t.raw() > 0.0)) { return std::unexpected(BasisError::BadTenor); }

    const std::int64_t raw_skew = (q.future_ts - q.spot_ts).raw();
    const Duration skew{raw_skew < 0 ? -raw_skew : raw_skew};
    if (skew.raw() > max_skew.raw()) {
        return std::unexpected(BasisError::StaleLeg);
    }

    const double S = static_cast<double>(q.spot.raw());
    const double F = static_cast<double>(q.future.raw());
    const double T = q.t.raw();

    Basis b{};
    b.skew = skew;
    b.basis = q.future.raw() - q.spot.raw();
    // Continuous carry. exp(x) - 1 rather than exp(x) - S: for the small x a
    // real carry produces (r-q)T is order 0.01, and forming the difference of
    // two numbers near 24,000 to get one near 40 throws away four digits.
    b.fair_basis = S * std::expm1((q.rate - q.dividend_yield) * T);
    b.mispricing = (F - S) - b.fair_basis;
    b.implied_repo = std::log(F / S) / T + q.dividend_yield;
    return b;
}

/// A priced, executability-tagged opportunity.
///
/// There is deliberately no `gross_edge` field. Rule 5: a strategy never sees
/// a pre-cost number, so the only edge this type can report is the one that
/// survives the bill.
struct BasisOpportunity {
    BasisDirection direction = BasisDirection::Unknown;
    Executability executability = Executability::Unknown;
    Basis basis{};
    /// Total cost of BOTH legs, both ways, in paise. Itemised behind it.
    Notional cost{};
    /// The mispricing, in paise, for the quantity scanned.
    Notional mispricing_value{};
    /// mispricing_value - cost. Signed. THE number. Negative is the usual
    /// answer, and reporting it rather than hiding it is the point of Phase 5.
    Notional net{};
    /// Cost decomposition, so a negative answer can be explained rather than
    /// merely believed (P3-09 D5).
    CostBreakdown cash_buy{};
    CostBreakdown cash_sell{};
    CostBreakdown fut_sell{};
    CostBreakdown fut_buy{};

    /// The edge, and ONLY when the trade can be reached. An unexecutable
    /// mispricing has no edge -- it has a number, which is a different thing,
    /// and a scanner that returned it would be reporting profit on a trade
    /// that cannot be placed.
    [[nodiscard]] std::expected<Notional, Executability> net_edge() const
        noexcept {
        if (executability != Executability::Executable) {
            return std::unexpected(executability);
        }
        return net;
    }

    /// True when this is worth handing to the trade handler at all.
    [[nodiscard]] bool actionable() const noexcept {
        return executability == Executability::Executable && net.raw() > 0;
    }
};

/// Whether a short cash leg can be carried.
///
/// Passed in rather than assumed, because it is a fact about the account and
/// the SLB market on the day, not about the arithmetic. `false` is the
/// realistic retail default and the caller must say so explicitly.
struct ShortCashCapability {
    bool intraday_short = false;
    bool borrow_available = false;
};

/// Executable cash/futures touches for one observation. Prices are integer
/// paise and timestamps are UTC nanoseconds. All four prices are required by
/// `scan_cash_futures_executable`; a zero touch is refused rather than replaced
/// by the analytical mark in `BasisQuote`.
struct CashFuturesExecutionQuote {
    Price cash_bid{0};
    Price cash_ask{0};
    Price future_bid{0};
    Price future_ask{0};
    Timestamp cash_ts{};
    Timestamp future_ts{};
    Years t{0.0};
    double rate{};
    double dividend_yield{};
    /// Borrow cost per unit for the reverse-carry cash short, in paise.
    Notional borrow_cost_per_unit{};
};

struct CashFuturesExecutableOpportunity {
    PremiumDiscount classification{PremiumDiscount::Unknown};
    BasisDirection direction{BasisDirection::Unknown};
    Executability executability{Executability::Unknown};
    Duration skew{};
    Notional entry_edge{};
    Notional cost{};
    Notional borrow_cost{};
    Notional net{};
    CostBreakdown open_cash{};
    CostBreakdown open_future{};
    CostBreakdown close_cash{};
    CostBreakdown close_future{};

    [[nodiscard]] bool actionable() const noexcept {
        return executability == Executability::Executable && net.raw() > 0;
    }
};

/// Account and venue constraints for one same-session cross-venue pair.
///
/// UNIT: quantities are instrument units. `sell_inventory` is stock already
/// deliverable on the selling venue; `permitted_short` is the caller's bounded
/// short capacity for the residual. Settlement must be explicit: Unknown is
/// refused rather than treated as eligible. PRECONDITION: quantities are
/// non-negative; the scanner refuses negative `Qty` values.
enum class CrossVenueSettlement : std::uint8_t {
    Unknown = 0,
    Eligible,
    Ineligible
};

struct CrossVenueExecutionConstraints {
    Qty sell_inventory{0};
    Qty permitted_short{0};
    CrossVenueSettlement settlement{CrossVenueSettlement::Unknown};
};

namespace detail {

[[nodiscard]] inline Trade leg(Segment seg, Exchange ex, Side side, Qty qty,
                               Price px, bool delivery, Timestamp ts) noexcept {
    Trade t{};
    t.segment = seg;
    t.exchange = ex;
    t.side = side;
    t.qty = qty;
    t.price = px;
    t.delivery = delivery;
    t.trade_ts = ts;
    return t;
}

} // namespace detail

/// Scan one cash-futures pair, carried to expiry.
///
/// `carried_to_expiry` is what makes the short-cash question bite: a carry
/// held to expiry needs a delivery position on the cash leg, and a short
/// delivery position is stock you do not have.
[[nodiscard]] inline std::expected<BasisOpportunity, BasisError>
scan_cash_futures(const BasisQuote& q, Qty qty, Exchange ex,
                  Duration max_skew, const ShortCashCapability& cap,
                  const ChargeSchedule& sch, const BrokerageRule& br) noexcept {
    const auto b = measure_basis(q, max_skew);
    if (!b) { return std::unexpected(b.error()); }

    BasisOpportunity o{};
    o.basis = *b;
    o.direction = b->mispricing > 0.0 ? BasisDirection::CashAndCarry
                                      : BasisDirection::ReverseCashAndCarry;

    // A carry to expiry needs DELIVERY on the cash leg. The reverse trade
    // therefore needs a short delivery position, which needs borrow.
    o.executability =
        (o.direction == BasisDirection::CashAndCarry || cap.borrow_available)
            ? Executability::Executable
            : Executability::ShortCashUnavailable;

    // Four legs: open and close, cash and future. Priced at the observed
    // prices, which understates nothing -- the exit prices converge by
    // arbitrage, so the entry price is the right basis for the exit's
    // turnover to within the mispricing itself.
    const Timestamp ts = q.future_ts;
    const auto cb = compute_cost(
        detail::leg(Segment::Cash, ex, Side::Buy, qty, q.spot, true, ts),
        sch, br);
    const auto cs = compute_cost(
        detail::leg(Segment::Cash, ex, Side::Sell, qty, q.spot, true, ts),
        sch, br);
    const auto fs = compute_cost(
        detail::leg(Segment::Fut, ex, Side::Sell, qty, q.future, false, ts),
        sch, br);
    const auto fb = compute_cost(
        detail::leg(Segment::Fut, ex, Side::Buy, qty, q.future, false, ts),
        sch, br);
    if (!cb || !cs || !fs || !fb) {
        return std::unexpected(BasisError::CostUnavailable);
    }
    o.cash_buy = *cb; o.cash_sell = *cs; o.fut_sell = *fs; o.fut_buy = *fb;
    o.cost = Notional{cb->total.raw() + cs->total.raw() + fs->total.raw()
                      + fb->total.raw()};

    // The mispricing is per unit; the value is per unit times quantity. Both
    // directions capture |mispricing| if they can be traded at all.
    const double per_unit = b->mispricing < 0.0 ? -b->mispricing
                                                : b->mispricing;
    const double value = per_unit * static_cast<double>(qty.raw());
    o.mispricing_value = Notional{static_cast<std::int64_t>(value + 0.5)};
    o.net = Notional{o.mispricing_value.raw() - o.cost.raw()};
    return o;
}

/// Scan executable cash/futures touches while retaining analytical premium /
/// discount classification. Direction-specific entry prices are used:
/// cash-and-carry buys cash at ask and sells futures at bid; reverse carry buys
/// futures at ask and sells cash at bid. `max_skew` is nanoseconds and `qty` is
/// units. Unknown/negative assumptions return a typed error or refusal.
[[nodiscard]] inline std::expected<CashFuturesExecutableOpportunity, BasisError>
scan_cash_futures_executable(const CashFuturesExecutionQuote& q, Qty qty,
                             Exchange ex, Duration max_skew,
                             const ShortCashCapability& cap,
                             const ChargeSchedule& sch,
                             const BrokerageRule& br) noexcept {
    if (qty.raw() <= 0) return std::unexpected(BasisError::BadQuantity);
    if (max_skew.raw() <= 0) return std::unexpected(BasisError::NoSkewLimit);
    if (q.cash_bid.raw() <= 0 || q.cash_ask.raw() <= 0
        || q.future_bid.raw() <= 0 || q.future_ask.raw() <= 0
        || q.cash_bid.raw() > q.cash_ask.raw()
        || q.future_bid.raw() > q.future_ask.raw())
        return std::unexpected(BasisError::BadPrice);
    if (!(q.t.raw() > 0.0) || !std::isfinite(q.rate)
        || !std::isfinite(q.dividend_yield)
        || q.borrow_cost_per_unit.raw() < 0)
        return std::unexpected(BasisError::BadTenor);
    const std::int64_t raw_skew = (q.future_ts - q.cash_ts).raw();
    const Duration skew{raw_skew < 0 ? -raw_skew : raw_skew};
    if (skew.raw() > max_skew.raw())
        return std::unexpected(BasisError::StaleLeg);

    const double cash_mid = 0.5 * static_cast<double>(q.cash_bid.raw()
                                                       + q.cash_ask.raw());
    const double future_mid = 0.5 * static_cast<double>(q.future_bid.raw()
                                                         + q.future_ask.raw());
    const double fair = cash_mid * std::expm1((q.rate - q.dividend_yield)
                                               * q.t.raw());
    const double screen = (future_mid - cash_mid) - fair;
    CashFuturesExecutableOpportunity out{};
    out.classification = screen > 0.5 ? PremiumDiscount::Premium
                       : screen < -0.5 ? PremiumDiscount::Discount
                                       : PremiumDiscount::Fair;
    out.skew = skew;
    out.direction = screen >= 0.0 ? BasisDirection::CashAndCarry
                                  : BasisDirection::ReverseCashAndCarry;
    if (out.direction == BasisDirection::ReverseCashAndCarry
        && !cap.borrow_available) {
        out.executability = Executability::ShortCashUnavailable;
    } else {
        out.executability = Executability::Executable;
    }

    const Timestamp cash_ts = q.cash_ts;
    const Timestamp future_ts = q.future_ts;
    const bool reverse = out.direction == BasisDirection::ReverseCashAndCarry;
    const auto cash_open = detail::leg(Segment::Cash, ex,
        reverse ? Side::Sell : Side::Buy, qty,
        reverse ? q.cash_bid : q.cash_ask, true, cash_ts);
    const auto future_open = detail::leg(Segment::Fut, ex,
        reverse ? Side::Buy : Side::Sell, qty,
        reverse ? q.future_ask : q.future_bid, false, future_ts);
    const auto cash_close = detail::leg(Segment::Cash, ex,
        reverse ? Side::Buy : Side::Sell, qty,
        reverse ? q.cash_ask : q.cash_bid, true, cash_ts);
    const auto future_close = detail::leg(Segment::Fut, ex,
        reverse ? Side::Sell : Side::Buy, qty,
        reverse ? q.future_bid : q.future_ask, false, future_ts);
    const auto cb = compute_cost(cash_open, sch, br);
    const auto fb = compute_cost(future_open, sch, br);
    const auto cc = compute_cost(cash_close, sch, br);
    const auto fc = compute_cost(future_close, sch, br);
    if (!cb || !fb || !cc || !fc)
        return std::unexpected(BasisError::CostUnavailable);
    out.open_cash = *cb; out.open_future = *fb;
    out.close_cash = *cc; out.close_future = *fc;
    out.cost = Notional{cb->total.raw() + fb->total.raw()
                       + cc->total.raw() + fc->total.raw()};
    out.borrow_cost = Notional{reverse
        ? q.borrow_cost_per_unit.raw() * qty.raw() : 0};
    const std::int64_t gross_per_unit = reverse
        ? q.cash_bid.raw() - q.future_ask.raw()
        : q.future_bid.raw() - q.cash_ask.raw();
    out.entry_edge = Notional{gross_per_unit * qty.raw()};
    out.net = Notional{out.entry_edge.raw() - out.cost.raw()
                       - out.borrow_cost.raw()};
    return out;
}

// ---------------------------------------------------------------------------
// Cross-venue
// ---------------------------------------------------------------------------

/// The allowance a scan must clear before its edge is real.
///
/// An allowance is a MEASUREMENT, not a policy this header may invent, so it
/// is passed in. `impact_bps` is what crossing past the touch would cost, and
/// `latency_bps` is what the market moving between the two legs and the order
/// arriving would cost. `risk/slippage.hpp` (P3-10) is the producer of the
/// impact figure -- its `predict_upper` returns bps and is the one to size on,
/// because sizing on the point estimate assumes the fitted coefficient is the
/// true one. This header consumes bps and does not fit anything; that keeps the
/// square-root law in one place instead of two.
///
/// UNIT: basis points of traded turnover, per leg-side, non-negative.
struct ExecutionAllowance {
    double impact_bps = 0.0;
    double latency_bps = 0.0;

    [[nodiscard]] double total_bps() const noexcept {
        return impact_bps + latency_bps;
    }
};

/// The same instrument quoted on two exchanges at the same instant.
struct CrossVenueQuote {
    /// Best ask where we would BUY, and best bid where we would SELL. Not
    /// mids: a mid-to-mid "spread" between two venues is an arbitrage you
    /// cannot fill, and it is the single most common way a cross-venue
    /// scanner manufactures edge that does not exist.
    Exchange buy_venue = Exchange::NSE;
    Price buy_ask{0};
    /// Size actually resting at `buy_ask`. UNIT: units.
    ///
    /// Zero means "the caller did not say", which refuses. A scanner that
    /// assumed unlimited size at the touch reports arbitrage in a size it
    /// could never get, which is the same class of error as comparing mids.
    Qty buy_ask_qty{0};
    Timestamp buy_ts{};
    Exchange sell_venue = Exchange::BSE;
    Price sell_bid{0};
    /// Size actually resting at `sell_bid`. UNIT: units. Zero refuses.
    Qty sell_bid_qty{0};
    Timestamp sell_ts{};
};

struct CrossVenueOpportunity {
    Executability executability = Executability::Unknown;
    /// sell_bid - buy_ask, in paise per unit. Signed; usually negative,
    /// because the spread is normally wider than the venue difference.
    std::int64_t cross_spread{0};
    Duration skew{0};
    /// The size the touch could actually support: the smaller of the two
    /// resting sizes. Reported even on a refusal, so a caller can see how far
    /// off it was rather than only that it was refused. UNIT: units.
    Qty executable_size{0};
    /// Scanned size minus `executable_size`, when the scan was too large.
    /// UNIT: units. Zero when the size was available.
    Qty size_short{0};
    /// The charge total. Does NOT include the execution allowance -- the
    /// allowance is not a charge, and folding it in here would hide a
    /// measurement inside a tax breakdown.
    Notional cost{};
    /// The allowance in paise: turnover x total_bps / 10000.
    Notional allowance{};
    Notional gross_value{};
    /// gross_value - cost - allowance. Signed. The allowance is subtracted
    /// because an edge that does not clear impact and latency is not an edge.
    Notional net{};
    CostBreakdown buy_leg{};
    CostBreakdown sell_leg{};

    [[nodiscard]] std::expected<Notional, Executability> net_edge() const
        noexcept {
        if (executability != Executability::Executable) {
            return std::unexpected(executability);
        }
        return net;
    }
    [[nodiscard]] bool actionable() const noexcept {
        return executability == Executability::Executable && net.raw() > 0;
    }
};

/// Scan the same scrip across two venues, INTRADAY.
///
/// Intraday is what makes this direction-symmetric: a short opened and closed
/// inside the session is permitted in the cash segment, so unlike the carry
/// above, both directions are reachable -- provided the caller says intraday
/// short is enabled on the account. It still is not free: intraday STT is
/// 0.025% sell-side, and it applies to the selling venue's leg.
///
/// THE SIZE HAS TO BE THERE AND THE EDGE HAS TO CLEAR THE ALLOWANCE.
///
///   SIZE. A price is not a size. The touch may hold 200 shares while the scan
///   asks for 500, and a two-leg arbitrage cannot take 200 of one leg and 500
///   of the other. The honest outcome is a refusal that reports the size that
///   WAS there, not a quietly smaller trade.
///
///   ALLOWANCE. Crossing the touch, and waiting for the second leg to arrive,
///   both cost money the charge schedule does not contain. `allow` carries
///   those as bps (produced by risk/slippage.hpp) and they are subtracted from
///   the net before `actionable()` can ever be true.
///
/// Refusal precedence, so the reason reported is the one to act on:
///   no skew limit -> bad allowance -> bad quantity -> bad price -> stale leg
///   -> insufficient size -> settlement unavailable -> inventory unavailable
///   -> short cash unavailable -> executable.
/// Size is decided BEFORE reachability because "you asked for more than is
/// there" is a fact about this scan, while short-cash is a fact about the
/// account; the size problem is the one the caller can fix now.
[[nodiscard]] inline std::expected<CrossVenueOpportunity, BasisError>
scan_cross_venue(const CrossVenueQuote& q, Qty qty, Duration max_skew,
                 const ShortCashCapability& cap, const ExecutionAllowance& allow,
                 const CrossVenueExecutionConstraints& constraints,
                 const ChargeSchedule& sch, const BrokerageRule& br) noexcept {
    if (max_skew.raw() <= 0) {
        return std::unexpected(BasisError::NoSkewLimit);
    }
    const double total_bps = allow.total_bps();
    if (!(total_bps >= 0.0) || !std::isfinite(total_bps)) {
        return std::unexpected(BasisError::BadAllowance);
    }
    if (qty.raw() <= 0) {
        return std::unexpected(BasisError::BadQuantity);
    }
    if (q.buy_ask.raw() <= 0 || q.sell_bid.raw() <= 0) {
        return std::unexpected(BasisError::BadPrice);
    }
    const std::int64_t raw = (q.sell_ts - q.buy_ts).raw();
    const Duration skew{raw < 0 ? -raw : raw};
    if (skew.raw() > max_skew.raw()) {
        return std::unexpected(BasisError::StaleLeg);
    }

    CrossVenueOpportunity o{};
    o.skew = skew;
    // TOUCH TO TOUCH. Buy at the ask, sell at the bid -- the prices we would
    // actually get, not the mids we would like to.
    o.cross_spread = q.sell_bid.raw() - q.buy_ask.raw();

    // SIZE. The smaller of the two resting sizes is what the pair can carry.
    // Zero means the caller did not supply a size, which cannot be checked and
    // therefore refuses rather than assuming depth that was never quoted.
    const std::int64_t resting =
        q.buy_ask_qty.raw() < q.sell_bid_qty.raw() ? q.buy_ask_qty.raw()
                                                   : q.sell_bid_qty.raw();
    o.executable_size = Qty{resting > 0 ? resting : 0};
    const bool size_ok = resting >= qty.raw();

    if (constraints.settlement != CrossVenueSettlement::Eligible) {
        o.executability = Executability::SettlementUnavailable;
    } else if (constraints.sell_inventory.raw() < 0
               || constraints.permitted_short.raw() < 0) {
        o.executability = Executability::InventoryUnavailable;
    } else {
        const std::int64_t inventory = constraints.sell_inventory.raw();
        const std::int64_t residual = inventory >= qty.raw()
            ? 0 : qty.raw() - inventory;
        if (residual > 0 && constraints.permitted_short.raw() < residual) {
            o.executability = Executability::InventoryUnavailable;
        } else if (residual > 0 && !cap.intraday_short) {
            o.executability = Executability::ShortCashUnavailable;
        } else {
            o.executability = Executability::Executable;
        }
    }
    if (!size_ok) {
        o.executability = Executability::InsufficientSize;
    }
    if (!size_ok) {
        o.size_short = Qty{resting > 0 ? qty.raw() - resting : qty.raw()};
    }

    const auto lb = compute_cost(
        detail::leg(Segment::Cash, q.buy_venue, Side::Buy, qty, q.buy_ask,
                    false, q.buy_ts), sch, br);
    const auto ls = compute_cost(
        detail::leg(Segment::Cash, q.sell_venue, Side::Sell, qty, q.sell_bid,
                    false, q.sell_ts), sch, br);
    if (!lb || !ls) { return std::unexpected(BasisError::CostUnavailable); }
    o.buy_leg = *lb;
    o.sell_leg = *ls;
    o.cost = Notional{lb->total.raw() + ls->total.raw()};

    // The allowance scales with the turnover both legs actually do.
    const double turnover =
        (static_cast<double>(q.buy_ask.raw())
         + static_cast<double>(q.sell_bid.raw()))
        * static_cast<double>(qty.raw());
    const double allow_paise = turnover * total_bps / 10'000.0;
    o.allowance = Notional{static_cast<std::int64_t>(allow_paise + 0.5)};

    o.gross_value = Notional{o.cross_spread * qty.raw()};
    o.net = Notional{o.gross_value.raw() - o.cost.raw() - o.allowance.raw()};
    return o;
}

/// Compatibility form for callers that predate explicit account constraints.
/// It retains the former intraday-only contract by treating the requested size
/// as permitted short capacity and assuming the caller's existing same-session
/// settlement policy. New callers should pass CrossVenueExecutionConstraints.
[[nodiscard]] inline std::expected<CrossVenueOpportunity, BasisError>
scan_cross_venue(const CrossVenueQuote& q, Qty qty, Duration max_skew,
                 const ShortCashCapability& cap, const ExecutionAllowance& allow,
                 const ChargeSchedule& sch, const BrokerageRule& br) noexcept {
    CrossVenueExecutionConstraints constraints{};
    constraints.permitted_short = qty;
    constraints.settlement = CrossVenueSettlement::Eligible;
    return scan_cross_venue(q, qty, max_skew, cap, allow, constraints, sch, br);
}

} // namespace altair
