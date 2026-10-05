// The cross-exchange arbitrage (live/arbitrage.hpp) on quotes whose answers
// are known: NSE and BSE listings pair up by symbol; a gap that does not cover
// the costs, both spreads and the margin is left alone; one that does is
// entered both ways at the touch, sized to the notional and the touch; the
// position is unwound when the two mids meet; nothing is entered after 15:10
// or on a stale quote; and the SIM's BSE twins follow their NSE twin.
//
// No check description here may contain the substring "F" "AIL" joined.

#include <live/arbitrage.hpp>
#include <live/sim.hpp>

#include <cmath>
#include <cstdio>
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

LiveInstrument eq(std::uint32_t tok, const char* sym, const char* venue) {
    LiveInstrument i;
    i.token = tok;
    i.symbol = sym;
    i.fyers = std::string(venue) + ":" + sym + (std::string(venue) == "NSE" ? "-EQ" : "-A");
    i.underlying = sym;
    i.kind = LiveKind::Equity;
    i.lot = 1;
    i.tick = 0.05;
    return i;
}

struct Feed {
    LiveEngine& e;
    void quote(std::uint32_t tok, std::int64_t bid, std::int64_t ask, std::int64_t at, std::int64_t qty = 100) {
        QuotePayload q;
        q.token = tok;
        q.flags = kQuoteHasTop;
        q.bid = bid; q.ask = ask; q.bid_qty = qty; q.ask_qty = qty;
        e.on_quote(q, at);
    }
    void trade(std::uint32_t tok, std::int64_t px, std::int64_t at) {
        PricePayload p;
        p.token = tok;
        p.last_paise = px;
        p.last_qty = 1;
        p.exchange_ts_ns = at;
        e.on_trade(p, at);
    }
};

} // namespace

int main() {
    std::printf("live cross-exchange arbitrage\n");
    const std::vector<LiveInstrument> u{eq(1, "RELIANCE", "NSE"), eq(2, "RELIANCE", "BSE"), eq(3, "INFY", "NSE")};
    const auto pairs = live_cross_pairs(u);
    check(pairs.size() == 1 && pairs[0].symbol == "RELIANCE" && pairs[0].nse == 1 && pairs[0].bse == 2,
          "a stock listed on both exchanges pairs up; one on NSE alone does not");

    LiveState n, b;
    n.bid = 100000; n.ask = 100010; b.bid = 99850; b.ask = 99860;
    const auto q = live_cross_quote(&n, &b);
    check(q.ok && std::fabs(q.sell_nse_bps - 140.0 / q.mid * 1e4) < 1e-9 && q.sell_bse_bps < 0
              && std::fabs(q.spreads_bps - 20.0 / q.mid * 1e4) < 1e-9,
          "the edge is the crossed spread, in bp of the price; the spreads are what getting out costs");

    LiveExecPolicy instant;
    instant.latency_ns = 0;
    // Expenses: 1.5 bp a fill, so the four fills cost 6 bp.
    const LiveCostFn cost = [](const LiveInstrument&, bool, double qty, double px, std::int64_t) { return 0.00015 * qty * px; };
    LiveEngine e(u, cost, instant);
    e.add_model(std::make_unique<LiveCrossArbModel>());
    Feed f{e};
    const std::int64_t day = parse_day("2026-10-05");
    const std::int64_t t0 = (day * 86400 + 10 * 3600 - 19800) * kSec;   // 10:00 IST
    f.trade(1, 100000, t0);
    f.trade(2, 100000, t0);

    // 9 bp across: under the four fills' expenses (6) + spreads (2) + margin (2).
    f.quote(1, 100000, 100010, t0 + kSec);
    f.quote(2, 99900, 99910, t0 + kSec);
    check(e.book().flat("Cross-exchange arbitrage"), "a 9 bp gap under the 10 bp needed is left alone");

    // 14 bp across: NSE bid 1000.00 over BSE ask 998.60.
    f.quote(2, 99850, 99860, t0 + 2 * kSec);
    const auto ps = e.book().positions();
    const LivePosition* longb = nullptr;
    const LivePosition* shortn = nullptr;
    for (const auto& p : ps) {
        if (p.inst.token == 2 && p.side > 0) longb = &p;
        if (p.inst.token == 1 && p.side < 0) shortn = &p;
    }
    check(ps.size() == 2 && longb != nullptr && shortn != nullptr, "14 bp: bought on BSE, sold on NSE, both legs");
    check(longb != nullptr && shortn != nullptr && longb->qty == 100 && shortn->qty == 100
              && std::fabs(longb->entry - 998.60) < 1e-9 && std::fabs(shortn->entry - 1000.00) < 1e-9,
          "Rs 2 lakh a leg is 200 shares, but the touch shows 100: 100, at the touch on each exchange");

    // The prices meet: both mids within a basis point.
    f.quote(2, 99995, 100005, t0 + 30 * kSec);
    f.quote(1, 99995, 100005, t0 + 30 * kSec);
    const auto& trades = e.book().trades();
    double gross = 0.0;
    for (const auto& t : trades) gross += t.gross;
    check(e.book().flat("Cross-exchange arbitrage") && trades.size() == 2, "the prices met: both legs are out");
    check(std::fabs(gross - ((999.95 - 998.60) * 100 + (1000.00 - 1000.05) * 100)) < 1e-6,
          "and the gap, less the spreads crossed to get out, is the profit");

    // Nothing after 15:10, and nothing on a stale quote.
    const std::int64_t late = (day * 86400 + 15 * 3600 + 11 * 60 - 19800) * kSec;
    f.trade(1, 100000, late);
    f.quote(1, 100000, 100010, late);
    f.quote(2, 99850, 99860, late);
    check(e.book().flat("Cross-exchange arbitrage"), "no entry after 15:10");
    const std::int64_t mid = t0 + 600 * kSec;
    LiveEngine e2(u, cost, instant);
    e2.add_model(std::make_unique<LiveCrossArbModel>());
    Feed f2{e2};
    f2.trade(1, 100000, t0);
    f2.quote(1, 100000, 100010, t0);
    f2.trade(1, 100000, mid);
    f2.quote(2, 99850, 99860, mid);
    check(e2.book().flat("Cross-exchange arbitrage"), "an NSE quote ten minutes old is not traded against");

    // The SIM's BSE twin follows its NSE twin within a few dozen bp.
    LiveSimSeeds seeds;
    seeds.nifty = 24000; seeds.banknifty = 54000; seeds.vix = 13;
    seeds.stocks["RELIANCE"] = 1400.0;
    LiveSim sim(u, seeds, 11, t0);
    double worst = 0.0;
    int dislocations = 0;
    std::int64_t last_n = 0, last_b = 0;
    for (int s = 0; s < 3600; ++s) {
        sim.step(kSec, [&](const LiveInstrument& in, const LiveSimEvent& ev) {
            if (in.token == 1 && ev.quote.bid > 0) last_n = ev.quote.bid + ev.quote.ask;
            if (in.token == 2 && ev.quote.bid > 0) last_b = ev.quote.bid + ev.quote.ask;
        });
        if (last_n > 0 && last_b > 0) {
            const double bp = std::fabs(static_cast<double>(last_b - last_n)) / static_cast<double>(last_n) * 1e4;
            worst = std::max(worst, bp);
            if (bp > 10.0) ++dislocations;
        }
    }
    std::printf("    SIM: widest NSE/BSE gap in an hour %.1f bp; %d seconds over 10 bp\n", worst, dislocations);
    check(worst < 60.0 && dislocations > 0, "the SIM's BSE price tracks NSE, with dislocations to trade");

    std::printf("%s\n", failures == 0 ? "all cross-exchange arbitrage checks passed" : "cross-exchange arbitrage checks did not pass");
    return failures == 0 ? 0 : 1;
}
