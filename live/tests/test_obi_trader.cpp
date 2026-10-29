// The order-book imbalance trader (live/obi_trader.hpp), in an engine, on
// books whose answers are known: a long setup with every confirmation is
// entered on the future with its reasons and taken off at the target; the
// same setup without the breakout is not; the option mode SELLS a put on it
// and never buys an option; a stop starts the cooldown; the daily loss halts.
//
// No check description here may contain the substring "F" "AIL" joined.

#include <live/obi_trader.hpp>

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

constexpr std::int64_t kMs = 1'000'000LL;
constexpr std::uint32_t kIdx = 1, kFut = 13;

LiveInstrument inst(std::uint32_t tok, const std::string& sym, LiveKind k, double strike, std::int64_t expiry) {
    LiveInstrument i;
    i.token = tok; i.symbol = sym; i.fyers = "NSE:" + sym; i.underlying = "NIFTY";
    i.kind = k; i.lot = k == LiveKind::Index ? 1 : 75; i.tick = 0.05; i.strike = strike; i.expiry_day = expiry;
    return i;
}

struct Rig {
    std::int64_t t0 = 0, vol = 0, base = 0;   ///< base: ms added to every time below
    std::vector<LiveInstrument> u;
    std::unique_ptr<LiveEngine> e;
    LiveObiTraderModel* m = nullptr;

    explicit Rig(ObiMode mode, ObiTraderRules r = {}) {
        const std::int64_t day = parse_day("2026-10-05"), expiry = parse_day("2026-10-27");
        t0 = (day * 86400 + 10 * 3600 - 19800) * 1'000'000'000LL;   // 10:00 IST
        u.push_back(inst(kIdx, "NIFTY", LiveKind::Index, 0, 0));
        u.push_back(inst(kFut, "NIFTY26OCTFUT", LiveKind::Future, 0, expiry));
        std::uint32_t tok = 100;
        for (int k = 23850; k <= 24150; k += 50) {
            u.push_back(inst(tok++, "NIFTY26OCT" + std::to_string(k) + "CE", LiveKind::Call, k, expiry));
            u.push_back(inst(tok++, "NIFTY26OCT" + std::to_string(k) + "PE", LiveKind::Put, k, expiry));
        }
        LiveExecPolicy instant;
        instant.latency_ns = 0;
        // One rupee a unit a fill: the round trip costs 2 points.
        const LiveCostFn cost = [](const LiveInstrument&, bool, double qty, double, std::int64_t) { return 1.0 * qty; };
        e = std::make_unique<LiveEngine>(u, cost, instant);
        r.mode = mode;
        auto mm = std::make_unique<LiveObiTraderModel>(r);
        m = mm.get();
        e->add_model(std::move(mm));
        quote(kIdx, 2399995, 2400005, 1, 1, 0);
        print(kFut, 2400000, 0);
    }
    [[nodiscard]] std::uint32_t token_of(const std::string& sym) const {
        for (const auto& i : u) if (i.symbol == sym) return i.token;
        return 0;
    }
    void quote(std::uint32_t tok, std::int64_t bid, std::int64_t ask, std::int64_t bq, std::int64_t aq, std::int64_t at_ms) {
        QuotePayload q;
        q.token = tok; q.flags = kQuoteHasTop; q.bid = bid; q.ask = ask; q.bid_qty = bq; q.ask_qty = aq;
        e->on_quote(q, t0 + (base + at_ms) * kMs);
    }
    /// A 5-level book (and its touch as a quote), every level `bq` / `aq`;
    /// `ask0` overrides the best ask's size.
    void book(std::uint32_t tok, std::int64_t bid, std::int64_t ask, std::int64_t bq, std::int64_t aq, std::int64_t at_ms,
              std::int64_t ask0 = -1) {
        LiveLevel b[5], a[5];
        for (int k = 0; k < 5; ++k) { b[k] = {bid - 5 * k, bq}; a[k] = {ask + 5 * k, aq}; }
        if (ask0 >= 0) a[0].qty = ask0;
        quote(tok, bid, ask, bq, a[0].qty, at_ms);
        e->on_book(tok, 5, b, a, t0 + (base + at_ms) * kMs);
    }
    void print(std::uint32_t tok, std::int64_t px, std::int64_t at_ms, std::int64_t qty = 0) {
        vol += qty;
        PricePayload p;
        p.token = tok; p.last_paise = px; p.flags |= kPriceHasVolume; p.volume = vol; p.exchange_ts_ns = t0 + (base + at_ms) * kMs;
        e->on_trade(p, p.exchange_ts_ns, p.exchange_ts_ns);
    }
    /// 30 s of history, then a bid-heavy book with buyers lifting the ask;
    /// `breakout` moves the touch above the short-term high at the end.
    void long_setup(bool breakout) {
        for (std::int64_t t = 500; t <= 10'000; t += 500) book(kFut, 2399500, 2399505, 1000, 1000, t);
        for (std::int64_t t = 10'500; t <= 20'000; t += 500) book(kFut, 2400000, 2400005, 1000, 1000, t);
        for (std::int64_t t = 20'500; t <= 22'000; t += 500) {
            print(kFut, 2400005, t - 100, 300);
            book(kFut, 2400000, 2400005, 3000, 800, t, 500);
        }
        if (breakout) book(kFut, 2400100, 2400105, 3000, 800, 22'500);
        else book(kFut, 2400000, 2400005, 3000, 800, 22'500);
    }
    [[nodiscard]] bool noted(const std::string& what) {
        for (const auto& d : e->take_decisions()) if (d.text.find(what) != std::string::npos) return true;
        return false;
    }
};

} // namespace

int main() {
    std::printf("order-book imbalance trader\n");
    {
        Rig g(ObiMode::Futures);
        g.long_setup(true);
        const LivePosition* p = g.e->book().position(g.m->name(), kFut);
        check(p != nullptr && p->side > 0 && p->filled(), "every confirmation present: long one lot of the future");
        check(p != nullptr && std::fabs(p->entry - 24001.05) < 1e-9, "bought at the ask");
        check(p != nullptr && p->why_in.find("LONG: OBI5 +0.58") != std::string::npos
                  && p->why_in.find("edge") != std::string::npos && p->why_in.find("executed off the ask") != std::string::npos,
              "the entry carries its reasons: OBI, OFI, executed buying, the breakout and the cost check");
        check(g.noted("BUY NIFTY26OCTFUT"), "and the decision is in the log");
        const auto* f = g.m->flow_of(kFut);
        check(f != nullptr && f->signals().deep_used == 5, "the future's engine read the 5-level book");
        g.book(kFut, 2401110, 2401115, 1000, 1000, 23'000);
        check(g.e->book().flat(g.m->name()), "10 points up: out at the target");
        const auto& t = g.e->book().trades().back();
        check(std::fabs(t.gross - (24011.10 - 24001.05) * 75) < 1e-6 && t.why_out.find("target") != std::string::npos,
              "sold at the bid, the reason recorded");
    }
    {
        Rig g(ObiMode::Futures);
        g.long_setup(false);
        check(g.e->book().flat(g.m->name()), "the same flow without the breakout is not entered");
        check(g.noted("has not broken the short-term high"), "and why is recorded");
    }
    {
        Rig g(ObiMode::SellOptions);
        const std::uint32_t pe = g.token_of("NIFTY26OCT23950PE"), ce = g.token_of("NIFTY26OCT24050CE");
        for (std::int64_t t = 100; t <= 20'000; t += 1'000) {
            g.book(pe, 5000, 5010, 500, 700, t);
            g.book(ce, 5000, 5010, 500, 700, t);
        }
        g.long_setup(true);
        const LivePosition* p = g.e->book().position(g.m->name(), pe);
        check(p != nullptr && p->side < 0, "option mode: the long signal SELLS the 23950 put, one strike out of the money");
        check(g.e->book().position(g.m->name(), kFut) == nullptr, "and holds no future");
        bool bought = false;
        for (const auto& q : g.e->book().positions())
            bought = bought || (q.side > 0 && (q.inst.kind == LiveKind::Call || q.inst.kind == LiveKind::Put));
        check(!bought, "no option is ever bought to open");
        g.book(pe, 4200, 4210, 500, 700, 23'000);   // premium down 16 %
        check(g.e->book().flat(g.m->name()), "premium down past 15 %: bought back at the target");
    }
    {
        ObiTraderRules r;
        Rig g(ObiMode::Futures, r);
        g.long_setup(true);
        check(!g.e->book().flat(g.m->name()), "entered");
        g.book(kFut, 2399400, 2399405, 1000, 1000, 23'000);   // 6.6 points down
        check(g.e->book().flat(g.m->name()), "6 points against: stopped");
        g.base = 60'000;      // the same setup a minute later, inside the two-minute cooldown
        g.long_setup(true);
        check(g.e->book().flat(g.m->name()), "no new entry while cooling down after the loss");
    }
    {
        ObiTraderRules r;
        r.max_daily_loss = 100.0;
        r.cooldown_ns = 0;
        Rig g(ObiMode::Futures, r);
        g.long_setup(true);
        g.book(kFut, 2399400, 2399405, 1000, 1000, 23'000);
        check(g.noted("no entries until tomorrow"), "a loss past the daily limit halts the model for the day");
    }
    std::printf("%s\n", failures == 0 ? "all OBI trader checks passed" : "OBI trader checks did not pass");
    return failures == 0 ? 0 : 1;
}
