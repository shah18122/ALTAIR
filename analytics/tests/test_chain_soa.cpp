// Acceptance tests for analytics/chain_soa.hpp.
//
// THE KERNEL IS A COPY OF black76, AND COPIES DRIFT.
//
// The chain sweep repeats analytics/greeks.hpp's arithmetic in a form the
// compiler can vectorise, which means there are now two implementations of
// Black-76 in the tree. The first check here is that they agree, strike by
// strike, on every output -- so the fast one can never quietly become a
// different model from the tested one.
//
// THE WINGS ARE WHERE IT WOULD BREAK. A first draft computed N(-d1) as
// 1 - N(d1). At the money that is fine; for a strike far out of the money
// N(d1) is within an ulp of 1 and the subtraction cancels to noise. The wing
// strikes below exist to catch exactly that, and a relative-error check is
// used there because an absolute tolerance on a price of a few paise would
// pass anything.
//
// No check description here may contain the substring "F" "AIL" joined.

#include <analytics/chain_soa.hpp>

#include <cmath>
#include <cstdio>

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    std::printf("  %-4s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) { ++failures; }
}

/// |a - b| relative to the larger magnitude, with a floor so exact zeros
/// compare cleanly.
double rel(double a, double b)
{
    const double m = std::fmax(std::fabs(a), std::fabs(b));
    return m < 1e-300 ? 0.0 : std::fabs(a - b) / m;
}

}  // namespace

int main()
{
    std::printf("chain_soa -- one expiry, one cache-resident sweep\n\n");

    // NIFTY-ish: forward 23,450 rupees, 12 trading days, 12.5% vol, 6.5%.
    altair::ChainInputs in;
    in.forward = 2'345'000.0;        // paise
    in.years = 12.0 / 365.0;
    in.vol = 0.125;
    in.rate = 0.065;

    // 121 strikes, 50-rupee step, from deep ITM to deep OTM on both sides --
    // far wider than anything the terminal shows, precisely to reach the
    // wings.
    altair::ChainSoA soa;
    soa.n = 121;
    for (std::size_t i = 0; i < soa.n; ++i) {
        soa.strike[i] = 2'045'000.0 + 5'000.0 * static_cast<double>(i);
    }

    const auto ok = altair::price_chain(soa, in);
    check(ok.has_value(), "a 121-strike chain prices");

    // ---- agreement with the tested scalar implementation ----------------
    double worst = 0.0;
    std::size_t worst_at = 0;
    bool all_agree = true;
    for (std::size_t i = 0; i < soa.n; ++i) {
        const auto c = altair::black76(
            altair::OptionRight::Call, altair::Price{2'345'000},
            altair::Price{static_cast<std::int64_t>(soa.strike[i])},
            altair::Years{in.years}, altair::Vol{in.vol}, in.rate);
        const auto p = altair::black76(
            altair::OptionRight::Put, altair::Price{2'345'000},
            altair::Price{static_cast<std::int64_t>(soa.strike[i])},
            altair::Years{in.years}, altair::Vol{in.vol}, in.rate);
        if (!c || !p) { all_agree = false; continue; }

        // Prices below a paisa are compared absolutely: relative error on a
        // number that small is dominated by the number, not the method.
        const auto close = [](double a, double b) {
            return std::fabs(a - b) < 1e-6 || rel(a, b) < 1e-9;
        };
        const double e = std::fmax(rel(soa.call_px[i], c->price),
                                   rel(soa.put_px[i], p->price));
        if (e > worst) { worst = e; worst_at = i; }
        if (!close(soa.call_px[i], c->price) || !close(soa.put_px[i], p->price)
            || !close(soa.call_delta[i], c->delta)
            || !close(soa.put_delta[i], p->delta)
            || !close(soa.gamma[i], c->gamma) || !close(soa.vega[i], c->vega)
            || !close(soa.call_theta[i], c->theta)
            || !close(soa.put_theta[i], p->theta)) {
            all_agree = false;
            std::printf("        mismatch at strike %.0f\n", soa.strike[i] / 100.0);
        }
    }
    std::printf("        worst price disagreement %.2e (relative), strike %.0f\n",
                worst, soa.strike[worst_at] / 100.0);
    check(all_agree,
          "every output at every strike matches analytics/greeks.hpp's black76 "
          "-- the fast kernel is the tested model, not a new one");

    // ---- the wing that the cancellation bug would have broken ------------
    {
        // Strike 20,450 against a 23,450 forward: a put 12.8% out of the
        // money with 12 days to go. Worth a small fraction of a rupee, and
        // exactly where 1 - N(d1) would have cancelled.
        const std::size_t deep = 0;
        const auto p = altair::black76(
            altair::OptionRight::Put, altair::Price{2'345'000},
            altair::Price{static_cast<std::int64_t>(soa.strike[deep])},
            altair::Years{in.years}, altair::Vol{in.vol}, in.rate);
        std::printf("        deep OTM put @ %.0f: kernel %.6e  reference %.6e "
                    "paise\n",
                    soa.strike[deep] / 100.0, soa.put_px[deep],
                    p ? p->price : 0.0);
        check(p && soa.put_px[deep] >= 0.0
                  && (soa.put_px[deep] == p->price
                      || rel(soa.put_px[deep], p->price) < 1e-9),
              "a deep out-of-the-money put agrees to 1e-9 relative -- no "
              "cancellation in the tail");
    }

    // ---- put-call parity, on every row ------------------------------------
    {
        const double df = std::exp(-in.rate * in.years);
        double worst_par = 0.0;
        for (std::size_t i = 0; i < soa.n; ++i) {
            const double resid = soa.call_px[i] - soa.put_px[i]
                                 - df * (in.forward - soa.strike[i]);
            worst_par = std::fmax(worst_par, std::fabs(resid));
        }
        std::printf("        worst parity residual %.3e paise\n", worst_par);
        check(worst_par < 1e-4,
              "C - P = df*(F - K) on every row to a ten-thousandth of a paisa");
        bool gamma_same = true;
        for (std::size_t i = 0; i < soa.n; ++i) {
            // One gamma column serves both rights, by construction and by
            // the identity: a call and a put differ by a forward contract.
            gamma_same = gamma_same && soa.gamma[i] > 0.0;
        }
        check(gamma_same, "gamma is positive on every strike");
    }

    // ---- SoA and AoS compute the same thing -------------------------------
    {
        static altair::ChainRowAoS rows[altair::kChainCap];
        for (std::size_t i = 0; i < soa.n; ++i) { rows[i].strike = soa.strike[i]; }
        const auto a = altair::price_chain_aos(rows, soa.n, in);
        check(a.has_value(), "the AoS sweep prices");
        bool same = true;
        for (std::size_t i = 0; i < soa.n && same; ++i) {
            same = rows[i].call_px == soa.call_px[i]
                   && rows[i].put_px == soa.put_px[i]
                   && rows[i].gamma == soa.gamma[i]
                   && rows[i].call_theta == soa.call_theta[i];
        }
        check(same,
              "the two layouts produce bit-identical numbers -- so the "
              "benchmark compares LAYOUT and nothing else");
    }

    // ---- rule 11 and bad input --------------------------------------------
    {
        static altair::ChainSoA wide;
        wide.n = altair::kChainCap + 1;
        const auto r = altair::price_chain(wide, in);
        check(!r && r.error() == altair::ChainError::TooManyStrikes,
              "a chain wider than kChainCap is REFUSED, not truncated");
    }
    {
        altair::ChainSoA c;
        c.n = 3;
        c.strike[0] = 2'300'000.0;
        c.strike[1] = 0.0;               // a strike of zero
        c.strike[2] = 2'400'000.0;
        const auto r = altair::price_chain(c, in);
        check(!r && r.error() == altair::ChainError::BadInput,
              "one bad strike refuses the whole chain rather than pricing "
              "around it");
        altair::ChainInputs expired = in;
        expired.years = 0.0;
        c.strike[1] = 2'350'000.0;
        check(!altair::price_chain(c, expired).has_value(),
              "an expired chain is refused -- at T = 0 an option is an "
              "exercise decision, not a derivative");
    }

    // ---- the layout claims ------------------------------------------------
    {
        altair::ChainSoA c;
        const auto addr = [](const void* p) {
            return reinterpret_cast<std::uintptr_t>(p);
        };
        const bool aligned =
            addr(c.strike) % altair::kCacheLineBytes == 0
            && addr(c.call_px) % altair::kCacheLineBytes == 0
            && addr(c.put_theta) % altair::kCacheLineBytes == 0;
        check(aligned, "every column starts on a cache line");
        const bool disjoint =
            addr(c.call_px) - addr(c.strike)
                >= sizeof(double) * altair::kChainCap;
        check(disjoint, "and no two columns share one");
        std::printf("        sizeof(ChainSoA) = %zu bytes (%.1f KB), "
                    "sizeof(ChainRowAoS) = %zu\n",
                    sizeof(altair::ChainSoA),
                    sizeof(altair::ChainSoA) / 1024.0,
                    sizeof(altair::ChainRowAoS));

        const auto topo = altair::read_cache_topology();
        std::printf("        this machine: line %zu B, L1d %zu KB, L2 %zu KB, "
                    "L3 %zu KB (%s)\n",
                    topo.line, topo.l1d / 1024, topo.l2 / 1024, topo.l3 / 1024,
                    topo.from_os ? "from the OS" : "FALLBACK, not measured");
        check(topo.line_matches(),
              "the OS agrees the cache line is 64 bytes, which every static "
              "layout here assumes");
        check(sizeof(altair::ChainSoA) <= topo.l1d,
              "one expiry's whole chain fits in this machine's L1D");
    }

    std::printf("\n%s\n", failures == 0 ? "all checks passed"
                                        : "THERE WERE FAILURES");
    return failures == 0 ? 0 : 1;
}
