// Tests for book/diffusion.hpp, information diffusion between two contracts,
// on synthetic tapes whose answer is known: the follower copies the leader
// 300 ms late (plus its own sampling), so the lead-lag profile must peak at
// +300 ms, the leader must carry the price discovery, and the gap must show
// a leader jump the follower has not printed yet. A follower that is an
// independent walk must show no lead (the placebo).
//
// No check description here may contain the substring FAIL.

#include <book/diffusion.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("  %s: %s\n", ok ? "ok  " : "BAD ", what);
    if (!ok) ++failures;
}

using namespace altair::diffusion;

/// A fixed generator: the same numbers on every standard library.
struct Lcg {
    std::uint64_t s;
    explicit Lcg(std::uint64_t seed) : s(seed) {}
    std::uint64_t next() { s = s * 6364136223846793005ULL + 1442695040888963407ULL; return s >> 33; }
    double uniform() { return static_cast<double>(next() % 1'000'000) / 1'000'000.0; }
    std::int64_t between(std::int64_t lo, std::int64_t hi) { return lo + static_cast<std::int64_t>(next() % static_cast<std::uint64_t>(hi - lo + 1)); }
};

/// The true price path: a +/- one-tick step at each change time.
struct Path {
    std::vector<std::int64_t> t;
    std::vector<double> p;
    double at(std::int64_t x) const {
        if (t.empty() || x < t.front()) return p.empty() ? 0.0 : p.front();
        std::size_t lo = 0, hi = t.size();
        while (hi - lo > 1) { const std::size_t mid = (lo + hi) / 2; if (t[mid] <= x) lo = mid; else hi = mid; }
        return p[lo];
    }
};

Path walk(Lcg& g, std::int64_t t0, std::int64_t t1, double start, double tick) {
    Path w;
    double px = start;
    for (std::int64_t t = t0; t <= t1; t += g.between(40, 160) * kMsNs) {
        if (g.uniform() < 0.7) px += g.uniform() < 0.5 ? tick : -tick;
        w.t.push_back(t);
        w.p.push_back(px);
    }
    return w;
}

/// Feed both sides in time order: the leader sees the path at its own
/// times, the follower sees it `lag` late at its own times.
void run(PairStats& st, Lcg& g, const Path& lead, const Path& follow_src, std::int64_t lag, std::int64_t t0, std::int64_t t1) {
    std::int64_t tl = t0, tf = t0 + 7 * kMsNs;
    while (tl <= t1 || tf <= t1) {
        if (tl <= tf) { st.on_leader(lead.at(tl), tl); tl += g.between(30, 120) * kMsNs; }
        else { st.on_follower(follow_src.at(tf - lag), tf); tf += g.between(60, 180) * kMsNs; }
    }
}

} // namespace

int main() {
    std::printf("information diffusion\n");
    const std::int64_t t0 = 1'000 * kSecNs, t1 = t0 + 600 * kSecNs;
    {
        Lcg g(7);
        const Path p = walk(g, t0 - 10 * kSecNs, t1, 50'000.0, 5.0);
        Config c;
        c.same_asset = true;
        PairStats st(c);
        run(st, g, p, p, 300 * kMsNs, t0, t1);
        const LeadLag& ll = st.lead_lag();
        check(ll.valid, "enough moves each side: the profile is read");
        std::printf("    best lag %lld ms, rho %.3f, LLR %.2f, beta %.3f\n", static_cast<long long>(ll.best_lag_ns / kMsNs), ll.best_rho, ll.llr, ll.beta);
        check(ll.best_lag_ns == 300 * kMsNs, "the follower copies 300 ms late: the profile peaks at +300 ms");
        check(ll.llr > 2.0, "the lead-lag ratio says the leader leads");
        check(std::fabs(ll.beta - 1.0) < 0.15, "the same asset: beta is about one");
        const Discovery& d = st.discovery();
        std::printf("    component share %.3f, information share %.3f..%.3f\n", d.cs_leader, d.is_leader_lo, d.is_leader_hi);
        check(d.valid && d.cs_leader > 0.8, "the leader carries the price discovery (component share)");
        check(d.valid && d.is_leader_hi > 0.6 && d.is_leader_lo <= d.is_leader_hi, "and the information share bounds agree");
        check(std::isfinite(st.half_life_s()) && st.half_life_s() < 5.0, "the gap closes: a finite half-life of a few seconds at most");

        // A 1% leader jump the follower has not printed yet.
        const double lp = st.leader_price(), fp = st.follower_price();
        st.on_leader(lp * 1.01, t1 + 20 * kMsNs);
        const Gap gp = st.gap();
        std::printf("    gap after a 1%% jump: %.2f rupees on %.2f (horizon %lld ms)\n", gp.rupees, fp, static_cast<long long>(gp.horizon_ns / kMsNs));
        check(gp.valid && gp.rupees > 0.007 * fp && gp.rupees < 0.013 * fp, "a 1% leader jump: the follower owes about 1%");
        st.on_follower(fp * 1.01, t1 + 320 * kMsNs);
        check(std::fabs(st.gap().rupees) < 0.004 * fp, "the follower catches up: the gap closes");
    }
    {
        // The mirror: the second contract moves first, so the lag is negative.
        Lcg g(11);
        const Path p = walk(g, t0 - 10 * kSecNs, t1, 50'000.0, 5.0);
        PairStats st;
        run(st, g, p, p, -500 * kMsNs, t0, t1);
        const LeadLag& ll = st.lead_lag();
        std::printf("    best lag %lld ms, LLR %.2f\n", static_cast<long long>(ll.best_lag_ns / kMsNs), ll.llr);
        check(ll.valid && ll.best_lag_ns == -500 * kMsNs, "the follower moves first: the profile peaks at -500 ms");
        check(ll.llr < 0.5, "and the lead-lag ratio says the 'leader' follows");
    }
    {
        // The placebo: two independent walks.
        Lcg g(23);
        const Path a = walk(g, t0 - 10 * kSecNs, t1, 50'000.0, 5.0);
        const Path b = walk(g, t0 - 10 * kSecNs, t1, 50'000.0, 5.0);
        PairStats st;
        run(st, g, a, b, 0, t0, t1);
        const LeadLag& ll = st.lead_lag();
        std::printf("    placebo best |rho| %.3f\n", std::fabs(ll.best_rho));
        check(ll.valid && std::fabs(ll.best_rho) < 0.12, "independent walks: no lag carries a real correlation");
    }
    {
        PairStats st;
        st.on_leader(100.0, kSecNs);
        st.on_follower(100.0, kSecNs);
        st.on_leader(-1.0, 2 * kSecNs);
        st.on_leader(std::nan(""), 3 * kSecNs);
        check(!st.lead_lag().valid && !st.gap().valid, "too few moves and bad prices: nothing is read");
    }
    std::printf("%s (%d)\n", failures == 0 ? "all passed" : "problems", failures);
    return failures == 0 ? 0 : 1;
}
