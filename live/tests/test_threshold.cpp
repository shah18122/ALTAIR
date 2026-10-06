// The OHL threshold strategy (live/threshold.hpp) on opens whose answers are
// known: open = high sells with the stop at high + 0.5 %; open = low buys
// with the stop at low - 0.5 %; an open that is both or neither is no signal;
// the stop exits at the touch; after 1.5 % of profit a 0.25 % trailing stop
// follows the best price and exits on the pull-back.
//
// No check description here may contain the substring "F" "AIL" joined.

#include <live/threshold.hpp>

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

LiveInstrument fut(std::uint32_t tok, const char* sym, const char* under) {
    LiveInstrument i;
    i.token = tok; i.symbol = sym; i.fyers = std::string("NSE:") + sym; i.underlying = under;
    i.kind = LiveKind::Future; i.lot = 75; i.tick = 0.05; i.expiry_day = parse_day("2026-10-27");
    return i;
}

struct Feed {
    LiveEngine& e;
    void quote(std::uint32_t tok, std::int64_t bid, std::int64_t ask, std::int64_t at, std::int64_t open = 0,
               std::int64_t high = 0, std::int64_t low = 0) {
        QuotePayload q;
        q.token = tok;
        q.flags = kQuoteHasTop | (open > 0 ? kQuoteHasOhlc : 0);
        q.bid = bid; q.ask = ask; q.bid_qty = 750; q.ask_qty = 750;
        q.open = open; q.high = high; q.low = low;
        e.on_quote(q, at);
    }
    void trade(std::uint32_t tok, std::int64_t px, std::int64_t at) {
        PricePayload p;
        p.token = tok; p.last_paise = px; p.last_qty = 75; p.exchange_ts_ns = at;
        e.on_trade(p, at);
    }
};

} // namespace

int main() {
    std::printf("OHL threshold strategy\n");
    const std::vector<LiveInstrument> u{fut(1, "NIFTY26OCTFUT", "NIFTY"), fut(2, "BANKNIFTY26OCTFUT", "BANKNIFTY"),
                                        fut(3, "SBIN26OCTFUT", "SBIN"), fut(4, "INFY26OCTFUT", "INFY")};
    LiveExecPolicy instant;
    instant.latency_ns = 0;
    LiveEngine e(u, nullptr, instant);
    auto owned = std::make_unique<LiveOhlModel>();
    LiveOhlModel* ohl = owned.get();
    e.add_model(std::move(owned));
    Feed f{e};
    const std::int64_t day = parse_day("2026-10-05");
    const std::int64_t open_ns = (day * 86400 + 9 * 3600 + 15 * 60 - 19800) * kSec;   // 09:15:00 IST

    // The first prints of the session; the exchange's OHLC rides on the quotes.
    for (std::uint32_t t = 1; t <= 4; ++t) f.trade(t, 2400000, open_ns + 100'000'000);
    // NIFTY: open 24000 = high, traded down to 23980 -> SELL.
    f.quote(1, 2398000, 2398100, open_ns + 200'000'000, 2400000, 2400000, 2398000);
    // BANKNIFTY: open 24000 = low, traded up to 24030 -> BUY.
    f.quote(2, 2402900, 2403000, open_ns + 300'000'000, 2400000, 2403000, 2400000);
    // SBIN: open between the high and the low -> nothing.
    f.quote(3, 2400000, 2400100, open_ns + 400'000'000, 2400000, 2401000, 2399000);
    // INFY: nothing traded away from the open -> nothing.
    f.quote(4, 2400000, 2400100, open_ns + 500'000'000, 2400000, 2400000, 2400000);
    check(e.book().flat("OHL"), "nothing is decided inside the first second");
    // 09:15:01.2: the check.
    f.quote(1, 2398000, 2398100, open_ns + 1'200'000'000, 2400000, 2400000, 2398000);
    const LivePosition* n = e.book().position("OHL", 1);
    const LivePosition* b = e.book().position("OHL", 2);
    check(n != nullptr && n->side < 0 && n->qty == 75 && std::fabs(n->entry - 23980.0) < 1e-9,
          "open = high: NIFTY is sold, one lot, at the bid");
    check(b != nullptr && b->side > 0 && std::fabs(b->entry - 24030.0) < 1e-9, "open = low: BANKNIFTY is bought at the ask");
    check(e.book().position("OHL", 3) == nullptr && e.book().position("OHL", 4) == nullptr,
          "an open between the high and the low, or equal to both, is no signal");
    const auto& legs = ohl->legs();
    check(std::fabs(legs.at(1).stop - 24000.0 * 1.005) < 1e-6 && std::fabs(legs.at(2).stop - 24000.0 * 0.995) < 1e-6,
          "stops: the high + 0.5 % for the sell, the low - 0.5 % for the buy");

    // NIFTY rallies through its stop: bought back at the ask.
    f.quote(1, 2411900, 2412100, open_ns + 60 * kSec);
    check(e.book().position("OHL", 1) == nullptr, "the sell is stopped out once the ask reaches high + 0.5 %");
    bool stop_text = false;
    for (const auto& t : e.book().trades()) stop_text = stop_text || (t.symbol == "NIFTY26OCTFUT" && t.why_out.find("stop-loss") != std::string::npos);
    check(stop_text, "and the round trip says stop-loss");

    // BANKNIFTY: +1.5 % starts the trail; a 0.25 % pull-back from the best exits.
    const double entry = 24030.0;
    const std::int64_t up = std::llround(entry * 1.02 * 100.0);   // +2 %: bid 24510.60
    f.quote(2, up, up + 10, open_ns + 120 * kSec);
    check(e.book().position("OHL", 2) != nullptr && ohl->legs().at(2).trailing, "two per cent up: still held, the trail is on");
    const double trail = static_cast<double>(up) / 100.0 * (1.0 - 0.0025);
    check(std::fabs(ohl->legs().at(2).stop - trail) < 1e-6, "the trail sits 0.25 % under the best bid");
    const std::int64_t back = std::llround(static_cast<double>(up) * 0.9990);   // a 0.10 % dip: inside the trail
    f.quote(2, back, back + 10, open_ns + 121 * kSec);
    check(e.book().position("OHL", 2) != nullptr, "a 0.10 % dip does not take it out");
    const std::int64_t out = std::llround(static_cast<double>(up) * 0.9970);   // 0.30 % under the best
    f.quote(2, out, out + 10, open_ns + 122 * kSec);
    check(e.book().position("OHL", 2) == nullptr, "a 0.30 % pull-back from the best exits on the trail");
    double gross = 0.0;
    for (const auto& t : e.book().trades()) if (t.symbol == "BANKNIFTY26OCTFUT") gross = t.gross;
    check(gross > 0.0, "and the trail locked the profit in");

    std::printf("%s\n", failures == 0 ? "all OHL checks passed" : "OHL checks did not pass");
    return failures == 0 ? 0 : 1;
}
