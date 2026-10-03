// The live paper path against the review's findings, one at a time:
// executable fills (fresh quote, size, latency, no last-trade fallback),
// entries that time out and unwind their other legs, exits that are
// obligations across a feed gap and keep working until filled, the watchdog,
// late prints and the first tick of a session, trade-frame gaps, snapshot
// frames, the risk gate, the journal and the single-writer lock.
//
// No check description here may contain the substring "F" "AIL" joined.

#include <live/engine.hpp>
#include <live/feed_consumer.hpp>
#include <live/file_lock.hpp>
#include <live/latency.hpp>
#include <live/margin.hpp>
#include <live/paper.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) { ++failures; }
}

using namespace altair;
using namespace altair::live;

constexpr std::int64_t kSec = 1'000'000'000LL;
const std::int64_t kToday = parse_day("2026-10-01");

std::int64_t at(int hh, int mm, int ss = 0) {
    return (kToday * 86400 + hh * 3600 + mm * 60 + ss - 19800) * kSec;
}

LiveInstrument inst(std::uint32_t tok, const char* sym, LiveKind k, std::int64_t lot, std::int64_t expiry = 0) {
    LiveInstrument i;
    i.token = tok; i.symbol = sym; i.fyers = std::string("NSE:") + sym; i.underlying = "NIFTY"; i.kind = k;
    i.lot = lot; i.expiry_day = expiry; i.tick = 0.05;
    return i;
}

std::vector<LiveInstrument> universe() {
    return {inst(kLiveNiftyToken, "NIFTY 50", LiveKind::Index, 1), inst(10, "FUT_A", LiveKind::Future, 50, kToday + 20),
            inst(11, "FUT_B", LiveKind::Future, 50, kToday + 20), inst(12, "FUT_EXP", LiveKind::Future, 50, kToday)};
}

void quote(LiveEngine& e, std::uint32_t tok, std::int64_t bid, std::int64_t ask, std::int64_t qty, std::int64_t ns) {
    QuotePayload q;
    q.token = tok; q.flags = kQuoteHasTop; q.bid = bid; q.ask = ask; q.bid_qty = qty; q.ask_qty = qty;
    e.on_quote(q, ns);
}
void trade(LiveEngine& e, std::uint32_t tok, std::int64_t px, std::int64_t ns) {
    PricePayload p;
    p.token = tok; p.last_paise = px;
    e.on_trade(p, ns);
}

double cost5bp(const LiveInstrument&, bool, double q, double px, std::int64_t) { return 0.0005 * q * px; }

/// A model that does nothing: the engine's own obligations are what is tested.
struct Idle final : LiveModel {
    std::string name() const override { return "idle"; }
    std::string family() const override { return "test"; }
    void on_minute(LiveEngine&, int) override { ++minutes; }
    LiveModelView view(const LiveEngine&) const override { return {}; }
    int minutes = 0;
};

}  // namespace

int main() {
    std::printf("live execution\n");
    const auto u = universe();

    // ---- executable fills ---------------------------------------------------
    {
        LiveEngine e(u, cost5bp);   // default policy: 250 ms latency, 10 s quote age, 60 s entry timeout
        trade(e, kLiveNiftyToken, 2400000, at(10, 0));
        std::string why;
        check(!e.book().open("m", u[1], 1, 1, at(10, 0), "x", false, &why) && why.find("never quoted") != std::string::npos,
              "no quote: the entry is refused, not filled at the last trade");
        trade(e, 10, 2400000, at(10, 0, 1));
        check(!e.book().open("m", u[1], 1, 1, at(10, 0, 1), "x", false, &why), "a last trade alone is not a price to deal at");
        quote(e, 10, 2399900, 2400100, 50, at(10, 0, 2));
        trade(e, kLiveNiftyToken, 2400000, at(10, 0, 30));
        check(!e.book().open("m", u[1], 1, 1, at(10, 0, 30), "x", false, &why) && why.find("s old") != std::string::npos,
              "a 28-second-old quote is stale: refused, and the reason gives its age");

        quote(e, 10, 2399900, 2400100, 50, at(10, 1));
        check(e.book().open("m", u[1], 1, 2, at(10, 1), "two lots", false, &why), "a fresh quote: the entry is accepted");
        const LivePosition* p = e.book().position("m", 10);
        check(p != nullptr && p->state == LivePosState::Opening && !p->filled() && e.book().fills().empty(),
              "it works as an order: nothing fills before the latency");
        quote(e, 10, 2399900, 2400100, 50, at(10, 1) + 300'000'000);
        p = e.book().position("m", 10);
        check(p != nullptr && p->qty == 50 && p->state == LivePosState::Opening && e.book().fills().size() == 1
                  && e.book().fills().back().partial,
              "after the latency, it takes only the 50 shown at the ask: a partial fill");
        check(p != nullptr && p->entry == 24001.0, "at the ask, never the mid");
        quote(e, 10, 2400000, 2400300, 50, at(10, 1, 2));
        p = e.book().position("m", 10);
        check(p != nullptr && p->qty == 100 && p->state == LivePosState::Open && std::fabs(p->entry - 24002.0) < 1e-9,
              "the rest fills against the next quote; the entry is volume-weighted");

        // Five levels: a fresh book is walked before the touch.
        LiveLevel bids[kLiveDepth] = {{2400000, 30}, {2399900, 30}, {2399800, 100}, {}, {}};
        LiveLevel asks[kLiveDepth] = {{2400100, 30}, {2400200, 30}, {2400300, 100}, {}, {}};
        e.on_book(10, 3, bids, asks, at(10, 1, 3));
        check(e.book().close("m", 10, at(10, 1, 3), "test exit"), "an exit is submitted");
        e.advance(at(10, 1, 4));
        const auto& tr = e.book().trades();
        check(tr.size() == 1 && std::fabs(tr.back().exit - (24000.0 * 30 + 23999.0 * 30 + 23998.0 * 40) / 100.0) < 1e-9,
              "a sell of 100 walks three bid levels: 30, 30 and 40 of the third");
        check(tr.size() == 1 && std::isfinite(tr.back().net) && tr.back().expenses > 0.0,
              "priced on every fill: net = gross - expenses");
    }

    // ---- an entry that never fills is cancelled; its other leg is unwound -----
    {
        LiveEngine e(u, cost5bp);
        trade(e, kLiveNiftyToken, 2400000, at(11, 0));
        quote(e, 10, 2399900, 2400100, 50, at(11, 0));
        quote(e, 11, 2399900, 2400100, 0, at(11, 0));   // priced, but no size shown: nothing to sell into
        check(e.book().open("pair", u[1], 1, 1, at(11, 0), "leg a", true) && e.book().open("pair", u[2], -1, 1, at(11, 0), "leg b", true),
              "two legs of one decision are working");
        quote(e, 10, 2399900, 2400100, 50, at(11, 0, 1));   // leg a fills; leg b never sees another quote
        e.advance(at(11, 0, 1));
        check(e.book().position("pair", 10) && e.book().position("pair", 10)->filled() && !e.book().position("pair", 11)->filled(),
              "one leg filled, the other did not");
        trade(e, kLiveNiftyToken, 2400000, at(11, 1, 5));    // 65 s later
        const auto notes = e.book().take_cancelled();
        const LivePosition* a = e.book().position("pair", 10);
        check(e.book().position("pair", 11) == nullptr && !notes.empty(), "the unfilled leg is cancelled at its timeout");
        check(a != nullptr && a->state == LivePosState::Closing && a->exit_reason == "other leg unfilled",
              "and the filled leg is unwound, not left on alone");
        // The exit has no fresh quote; it keeps working past every timeout.
        trade(e, kLiveNiftyToken, 2400000, at(11, 10));
        check(e.book().position("pair", 10) != nullptr && e.book().working_orders() == 1,
              "an exit never expires: unquoted for nine minutes, it is still working");
        quote(e, 10, 2399000, 2399200, 500, at(11, 10, 1));
        check(e.book().position("pair", 10) == nullptr && e.book().trades().size() == 1, "and fills when a quote returns");
    }

    // ---- cancels and fills that cross ------------------------------------------
    {
        // A quote does not move the clock; one that arrives after an entry's
        // deadline, before any trade has, must not fill it.
        LiveEngine e(u, cost5bp);
        trade(e, kLiveNiftyToken, 2400000, at(12, 0));
        quote(e, 10, 2399900, 2400100, 50, at(12, 0));
        check(e.book().open("m", u[1], 1, 1, at(12, 0), "x", false), "an entry is working");
        quote(e, 10, 2399900, 2400100, 0, at(12, 0));   // the size goes before the latency is up
        e.advance(at(12, 0, 1));
        check(e.book().position("m", 10) && !e.book().position("m", 10)->filled(), "nothing to take: it keeps working");
        quote(e, 10, 2399900, 2400100, 500, at(12, 1, 1));   // 61 s on; the clock still says 12:00:01
        check(e.book().position("m", 10) == nullptr && e.book().fills().empty(),
              "a quote after the deadline does not fill it: the entry is cancelled, not filled late");
        check(!e.book().take_cancelled().empty(), "and the cancellation is reported");

        // The last instant before the deadline still fills.
        LiveEngine f(u, cost5bp);
        trade(f, kLiveNiftyToken, 2400000, at(12, 0));
        quote(f, 10, 2399900, 2400100, 50, at(12, 0));
        (void)f.book().open("m", u[1], 1, 1, at(12, 0), "x", false);
        quote(f, 10, 2399900, 2400100, 0, at(12, 0));
        quote(f, 10, 2399900, 2400100, 500, at(12, 1) - 1'000'000);
        check(f.book().position("m", 10) && f.book().position("m", 10)->filled() && f.book().fills().size() == 1,
              "one millisecond before the deadline, it fills");

        // An exit decided while the entry is part-filled: the rest of the
        // entry is withdrawn, only what filled is sold, and a quote arriving
        // after adds nothing to the position.
        LiveEngine g(u, cost5bp);
        trade(g, kLiveNiftyToken, 2400000, at(12, 0));
        quote(g, 10, 2399900, 2400100, 50, at(12, 0));
        (void)g.book().open("m", u[1], 1, 2, at(12, 0), "two lots", false);
        quote(g, 10, 2399900, 2400100, 30, at(12, 0, 1));   // 30 of 100
        check(g.book().position("m", 10) && g.book().position("m", 10)->qty == 30, "30 of 100 filled");
        check(g.book().close("m", 10, at(12, 0, 1), "stop"), "an exit crosses the working entry");
        quote(g, 10, 2399800, 2400200, 500, at(12, 0, 2));
        g.advance(at(12, 0, 2));
        std::int64_t opened = 0, closed = 0;
        for (const auto& fl : g.book().fills()) (fl.role == LiveFillRole::Open ? opened : closed) += fl.qty;
        check(opened == 30 && closed == 30 && g.book().position("m", 10) == nullptr && g.book().working_orders() == 0,
              "30 bought, 30 sold, nothing left working: the withdrawn 70 never fill");
        check(g.book().trades().size() == 1 && g.book().trades().back().qty == 30, "one round trip of exactly what was held");

        // An exit decided before an entry's first fill: cancelled outright.
        LiveEngine h(u, cost5bp);
        trade(h, kLiveNiftyToken, 2400000, at(12, 0));
        quote(h, 10, 2399900, 2400100, 50, at(12, 0));
        (void)h.book().open("m", u[1], 1, 1, at(12, 0), "x", false);
        check(h.book().close("m", 10, at(12, 0) + 100'000'000, "changed its mind"), "an exit before the latency is up");
        quote(h, 10, 2399900, 2400100, 500, at(12, 0, 1));
        check(h.book().fills().empty() && h.book().position("m", 10) == nullptr && h.book().working_orders() == 0,
              "cancels the entry: no fill in either direction");
    }

    // ---- exits are obligations, across a gap in the feed ----------------------
    {
        LiveEngine e(u, cost5bp, LiveExecPolicy{0, 10 * kSec, 60 * kSec});
        auto* idle = new Idle;
        e.add_model(std::unique_ptr<LiveModel>(idle));
        trade(e, kLiveNiftyToken, 2400000, at(9, 15, 1));
        quote(e, 10, 2399900, 2400100, 500, at(14, 39, 55));
        quote(e, 12, 2399900, 2400100, 500, at(14, 39, 55));
        trade(e, kLiveNiftyToken, 2400000, at(14, 40));
        check(e.book().open("intraday", u[1], 1, 1, at(14, 40), "x", false), "an intraday position at 14:40");
        check(e.book().open("carried", u[3], 1, 1, at(14, 40), "x", true), "a carried one in a contract expiring today");
        // The feed is silent from 14:40 to 15:25: no 15:15 tick, no 15:20 tick.
        quote(e, 10, 2399900, 2400100, 500, at(15, 25));
        quote(e, 12, 2399900, 2400100, 500, at(15, 25));
        trade(e, kLiveNiftyToken, 2400000, at(15, 25));
        check(e.book().position("intraday", 10) == nullptr, "the 15:20 square-off still happens when the clock jumps past it");
        check(e.book().position("carried", 12) == nullptr, "and so does the 15:15 roll of a contract expiring today");
        bool overdue = false;
        for (const auto& t : e.book().trades()) overdue = overdue || t.why_out.find("overdue") != std::string::npos;
        check(overdue, "the reason says the exit was overdue");

        // An exit that cannot fill at 15:20 is retried until it does -- even after 15:30.
        LiveEngine f(u, cost5bp, LiveExecPolicy{0, 10 * kSec, 60 * kSec});
        trade(f, kLiveNiftyToken, 2400000, at(15, 0));
        quote(f, 11, 2399900, 2400100, 500, at(15, 0));
        check(f.book().open("late", u[2], -1, 1, at(15, 0), "x", false), "a short at 15:00");
        trade(f, kLiveNiftyToken, 2400000, at(15, 20, 30));   // 15:20: the quote is twenty minutes old
        const LivePosition* p = f.book().position("late", 11);
        check(p != nullptr && p->state == LivePosState::Closing, "at 15:20 the exit is submitted but has nothing to fill against");
        trade(f, kLiveNiftyToken, 2400000, at(15, 35));
        check(f.book().position("late", 11) != nullptr, "still working at 15:35, not forgotten");
        quote(f, 11, 2399900, 2400100, 500, at(15, 36));
        check(f.book().position("late", 11) == nullptr && f.book().trades().size() == 1, "filled at the first fresh quote");
        std::string why;
        quote(f, 10, 2399900, 2400100, 500, at(15, 36));
        check(!f.book().open("again", u[1], 1, 1, at(15, 36), "x", false, &why) && why.find("15:20") != std::string::npos,
              "no intraday entry after the square-off");
    }

    // ---- the watchdog: the feed stops before 15:20 ----------------------------
    {
        LiveEngine e(u, cost5bp, LiveExecPolicy{0, 10 * kSec, 60 * kSec});
        trade(e, kLiveNiftyToken, 2400000, at(15, 0));
        quote(e, 10, 2399900, 2400100, 500, at(15, 0));
        check(e.book().open("w", u[1], 1, 1, at(15, 0), "x", false), "an intraday position at 15:00, then the feed stops");
        e.watchdog(at(15, 10));
        check(e.book().position("w", 10)->state == LivePosState::Open, "before 15:20 the watchdog does nothing");
        e.watchdog(at(15, 22));
        check(e.book().position("w", 10)->state == LivePosState::Closing
                  && e.book().position("w", 10)->exit_reason.find("watchdog") != std::string::npos,
              "at 15:22 on the machine's clock, the square-off is submitted without the feed");
        quote(e, 10, 2399900, 2400100, 500, at(15, 23));
        check(e.book().position("w", 10) == nullptr, "and fills the moment a quote returns");
    }

    // ---- bars: the clock first, late prints refused ---------------------------
    {
        LiveEngine e(u, cost5bp);
        trade(e, 10, 2400000, at(10, 0, 10));
        trade(e, 10, 2401000, at(10, 1, 5));    // closes 10:00
        trade(e, 10, 2500000, at(10, 0, 50));   // late: stamped inside the closed 10:00 bar
        check(e.bars(10).size() == 1 && e.bars(10).back().c == 24000.0 && e.late_prints() == 1,
              "a late print for a closed minute is refused, not written into it");
        trade(e, 10, 2402000, at(10, 2, 1));
        check(e.bars(10).size() == 2 && e.bars(10).back().start_of_day() == 601, "the bar stream carries on in order");
        // The next session's first tick: the day resets BEFORE it is kept.
        const std::int64_t next = at(9, 15, 2) + 86400 * kSec;
        trade(e, 10, 2410000, next);
        trade(e, 10, 2411000, next + 60 * kSec);
        check(e.bars(10).size() == 1 && e.bars(10).front().o == 24100.0 && e.bars(10).front().start_of_day() == 555,
              "the first tick of a session opens that session's first bar, not yesterday's");
    }

    // ---- trade-frame gaps and snapshots, through the consumer -----------------
    {
        LiveEngine e(u, cost5bp, LiveExecPolicy{0, 10 * kSec, 60 * kSec});
        auto* idle = new Idle;
        e.add_model(std::unique_ptr<LiveModel>(idle));
        LiveFeedConsumer c(e);
        std::uint8_t body[256];
        const auto frame = [&](FrameKind kind, std::uint32_t topic, std::uint64_t seq, std::uint32_t tok, std::int64_t px,
                               std::int64_t ns) {
            FrameHeader h;
            h.kind = kind; h.channel = Channel::State; h.topic = topic; h.seq = seq; h.engine_time_ns = ns;
            if (topic == kTopicQuote) {
                QuotePayload q; q.token = tok; q.flags = kQuoteHasTop; q.bid = px - 100; q.ask = px + 100; q.bid_qty = 500; q.ask_qty = 500;
                h.payload_len = static_cast<std::uint32_t>(*encode_quote(q, body, sizeof body));
            } else {
                PricePayload p; p.token = tok; p.last_paise = px; p.exchange_ts_ns = ns;
                h.payload_len = static_cast<std::uint32_t>(*encode_price(p, nullptr, nullptr, body, sizeof body));
            }
            return c.on_frame(h, body);
        };
        frame(FrameKind::Snapshot, kTopicTrades, 40, 10, 2300000, at(9, 50));   // a baseline from 09:50
        check(e.ltp(10) == 23000.0 && e.clock_ns() == 0 && e.bars(10).empty(),
              "a snapshot trade sets the price but is not a trade: no clock, no bar");
        frame(FrameKind::Delta, kTopicTrades, 41, 10, 2400000, at(10, 0, 1));
        frame(FrameKind::Delta, kTopicTrades, 42, 10, 2400000, at(10, 1, 1));
        const int before = idle->minutes;
        frame(FrameKind::Delta, kTopicTrades, 45, 10, 2400000, at(10, 2, 1));   // 43 and 44 lost
        check(c.missed(kTopicTrades) == 2 && e.trade_gaps() == 1 && e.paused(), "a sequence jump is a gap: decisions pause");
        frame(FrameKind::Delta, kTopicQuote, 1, 10, 2400000, at(10, 2, 2));
        std::string why;
        check(!e.book().open("m", u[1], 1, 1, at(10, 2, 2), "x", false, &why) && why.find("paused") != std::string::npos,
              "and the risk check refuses new entries while paused");
        frame(FrameKind::Delta, kTopicTrades, 45, 10, 2400000, at(10, 2, 3));   // a duplicate
        frame(FrameKind::Delta, kTopicTrades, 46, 10, 2400000, at(10, 3, 1));
        check(idle->minutes == before, "the minute with the gap runs no model");
        frame(FrameKind::Delta, kTopicTrades, 47, 10, 2400000, at(10, 4, 1));
        check(idle->minutes == before + 1 && !e.paused(), "after one whole clean minute, decisions resume");
        c.on_reconnect();
        frame(FrameKind::Snapshot, kTopicTrades, 60, 10, 2400000, at(10, 6));
        check(c.missed(kTopicTrades) == 15 && e.trade_gaps() == 2, "a reconnect that finds the stream moved on is a gap too");
    }

    // ---- the risk gate ---------------------------------------------------------
    {
        LiveEngine e(u, cost5bp, LiveExecPolicy{0, 10 * kSec, 60 * kSec});
        trade(e, kLiveNiftyToken, 2400000, at(10, 0));
        quote(e, 10, 2399900, 2400100, 500, at(10, 0));
        quote(e, 11, 2399900, 2400100, 500, at(10, 0));
        std::string why;
        e.set_kill(true);
        check(!e.book().open("m", u[1], 1, 1, at(10, 0), "x", false, &why) && why.find("kill") != std::string::npos,
              "a kill request refuses every entry");
        e.set_kill(false);
        e.set_halt("cannot write journal.csv");
        check(!e.book().open("m", u[1], 1, 1, at(10, 0), "x", false, &why) && why.find("halted") != std::string::npos,
              "so does a ledger that cannot be written");
        e.set_halt({});
        e.set_stale(true);
        check(!e.book().open("m", u[1], 1, 1, at(10, 0), "x", false, &why) && why.find("stale") != std::string::npos,
              "and a stale feed");
        e.set_stale(false);
        LiveRiskLimits l;
        l.max_positions = 1;
        e.set_limits(l);
        check(e.book().open("m", u[1], 1, 1, at(10, 0), "x", false, &why), "inside the limits, an entry passes");
        check(!e.book().open("m", u[2], 1, 1, at(10, 0), "x", false, &why) && why.find("position limit") != std::string::npos,
              "the position limit refuses the next");
        l.max_positions = 80;
        l.max_gross_notional = 1.5e6;   // one lot is 1.2 million
        e.set_limits(l);
        check(!e.book().open("m", u[2], 1, 1, at(10, 0), "x", false, &why) && why.find("gross") != std::string::npos,
              "the gross notional limit counts what is held plus the new order");
        l.max_gross_notional = 1.0e8;
        l.max_margin = 2.0e5;           // one lot's estimate is 12 % of 1.2 million: 144,000
        e.set_limits(l);
        check(std::fabs(e.margin_estimate() - 144000.0) < 1e-6, "the margin estimate of a held future: scan plus exposure, 12 %");
        check(!e.book().open("m", u[2], 1, 1, at(10, 0), "x", false, &why) && why.find("margin estimate") != std::string::npos,
              "the margin limit counts what is held plus the new order");
        l.max_margin = 3.0e5;
        e.set_limits(l);
        check(e.book().open("m", u[2], 1, 1, at(10, 0), "x", false, &why) && std::fabs(e.margin_estimate() - 288000.0) < 1e-6,
              "and a working entry counts at what it asked for");
        e.set_kill(true);
        check(e.book().close("m", 10, at(10, 0), "exit under kill") && e.book().position("m", 10) == nullptr,
              "an exit is never refused, kill request or not");
    }

    // ---- the journal: every fill on the record, the book rebuilt from it ------
    {
        namespace fs = std::filesystem;
        // A directory of this run's own: two runs at once (parallel CI jobs on
        // one machine) must not share a ledger -- or a lock.
        const auto stamp = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
                         + "_" + std::to_string(std::random_device{}());
        const fs::path dir = fs::temp_directory_path() / ("altair_live_journal_test_" + stamp);
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        const std::string path = (dir / "journal.csv").string();
        LiveEngine e(u, nullptr, LiveExecPolicy{0, 10 * kSec, 60 * kSec});
        trade(e, kLiveNiftyToken, 2400000, at(10, 0));
        quote(e, 10, 2399900, 2400100, 30, at(10, 0));
        (void)e.book().open("j", u[1], 1, 1, at(10, 0), "journal test", true);   // 30 of 50 fill now
        quote(e, 10, 2399900, 2400300, 30, at(10, 0, 1));                        // the other 20
        quote(e, 11, 2399900, 2400100, 500, at(10, 0, 1));
        (void)e.book().open("j", u[2], -1, 1, at(10, 0, 1), "closed later", true);
        (void)e.book().close("j", 11, at(10, 0, 2), "done");
        std::vector<std::string> rows;
        for (const auto& f : e.book().fills()) rows.push_back(live_journal_row(f));
        check(live_append_rows(path, kLiveJournalHeader, rows), "the fills are appended and acknowledged");
        std::vector<std::string> orphans;
        std::size_t n = 0;
        const auto back = live_replay_journal(path, u, orphans, &n);
        check(n == 4 && back.size() == 1 && back[0].model == "j" && back[0].qty == 50 && back[0].carry,
              "replayed: two entry fills and a round trip give one held position of 50");
        check(!back.empty() && std::fabs(back[0].entry - (24001.0 * 30 + 24003.0 * 20) / 50.0) < 1e-9
                  && !std::isfinite(back[0].entry_expenses),
              "at the volume-weighted entry, and unpriced expenses stay unpriced");
        const std::string snap = (dir / "open_positions.csv").string();
        { std::ofstream old(snap); old << "stale\n"; }
        check(live_write_positions(snap, e.book().held()), "the snapshot replaces the old file in one step");
        std::ifstream in(snap);
        std::string head;
        std::getline(in, head);
        check(head == kLivePositionsHeader && !fs::exists(snap + ".tmp"), "the new file is whole and no temporary is left");

        const std::string lockp = (dir / "engine.lock").string();
        const LiveFileLock first(lockp);
        const LiveFileLock second(lockp);
        check(first.held() && !second.held() && second.why().find("another process") != std::string::npos,
              "one engine per ledger: a second lock is refused");
        fs::remove_all(dir, ec);
    }

    // ---- the margin estimate (not SPAN: conservative by construction) -------------
    {
        const auto close_to = [](double a, double b) { return std::fabs(a - b) < 1e-6; };
        LiveMarginLeg f;
        f.kind = LiveKind::Future; f.side = -1; f.qty = 75; f.price = 23000.0;
        check(close_to(live_margin(f), 0.12 * 23000.0 * 75), "a future, either side: index scan 10 % plus exposure 2 %");
        f.index = false;
        check(close_to(live_margin(f), (0.20 + 0.035) * 23000.0 * 75), "a stock future at the stock rates");
        LiveMarginLeg c;
        c.kind = LiveKind::Call; c.side = -1; c.qty = 75; c.price = 40.0; c.underlying = 23000.0; c.strike = 23500.0;
        const double un = 23000.0 * 75;
        check(close_to(live_margin(c), 0.10 * un - 500.0 * 75 + 0.02 * un), "a short call: scan less the out-of-the-money amount, plus exposure");
        LiveMarginLeg pt = c;
        pt.kind = LiveKind::Put; pt.strike = 20000.0;
        check(close_to(live_margin(pt), 0.03 * un + 0.02 * un), "a far short put: never below the short-option minimum");
        pt.strike = 23500.0;
        check(close_to(live_margin(pt), 0.12 * un), "an in-the-money short put: the whole scan, no credit");
        LiveMarginLeg lc = c;
        lc.side = 1;
        check(close_to(live_margin(lc), 40.0 * 75), "a long option blocks its premium");
        c.underlying = 0.0;
        check(std::isnan(live_margin(c)), "a short option with no underlying level is unknown, not zero");
        LiveMarginLeg eq;
        eq.kind = LiveKind::Equity; eq.index = false; eq.side = 1; eq.qty = 10; eq.price = 800.0; eq.carry = true;
        check(close_to(live_margin(eq), 8000.0), "cash equity held overnight: its whole value");
        eq.carry = false;
        check(close_to(live_margin(eq), 1600.0), "held intraday: the intraday fraction");
        eq.side = -1; eq.carry = true;
        check(close_to(live_margin(eq), 1600.0), "a cash short is intraday whatever it says");
        check(live_margin(LiveMarginLeg{}) == 0.0, "nothing held, nothing blocked");
    }

    // ---- latency percentiles ----------------------------------------------------
    {
        LatencyHistogram h;
        for (std::int64_t v = 1; v <= 1'000'000; ++v) h.record(v * 1000);   // 1 us .. 1 s, uniform
        const auto close_to = [](std::uint64_t got, double want) { return std::fabs(static_cast<double>(got) / want - 1.0) < 0.04; };
        check(h.count() == 1'000'000 && h.max() == 1'000'000'000ull, "every sample counted, and the max is exact");
        check(close_to(h.quantile(0.5), 5e8) && close_to(h.quantile(0.99), 9.9e8) && close_to(h.quantile(0.999), 9.99e8),
              "p50, p99 and p99.9 within the buckets' 3 % of the truth");
        check(h.quantile(0.5) >= 500'000'000ull, "a quantile is its bucket's upper edge: never optimistic");
        LatencyHistogram tiny;
        for (int v = 0; v < 32; ++v) tiny.record(v);
        check(tiny.quantile(0.5) == 15 || tiny.quantile(0.5) == 16, "below 32 ns every value is its own bucket");
        check(h.json(1e3).find("\"p999\"") != std::string::npos, "and it reads out as JSON for the state file");
    }

    std::printf("%s\n", failures == 0 ? "all live execution checks passed" : "live execution checks did not pass");
    return failures == 0 ? 0 : 1;
}
