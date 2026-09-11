// analytics/bench/bench_chain.cpp -- SoA versus AoS, at L1, L2, L3 and RAM.
//
// A layout is a bet about which bytes a loop reads together. This measures
// the bet instead of asserting it, on THIS machine's cache sizes (read from
// the OS, printed first), with two workloads chosen because each layout
// should win one of them:
//
//   SWEEP   price every strike of every chain. Reads one input column, writes
//           eight output columns in order. Structure-of-arrays should win:
//           every line fetched is eight useful doubles of one field.
//
//   LOOKUP  read all nine fields of ONE random strike -- what the chain grid
//           does to paint a row. Array-of-structures should win: one strike
//           is two cache lines in AoS and nine in SoA, one per column.
//
// If SoA won both, the AoS comment in chain_soa.hpp would be wrong, and this
// is the file that would say so.
//
// NOT A CTEST TEST, deliberately. Timing under a parallel test run is noise,
// and a latency number that fails the build on a busy afternoon teaches
// everybody to ignore latency numbers. Run it by hand on a quiet machine.

#include <analytics/chain_soa.hpp>
#include <core/cache_topology.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <random>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

/// Defeat dead-code elimination without a library: the result is read
/// through a volatile, so the compiler must compute it.
volatile double g_sink = 0.0;

/// Repeat `body` until at least `min_ms` has elapsed; return ns per unit.
template <typename F>
double time_ns_per(F body, std::size_t units_per_call, int min_ms = 250)
{
    body();                                             // warm up
    std::size_t calls = 0;
    const auto t0 = Clock::now();
    auto t1 = t0;
    do {
        body();
        ++calls;
        t1 = Clock::now();
    } while (std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0)
                 .count() < min_ms);
    const double ns =
        static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
                .count());
    return ns / static_cast<double>(calls * units_per_call);
}

struct Level {
    const char* name;
    std::size_t bytes;     ///< target working set, SoA
};

}  // namespace

int main()
{
    const altair::CacheTopology topo = altair::read_cache_topology();
    std::printf("SoA vs AoS -- option-chain pricing\n\n");
    std::printf("  this machine: line %zu B | L1d %zu KB | L2 %zu KB | L3 %zu KB  "
                "(%s)\n",
                topo.line, topo.l1d / 1024, topo.l2 / 1024, topo.l3 / 1024,
                topo.from_os ? "from the OS" : "FALLBACK -- not measured");
    std::printf("  sizeof(ChainSoA) %zu B per 256-strike chain = %zu B/strike\n",
                sizeof(altair::ChainSoA),
                sizeof(altair::ChainSoA) / altair::kChainCap);
    std::printf("  sizeof(ChainRowAoS) %zu B per strike (padded to 2 lines)\n\n",
                sizeof(altair::ChainRowAoS));

    altair::ChainInputs in;
    in.forward = 2'345'000.0;
    in.years = 12.0 / 365.0;
    in.vol = 0.125;
    in.rate = 0.065;

    // Half of each cache, so the working set genuinely FITS rather than
    // straddling the boundary, and 8x L3 for a set that cannot.
    const Level levels[] = {
        {"L1d ", topo.l1d / 2},
        {"L2  ", topo.l2 / 2},
        {"L3  ", topo.l3 / 2},
        {"RAM ", topo.l3 * 8},
    };

    std::printf("  %-6s %8s %10s   %12s %12s %7s   %12s %12s %7s\n",
                "set", "chains", "strikes", "SWEEP SoA", "SWEEP AoS", "SoA x",
                "LOOKUP SoA", "LOOKUP AoS", "AoS x");
    std::printf("  %-6s %8s %10s   %12s %12s %7s   %12s %12s %7s\n", "", "", "",
                "ns/strike", "ns/strike", "", "ns/lookup", "ns/lookup", "");

    for (const Level& lv : levels) {
        const std::size_t chains = std::max<std::size_t>(
            1, lv.bytes / sizeof(altair::ChainSoA));
        const std::size_t strikes = chains * altair::kChainCap;

        // C++17 aligned new honours alignas(64) on both types.
        std::unique_ptr<altair::ChainSoA[]> soa(new altair::ChainSoA[chains]);
        std::unique_ptr<altair::ChainRowAoS[]> aos(
            new altair::ChainRowAoS[strikes]);
        for (std::size_t c = 0; c < chains; ++c) {
            soa[c].n = altair::kChainCap;
            for (std::size_t i = 0; i < altair::kChainCap; ++i) {
                const double k = 2'045'000.0 + 2'500.0 * static_cast<double>(i);
                soa[c].strike[i] = k;
                aos[c * altair::kChainCap + i].strike = k;
            }
        }

        // ---- SWEEP -------------------------------------------------------
        const double sweep_soa = time_ns_per([&] {
            for (std::size_t c = 0; c < chains; ++c) {
                (void)altair::price_chain(soa[c], in);
            }
            g_sink = g_sink + soa[chains - 1].call_px[128];
        }, strikes);
        const double sweep_aos = time_ns_per([&] {
            for (std::size_t c = 0; c < chains; ++c) {
                (void)altair::price_chain_aos(&aos[c * altair::kChainCap],
                                              altair::kChainCap, in);
            }
            g_sink = g_sink + aos[strikes - 1].call_px;
        }, strikes);

        // ---- LOOKUP: all nine fields of one random strike ----------------
        //
        // The indices are generated BEFORE timing so the RNG is not measured,
        // and are random so the hardware prefetcher cannot hide the cost of
        // the lines each layout has to touch.
        constexpr std::size_t kLookups = 1u << 16;
        std::vector<std::uint32_t> idx(kLookups);
        std::mt19937 rng(20260911u);
        std::uniform_int_distribution<std::uint32_t> pick(
            0, static_cast<std::uint32_t>(strikes - 1));
        for (auto& v : idx) { v = pick(rng); }

        const double look_soa = time_ns_per([&] {
            double acc = 0.0;
            for (const std::uint32_t g : idx) {
                const altair::ChainSoA& c = soa[g / altair::kChainCap];
                const std::size_t i = g % altair::kChainCap;
                acc += c.strike[i] + c.call_px[i] + c.put_px[i]
                       + c.call_delta[i] + c.put_delta[i] + c.gamma[i]
                       + c.vega[i] + c.call_theta[i] + c.put_theta[i];
            }
            g_sink = g_sink + acc;
        }, kLookups);
        const double look_aos = time_ns_per([&] {
            double acc = 0.0;
            for (const std::uint32_t g : idx) {
                const altair::ChainRowAoS& r = aos[g];
                acc += r.strike + r.call_px + r.put_px + r.call_delta
                       + r.put_delta + r.gamma + r.vega + r.call_theta
                       + r.put_theta;
            }
            g_sink = g_sink + acc;
        }, kLookups);

        std::printf("  %-6s %8zu %10zu   %12.2f %12.2f %6.2fx   %12.2f %12.2f "
                    "%6.2fx\n",
                    lv.name, chains, strikes, sweep_soa, sweep_aos,
                    sweep_aos / sweep_soa, look_soa, look_aos,
                    look_soa / look_aos);
    }

    std::printf("\n  SoA x  = how many times faster SoA sweeps than AoS\n");
    std::printf("  AoS x  = how many times faster AoS looks up one strike "
                "than SoA\n");
    std::printf("\n  (checksum %.6e -- printed so the work cannot be elided)\n",
                static_cast<double>(g_sink));
    return 0;
}
