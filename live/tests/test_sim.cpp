// Tests for live/sim.hpp: a simulated market must be plainly marked as one,
// internally consistent (a terminal shows OHLC, spreads and books, and a
// broken one would look like a broken terminal), and repeatable.
//
// No check description here may contain the substring "F" "AIL" joined.

#include <live/sim.hpp>

#include <cstdio>
#include <cmath>
#include <map>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) { ++failures; }
}

std::vector<altair::live::LiveInstrument> universe() {
    using namespace altair::live;
    std::vector<LiveInstrument> u;
    const auto add = [&u](std::uint32_t tok, const char* sym, const char* under, LiveKind k, double strike,
                          std::int64_t lot, bool depth) {
        LiveInstrument i;
        i.token = tok; i.symbol = sym; i.underlying = under; i.kind = k; i.strike = strike; i.lot = lot;
        i.tick = k == LiveKind::Index ? 0.05 : (k == LiveKind::Future ? 0.1 : 0.05);
        i.expiry_day = k == LiveKind::Index || k == LiveKind::Equity ? 0 : parse_day("2026-10-06");
        i.depth = depth;
        u.push_back(i);
    };
    add(kLiveNiftyToken, "NIFTY 50", "NIFTY", LiveKind::Index, 0, 1, false);
    add(kLiveBankNiftyToken, "NIFTY BANK", "BANKNIFTY", LiveKind::Index, 0, 1, false);
    add(kLiveVixToken, "INDIA VIX", "INDIAVIX", LiveKind::Index, 0, 1, false);
    add(1, "NIFTY26OCTFUT", "NIFTY", LiveKind::Future, 0, 65, true);
    add(2, "NIFTY26O0624000CE", "NIFTY", LiveKind::Call, 24000, 65, true);
    add(3, "NIFTY26O0624000PE", "NIFTY", LiveKind::Put, 24000, 65, true);
    add(4, "SBIN", "SBIN", LiveKind::Equity, 0, 1, true);
    return u;
}

struct Seen {
    int trades = 0, quotes = 0, books = 0;
    bool sim_flag = true, ohlc_ok = true, spread_ok = true, book_ok = true, index_clean = true;
    std::int64_t last = 0;
};

double worst_parity = 0.0;
int parity_steps = 0;

std::map<std::uint32_t, Seen> run(std::uint64_t seed, int steps, std::vector<std::int64_t>* trail) {
    using namespace altair;
    using namespace altair::live;
    LiveSimSeeds seeds;
    seeds.nifty = 24000; seeds.banknifty = 54000; seeds.vix = 13;
    seeds.stocks["SBIN"] = 800;
    // 2026-10-01 09:15 IST
    const std::int64_t start = (parse_day("2026-10-01") * 86400 + 3 * 3600 + 45 * 60) * 1'000'000'000LL;
    LiveSim sim(universe(), seeds, seed, start);
    std::map<std::uint32_t, Seen> seen;
    for (int k = 0; k < steps; ++k) {
        std::map<std::uint32_t, double> mid;
        sim.step(100'000'000, [&](const LiveInstrument& in, const LiveSimEvent& ev) {
            if (ev.quote.has(kQuoteHasTop)) mid[in.token] = 0.5 * static_cast<double>(ev.quote.bid + ev.quote.ask) / 100.0;
            auto& s = seen[in.token];
            ++s.quotes;
            s.sim_flag = s.sim_flag && ev.quote.has(kQuoteSimulated);
            if (ev.trade) {
                ++s.trades;
                s.sim_flag = s.sim_flag && ev.price.has(kPriceSimulated);
                s.last = ev.price.last_paise;
                if (trail && in.token == 4) trail->push_back(ev.price.last_paise);
            }
            if (ev.quote.has(kQuoteHasOhlc))
                s.ohlc_ok = s.ohlc_ok && ev.quote.low <= ev.quote.high && ev.quote.low > 0
                         && (!ev.trade || (ev.price.last_paise >= ev.quote.low && ev.price.last_paise <= ev.quote.high));
            if (ev.quote.has(kQuoteHasTop)) s.spread_ok = s.spread_ok && ev.quote.bid > 0 && ev.quote.bid < ev.quote.ask;
            if (ev.book) {
                ++s.books;
                for (std::size_t k2 = 1; k2 < kMaxDepthLevels; ++k2)
                    s.book_ok = s.book_ok && ev.bids[k2].price_paise <= ev.bids[k2 - 1].price_paise
                             && ev.asks[k2].price_paise > ev.asks[k2 - 1].price_paise && ev.bids[0].price_paise < ev.asks[0].price_paise;
            }
            if (in.kind == LiveKind::Index)
                s.index_clean = s.index_clean && !ev.book && !ev.quote.has(kQuoteHasTop) && !ev.price.has(kPriceHasVolume);
        });
        if (mid.count(1) && mid.count(2) && mid.count(3)) {
            // Put-call parity at one instant: C - P = (F - K) e^(-rT); the
            // quotes are rounded to the tick and the spread is 0.2 %, so a few
            // rupees is the tolerance, not tens.
            const double gap = std::fabs((mid[2] - mid[3]) - (mid[1] - 24000.0) * std::exp(-0.065 * 5.0 / 365.0));
            worst_parity = gap > worst_parity ? gap : worst_parity;
            ++parity_steps;
        }
    }
    return seen;
}

}  // namespace

int main() {
    using namespace altair::live;
    std::printf("live sim\n");
    std::vector<std::int64_t> a, b;
    const auto seen = run(42, 3000, &a);   // five simulated minutes
    (void)run(42, 3000, &b);
    check(!a.empty() && a == b, "the same seed gives the same tape");
    std::vector<std::int64_t> c;
    (void)run(43, 3000, &c);
    check(a != c, "a different seed gives a different tape");

    bool all_sim = true, ohlc = true, spread = true, book = true, idx = true;
    for (const auto& [tok, s] : seen) {
        all_sim = all_sim && s.sim_flag; ohlc = ohlc && s.ohlc_ok; spread = spread && s.spread_ok;
        book = book && s.book_ok; idx = idx && s.index_clean;
    }
    check(all_sim, "every frame is marked simulated");
    check(ohlc, "low <= last <= high on every quote");
    check(spread, "bid is below ask on every quote");
    check(book, "book levels are ordered and uncrossed");
    check(idx, "indices carry no book, no bid/ask and no volume");
    const auto n = seen.find(kLiveNiftyToken);
    check(n != seen.end() && n->second.trades >= 290 && n->second.trades <= 301, "the index prints once a second");
    const auto fut = seen.find(1);
    check(fut != seen.end() && fut->second.trades > 1500, "the future prints on most steps");
    const auto call = seen.find(2), put = seen.find(3);
    check(call != seen.end() && put != seen.end() && call->second.trades > 0 && put->second.trades > 0,
          "at-the-money options print");
    const auto vix = seen.find(kLiveVixToken);
    check(vix != seen.end() && vix->second.last > 500 && vix->second.last < 3000, "VIX stays in a sane range");
    std::printf("    parity checked on %d steps, worst gap %.2f rupees\n", parity_steps, worst_parity);
    check(parity_steps > 100 && worst_parity < 5.0, "calls, puts and the future are priced off one forward");
    std::printf("%s\n", failures == 0 ? "all live sim checks passed" : "live sim checks did not pass");
    return failures == 0 ? 0 : 1;
}
