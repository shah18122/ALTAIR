// app/demo_costs.hpp -- one fill's charges, for the research demos.
//
// Every figure comes from config/charges.toml through risk/cost.hpp under the
// schedule live on the fill's date. Brokerage is commercial, not regulatory,
// so it is not in charges.toml; the literals here are the ones
// desktop/cost_panel.hpp states: Rs 20 an option order, and Rs 20 or 0.03 %
// (whichever is lower) a futures or intraday equity order. The exchange
// matters: a BSE fill pays BSE's transaction charge, not NSE's.

#pragma once

#include <risk/cost.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

namespace altair::demo_costs {

struct Costs {
    bool priced = false;
    double brokerage = 0, stt = 0, exchange = 0, sebi = 0, stamp = 0, ipft = 0, gst = 0, total = 0;

    void add(const Costs& o) {
        brokerage += o.brokerage;
        stt += o.stt;
        exchange += o.exchange;
        sebi += o.sebi;
        stamp += o.stamp;
        ipft += o.ipft;
        gst += o.gst;
        total += o.total;
    }
};

/// One fill: `ist_seconds` is the fill time, IST seconds since the epoch.
/// Unpriced (priced = false) when no schedule covers the date or the charge
/// engine refuses. Cash is intraday. `option`: a currency or commodity option
/// (charges.toml has a currency_options line; no commodity-options one, so
/// an MCX option is unpriced rather than charged as a future).
[[nodiscard]] inline Costs fill(Segment seg, Side side, double qty, double price, std::int64_t ist_seconds,
                                const std::vector<ChargeSchedule>& schedules, Exchange venue = Exchange::NSE,
                                bool option = false) {
    Costs c;
    if (seg == Segment::Commodity && option) return c;
    BrokerageRule br{};
    br.flat_per_order = Notional{2'000};
    if (seg == Segment::Fut || seg == Segment::Cash || ((seg == Segment::Currency || seg == Segment::Commodity) && !option)) {
        br.pct = rate_from(0.0003L);
        br.take_lower = true;
    } else {
        br.pct = 0;
        br.take_lower = false;
    }
    Trade tr{};
    tr.segment = seg;
    tr.exchange = venue;
    tr.side = side;
    tr.qty = Qty{static_cast<std::int64_t>(std::llround(qty))};
    tr.price = Price{static_cast<std::int64_t>(std::llround(price * 100.0))};
    tr.trade_ts = Timestamp{ist_seconds * 1'000'000'000LL};
    tr.option = option;
    const auto* s = schedule_for(schedules.data(), schedules.size(), tr.trade_ts);
    if (s == nullptr) { return c; }
    const auto b = compute_cost(tr, *s, br);
    if (!b) { return c; }
    const auto rs = [](Notional n) { return static_cast<double>(n.raw()) / 100.0; };
    c.brokerage = rs(b->brokerage);
    c.stt = rs(b->stt);
    c.exchange = rs(b->exchange_txn);
    c.sebi = rs(b->sebi);
    c.stamp = rs(b->stamp);
    c.ipft = rs(b->ipft);
    c.gst = rs(b->gst);
    c.total = rs(b->total);
    c.priced = true;
    return c;
}

} // namespace altair::demo_costs
