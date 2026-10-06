// Option arbitrage (live/option_arb.hpp) on books whose answers are known:
// the parity and box arithmetic; a lock that does not beat its costs is left
// alone; one that does is entered on every leg; a book leaning against a leg
// prices it a tick worse and can stop a thin lock; legs go in order of
// urgency; and the lock is unwound when the gap closes.
//
// No check description here may contain the substring "F" "AIL" joined.

#include <live/option_arb.hpp>

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("  %s: %s\n", ok ? "ok  " : "BAD ", what);
    if (!ok) ++failures;
}

using namespace altair;
using namespace altair::live;

constexpr std::int64_t kSec = 1'000'000'000LL;

LiveInstrument inst(std::uint32_t tok, const char* sym, LiveKind k, double strike, std::int64_t expiry) {
    LiveInstrument i;
    i.token = tok; i.symbol = sym; i.fyers = std::string("NSE:") + sym; i.underlying = "NIFTY";
    i.kind = k; i.lot = 75; i.tick = 0.05; i.strike = strike; i.expiry_day = expiry;
    return i;
}

LiveTop top(std::int64_t bid, std::int64_t ask, std::int64_t bq = 750, std::int64_t aq = 750) {
    LiveTop t;
    t.bid = bid; t.ask = ask; t.bid_qty = bq; t.ask_qty = aq; t.quote_ns = 1;
    return t;
}

} // namespace

int main() {
    std::printf("option arbitrage\n");
    // ---- the arithmetic -------------------------------------------------------------
    LiveOptionArbRule r;
    r.rate = 0.0;   // no carry: the numbers below are exact
    {
        LiveInstrument c = inst(1, "C", LiveKind::Call, 24000, 0), p = inst(2, "P", LiveKind::Put, 24000, 0),
                       f = inst(3, "F", LiveKind::Future, 0, 0);
        const std::vector<LiveArbLeg> legs{{&c, -1, 1.0}, {&p, 1, 1.0}, {&f, 1, 1.0}};
        // C 110/111, P 100/101, F 24005/24006: conversion = K - F_ask + C_bid - P_ask = 24000 - 24006 + 110 - 101 = 3.
        const auto q = live_lock_quote(legs, {top(11000, 11100), top(10000, 10100), top(2400500, 2400600)}, 24000.0, r, 0.0);
        check(q.ok && q.dir == 1 && std::fabs(q.edge - 3.0) < 1e-9, "conversion locks K - F + (C - P) at the touch: 3.00");
        // Reversal of the same book: F_bid - K - C_ask + P_bid = 24005 - 24000 - 111 + 100 = -6.
        check(std::fabs(q.costs - (1.0 + 1.0 + 1.0)) < 1e-9, "unwinding early crosses all three spreads (expenses here zero)");
        // A book leaning against the future (bid-heavy, so its ask is about to rise) costs a tick.
        const auto lean = live_lock_quote(legs, {top(11000, 11100), top(10000, 10100), top(2400500, 2400600, 9000, 100)},
                                          24000.0, r, 0.0);
        check(std::fabs(lean.edge_lean - (3.0 - 0.05)) < 1e-9, "a leg whose book leans against it is priced a tick worse");
        check(std::fabs(live_imbalance(top(0, 0, 9000, 1000), 5) - 0.8) < 1e-12, "imbalance (9000 - 1000) / 10000 = 0.8");
        check(std::fabs(live_microprice(top(10000, 10100, 3000, 1000)) - 100.75) < 1e-9,
              "microprice leans to the side about to be taken: 100.75");
    }
    {
        LiveInstrument c1 = inst(1, "C1", LiveKind::Call, 24000, 0), c2 = inst(2, "C2", LiveKind::Call, 24100, 0),
                       p1 = inst(3, "P1", LiveKind::Put, 24000, 0), p2 = inst(4, "P2", LiveKind::Put, 24100, 0);
        const std::vector<LiveArbLeg> legs{{&c1, 1, 1.0}, {&c2, -1, 1.0}, {&p2, 1, 1.0}, {&p1, -1, 1.0}};
        // Long box: 100 - C1_ask + C2_bid - P2_ask + P1_bid = 100 - 150 + 100 - 120 + 75 = 5.
        const auto q = live_lock_quote(legs, {top(14900, 15000), top(10000, 10100), top(11900, 12000), top(7500, 7600)},
                                       100.0, r, 0.0);
        check(q.ok && q.dir == 1 && std::fabs(q.edge - 5.0) < 1e-9, "a long box locks its width less what it costs: 5.00");
        check(std::fabs(q.costs - 4.0) < 1e-9, "and four spreads to unwind early");
    }

    // ---- the model, in an engine --------------------------------------------------------
    const std::int64_t day = parse_day("2026-10-05"), expiry = parse_day("2026-10-27");
    const std::vector<LiveInstrument> u{inst(11, "NIFTY26OCT24000CE", LiveKind::Call, 24000, expiry),
                                        inst(12, "NIFTY26OCT24000PE", LiveKind::Put, 24000, expiry),
                                        inst(13, "NIFTY26OCTFUT", LiveKind::Future, 0, expiry)};
    LiveExecPolicy instant;
    instant.latency_ns = 0;
    // Expenses: one rupee a fill per unit, so the six fills in and out cost 6.
    const LiveCostFn cost = [](const LiveInstrument&, bool, double qty, double, std::int64_t) { return 1.0 * qty; };
    LiveOptionArbRule rule;
    rule.rate = 0.0;
    rule.margin_bp = 1.1;     // 2.64 on a 24000 strike
    rule.exit_bp = 0.5;       // 1.20
    // Costs: 6 (fills) + 0.50 + 0.50 + 1.00 (spreads) + 2.64 (margin) = 10.64 to enter.
    const std::int64_t t0 = (day * 86400 + 10 * 3600 - 19800) * kSec;
    PricePayload p;
    p.token = 13; p.last_paise = 2400000; p.exchange_ts_ns = t0;
    const auto feed = [](LiveEngine& x, std::uint32_t tok, std::int64_t bid, std::int64_t ask, std::int64_t at,
                         std::int64_t bq = 750, std::int64_t aq = 750) {
        QuotePayload q;
        q.token = tok; q.flags = kQuoteHasTop; q.bid = bid; q.ask = ask; q.bid_qty = bq; q.ask_qty = aq;
        x.on_quote(q, at);
    };
    {
        LiveEngine e(u, cost, instant);
        e.add_model(std::make_unique<LiveOptionArbModel>(rule));
        e.on_trade(p, t0);
        // Fair: conversion locks K - F_ask + C_bid - P_ask = 24000 - 24000.50 + 110.00 - 110.50 = -1.
        feed(e, 11, 11000, 11050, t0 + kSec);
        feed(e, 12, 11000, 11050, t0 + kSec);
        feed(e, 13, 2399950, 2400050, t0 + kSec);
        check(e.book().flat("Option arbitrage"), "a fair book is left alone");
        // The call bids rich: the conversion locks 125 - 111 = 14 against 10.64.
        feed(e, 11, 12500, 12550, t0 + 2 * kSec);
        std::size_t legs = 0;
        for (const auto& pos : e.book().positions()) legs += pos.model == "Option arbitrage" ? 1u : 0u;
        const LivePosition* fpos = e.book().position("Option arbitrage", 13);
        const LivePosition* cpos = e.book().position("Option arbitrage", 11);
        check(legs == 3 && fpos != nullptr && fpos->side > 0 && cpos != nullptr && cpos->side < 0,
              "a conversion is entered on all three legs: call sold, put and future bought");
        check(cpos != nullptr && std::fabs(cpos->entry - 125.0) < 1e-9 && fpos != nullptr && std::fabs(fpos->entry - 24000.5) < 1e-9,
              "each at its touch");
        // The call cheapens back: the held conversion would lock -1 now, under 1.20: unwound.
        feed(e, 11, 11000, 11050, t0 + 3 * kSec);
        check(e.book().flat("Option arbitrage"), "the gap closed: every leg is unwound");
    }
    {
        LiveEngine e(u, cost, instant);
        e.add_model(std::make_unique<LiveOptionArbModel>(rule));
        e.on_trade(p, t0);
        feed(e, 12, 11000, 11050, t0 + kSec);
        feed(e, 13, 2399950, 2400050, t0 + kSec, 9000, 100);   // the future's book leans to the bid
        feed(e, 11, 12050, 12100, t0 + kSec);                   // locks 9.50
        check(e.book().flat("Option arbitrage"), "a lock under its costs (9.50 < 10.64) is not entered");
        feed(e, 11, 12165, 12215, t0 + 2 * kSec);               // locks 10.65 at the touch
        check(e.book().flat("Option arbitrage"),
              "10.65 clears 10.64 only at the touch: the future's leaning book makes it 10.60, so it is not chased");
        feed(e, 13, 2399950, 2400050, t0 + 3 * kSec);           // the future's book evens out
        check(!e.book().flat("Option arbitrage"), "with the books even, the same 10.65 is entered");
    }

    std::printf("%s\n", failures == 0 ? "all option arbitrage checks passed" : "option arbitrage checks did not pass");
    return failures == 0 ? 0 : 1;
}
