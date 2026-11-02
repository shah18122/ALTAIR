// The information diffusion engine and trader (live/diffusion.hpp), in an
// engine, on a tape whose answer is known: the index, the options and the
// future answer their leader 300 ms late. The hub must find the lead and pass
// the gate; after a leader jump the option-sell trader SELLS the put whose bid
// is still stale (never buys the stale call), the futures trader buys the
// future behind its bank stock, each with its reasons in the log and the
// trades file, and each is out when the follower catches up. A follower with
// no lead (lag under the jitter floor) is never traded.
//
// No check description here may contain the substring "F" "AIL" joined.

#include <live/diffusion.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
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
constexpr std::uint32_t kIdx = 1, kFut = 2, kStock = 3, kCe = 10, kPe = 11, kCeLo = 12, kPeLo = 13, kCeHi = 14, kPeHi = 15;

struct Lcg {
    std::uint64_t s;
    explicit Lcg(std::uint64_t seed) : s(seed) {}
    std::uint64_t next() { s = s * 6364136223846793005ULL + 1442695040888963407ULL; return s >> 33; }
};

LiveInstrument inst(std::uint32_t tok, const std::string& sym, const std::string& under, LiveKind k, double strike, std::int64_t expiry,
                    std::int64_t lot) {
    LiveInstrument i;
    i.token = tok; i.symbol = sym; i.fyers = "NSE:" + sym; i.underlying = under; i.kind = k;
    i.lot = lot; i.tick = 0.05; i.strike = strike; i.expiry_day = expiry;
    return i;
}

struct Rig {
    std::int64_t t0 = 0;
    std::vector<LiveInstrument> u;
    std::unique_ptr<LiveEngine> e;
    DiffusionHub* hub = nullptr;
    LiveDiffusionTraderModel* m = nullptr;
    std::string log;

    Rig(DiffMode mode, std::int64_t min_lag_ms = 100) {
        const std::int64_t day = parse_day("2026-10-05"), expiry = parse_day("2026-10-27");
        t0 = (day * 86400 + 10 * 3600 - 19800) * 1'000'000'000LL;   // 10:00 IST
        u.push_back(inst(kIdx, "BANKNIFTY", "BANKNIFTY", LiveKind::Index, 0, 0, 1));
        u.push_back(inst(kFut, "BANKNIFTY26OCTFUT", "BANKNIFTY", LiveKind::Future, 0, expiry, 35));
        u.push_back(inst(kStock, "HDFCBANK", "HDFCBANK", LiveKind::Equity, 0, 0, 1));
        u.push_back(inst(kCe, "BANKNIFTY26OCT24000CE", "BANKNIFTY", LiveKind::Call, 24000, expiry, 35));
        u.push_back(inst(kPe, "BANKNIFTY26OCT24000PE", "BANKNIFTY", LiveKind::Put, 24000, expiry, 35));
        u.push_back(inst(kCeLo, "BANKNIFTY26OCT23900CE", "BANKNIFTY", LiveKind::Call, 23900, expiry, 35));
        u.push_back(inst(kPeLo, "BANKNIFTY26OCT23900PE", "BANKNIFTY", LiveKind::Put, 23900, expiry, 35));
        u.push_back(inst(kCeHi, "BANKNIFTY26OCT24100CE", "BANKNIFTY", LiveKind::Call, 24100, expiry, 35));
        u.push_back(inst(kPeHi, "BANKNIFTY26OCT24100PE", "BANKNIFTY", LiveKind::Put, 24100, expiry, 35));
        LiveExecPolicy instant;
        instant.latency_ns = 0;
        const LiveCostFn cost = [](const LiveInstrument&, bool, double qty, double, std::int64_t) { return 0.05 * qty; };
        e = std::make_unique<LiveEngine>(u, cost, instant);
        log = (std::filesystem::temp_directory_path() / ("altair_diffusion_test_" + std::to_string(static_cast<int>(mode)) + "_" + std::to_string(min_lag_ms) + ".csv")).string();
        std::filesystem::remove(log);
        DiffusionHub::Config hc;
        hc.stock_leaders = {{"BANKNIFTY", {"HDFCBANK"}}};
        hc.gate.min_lag_ns = min_lag_ms * kMs;
        auto hh = std::make_unique<DiffusionHub>(hc);
        hub = hh.get();
        e->add_model(std::move(hh));
        DiffusionRules r;
        r.mode = mode;
        r.log_file = log;
        r.option_safety = 2.0;   // the history's one-point wiggles are not worth a trade; a 20-point jump is
        r.safety_pts = 5.0;
        auto mm = std::make_unique<LiveDiffusionTraderModel>(r, hub);
        m = mm.get();
        e->add_model(std::move(mm));
    }
    void quote(std::uint32_t tok, double mid, double half, std::int64_t ns) {
        QuotePayload q;
        q.token = tok; q.flags = kQuoteHasTop;
        q.bid = std::llround((mid - half) * 100.0); q.ask = std::llround((mid + half) * 100.0); q.bid_qty = 100; q.ask_qty = 100;
        e->on_quote(q, ns);
    }
    void print(std::uint32_t tok, double px, std::int64_t ns) {
        PricePayload p;
        p.token = tok; p.last_paise = std::llround(px * 100.0); p.exchange_ts_ns = ns;
        e->on_trade(p, ns, ns);
    }
    /// The future's level drives everything: the put loses 0.5 a point, the call gains it.
    void followers(double f, std::int64_t ns) {
        print(kIdx, f - 50.0, ns);
        quote(kPe, 300.0 - 0.5 * (f - 24000.0), 0.05, ns);
        quote(kCe, 350.0 + 0.5 * (f - 24000.0), 0.05, ns);
    }
    void future(double f, std::int64_t ns) {
        quote(kFut, f, 0.05, ns);
        print(kFut, f, ns);
    }
    /// 300 s in which the future leads the index and the options by 300 ms
    /// (option mode), or HDFCBANK leads the future by 300 ms (futures mode).
    double history(DiffMode mode) {
        Lcg g(5);
        double f = 24000.0;
        std::vector<std::pair<std::int64_t, double>> pending;
        for (std::int64_t k = 0; k < 3000; ++k) {
            const std::int64_t ns = t0 + k * 100 * kMs;
            for (std::size_t i = 0; i < pending.size();)
                if (pending[i].first <= ns) {
                    if (mode == DiffMode::SellOptions) followers(pending[i].second, pending[i].first);
                    else future(pending[i].second, pending[i].first);
                    pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(i));
                } else ++i;
            const auto r = g.next() % 10;
            if (r < 3) f += 1.0; else if (r < 6) f -= 1.0;
            if (f > 24040.0) f -= 2.0;
            if (f < 23960.0) f += 2.0;
            if (mode == DiffMode::SellOptions) future(f, ns);
            else { quote(kStock, f / 15.0, 0.05, ns); print(kStock, f / 15.0, ns); followers(f, ns); }
            pending.push_back({ns + 300 * kMs, f});
        }
        for (const auto& [ns, v] : pending) {
            if (mode == DiffMode::SellOptions) followers(v, ns); else future(v, ns);
        }
        return f;
    }
    [[nodiscard]] const DiffPair* pair(std::uint32_t leader, std::uint32_t follower) const {
        for (const auto& p : hub->pairs()) if (p->leader.token == leader && p->follower.token == follower) return p.get();
        return nullptr;
    }
    [[nodiscard]] bool noted(const std::string& what) {
        for (const auto& d : e->take_decisions()) if (d.text.find(what) != std::string::npos) return true;
        return false;
    }
    [[nodiscard]] std::string file() const {
        std::ifstream i(log);
        return {std::istreambuf_iterator<char>(i), std::istreambuf_iterator<char>()};
    }
};

} // namespace

int main() {
    std::printf("information diffusion, live\n");
    {
        Rig g(DiffMode::SellOptions);
        const double f = g.history(DiffMode::SellOptions);
        const DiffPair* pe = g.pair(kFut, kPe);
        const DiffPair* spot = g.pair(kFut, kIdx);
        check(pe != nullptr && spot != nullptr, "the hub pairs the future with the index and the options near the money");
        check(g.pair(kFut, kCeLo) != nullptr && g.pair(kStock, kFut) != nullptr, "and the bank stock with the future");
        std::string why;
        check(pe != nullptr && diffusion_passes(*pe, g.hub->gate(), &why), "the future leads the put by 300 ms: the pair passes the gate");
        std::printf("    %s\n", why.c_str());
        check(pe != nullptr && pe->st.lead_lag().best_lag_ns == 300 * kMs, "the measured lag is 300 ms");
        check(pe != nullptr && pe->st.lead_lag().beta < 0.0, "a put moves against the future: negative beta");
        check(spot != nullptr && spot->st.discovery().valid && spot->st.discovery().cs_leader > 0.7, "the future carries the index's price discovery");
        check(g.e->book().flat(g.m->name()), "no trade while every follower is in line");

        // The future jumps 20 points; the options have not moved yet.
        const std::int64_t tj = g.t0 + 300'500 * kMs;
        g.future(f + 20.0, tj);
        const LivePosition* p = g.e->book().position(g.m->name(), kPe);
        check(p != nullptr && p->side < 0 && p->filled(), "the stale put's bid is above fair: SOLD at the bid");
        check(g.e->book().position(g.m->name(), kCe) == nullptr, "the stale call is cheap, and is never bought");
        check(p != nullptr && p->why_in.find("BANKNIFTY26OCTFUT moved first") != std::string::npos && p->why_in.find("edge") != std::string::npos,
              "the entry carries its reasons: the leader, the lag, the fair price and the edge");
        check(g.noted("SELL BANKNIFTY26OCT24000PE"), "and the decision is in the log");
        // The put answers 300 ms later.
        g.followers(f + 20.0, tj + 300 * kMs);
        g.future(f + 20.0, tj + 301 * kMs);
        check(g.e->book().flat(g.m->name()), "the put catches up: bought back, the gap closed");
        check(!g.e->book().trades().empty() && g.e->book().trades().back().why_out.find("gap closed") != std::string::npos
                  && g.e->book().trades().back().gross > 0.0,
              "out on the gap, at a profit");
        const std::string csv = g.file();
        check(csv.find("ENTRY,BANKNIFTY26OCT24000PE,-1") != std::string::npos && csv.find(",EXIT,") != std::string::npos,
              "trades.csv holds the entry and the exit");
    }
    {
        Rig g(DiffMode::Futures);
        const double f = g.history(DiffMode::Futures);
        std::string why;
        const DiffPair* sf = g.pair(kStock, kFut);
        check(sf != nullptr && diffusion_passes(*sf, g.hub->gate(), &why), "HDFCBANK leads the future by 300 ms: it passes the gate");
        std::printf("    %s\n", why.c_str());
        const std::int64_t tj = g.t0 + 300'500 * kMs;
        g.quote(kStock, (f + 20.0) / 15.0, 0.05, tj);
        g.print(kStock, (f + 20.0) / 15.0, tj);
        const LivePosition* p = g.e->book().position(g.m->name(), kFut);
        check(p != nullptr && p->side > 0 && p->filled(), "the stock jumped and the future has not: bought the future at the ask");
        g.future(f + 20.0, tj + 300 * kMs);
        check(g.e->book().flat(g.m->name()), "the future catches up: sold, the gap closed");
    }
    {
        // The same tape with a 500 ms jitter floor: a 300 ms lead is not trusted.
        Rig g(DiffMode::SellOptions, 500);
        const double f = g.history(DiffMode::SellOptions);
        std::string why;
        const DiffPair* pe = g.pair(kFut, kPe);
        check(pe != nullptr && !diffusion_passes(*pe, g.hub->gate(), &why) && why.find("jitter") != std::string::npos,
              "a lag under the jitter floor fails the gate, with its reason");
        g.future(f + 20.0, g.t0 + 300'500 * kMs);
        check(g.e->book().flat(g.m->name()), "and nothing is traded on it");
    }
    std::printf("%s (%d)\n", failures == 0 ? "all passed" : "problems", failures);
    return failures == 0 ? 0 : 1;
}
