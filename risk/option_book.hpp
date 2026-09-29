// risk/option_book.hpp -- the numbers behind the GETS-style Greek screens.
//
// Greeksoft GETS gives an options desk four views that Altair did not have:
// a Greek market watch (IV and Greeks per contract), a portfolio Greek summary
// grouped by underlying and expiry, a what-if simulation over spot, vol and
// days, and an expense report per instrument. prompts/P4-04 (now in git
// history) mapped their columns onto Altair's types. This header is the
// engine half; desktop/gets_* only formats what it returns.
//
// MODEL. Black-Scholes on SPOT (analytics/greeks.hpp), IV solved from the
// last traded price (analytics/iv.hpp), the same basis the option chain page
// uses. A user IV, when set, replaces the solved IV for Greeks and theoretical
// price only; the solved IV is still reported beside it.
//
// UNITS. Per-unit Greeks are returned as analytics/greeks.hpp defines them
// (paise, per year, per 1.0 of vol). Position Greeks are converted ONCE, here,
// to desk units and named for them: delta in underlying units, gamma in delta
// per rupee, vega in rupees per vol point, theta in rupees per calendar day.
//
// MONEY. Mark-to-market is exact integer paise ((mark - average) x units).
// Scenario P&L is a model value: computed in double and rounded half away
// from zero to whole paise at this boundary, never passed on as a double.
//
// REFUSALS. A leg without a mark has no MTM; a leg whose IV cannot be solved
// and has no user IV has no Greeks. Either makes the group it belongs to
// INCOMPLETE and the caller is told so, never handed a partial sum as a total.
#pragma once

#include <analytics/greeks.hpp>
#include <analytics/iv.hpp>
#include <core/time/timestamp.hpp>
#include <core/types/units.hpp>
#include <risk/cost.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace altair {

enum class BookLegKind : std::uint8_t { Cash, Future, Option };

/// One contract as the book sees it. `units` is signed and in units, not
/// lots. Watch-only rows carry units == 0 and still get per-unit Greeks.
struct BookLeg {
    std::uint32_t id{};              ///< caller's row key
    std::uint32_t underlying{};      ///< caller's underlying key; groups legs
    BookLegKind kind{BookLegKind::Cash};
    OptionRight right{OptionRight::Call};
    Price strike{};
    Timestamp expiry{};              ///< exact expiry instant; epoch() when unknown
    bool expiry_known{};
    Qty units{};
    Price average{};                 ///< net average price of the open units
    std::optional<Price> mark{};     ///< last traded price
    std::optional<Vol> user_vol{};   ///< user IV override (fraction, 0.15 = 15%)
    std::optional<LotSize> lot{};
};

struct BookParams {
    Timestamp now{};
    double rate{0.065};              ///< continuously compounded, fraction
    double dividend_yield{0.0};
};

enum class BookLegIssue : std::uint8_t {
    None,
    NoSpot,          ///< the underlying has no spot quote
    NoExpiry,        ///< monthly contract and no master date
    Expired,         ///< now is at or past expiry
    NoMark,          ///< no last price: no IV and no MTM
    IvUnsolved,      ///< the mark admits no volatility (below intrinsic, ...)
    GreeksFailed
};

/// Everything the market watch shows for one leg.
struct BookLegView {
    std::uint32_t id{};
    double years{};                          ///< to expiry; 0 for cash/futures
    double days{};                           ///< calendar days to expiry
    std::optional<Vol> implied{};            ///< solved from the mark
    std::optional<Vol> used{};               ///< user IV if set, else implied
    std::optional<Greeks> unit{};            ///< per unit, at `used`
    std::optional<Notional> mtm{};           ///< exact
    std::optional<Price> intrinsic{};        ///< at spot
    std::optional<Price> time_value{};       ///< mark - intrinsic
    double delta{};                          ///< position, underlying units
    double gamma{};                          ///< position, delta per rupee
    double vega{};                           ///< position, rupees per vol point
    double theta{};                          ///< position, rupees per day
    bool greeks_ok{};
    BookLegIssue issue{BookLegIssue::None};
};

namespace option_book_detail {

inline constexpr double kNsPerYear = 365.0 * 86'400.0 * 1e9;

/// a + b in paise, or false on int64 overflow.
[[nodiscard]] constexpr bool add_paise(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept {
    constexpr std::int64_t hi = std::numeric_limits<std::int64_t>::max();
    constexpr std::int64_t lo = std::numeric_limits<std::int64_t>::min();
    if ((b > 0 && a > hi - b) || (b < 0 && a < lo - b)) return false;
    out = a + b;
    return true;
}

[[nodiscard]] inline std::optional<Notional> round_paise(double v) noexcept {
    if (!(v == v) || v > 9.0e18 || v < -9.0e18) return std::nullopt;
    return Notional{static_cast<std::int64_t>(v < 0.0 ? v - 0.5 : v + 0.5)};
}

/// Desk-unit position Greeks from per-unit Greeks in paise units.
inline void position_greeks(const Greeks& g, std::int64_t units, BookLegView& v) noexcept {
    const double u = static_cast<double>(units);
    v.delta = g.delta * u;
    v.gamma = g.gamma * 100.0 * u;                        // 1/paise -> 1/rupee
    v.vega = vega_per_vol_point(g.vega) / 100.0 * u;      // paise -> rupees
    v.theta = theta_per_day(g.theta) / 100.0 * u;
}

} // namespace option_book_detail

/// Value one leg against a spot. `spot` is absent when the underlying has no
/// quote; cash and futures legs still get delta = units and an MTM.
[[nodiscard]] inline BookLegView value_book_leg(const BookLeg& leg,
                                                std::optional<Price> spot,
                                                const BookParams& p) noexcept {
    namespace d = option_book_detail;
    BookLegView v{};
    v.id = leg.id;
    if (leg.mark) {
        const auto diff = notional_of(Price{leg.mark->raw() - leg.average.raw()}, leg.units);
        if (diff) v.mtm = *diff;
    }
    if (leg.kind != BookLegKind::Option) {
        v.delta = static_cast<double>(leg.units.raw());
        v.greeks_ok = true;
        if (!leg.mark) v.issue = BookLegIssue::NoMark;
        return v;
    }
    if (!leg.expiry_known) { v.issue = BookLegIssue::NoExpiry; return v; }
    const double ns = static_cast<double>(leg.expiry.ns_since_epoch() - p.now.ns_since_epoch());
    if (!(ns > 0.0)) { v.issue = BookLegIssue::Expired; return v; }
    v.years = ns / d::kNsPerYear;
    v.days = ns / (86'400.0 * 1e9);
    if (!spot || spot->raw() <= 0) { v.issue = BookLegIssue::NoSpot; return v; }

    v.intrinsic = intrinsic(leg.right, *spot, leg.strike);
    if (leg.mark) {
        v.time_value = Price{leg.mark->raw() - v.intrinsic->raw()};
        const auto iv = implied_vol_black_scholes(leg.right, *leg.mark, *spot, leg.strike,
                                                  Years{v.years}, p.rate, p.dividend_yield);
        if (iv) v.implied = iv->vol;
    }
    v.used = leg.user_vol ? leg.user_vol : v.implied;
    if (!v.used) {
        v.issue = leg.mark ? BookLegIssue::IvUnsolved : BookLegIssue::NoMark;
        return v;
    }
    const auto g = black_scholes(leg.right, *spot, leg.strike, Years{v.years}, *v.used,
                                 p.rate, p.dividend_yield);
    if (!g) { v.issue = BookLegIssue::GreeksFailed; return v; }
    v.unit = *g;
    d::position_greeks(*g, leg.units.raw(), v);
    v.greeks_ok = true;
    if (!leg.mark) v.issue = BookLegIssue::NoMark;
    return v;
}

/// One row of the portfolio Greek summary: an underlying and one expiry, or
/// the underlying's total when `total` is set.
struct BookSummaryRow {
    std::uint32_t underlying{};
    bool total{};
    Timestamp expiry{};
    bool expiry_known{};
    std::optional<Price> spot{};
    std::size_t legs{};
    std::optional<Notional> mtm{};          ///< absent if any leg lacks a mark
    std::optional<Vol> call_iv{};           ///< |units|-weighted mean of used IV
    std::optional<Vol> put_iv{};
    double days{};                          ///< nearest expiry in the group
    double delta{};
    double gamma{};
    double vega{};
    double theta{};
    std::int64_t cash_units{};              ///< equity units in the group (EQPosn)
    std::optional<double> hedge_lots{};     ///< -delta / lot; absent without a lot
    bool complete{true};                    ///< every leg valued and marked
};

/// Group legs by (underlying, expiry), then add one total row per underlying.
/// `spot_of(underlying)` returns the spot used for the legs. Rows come back
/// in first-seen order, each underlying's expiries followed by its total.
template <typename SpotFn>
[[nodiscard]] std::vector<BookSummaryRow>
summarise_book(std::span<const BookLeg> legs, std::span<const BookLegView> views,
               SpotFn&& spot_of) {
    std::vector<BookSummaryRow> rows;
    if (legs.size() != views.size()) return rows;
    std::vector<std::uint32_t> order;
    for (const auto& leg : legs) {
        bool seen = false;
        for (const auto u : order) seen = seen || u == leg.underlying;
        if (!seen) order.push_back(leg.underlying);
    }
    struct Weight { double call_sum{}, call_w{}, put_sum{}, put_w{}; };
    const auto fold = [](BookSummaryRow& r, Weight& w, const BookLeg& leg,
                         const BookLegView& v) {
        ++r.legs;
        std::int64_t sum = 0;
        if (v.mtm && r.mtm && option_book_detail::add_paise(r.mtm->raw(), v.mtm->raw(), sum))
            r.mtm = Notional{sum};
        else
            r.mtm.reset();
        if (!v.mtm || !v.greeks_ok) r.complete = false;
        r.delta += v.delta;
        r.gamma += v.gamma;
        r.vega += v.vega;
        r.theta += v.theta;
        if (leg.kind == BookLegKind::Cash) r.cash_units += leg.units.raw();
        if (leg.kind == BookLegKind::Option) {
            if (v.days > 0.0 && (r.days == 0.0 || v.days < r.days)) r.days = v.days;
            if (v.used && leg.units.raw() != 0) {
                const double wt = std::fabs(static_cast<double>(leg.units.raw()));
                if (leg.right == OptionRight::Call) {
                    w.call_sum += v.used->raw() * wt; w.call_w += wt;
                } else {
                    w.put_sum += v.used->raw() * wt; w.put_w += wt;
                }
            }
        }
        if (!r.hedge_lots && leg.lot && leg.lot->raw() > 0 && leg.kind != BookLegKind::Cash)
            r.hedge_lots = static_cast<double>(leg.lot->raw());   // lot, finished below
    };
    const auto finish = [](BookSummaryRow& r, const Weight& w) {
        if (w.call_w > 0.0) r.call_iv = Vol{w.call_sum / w.call_w};
        if (w.put_w > 0.0) r.put_iv = Vol{w.put_sum / w.put_w};
        if (r.hedge_lots) r.hedge_lots = -r.delta / *r.hedge_lots;
    };
    for (const auto u : order) {
        const std::optional<Price> spot = spot_of(u);
        const std::size_t first = rows.size();
        for (std::size_t i = 0; i < legs.size(); ++i) {
            if (legs[i].underlying != u) continue;
            const bool dated = legs[i].kind != BookLegKind::Cash && legs[i].expiry_known;
            const Timestamp e = dated ? legs[i].expiry : Timestamp::epoch();
            bool placed = false;
            for (std::size_t r = first; r < rows.size(); ++r)
                placed = placed || (rows[r].expiry == e && rows[r].expiry_known == dated);
            if (placed) continue;
            BookSummaryRow row{};
            row.underlying = u;
            row.expiry = e;
            row.expiry_known = dated;
            row.spot = spot;
            row.mtm = Notional{0};
            Weight w{};
            for (std::size_t j = i; j < legs.size(); ++j) {
                if (legs[j].underlying != u) continue;
                const bool dj = legs[j].kind != BookLegKind::Cash && legs[j].expiry_known;
                if (dj != dated || (dated && legs[j].expiry != e)) continue;
                fold(row, w, legs[j], views[j]);
            }
            finish(row, w);
            rows.push_back(row);
        }
        BookSummaryRow total{};
        total.underlying = u;
        total.total = true;
        total.spot = spot;
        total.mtm = Notional{0};
        Weight w{};
        for (std::size_t j = 0; j < legs.size(); ++j)
            if (legs[j].underlying == u) fold(total, w, legs[j], views[j]);
        finish(total, w);
        rows.push_back(total);
    }
    return rows;
}

/// One what-if move: spot by a percentage, IV by vol points, time by days.
struct BookShift {
    double spot_pct{};
    double vol_points{};
    double days{};
};

struct BookScenarioRow {
    BookShift shift{};
    Price spot{};
    std::optional<Notional> pnl{};           ///< model value now vs current marks
    std::optional<Notional> pnl_at_expiry{}; ///< payoff at expiry vs current marks
    double delta{};
    double gamma{};
    double vega{};
    double theta{};
    std::optional<double> hedge_lots{};
    bool complete{true};
};

/// Revalue every leg of ONE underlying under each shift. Legs of other
/// underlyings are ignored. A leg that could not be valued now (no mark, no
/// vol) marks every row incomplete and is left out of its sums.
[[nodiscard]] inline std::vector<BookScenarioRow>
simulate_book(std::span<const BookLeg> legs, std::span<const BookLegView> views,
              std::uint32_t underlying, Price spot, const BookParams& p,
              std::span<const BookShift> shifts) {
    namespace d = option_book_detail;
    std::vector<BookScenarioRow> out;
    if (legs.size() != views.size() || spot.raw() <= 0) return out;
    std::optional<double> lot;
    for (const auto& leg : legs)
        if (leg.underlying == underlying && !lot && leg.lot && leg.lot->raw() > 0
            && leg.kind != BookLegKind::Cash)
            lot = static_cast<double>(leg.lot->raw());
    const double s0 = static_cast<double>(spot.raw());
    for (const auto& sh : shifts) {
        BookScenarioRow row{};
        row.shift = sh;
        const double s1 = s0 * (1.0 + sh.spot_pct / 100.0);
        row.spot = Price{static_cast<std::int64_t>(s1 + 0.5)};
        double pnl = 0.0;
        double pnl_exp = 0.0;
        for (std::size_t i = 0; i < legs.size(); ++i) {
            const auto& leg = legs[i];
            const auto& v = views[i];
            if (leg.underlying != underlying) continue;
            const double units = static_cast<double>(leg.units.raw());
            if (leg.kind != BookLegKind::Option) {
                // Futures and cash move one-for-one with spot (constant basis).
                pnl += units * (s1 - s0);
                pnl_exp += units * (s1 - s0);
                row.delta += units;
                continue;
            }
            if (!leg.mark || !v.used || !v.greeks_ok) { row.complete = false; continue; }
            const double mark = static_cast<double>(leg.mark->raw());
            const double payoff = static_cast<double>(
                intrinsic(leg.right, row.spot, leg.strike).raw());
            pnl_exp += units * (payoff - mark);
            const double t1 = v.years - sh.days / 365.0;
            const double vol1 = v.used->raw() + sh.vol_points / 100.0;
            if (!(t1 > 0.0)) {                 // expired inside the scenario
                pnl += units * (payoff - mark);
                continue;
            }
            if (!(vol1 > 0.0)) { row.complete = false; continue; }
            const auto g = black_scholes(leg.right, row.spot, leg.strike, Years{t1},
                                         Vol{vol1}, p.rate, p.dividend_yield);
            if (!g) { row.complete = false; continue; }
            pnl += units * (g->price - mark);
            BookLegView tmp{};
            d::position_greeks(*g, leg.units.raw(), tmp);
            row.delta += tmp.delta;
            row.gamma += tmp.gamma;
            row.vega += tmp.vega;
            row.theta += tmp.theta;
        }
        row.pnl = d::round_paise(pnl);
        row.pnl_at_expiry = d::round_paise(pnl_exp);
        if (!row.pnl || !row.pnl_at_expiry) row.complete = false;
        if (lot) row.hedge_lots = -row.delta / *lot;
        out.push_back(row);
    }
    return out;
}

/// One executed order (fills already summed by the caller), for the expense
/// report.
struct BookFill {
    std::uint32_t instrument{};
    Side side{Side::Buy};
    Qty qty{};
    Notional value{};             ///< exact traded value, paise
    Segment segment{Segment::Cash};
    Exchange exchange{Exchange::NSE};
    bool delivery{};
    Timestamp at{};
};

struct BookExpenseRow {
    std::uint32_t instrument{};
    std::int64_t buy_qty{};
    Notional buy_value{};
    std::optional<Price> buy_avg{};     ///< rounded half away from zero
    std::int64_t sell_qty{};
    Notional sell_value{};
    std::optional<Price> sell_avg{};
    std::size_t orders{};
    std::optional<Notional> expense{};  ///< absent without a verified schedule
    std::optional<CostError> expense_error{};
    bool overflow{};                    ///< a value sum left int64 paise; row unusable
};

namespace option_book_detail {

[[nodiscard]] inline std::optional<Price> average_of(Notional value, std::int64_t qty) noexcept {
    if (qty <= 0) return std::nullopt;
    const std::int64_t v = value.raw();
    const std::int64_t half = qty / 2;
    return Price{(v >= 0 ? v + half : v - half) / qty};
}

} // namespace option_book_detail

/// Sum fills per instrument and price each order's charges with
/// risk/cost.hpp. `schedule_for(at)` returns the schedule in force on the
/// trade date or nullptr; `brokerage(fill)` returns the broker's rule for it.
/// The order price handed to the cost model is the order's average, rounded
/// to the paisa (turnover within qty/2 paise of the exact value).
template <typename ScheduleFn, typename BrokerageFn>
[[nodiscard]] std::vector<BookExpenseRow>
book_expenses(std::span<const BookFill> fills, ScheduleFn&& schedule_for,
              BrokerageFn&& brokerage) {
    namespace d = option_book_detail;
    std::vector<BookExpenseRow> rows;
    for (const auto& f : fills) {
        BookExpenseRow* row = nullptr;
        for (auto& r : rows) if (r.instrument == f.instrument) row = &r;
        if (row == nullptr) {
            rows.push_back(BookExpenseRow{});
            row = &rows.back();
            row->instrument = f.instrument;
            row->expense = Notional{0};
        }
        ++row->orders;
        std::int64_t sum = 0;
        if (f.side == Side::Buy) {
            row->buy_qty += f.qty.raw();
            if (!d::add_paise(row->buy_value.raw(), f.value.raw(), sum)) {
                row->overflow = true;
                continue;
            }
            row->buy_value = Notional{sum};
        } else {
            row->sell_qty += f.qty.raw();
            if (!d::add_paise(row->sell_value.raw(), f.value.raw(), sum)) {
                row->overflow = true;
                continue;
            }
            row->sell_value = Notional{sum};
        }
        if (!row->expense) continue;
        const ChargeSchedule* sch = schedule_for(f.at);
        const auto avg = d::average_of(f.value, f.qty.raw());
        if (sch == nullptr || !avg) {
            row->expense.reset();
            row->expense_error = sch == nullptr ? CostError::NoSchedule : CostError::BadQuantity;
            continue;
        }
        Trade t{};
        t.segment = f.segment;
        t.exchange = f.exchange;
        t.side = f.side;
        t.qty = f.qty;
        t.price = *avg;
        t.delivery = f.delivery;
        t.trade_ts = f.at;
        const auto cost = compute_cost(t, *sch, brokerage(f));
        if (!cost) {
            row->expense.reset();
            row->expense_error = cost.error();
            continue;
        }
        if (!d::add_paise(row->expense->raw(), cost->total.raw(), sum)) {
            row->expense.reset();
            row->expense_error = CostError::Overflow;
            continue;
        }
        row->expense = Notional{sum};
    }
    for (auto& r : rows) {
        r.buy_avg = d::average_of(r.buy_value, r.buy_qty);
        r.sell_avg = d::average_of(r.sell_value, r.sell_qty);
    }
    return rows;
}

} // namespace altair
