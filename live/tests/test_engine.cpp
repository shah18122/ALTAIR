// Tests for live/engine.hpp, live/paper.hpp, live/bars.hpp and live/models.hpp:
// a simulated session from 09:15 to 15:30, fed tick by tick through the
// engine, with the strangle, pairs and stat-arb models running.
//
// No check description here may contain the substring "F" "AIL" joined.

#include <live/models.hpp>
#include <live/sim.hpp>

#include <cstdio>
#include <string>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) { ++failures; }
}

using namespace altair;
using namespace altair::live;

std::vector<LiveInstrument> universe(std::int64_t today) {
    std::vector<LiveInstrument> u;
    const auto add = [&u](std::uint32_t tok, const std::string& sym, const char* under, LiveKind k, double strike,
                          std::int64_t lot, std::int64_t expiry, bool depth, double tick) {
        LiveInstrument i;
        i.token = tok; i.symbol = sym; i.fyers = "NSE:" + sym; i.underlying = under; i.kind = k; i.strike = strike;
        i.lot = lot; i.expiry_day = expiry; i.depth = depth; i.tick = tick;
        u.push_back(i);
    };
    const std::int64_t wk = today + 5, mo = parse_day("2026-10-27"), nx = parse_day("2026-11-24");
    add(kLiveNiftyToken, "NIFTY 50", "NIFTY", LiveKind::Index, 0, 1, 0, false, 0.05);
    add(kLiveBankNiftyToken, "NIFTY BANK", "BANKNIFTY", LiveKind::Index, 0, 1, 0, false, 0.05);
    add(kLiveVixToken, "INDIA VIX", "INDIAVIX", LiveKind::Index, 0, 1, 0, false, 0.05);
    add(10, "NIFTYEXPFUT", "NIFTY", LiveKind::Future, 0, 65, today, true, 0.1);   // expires today: the roll case
    add(11, "NIFTY26OCTFUT", "NIFTY", LiveKind::Future, 0, 65, mo, true, 0.1);
    add(12, "NIFTY26NOVFUT", "NIFTY", LiveKind::Future, 0, 65, nx, true, 0.1);
    add(21, "BANKNIFTY26OCTFUT", "BANKNIFTY", LiveKind::Future, 0, 30, mo, true, 0.2);
    std::uint32_t tok = 1000;
    for (int k = 22800; k <= 25200; k += 100) {
        add(++tok, "NIFTYW" + std::to_string(k) + "CE", "NIFTY", LiveKind::Call, k, 65, wk, false, 0.05);
        add(++tok, "NIFTYW" + std::to_string(k) + "PE", "NIFTY", LiveKind::Put, k, 65, wk, false, 0.05);
    }
    return u;
}

}  // namespace

int main() {
    std::printf("live engine\n");
    const std::int64_t today = parse_day("2026-10-01");
    const auto u = universe(today);

    // Expenses: 5 bp of turnover, so net = gross - expenses is checkable.
    LiveEngine e(u, [](const LiveInstrument&, bool, double qty, double px, std::int64_t) { return 0.0005 * qty * px; });
    LiveVolInputs vol{"NIFTY", 0.008, 0.75, 24000.0};
    e.add_model(std::make_unique<LiveVolBandModel>(std::vector<LiveVolInputs>{vol}, 0.81, 0.02));
    e.add_model(std::make_unique<LiveStrangleModel>(vol, 0.0));
    LivePairsInputs pf;
    pf.ok = true; pf.alpha = 0.0; pf.beta = 1.0; pf.sd = 0.01; pf.half_life = 10.0; pf.days = 250;
    pf.mu = std::log(54000.0 / 24000.0) - 0.03;   // the spread opens three sd rich
    e.add_model(std::make_unique<LivePairsModel>(pf));
    LiveStatArbInputs none;
    none.why = "no data/pairs history in this test";
    e.add_model(std::make_unique<LiveStatArbModel>(none));

    LiveSimSeeds seeds;
    seeds.nifty = 24000; seeds.banknifty = 54000; seeds.vix = 13;
    const std::int64_t open_ns = (today * 86400 + 9 * 3600 + 15 * 60 - 19800) * 1'000'000'000LL;
    LiveSim sim(u, seeds, 7, open_ns);
    sim.board([&](const LiveInstrument&, const LiveSimEvent& ev) { e.on_quote(ev.quote, open_ns); });

    // A positional holding in a future that expires today: rolled at 15:15.
    bool roll_opened = false, sq_seen = false, strangle_seen = false, pairs_seen = false;
    std::size_t strangle_legs = 0;
    std::string why_strangle;
    for (int step = 0; step < 6 * 3600 + 15 * 60; ++step) {   // one-second steps to 15:30
        sim.step(1'000'000'000, [&](const LiveInstrument&, const LiveSimEvent& ev) {
            e.on_quote(ev.quote, sim.now_ns());
            if (ev.trade) e.on_trade(ev.price, ev.price.exchange_ts_ns);
        });
        e.advance(sim.now_ns());   // the clock moves with the session even when nothing traded
        const int mod = live_minute_of_day(live_ist_minute_index(e.clock_ns()));
        if (!roll_opened && mod >= 9 * 60 + 30) {
            roll_opened = e.book().open("roll test", u[3], 1, 1, e.clock_ns(), "test", true);
        }
        if (!strangle_seen && mod >= 9 * 60 + 21) {
            strangle_seen = true;
            for (const auto& p : e.book().positions()) strangle_legs += p.model == "Strangle 80% NIFTY" ? 1u : 0u;
            for (const auto& p : e.book().positions())
                if (p.model == "Strangle 80% NIFTY") why_strangle = p.why_in;
        }
        if (!pairs_seen && mod >= 15 * 60 + 16) {
            pairs_seen = true;
            std::size_t legs = 0;
            for (const auto& p : e.book().positions()) legs += p.model == "Pairs BANKNIFTY/NIFTY" ? 1u : 0u;
            check(legs == 2, "the pair opened both legs at 15:15");
            const LivePosition* b = e.book().position("Pairs BANKNIFTY/NIFTY", 21);
            check(b != nullptr && b->side == -1 && b->carry, "rich spread: BANKNIFTY sold, carried overnight");
        }
        if (!sq_seen && mod >= 15 * 60 + 21) {
            sq_seen = true;
            bool any_intraday = false;
            for (const auto& p : e.book().positions()) any_intraday = any_intraday || !p.carry;
            check(!any_intraday, "every intraday position is flat after 15:20");
        }
    }
    std::printf("    strangle entry: %s\n", why_strangle.c_str());
    check(strangle_legs == 2, "the strangle sold both band edges at 09:20");
    check(roll_opened, "a positional test holding opened");
    check(e.book().position("roll test", 10) == nullptr, "a carried future that expires today is rolled out at 15:15");

    const auto& fills = e.book().fills();
    bool sides_ok = !fills.empty();
    for (const auto& f : fills) sides_ok = sides_ok && f.at_quote;
    check(sides_ok, "every fill dealt at a quote, not at the last trade");
    const auto& trades = e.book().trades();
    std::size_t strangle_trades = 0;
    bool net_ok = !trades.empty();
    for (const auto& t : trades) {
        strangle_trades += t.model == "Strangle 80% NIFTY" ? 1u : 0u;
        net_ok = net_ok && std::fabs(t.net - (t.gross - t.expenses)) < 1e-6 && t.expenses > 0.0;
    }
    check(strangle_trades == 2, "both strangle legs were bought back as round trips");
    check(net_ok, "every round trip records gross, expenses and net = gross - expenses");
    for (const auto& t : trades)
        if (t.model == "Strangle 80% NIFTY")
            std::printf("    %s %s %lld @ %.2f -> %.2f  gross %.0f  exp %.0f  net %.0f  (%s)\n", t.side > 0 ? "long" : "short",
                        t.symbol.c_str(), static_cast<long long>(t.qty), t.entry, t.exit, t.gross, t.expenses, t.net,
                        t.why_out.c_str());

    const std::string js = e.state_json("test");
    check(js.find("\"Strangle 80% NIFTY\"") != std::string::npos && js.find("\"source\": \"SIM\"") != std::string::npos
              && js.find("\"Pairs BANKNIFTY/NIFTY\"") != std::string::npos,
          "the state JSON names every model and says SIM");
    check(js.find("No history: no data/pairs history in this test") != std::string::npos,
          "a model without its inputs says why it abstains");

    // The stop: a strangle leg bought back at the first 5-minute close where it doubled.
    {
        LiveEngine s(u, nullptr, LiveExecPolicy{0, 10'000'000'000LL, 60'000'000'000LL});   // no latency: fills at decision
        LiveVolInputs v{"NIFTY", 0.008, 0.75, 24000.0};
        s.add_model(std::make_unique<LiveStrangleModel>(v, 2.0));
        const auto put_q = [&](std::uint32_t tok, std::int64_t bid, std::int64_t ask, std::int64_t ns) {
            QuotePayload q; q.token = tok; q.flags = kQuoteHasTop; q.bid = bid; q.ask = ask; q.bid_qty = 650; q.ask_qty = 650;
            s.on_quote(q, ns);
            PricePayload p; p.token = tok; p.last_paise = (bid + ask) / 2; s.on_trade(p, ns);
        };
        const std::int64_t t0 = (today * 86400 + 9 * 3600 + 15 * 60 - 19800) * 1'000'000'000LL;
        put_q(kLiveNiftyToken, 2400000, 2400000, t0);
        for (const auto& i : u) if (i.kind == LiveKind::Call || i.kind == LiveKind::Put) put_q(i.token, 5000, 5100, t0 + 299'000'000'000LL);
        put_q(kLiveNiftyToken, 2400000, 2400000, t0 + 300'000'000'000LL);   // 09:20: sells
        std::size_t legs = s.book().positions().size();
        check(legs == 2, "stop test: the strangle opened");
        const LivePosition* c = nullptr;
        for (const auto& p : s.book().positions()) if (p.inst.kind == LiveKind::Call) c = s.book().position(p.model, p.inst.token);
        check(c != nullptr && c->entry == 50.0 && c->side == -1, "a sell fills at the bid (50.00), not the mid or the last");
        if (c != nullptr) {
            const std::uint32_t ct = c->inst.token;
            put_q(ct, 10500, 10600, t0 + 301'000'000'000LL);                       // the call's premium has doubled
            for (const auto& p : s.book().positions())
                if (p.inst.token != ct) put_q(p.inst.token, 5000, 5100, t0 + 599'000'000'000LL);
            put_q(ct, 10500, 10600, t0 + 599'000'000'000LL);
            put_q(kLiveNiftyToken, 2400000, 2400000, t0 + 600'000'000'000LL);      // 09:25 close
            check(s.book().position("Strangle 80% NIFTY stop2x", ct) == nullptr, "a doubled leg is bought back at the 5-minute close");
            check(s.book().positions().size() == 1, "and the other leg stays sold");
            const auto& tr = s.book().trades();
            check(!tr.empty() && tr.back().exit == 106.0 && tr.back().why_out.find("stop") != std::string::npos,
                  "bought back at the ask, and the reason says stop");
            check(!tr.empty() && !std::isfinite(tr.back().expenses) && !std::isfinite(tr.back().net),
                  "with no cost function, expenses are unpriced (NaN) and so is net -- never zero");
        }
    }

    // A restart after 09:20: the strangle takes up the legs resumed from the
    // journal, and its stop still watches them.
    {
        const auto session = [&](LiveEngine& x, std::uint32_t tok, std::int64_t bid, std::int64_t ask, std::int64_t ns) {
            QuotePayload q; q.token = tok; q.flags = kQuoteHasTop; q.bid = bid; q.ask = ask; q.bid_qty = 650; q.ask_qty = 650;
            x.on_quote(q, ns);
            PricePayload p; p.token = tok; p.last_paise = (bid + ask) / 2; x.on_trade(p, ns);
        };
        const LiveExecPolicy pol{0, 10'000'000'000LL, 60'000'000'000LL};
        const LiveVolInputs v{"NIFTY", 0.008, 0.75, 24000.0};
        const std::int64_t t0 = (today * 86400 + 9 * 3600 + 15 * 60 - 19800) * 1'000'000'000LL;
        LiveEngine first(u, nullptr, pol);
        first.add_model(std::make_unique<LiveStrangleModel>(v, 2.0));
        session(first, kLiveNiftyToken, 2400000, 2400000, t0);
        for (const auto& i : u) if (i.kind == LiveKind::Call || i.kind == LiveKind::Put) session(first, i.token, 5000, 5100, t0 + 299'000'000'000LL);
        session(first, kLiveNiftyToken, 2400000, 2400000, t0 + 300'000'000'000LL);   // 09:20: sells
        const auto held = first.book().held();
        check(held.size() == 2, "restart test: the first session sold the strangle");

        LiveEngine again(u, nullptr, pol);                                              // the restart, at 09:21
        again.add_model(std::make_unique<LiveStrangleModel>(v, 2.0));
        for (const auto& p : held) again.book().restore(p);
        std::uint32_t ct = 0;
        for (const auto& p : held) if (p.inst.kind == LiveKind::Call) ct = p.inst.token;
        for (const auto& p : held) session(again, p.inst.token, 5000, 5100, t0 + 360'000'000'000LL);
        session(again, kLiveNiftyToken, 2400000, 2400000, t0 + 360'000'000'000LL);
        session(again, kLiveNiftyToken, 2400000, 2400000, t0 + 420'000'000'000LL);       // 09:22 close: the model's first minute
        bool resumed = false, missed = false;
        for (const auto& d : again.take_decisions()) {
            resumed = resumed || d.text.find("Resumed at") != std::string::npos;
            missed = missed || d.text.find("Missed today") != std::string::npos;
        }
        check(resumed && !missed, "the restarted model says it resumed its legs, not that it missed the day");
        session(again, ct, 10500, 10600, t0 + 590'000'000'000LL);                         // the call doubles
        for (const auto& p : held) if (p.inst.token != ct) session(again, p.inst.token, 5000, 5100, t0 + 599'000'000'000LL);
        session(again, ct, 10500, 10600, t0 + 599'000'000'000LL);
        session(again, kLiveNiftyToken, 2400000, 2400000, t0 + 600'000'000'000LL);      // 09:25 close
        check(again.book().position("Strangle 80% NIFTY stop2x", ct) == nullptr && again.book().positions().size() == 1,
              "and its stop buys back a resumed leg that doubled");
    }

    // Bars: one-minute bars on the IST grid, closed by the clock.
    {
        LiveBarBuilder b;
        int closed = 0;
        const std::int64_t t0 = (today * 86400 + 9 * 3600 + 15 * 60 - 19800) * 1'000'000'000LL;
        b.feed(1, 100.0, t0 + 1'000'000'000, 10, [&](std::uint32_t, const LiveBar&) { ++closed; });
        b.feed(1, 101.0, t0 + 30'000'000'000, 25, [&](std::uint32_t, const LiveBar&) { ++closed; });
        b.feed(1, 99.5, t0 + 59'000'000'000, 30, [&](std::uint32_t, const LiveBar&) { ++closed; });
        b.advance(t0 + 61'000'000'000, [&](std::uint32_t, const LiveBar&) { ++closed; });
        check(closed == 1 && b.bars(1).size() == 1, "a bar closes when the clock leaves its minute, traded or not");
        const LiveBar& x = b.bars(1).front();
        check(x.o == 100.0 && x.h == 101.0 && x.l == 99.5 && x.c == 99.5 && x.volume == 20 && x.start_of_day() == 555,
              "OHLC, volume from the cumulative count, and the 09:15 label");
        std::vector<LiveBar> one;
        for (int k = 0; k < 7; ++k) one.push_back(LiveBar{x.minute + k, 1.0 + k, 2.0 + k, 0.5 + k, 1.5 + k, 1, 1});
        const auto five = live_aggregate(one, 5);
        check(five.size() == 2 && five[0].o == 1.0 && five[0].c == 5.5 && five[0].h == 6.0 && five[1].start_of_day() == 560,
              "five-minute bars on the 09:15 grid");
    }

    std::printf("%s\n", failures == 0 ? "all live engine checks passed" : "live engine checks did not pass");
    return failures == 0 ? 0 : 1;
}
