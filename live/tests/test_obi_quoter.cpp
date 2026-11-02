// The OBI-skewed option quoter (live/obi_quoter.hpp), in an engine, on books
// whose answers are known: with a balanced future it rests a sale on both the
// out-of-the-money put and call; bids piling up on the future pull the call
// (the leg about to be lifted) and keep the put; asks piling up do the
// mirror; a resting sale fills only through the queue and is bought back by
// its bracket at the target or the stop; nothing is ever bought to open.
//
// No check description here may contain the substring "F" "AIL" joined.

#include <live/obi_quoter.hpp>

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

constexpr std::int64_t kMs = 1'000'000LL;
constexpr std::uint32_t kIdx = 1, kFut = 13;

LiveInstrument inst(std::uint32_t tok, const std::string& sym, LiveKind k, double strike, std::int64_t expiry) {
    LiveInstrument i;
    i.token = tok; i.symbol = sym; i.fyers = "NSE:" + sym; i.underlying = "NIFTY";
    i.kind = k; i.lot = k == LiveKind::Index ? 1 : 75; i.tick = 0.05; i.strike = strike; i.expiry_day = expiry;
    return i;
}

struct Rig {
    std::int64_t t0 = 0, vol = 0;
    std::vector<LiveInstrument> u;
    std::unique_ptr<LiveEngine> e;
    LiveObiQuoterModel* m = nullptr;

    explicit Rig(ObiQuoterRules r = {}, int hour = 10) {
        const std::int64_t day = parse_day("2026-10-05"), expiry = parse_day("2026-10-27");
        t0 = (day * 86400 + hour * 3600 - 19800) * 1'000'000'000LL;
        u.push_back(inst(kIdx, "NIFTY", LiveKind::Index, 0, 0));
        u.push_back(inst(kFut, "NIFTY26OCTFUT", LiveKind::Future, 0, expiry));
        std::uint32_t tok = 100;
        for (int k = 23850; k <= 24150; k += 50) {
            u.push_back(inst(tok++, "NIFTY26OCT" + std::to_string(k) + "CE", LiveKind::Call, k, expiry));
            u.push_back(inst(tok++, "NIFTY26OCT" + std::to_string(k) + "PE", LiveKind::Put, k, expiry));
        }
        LiveExecPolicy instant;
        instant.latency_ns = 0;
        const LiveCostFn cost = [](const LiveInstrument&, bool, double qty, double, std::int64_t) { return 1.0 * qty; };
        e = std::make_unique<LiveEngine>(u, cost, instant);
        r.under = "NIFTY";
        ObiHub::Config hc;
        hc.unders = {"NIFTY"};
        auto hh = std::make_unique<ObiHub>(hc);
        const ObiHub* hub = hh.get();
        e->add_model(std::move(hh));
        auto mm = std::make_unique<LiveObiQuoterModel>(r, hub);
        m = mm.get();
        e->add_model(std::move(mm));
        quote(kIdx, 2399995, 2400005, 1, 1, 0);
        print(kFut, 2400000, 0, 0);   // the session's first print dates it
    }
    [[nodiscard]] std::uint32_t token_of(const std::string& sym) const {
        for (const auto& i : u) if (i.symbol == sym) return i.token;
        return 0;
    }
    void quote(std::uint32_t tok, std::int64_t bid, std::int64_t ask, std::int64_t bq, std::int64_t aq, std::int64_t at_ms) {
        QuotePayload q;
        q.token = tok; q.flags = kQuoteHasTop; q.bid = bid; q.ask = ask; q.bid_qty = bq; q.ask_qty = aq;
        e->on_quote(q, t0 + at_ms * kMs);
    }
    void book(std::uint32_t tok, std::int64_t bid, std::int64_t ask, std::int64_t bq, std::int64_t aq, std::int64_t at_ms) {
        LiveLevel b[5], a[5];
        for (int k = 0; k < 5; ++k) { b[k] = {bid - 5 * k, bq}; a[k] = {ask + 5 * k, aq}; }
        quote(tok, bid, ask, bq, aq, at_ms);
        e->on_book(tok, 5, b, a, t0 + at_ms * kMs);
    }
    void print(std::uint32_t tok, std::int64_t px, std::int64_t at_ms, std::int64_t qty) {
        vol += qty;
        PricePayload p;
        p.token = tok; p.last_paise = px; p.flags |= kPriceHasVolume; p.volume = vol; p.exchange_ts_ns = t0 + at_ms * kMs;
        e->on_trade(p, p.exchange_ts_ns, p.exchange_ts_ns);
    }
    [[nodiscard]] const LivePosition* pos(std::uint32_t tok) const { return e->book().position(m->name(), tok); }
    std::vector<std::string> log;
    /// Whether the decision log (every note so far) holds `s`.
    [[nodiscard]] bool noted(const std::string& s) {
        for (auto& d : e->take_decisions()) log.push_back(d.text);
        for (const auto& t : log) if (t.find(s) != std::string::npos) return true;
        return false;
    }
    /// Balanced option books, then a future book with `bq` bids against `aq` asks.
    void setup(std::int64_t bq, std::int64_t aq, std::int64_t from_ms = 100) {
        for (std::int64_t t = from_ms; t <= from_ms + 5'000; t += 1'000)
            for (const char* s : {"NIFTY26OCT23900PE", "NIFTY26OCT24100CE"}) book(token_of(s), 5000, 5010, 500, 700, t);
        for (std::int64_t t = from_ms + 5'500; t <= from_ms + 8'000; t += 500) book(kFut, 2400000, 2400005, bq, aq, t);
    }
};

[[nodiscard]] bool no_long_option(const LiveEngine& e) {
    for (const auto& f : e.book().fills())
        if (f.role == LiveFillRole::Open && f.side > 0) return false;
    return true;
}

} // namespace

int main() {
    {
        Rig g;
        const std::uint32_t pe = g.token_of("NIFTY26OCT23900PE"), ce = g.token_of("NIFTY26OCT24100CE");
        g.setup(1000, 1000);
        const LivePosition* p = g.pos(pe);
        const LivePosition* c = g.pos(ce);
        check(p != nullptr && p->side < 0 && !p->filled(), "a balanced future: the 23900 put (two strikes out) is offered at its ask");
        check(c != nullptr && c->side < 0 && !c->filled(), "and so is the 24100 call");
        check(p != nullptr && p->why_in.find("edge") != std::string::npos && p->why_in.find("future OBI5") != std::string::npos,
              "the quote's reason carries the skew, the costs and the edge");

        // Bids pile up on the future: the call is the leg about to be lifted.
        for (std::int64_t t = 8'500; t <= 10'000; t += 500) g.book(kFut, 2400000, 2400005, 3000, 800, t);
        check(g.pos(ce) == nullptr, "bids heavy on the future: the call quote is pulled");
        check(g.noted("PULL NIFTY26OCT24100CE") && g.noted("buyers lean on the book"), "the pull is logged with its reason");
        check(g.pos(pe) != nullptr, "the put quote stays");

        // Buyers sweep through the put's ask: it is sold at its own price.
        g.print(pe, 5000, 10'100, 1);   // a sale at the bid: the volume starts counting
        g.print(pe, 5020, 10'200, 1'000);
        p = g.pos(pe);
        check(p != nullptr && p->filled() && p->entry == 50.10, "the resting put fills through the queue at the ask");
        g.book(pe, 4400, 4410, 500, 700, 11'000);   // premium down 12 %: past the 10 % target
        bool won = false, at_target = false;
        for (const auto& t : g.e->book().trades()) {
            if (t.model != g.m->name() || t.token != pe) continue;
            won = won || t.net > 0.0;
            at_target = at_target || (t.exit == 44.10 && t.why_out.find("bracket: target") != std::string::npos);
        }
        check(at_target, "the bracket buys it back at the target, at the ask now (44.10)");
        check(won, "a winning round trip net of costs");
        p = g.pos(pe);
        check(p != nullptr && !p->filled() && p->why_in.find("at the ask 44.10") != std::string::npos,
              "and the put is offered again at the new ask (the skew still allows it)");
        check(no_long_option(*g.e), "nothing was bought to open");
    }
    {
        Rig g;
        const std::uint32_t pe = g.token_of("NIFTY26OCT23900PE"), ce = g.token_of("NIFTY26OCT24100CE");
        g.setup(800, 3000);   // asks heavy from the start
        check(g.pos(pe) == nullptr && g.noted("no put quote") && g.noted("sellers lean on the book"),
              "asks heavy on the future: no put is offered, and the log says why");
        check(g.pos(ce) != nullptr && g.pos(ce)->side < 0, "the call is offered");
        g.print(ce, 5000, 9'000, 1);
        g.print(ce, 5020, 9'100, 1'000);
        check(g.pos(ce) != nullptr && g.pos(ce)->filled(), "and sold when buyers sweep it");
        g.book(ce, 6300, 6310, 500, 700, 10'000);   // premium up 26 %
        check(g.pos(ce) == nullptr, "premium up past 25 %: the bracket stops it");
        bool lost = false;
        for (const auto& t : g.e->book().trades()) lost = lost || (t.model == g.m->name() && t.token == ce && t.net < 0.0);
        check(lost, "a losing round trip, booked");
        // The same picture inside the one-minute cooldown: the call is not offered again.
        for (std::int64_t t = 11'000; t <= 14'000; t += 1'000) g.book(ce, 5000, 5010, 500, 700, t);
        for (std::int64_t t = 14'500; t <= 16'000; t += 500) g.book(kFut, 2400000, 2400005, 800, 3000, t);
        check(g.pos(ce) == nullptr, "after a loss that leg cools down");
        check(no_long_option(*g.e), "nothing was bought to open");
    }
    {
        Rig g;
        const std::uint32_t pe = g.token_of("NIFTY26OCT23900PE");
        for (std::int64_t t = 100; t <= 5'100; t += 1'000) g.book(pe, 5000, 5010, 2000, 300, t);   // the put itself is bid
        for (const char* s : {"NIFTY26OCT24100CE"})
            for (std::int64_t t = 100; t <= 5'100; t += 1'000) g.book(g.token_of(s), 5000, 5010, 500, 700, t);
        for (std::int64_t t = 5'600; t <= 8'100; t += 500) g.book(kFut, 2400000, 2400005, 1000, 1000, t);
        check(g.pos(pe) == nullptr && g.noted("its own book is bid"), "a leg whose own book is bid is not offered");
    }
    {
        ObiQuoterRules r;
        r.target_pct = 0.03;   // 1.50 a unit cannot pay 2.00 of costs
        Rig g(r);
        g.setup(1000, 1000);
        check(g.pos(g.token_of("NIFTY26OCT23900PE")) == nullptr && g.noted("does not cover costs"),
              "a target that does not pay the round trip is not quoted");
    }
    {
        Rig g({}, 9);   // 09:00-09:15 is not even the session; 09:15-09:20 is the opening
        g.setup(1000, 1000);
        check(g.e->book().flat(g.m->name()), "nothing is offered before 09:20");
    }
    {
        ObiQuoterRules r;
        check(r.pull_obi == 0.30 && r.strikes_otm == 2 && r.target_pct == 0.10 && r.stop_pct == 0.25 && r.max_daily_loss == 5'000.0,
              "the defaults: pull at OBI5 0.30, two strikes out, 10 % target, 25 % stop, Rs 5,000 a day");
    }
    std::printf("%s\n", failures == 0 ? "all obi quoter checks passed" : "obi quoter checks did not pass");
    return failures == 0 ? 0 : 1;
}


