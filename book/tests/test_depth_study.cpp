// Tests for book/depth_study.hpp -- order flow imbalance and the depth study.

#include <book/depth_study.hpp>

#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace {

using namespace altair;

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::printf("FAIL: %s\n", what); }
    else { std::printf("  ok  : %s\n", what); }
}

DepthQuote quote(double bid, double ask, double bq, double aq) {
    DepthQuote q;
    for (std::size_t k = 0; k < kDepthStudyLevels; ++k) {
        q.bid[k] = bid - 0.05 * static_cast<double>(k);
        q.ask[k] = ask + 0.05 * static_cast<double>(k);
        q.bid_q[k] = bq;
        q.ask_q[k] = aq;
    }
    return q;
}

void test_ofi_formula() {
    const auto a = quote(100.00, 100.05, 500, 300);
    check(ofi(a, quote(100.05, 100.10, 400, 300)) == 400.0 + 300.0,
          "bid and ask both step up: the new bid size counts, the old ask is gone (+400 +300)");
    check(ofi(a, quote(100.00, 100.05, 700, 300)) == 200.0, "bid size grows at the same price: +200");
    check(ofi(a, quote(100.00, 100.05, 500, 450)) == -150.0, "ask size grows at the same price: -150");
    check(ofi(a, quote(99.95, 100.05, 500, 300)) == -500.0, "the bid steps down: the old bid's 500 is gone");
    check(ofi(a, a) == 0.0, "an unchanged book has no imbalance");
    check(std::isnan(ofi(a, a, 6)), "more levels than the book holds: refused, not summed over five");
    check(std::fabs(l1_imbalance(quote(100, 100.05, 600, 200)) - 0.5) < 1e-12, "level-1 imbalance (600-200)/800 = 0.5");
    check(microprice_offset(quote(100, 100.05, 600, 200)) > 0.0, "a heavy bid pulls the microprice above the mid");
}

void test_reader() {
    DepthRecorderReader r;
    r.line(R"({"recv_ms":1000,"bid_price1":100.0,"ask_price1":100.05,"bid_size1":500,"ask_size1":300,"type":"dp","symbol":"NSE:X-EQ"})");
    r.line(R"({"recv_ms":1100,"bid_size1":700,"type":"dp","symbol":"NSE:X-EQ"})");   // partial: prices kept
    r.line(R"({"ltp":100.02,"type":"sf","symbol":"NSE:X-EQ"})");
    r.line(R"({"bid_price1":1,"ask_price1":2,"bid_size1":1,"ask_size1":1,"type":"dp","symbol":"NSE:Y-EQ"})");   // no stamp
    const auto& ev = r.events();
    check(ev.size() == 1 && ev.at("NSE:X-EQ").size() == 2, "stamped depth lines become events per symbol");
    if (ev.size() == 1 && ev.at("NSE:X-EQ").size() == 2) {
        const auto& e = ev.at("NSE:X-EQ")[1];
        check(e.q.bid[0] == 100.0 && e.q.bid_q[0] == 700.0 && e.ofi1 == 200.0,
              "a partial update keeps the fields it does not carry; OFI is taken against the previous book");
    }
    check(r.report().other == 1 && r.report().unstamped == 1, "scrip lines and unstamped lines are counted, not used");
}

/// Events every 100 ms for `hours`. OFI is random; when `predictive`, the mid
/// moves 2-3 s after the flow that causes it, so past OFI predicts it.
std::vector<DepthEvent> market(double hours, bool predictive, unsigned seed) {
    std::mt19937 g(seed);
    std::normal_distribution<double> z(0.0, 1.0);
    const std::size_t n = static_cast<std::size_t>(hours * 36'000.0);
    std::vector<double> flow(n);
    std::vector<DepthEvent> ev(n);
    double mid = 1000.0;
    for (std::size_t i = 0; i < n; ++i) {
        flow[i] = 100.0 * z(g);
        if (predictive && i >= 30) {
            double past = 0.0;
            for (std::size_t k = i - 30; k < i - 20; ++k) { past += flow[k]; }   // 2-3 s ago
            mid += 0.0004 * past / 10.0;
        }
        mid += 0.01 * z(g);
        ev[i].t_ms = static_cast<std::int64_t>(i) * 100;
        ev[i].q = quote(mid - 0.025, mid + 0.025, 500, 500);
        ev[i].ofi1 = flow[i];
        ev[i].ofi5 = flow[i];
    }
    return ev;
}

void test_study() {
    DepthStudyPolicy p;
    p.lookback_ms = 5000;
    p.horizon_ms = 5000;
    const auto yes = depth_study(market(3.0, true, 1), p);
    const auto no = depth_study(market(3.0, false, 2), p);
    check(yes && no, "both studies run");
    if (!yes || !no) { return; }
    std::printf("    predictive market: OOS R2 %.3f, hit %.3f | null market: OOS R2 %.3f, hit %.3f\n", yes->oos_r2_all,
                yes->hit_rate, no->oos_r2_all, no->hit_rate);
    check(yes->oos_r2_ofi > 0.2 && yes->hit_rate > 0.65, "where past flow moves the mid, the study finds it out of sample");
    check(no->oos_r2_all < 0.01 && std::fabs(no->hit_rate - 0.5) < 0.05, "where it does not, out-of-sample R2 is nil and the hit rate a coin");
    auto gappy = market(2.0, true, 3);
    for (std::size_t i = gappy.size() / 2; i < gappy.size(); ++i) { gappy[i].t_ms += 3'600'000; }   // an hour's hole
    const auto gp = depth_study(gappy, p);
    check(gp && gp->samples < yes->samples, "grid points across a gap in the recording are skipped");
    check(!depth_study(std::vector<DepthEvent>(50), p).has_value(), "too few events: no result");
}

} // namespace

int main() {
    std::printf("Depth study\n");
    test_ofi_formula();
    test_reader();
    test_study();
    std::printf("Depth study: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
