// strategies/parity.hpp -- put-call parity, box spreads, and the butterfly
// convexity test.
//
// P5-06. Detection only; oms/ decides what to do (see strategies/CMakeLists).
//
// NSE INDEX AND STOCK OPTIONS ARE EUROPEAN, so parity is an identity and not
// an approximation. C - P = df*(F - K) holds exactly, and any deviation is
// either a cost, a stale quote, or an opportunity. That is what makes these
// three the cleanest arbitrage tests in the book -- and the easiest to fool
// yourself with.
//
// THE STRIKE LADDER IS NOT UNIFORM, AND THE TEXTBOOK BUTTERFLY ASSUMES IT IS.
//
// This is the card. The convexity condition every reference gives is
//
//     C(K1) - 2*C(K2) + C(K3) >= 0
//
// and it is only valid when K2 - K1 == K3 - K2. NIFTY's ladder is 50 points
// near the money and 100 further out, so three CONSECUTIVE LISTED strikes
// routinely straddle the step change and are not equally spaced. Feed such a
// triple to the unweighted form and it reports a negative butterfly -- an
// arbitrage -- on a surface that is perfectly convex.
//
// It is not a small effect. On a locally linear stretch of the call curve the
// unweighted form returns -(K3 - 2*K2 + K1) * dC/dK, which for a 100/200
// ladder is a full 100 points of strike times the slope. The test measures it
// on a clean Black-76 chain and gets a confident, sizeable, entirely fictional
// opportunity.
//
// The correct condition weights by the spacing:
//
//     (K3-K2)*C(K1) - (K3-K1)*C(K2) + (K2-K1)*C(K3) >= 0
//
// which reduces to the textbook form when the spacing is equal. That is what
// `butterfly_margin` computes, and the strikes come from the spec store rather
// than from a step anyone assumed (rule 1).
//
// PARITY IS CHECKED AT THE TOUCH, NEVER AT THE MID.
//
// The second way to manufacture option arbitrage. A mid-to-mid parity
// deviation of two rupees on a pair of options each quoted two rupees wide is
// not an opportunity; it is the spread. Every quote here is a bid/ask pair and
// there is no mid anywhere in the file, because a mid cannot be traded.
//
// PARITY AGAINST A FUTURE IS SYMMETRIC. Against SPOT IT IS NOT.
//
// P5-05's asymmetry -- no short cash delivery in India -- applies to the
// hedge leg here too. A reversal hedged with a SHORT FUTURE is fine, because
// futures short freely. A reversal hedged by shorting the underlying stock is
// the same unavailable trade as before. The hedge instrument is therefore an
// input, it has no default, and a caller who does not name one is REFUSED
// rather than costed against a guess.
//
// A BOX IS A LOAN, AND ITS RATE IS THE COMPARABLE NUMBER.
//
// Measured on a 24000/24200 NIFTY box, 30 days, every leg quoted Rs 1.00
// wide: at the mid it lends at 6.53%, which is the rate the chain was priced
// with, recovered from four quotes. Crossing the four spreads costs Rs 4.00
// and turns it into MINUS 17.69% -- a swing of 24 percentage points of
// annualised rate out of four rupees. The same Rs 4.00 at six days instead of
// thirty gives minus 88.5%, five times the damage for identical rupees.
//
// Which is why the scanner ranks on implied RATE and not on the rupee gap:
// the two order a chain differently, and only one of them is comparable
// across tenors.

#pragma once

#include <core/types/units.hpp>
#include <risk/cost.hpp>
#include <strategies/basis.hpp>

#include <cmath>
#include <cstdint>
#include <expected>

namespace altair {

/// A two-sided quote. There is no mid accessor, deliberately.
struct Touch {
    Price bid{0};
    Price ask{0};
    [[nodiscard]] bool live() const noexcept {
        return bid.raw() > 0 && ask.raw() >= bid.raw();
    }
};

enum class ParityError : std::uint8_t {
    /// A leg had no two-sided quote.
    NoTouch,
    /// Strikes were not strictly increasing.
    BadStrikes,
    /// Time to expiry was zero or negative.
    BadTenor,
    /// The cost calculator refused.
    CostUnavailable,
    /// Three strikes were passed where the ladder step is unknown.
    UnknownStrikeStep,
    /// The caller did not say what the hedge instrument is. Refused rather
    /// than costed against a guess: the hedge decides both the segment the
    /// bill is computed under and whether a reversal is reachable at all, so
    /// there is no safe default to fall back on.
    HedgeUnknown
};

/// Which side of parity is rich, and therefore which trade it implies.
enum class ParityTrade : std::uint8_t {
    Unknown = 0,
    /// The synthetic is rich: sell call, buy put, buy the hedge.
    Conversion,
    /// The synthetic is cheap: buy call, sell put, sell the hedge.
    Reversal
};

/// What the hedge leg is. Decides whether a reversal can be reached at all.
enum class HedgeLeg : std::uint8_t {
    Unknown = 0,
    /// A listed future. Shorts freely, so both directions are reachable.
    Future,
    /// The underlying stock. A reversal needs a short cash position and
    /// inherits P5-05's asymmetry exactly.
    Spot
};

// ---------------------------------------------------------------------------
// Put-call parity
// ---------------------------------------------------------------------------

struct ParityQuote {
    Touch call{};
    Touch put{};
    /// The hedge instrument's touch: a future, or the underlying.
    Touch hedge{};
    HedgeLeg hedge_leg = HedgeLeg::Unknown;
    /// From the spec store, never a literal (rule 1).
    Price strike{0};
    Years t{0.0};
    double rate = 0.0;
};

struct ParityOpportunity {
    ParityTrade trade = ParityTrade::Unknown;
    Executability executability = Executability::Unknown;
    /// The deviation from parity at the TOUCH, per unit, in paise. Already
    /// net of crossing every spread; a positive number is a real gross edge
    /// and not a mid-to-mid artefact.
    double touch_deviation = 0.0;
    /// The same deviation computed MID to MID, carried only so a caller can
    /// see the difference. Never the basis for a decision.
    double mid_deviation = 0.0;
    Notional cost{};
    Notional gross_value{};
    Notional net{};

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

namespace detail {

[[nodiscard]] inline double mid_of(const Touch& t) noexcept {
    return 0.5 * static_cast<double>(t.bid.raw() + t.ask.raw());
}

/// Cost of one option leg, priced on PREMIUM (P3-09 D1).
[[nodiscard]] inline std::expected<CostBreakdown, ParityError>
opt_leg(Side side, Qty qty, Price premium, Exchange ex, Timestamp ts,
        const ChargeSchedule& sch, const BrokerageRule& br) noexcept {
    Trade t{};
    t.segment = Segment::Opt;
    t.exchange = ex;
    t.side = side;
    t.qty = qty;
    t.price = premium;          // the PREMIUM, so turnover is premium turnover
    t.trade_ts = ts;
    const auto c = compute_cost(t, sch, br);
    if (!c) { return std::unexpected(ParityError::CostUnavailable); }
    return *c;
}

} // namespace detail

/// Scan one strike for a put-call parity violation, at the touch.
[[nodiscard]] inline std::expected<ParityOpportunity, ParityError>
scan_parity(const ParityQuote& q, Qty qty, Exchange ex, Timestamp ts,
            const ShortCashCapability& cap, const ChargeSchedule& sch,
            const BrokerageRule& br) noexcept {
    if (!q.call.live() || !q.put.live() || !q.hedge.live()) {
        return std::unexpected(ParityError::NoTouch);
    }
    if (!(q.t.raw() > 0.0)) { return std::unexpected(ParityError::BadTenor); }
    if (q.strike.raw() <= 0) {
        return std::unexpected(ParityError::BadStrikes);
    }
    // An unspecified hedge is refused HERE, before anything is computed. It
    // would otherwise fall through to the cash segment by construction --
    // `Unknown` is not `Future`, so the ternary below picks Cash -- and the
    // caller would get a fully populated opportunity whose bill was priced
    // against an instrument nobody named.
    if (q.hedge_leg == HedgeLeg::Unknown) {
        return std::unexpected(ParityError::HedgeUnknown);
    }

    const double df = std::exp(-q.rate * q.t.raw());
    const double K = static_cast<double>(q.strike.raw());

    // Sell the synthetic (call at BID, put at ASK) against buying the hedge
    // at its ASK; or buy the synthetic against selling the hedge at its BID.
    // Every leg crosses the spread it would actually have to cross.
    const double sell_syn = static_cast<double>(q.call.bid.raw())
                          - static_cast<double>(q.put.ask.raw());
    const double buy_syn = static_cast<double>(q.call.ask.raw())
                         - static_cast<double>(q.put.bid.raw());
    const double hedge_buy =
        df * (static_cast<double>(q.hedge.ask.raw()) - K);
    const double hedge_sell =
        df * (static_cast<double>(q.hedge.bid.raw()) - K);

    ParityOpportunity o{};
    const double conversion = sell_syn - hedge_buy;   // synthetic rich
    const double reversal = hedge_sell - buy_syn;     // synthetic cheap
    if (conversion >= reversal) {
        o.trade = ParityTrade::Conversion;
        o.touch_deviation = conversion;
    } else {
        o.trade = ParityTrade::Reversal;
        o.touch_deviation = reversal;
    }

    // Carried for comparison only. A mid deviation is what a screen shows and
    // it is systematically larger than anything tradable.
    o.mid_deviation = (detail::mid_of(q.call) - detail::mid_of(q.put))
                    - df * (detail::mid_of(q.hedge) - K);

    // A conversion SELLS the synthetic and BUYS the hedge -- no short cash
    // anywhere, so it is always reachable. A reversal sells the hedge, and
    // whether that is possible depends on what the hedge is.
    if (o.trade == ParityTrade::Conversion
               || q.hedge_leg == HedgeLeg::Future
               || cap.borrow_available) {
        o.executability = Executability::Executable;
    } else {
        o.executability = Executability::ShortCashUnavailable;
    }

    // Four legs: two options and the hedge, in and out. The options are
    // charged on PREMIUM; the hedge on notional.
    const bool sell_call = o.trade == ParityTrade::Conversion;
    const auto c_in = detail::opt_leg(
        sell_call ? Side::Sell : Side::Buy, qty,
        sell_call ? q.call.bid : q.call.ask, ex, ts, sch, br);
    const auto p_in = detail::opt_leg(
        sell_call ? Side::Buy : Side::Sell, qty,
        sell_call ? q.put.ask : q.put.bid, ex, ts, sch, br);
    if (!c_in || !p_in) {
        return std::unexpected(ParityError::CostUnavailable);
    }

    Trade h{};
    h.segment = q.hedge_leg == HedgeLeg::Future ? Segment::Fut : Segment::Cash;
    h.exchange = ex;
    h.side = sell_call ? Side::Buy : Side::Sell;
    h.qty = qty;
    h.price = sell_call ? q.hedge.ask : q.hedge.bid;
    h.delivery = q.hedge_leg == HedgeLeg::Spot;
    h.trade_ts = ts;
    const auto hc = compute_cost(h, sch, br);
    if (!hc) { return std::unexpected(ParityError::CostUnavailable); }

    // Held to expiry: the options settle and the hedge closes. One more
    // hedge leg the other way; the options do not pay STT on exercise for
    // the buyer, so the entry legs are the bill.
    Trade h_out = h;
    h_out.side = h.side == Side::Buy ? Side::Sell : Side::Buy;
    const auto hc_out = compute_cost(h_out, sch, br);
    if (!hc_out) { return std::unexpected(ParityError::CostUnavailable); }

    o.cost = Notional{c_in->total.raw() + p_in->total.raw()
                      + hc->total.raw() + hc_out->total.raw()};
    o.gross_value = Notional{static_cast<std::int64_t>(
        o.touch_deviation * static_cast<double>(qty.raw()) + 0.5)};
    o.net = Notional{o.gross_value.raw() - o.cost.raw()};
    return o;
}

// ---------------------------------------------------------------------------
// Butterfly convexity
// ---------------------------------------------------------------------------

/// Three strikes and their call quotes, in increasing strike order.
struct ButterflyQuote {
    Price k1{0}, k2{0}, k3{0};
    Touch c1{}, c2{}, c3{};
};

struct ButterflyResult {
    /// The SPACING-WEIGHTED convexity margin, per unit, in paise. Negative
    /// means the surface is not convex in strike, which is a genuine
    /// arbitrage. Computed at the touch: buy the wings at the ask, sell the
    /// body at the bid.
    double margin = 0.0;
    /// What the UNWEIGHTED textbook form would have said. Carried so the
    /// difference is visible rather than argued about; never the basis for a
    /// decision on an unevenly spaced ladder.
    double unweighted = 0.0;
    /// True when the three strikes are equally spaced, in which case the two
    /// numbers agree up to the weighting constant.
    bool equally_spaced = false;
    /// The two gaps, in paise, so a reader can see the ladder.
    std::int64_t lower_gap = 0;
    std::int64_t upper_gap = 0;
};

/// The convexity test, weighted by the actual strike spacing.
///
/// For K1 < K2 < K3 the arbitrage-free condition on call prices is
///
///     (K3-K2)*C(K1) - (K3-K1)*C(K2) + (K2-K1)*C(K3) >= 0
///
/// which is convexity of C in K, stated for an arbitrary ladder. The strikes
/// come from the spec store; nothing here assumes a step.
[[nodiscard]] inline std::expected<ButterflyResult, ParityError>
butterfly_margin(const ButterflyQuote& q) noexcept {
    if (!(q.k1.raw() < q.k2.raw() && q.k2.raw() < q.k3.raw())) {
        return std::unexpected(ParityError::BadStrikes);
    }
    if (!q.c1.live() || !q.c2.live() || !q.c3.live()) {
        return std::unexpected(ParityError::NoTouch);
    }

    ButterflyResult r{};
    r.lower_gap = q.k2.raw() - q.k1.raw();
    r.upper_gap = q.k3.raw() - q.k2.raw();
    r.equally_spaced = r.lower_gap == r.upper_gap;

    // TOUCH prices, and the right side of each spread: a long butterfly buys
    // both wings and sells the body, so the wings cross at the ask and the
    // body at the bid.
    const double w1 = static_cast<double>(r.upper_gap);
    const double w2 = static_cast<double>(r.upper_gap + r.lower_gap);
    const double w3 = static_cast<double>(r.lower_gap);
    const double a1 = static_cast<double>(q.c1.ask.raw());
    const double b2 = static_cast<double>(q.c2.bid.raw());
    const double a3 = static_cast<double>(q.c3.ask.raw());

    // Normalised by the total width so the number is per unit of strike and
    // comparable across ladders of different steps.
    r.margin = (w1 * a1 - w2 * b2 + w3 * a3) / w2;
    r.unweighted = a1 - 2.0 * b2 + a3;
    return r;
}

// ---------------------------------------------------------------------------
// Box spread
// ---------------------------------------------------------------------------

/// A box: long the K1 call spread, short the K1 put spread. Its payoff at
/// expiry is exactly K2 - K1 regardless of where the underlying lands, which
/// makes it a synthetic zero-coupon loan and its implied rate the cleanest
/// financing number the option chain produces.
struct BoxQuote {
    Price k1{0}, k2{0};
    Touch c1{}, c2{};       // calls at K1 and K2
    Touch p1{}, p2{};       // puts  at K1 and K2
    Years t{0.0};
    double rate = 0.0;
};

struct BoxOpportunity {
    Executability executability = Executability::Unknown;
    /// What establishing the box costs at the touch, per unit, in paise.
    double touch_cost = 0.0;
    /// Its certain payoff, discounted: (K2 - K1) * df.
    double discounted_payoff = 0.0;
    /// The financing rate the box is quoting, per year. This is what makes a
    /// box comparable to a repo rate rather than to a rupee amount.
    double implied_rate = 0.0;
    Notional cost{};
    Notional gross_value{};
    Notional net{};

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

/// Scan a box. All four legs are options, so nothing here needs a short cash
/// position and both directions are always reachable -- which makes it the
/// one scanner in Phase 5 with no executability caveat.
[[nodiscard]] inline std::expected<BoxOpportunity, ParityError>
scan_box(const BoxQuote& q, Qty qty, Exchange ex, Timestamp ts,
         const ChargeSchedule& sch, const BrokerageRule& br) noexcept {
    if (!(q.k1.raw() < q.k2.raw())) {
        return std::unexpected(ParityError::BadStrikes);
    }
    if (!q.c1.live() || !q.c2.live() || !q.p1.live() || !q.p2.live()) {
        return std::unexpected(ParityError::NoTouch);
    }
    if (!(q.t.raw() > 0.0)) { return std::unexpected(ParityError::BadTenor); }

    BoxOpportunity o{};
    o.executability = Executability::Executable;

    // Buy the box: buy C(K1), sell C(K2), sell P(K1), buy P(K2). Every leg at
    // the price it would actually pay.
    o.touch_cost = static_cast<double>(q.c1.ask.raw())
                 - static_cast<double>(q.c2.bid.raw())
                 - static_cast<double>(q.p1.bid.raw())
                 + static_cast<double>(q.p2.ask.raw());
    const double payoff = static_cast<double>(q.k2.raw() - q.k1.raw());
    const double df = std::exp(-q.rate * q.t.raw());
    o.discounted_payoff = payoff * df;
    // ln(payoff / cost) / T. Undefined for a non-positive cost, which is
    // itself the arbitrage: a box that pays you to hold it.
    o.implied_rate = o.touch_cost > 0.0
                   ? std::log(payoff / o.touch_cost) / q.t.raw()
                   : std::numeric_limits<double>::infinity();

    const auto l1 = detail::opt_leg(Side::Buy, qty, q.c1.ask, ex, ts, sch, br);
    const auto l2 = detail::opt_leg(Side::Sell, qty, q.c2.bid, ex, ts, sch, br);
    const auto l3 = detail::opt_leg(Side::Sell, qty, q.p1.bid, ex, ts, sch, br);
    const auto l4 = detail::opt_leg(Side::Buy, qty, q.p2.ask, ex, ts, sch, br);
    if (!l1 || !l2 || !l3 || !l4) {
        return std::unexpected(ParityError::CostUnavailable);
    }
    o.cost = Notional{l1->total.raw() + l2->total.raw() + l3->total.raw()
                      + l4->total.raw()};
    o.gross_value = Notional{static_cast<std::int64_t>(
        (o.discounted_payoff - o.touch_cost) * static_cast<double>(qty.raw())
        + 0.5)};
    o.net = Notional{o.gross_value.raw() - o.cost.raw()};
    return o;
}

} // namespace altair
