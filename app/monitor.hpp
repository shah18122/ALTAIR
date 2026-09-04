// app/monitor.hpp -- production monitoring: latency, drops, drift, PnL,
// invariants.
//
// P12-03.
//
// A THRESHOLD ALARM CANNOT SEE A METRIC THAT STOPPED ARRIVING.
//
// This is the whole card, and it is the same finding this repo has hit in five
// other places wearing five other costumes: ABSENCE IS NOT ZERO.
//
// The obvious monitor holds a number per signal and compares it to a limit.
// Feed drops: 0, under the limit. Tick latency: 0 ns, well under the limit.
// Conservation residual: 0 paise, exact. Every light is green and the process
// has been disconnected from the exchange for forty minutes, because a feed
// that has stopped delivering ticks reports no drops and no latency, and a
// threshold has nothing to fire on. The monitor is not merely useless here --
// it is ACTIVELY REASSURING, which is worse than having none.
//
// So every signal carries a HEARTBEAT as well as a value, and a signal that has
// not been observed inside its staleness budget reports `Stale`, which ranks
// ABOVE `Critical`. A breached threshold means the system is still talking and
// telling you something bad. A stale one means it stopped talking, and you do
// not know what it would have said.
//
// AND A SIGNAL THAT HAS NEVER REPORTED IS NOT `Ok`.
//
// The initial state is `Unobserved`. A monitor that boots green and stays green
// because nothing was ever wired to it is the same failure at t=0: it passes
// every check it is given, and the check it needed was "did anyone connect?"
//
// THE MONITOR IS THE ONE COMPONENT THAT MUST NOT READ TIME OFF THE TICK.
//
// CLAUDE.md rule 7 says strategies read time off the tick and never from a wall
// clock, and it is right: a strategy that consults a wall clock can see past the
// end of the replay and the backtest becomes a lie.
//
// This file is the documented exception, and the reason is exactly the failure
// above. The event the monitor exists to detect is TICKS STOPPING. A clock
// derived from the tick stream FREEZES at the moment the stream dies, so the
// staleness budget never elapses, and the one alarm that mattered is the one
// that is structurally incapable of firing. `now` is therefore a parameter --
// supplied by the caller from a wall clock in production, and by the test from
// a fake one -- and it is never taken from the data being monitored.
//
// This does not weaken rule 7. Nothing here produces a signal, sizes a
// position, or crosses into a strategy; the monitor only reports, and the
// backtest does not run it.
//
// LATENCY IS A PERCENTILE. THE MEAN IS THE NUMBER THAT LOOKS FINE.
//
// A mean tick-to-decision latency of 40 us against a 200 us budget is a
// perfectly healthy-looking figure for a system that misses one tick in a
// hundred by 3 ms. Order-book imbalance decays in 10-200 ms (ROADMAP section
// 3); a p99 outside that window means the tail of the distribution is not
// trading on the signal it thinks it is. So the histogram reports percentiles,
// and it reports each bucket's UPPER edge -- a latency monitor that rounds DOWN
// tells you that you are fine.

#pragma once

#include <core/time/timestamp.hpp>

#include <array>
#include <bit>
#include <cstdint>

namespace altair {

// -------------------------------------------------------------------------
// Health
// -------------------------------------------------------------------------

/// Ordered WORST-LAST so `worst()` is a max. `Stale` is deliberately above
/// `Critical`: a breached threshold is the system reporting something bad, a
/// stale signal is the system having stopped reporting at all, and only one of
/// those tells you what your exposure is.
enum class Health : std::uint8_t {
    /// Nothing has ever been observed. NOT `Ok` -- see the header.
    Unobserved = 0,
    Ok         = 1,
    Warn       = 2,
    Critical   = 3,
    Stale      = 4
};

[[nodiscard]] inline const char* health_text(Health h) noexcept {
    switch (h) {
    case Health::Unobserved: return "UNOBSERVED";
    case Health::Ok:         return "ok";
    case Health::Warn:       return "WARN";
    case Health::Critical:   return "CRITICAL";
    case Health::Stale:      return "STALE";
    }
    return "?";
}

/// Which direction is bad.
///
/// Both exist because half the signals in this engine are healthy when small
/// (latency, drops, drift, conservation residual) and half when large (fill
/// rate, feed uptime, model coverage). A monitor that only knows `Above`
/// silently cannot watch the second half.
enum class Sense : std::uint8_t { Above, Below };

/// One monitored signal. Thresholds are plain doubles because these are
/// DIAGNOSTIC quantities, not ledger quantities -- CLAUDE.md rule 3 governs
/// money, and nothing here is money. Where a signal IS money (PnL, residual)
/// the caller converts paise to a double at the call site, once, for display.
struct GaugeSpec {
    const char* name = "";
    double warn_at = 0.0;
    double critical_at = 0.0;
    Sense sense = Sense::Above;
    /// How long a gap between observations before the signal is `Stale`.
    /// There is no "never stale" value on purpose: a signal nobody promised to
    /// update is a signal nobody will notice the loss of.
    Duration stale_after = duration::seconds(60);
};

// -------------------------------------------------------------------------
// Latency histogram
// -------------------------------------------------------------------------

/// Fixed-capacity, allocation-free, four sub-buckets per octave.
///
/// 256 buckets of `std::uint32_t` is 1 KiB, so this can live on the hot path;
/// CLAUDE.md rule 4 forbids allocation inside `ALTAIR_HOT` and a histogram that
/// grows a vector is exactly the thing that would violate it under load, which
/// is to say precisely when the latency being measured matters.
///
/// Resolution is a factor of 2^(1/4), about 19%. That is coarse for a mean and
/// entirely adequate for the question actually being asked, which is whether
/// the tail is inside the decay window of the signal being traded.
class LatencyHistogram {
public:
    static constexpr std::size_t kBuckets = 256;

    /// UNIT: nanoseconds. Non-positive values land in bucket 0 rather than
    /// being dropped -- a clock that runs backwards is a fact about the
    /// system, and discarding it hides it.
    void record(std::int64_t ns) noexcept {
        ++count_;
        sum_ns_ += ns > 0 ? ns : 0;
        if (ns > max_ns_) { max_ns_ = ns; }
        ++bucket_[index_of(ns)];
    }

    [[nodiscard]] std::uint64_t count() const noexcept { return count_; }
    [[nodiscard]] std::int64_t max_ns() const noexcept { return max_ns_; }

    /// UNIT: nanoseconds. Present for comparison against `percentile`, and to
    /// make the header's point checkable rather than asserted.
    [[nodiscard]] double mean_ns() const noexcept {
        return count_ == 0 ? 0.0
                           : static_cast<double>(sum_ns_)
                                 / static_cast<double>(count_);
    }

    /// UNIT: nanoseconds. `q` in [0, 1].
    ///
    /// Returns the containing bucket's UPPER edge, so the answer is an
    /// over-estimate and never an under-estimate. A latency figure that rounds
    /// down is a latency figure that clears a budget it did not clear.
    [[nodiscard]] std::int64_t percentile(double q) const noexcept {
        if (count_ == 0) { return 0; }
        if (q < 0.0) { q = 0.0; }
        if (q > 1.0) { q = 1.0; }
        const double want = q * static_cast<double>(count_);
        std::uint64_t seen = 0;
        for (std::size_t i = 0; i < kBuckets; ++i) {
            seen += bucket_[i];
            if (static_cast<double>(seen) >= want) {
                return upper_edge(i);
            }
        }
        return upper_edge(kBuckets - 1);
    }

    void reset() noexcept {
        bucket_.fill(0);
        count_ = 0;
        sum_ns_ = 0;
        max_ns_ = 0;
    }

    /// Bucket for a nanosecond value. Exposed so the test can assert the
    /// mapping directly rather than inferring it from a percentile.
    [[nodiscard]] static std::size_t index_of(std::int64_t ns) noexcept {
        if (ns <= 0) { return 0; }
        const auto u = static_cast<std::uint64_t>(ns);
        const auto k = static_cast<std::size_t>(
            63 - std::countl_zero(u));                 // highest set bit
        const std::uint64_t base = std::uint64_t{1} << k;
        const auto sub = static_cast<std::size_t>(((u - base) * 4U) / base);
        const std::size_t idx = k * 4 + sub;
        return idx < kBuckets ? idx : kBuckets - 1;
    }

    /// UNIT: nanoseconds. The first value NOT in bucket `i`.
    ///
    /// The first two octaves DEGENERATE, and that is arithmetic rather than a
    /// bug: the integers {1} and {2, 3} cannot be split into four sub-buckets,
    /// so some indices below 8 are unreachable and share an edge with their
    /// neighbour. `index_of` never produces them, they never accumulate a
    /// count, and `percentile` skips them. The invariant that holds -- and that
    /// the test asserts -- is over the buckets `index_of` can actually reach.
    /// Nothing in this system measures a 3-nanosecond latency anyway; the note
    /// is here so the gap is a recorded consequence and not a surprise.
    [[nodiscard]] static std::int64_t upper_edge(std::size_t i) noexcept {
        const std::size_t k = i / 4;
        const std::size_t sub = i % 4;
        if (k >= 62) { return Timestamp::max().ns_since_epoch(); }
        const std::int64_t base = std::int64_t{1} << k;
        return base + static_cast<std::int64_t>(sub + 1) * base / 4;
    }

private:
    std::array<std::uint32_t, kBuckets> bucket_{};
    std::uint64_t count_ = 0;
    std::int64_t sum_ns_ = 0;
    std::int64_t max_ns_ = 0;
};

// -------------------------------------------------------------------------
// Monitor
// -------------------------------------------------------------------------

/// What one gauge currently reports.
struct GaugeState {
    double value = 0.0;
    /// When it was last observed. Meaningless while never observed, and the
    /// caller must not read it as "observed at the epoch".
    Timestamp last = Timestamp::epoch();
    bool ever_observed = false;
};

/// A fixed set of gauges. `N` is a compile-time count so there is no
/// allocation and no map lookup; the caller indexes with its own enum.
template <std::size_t N>
class Monitor {
public:
    /// The worst health across every gauge, and which one it was.
    struct Worst {
        Health health = Health::Unobserved;
        std::size_t index = 0;
        const char* name = "";
    };

    explicit constexpr Monitor(const std::array<GaugeSpec, N>& specs) noexcept
        : spec_(specs) {}

    /// Record an observation. `now` is WALL time -- see the header for why
    /// this one component may not take it from the tick.
    void observe(std::size_t i, double value, Timestamp now) noexcept {
        if (i >= N) { return; }
        state_[i].value = value;
        state_[i].last = now;
        state_[i].ever_observed = true;
    }

    /// Health of one gauge as of `now`.
    ///
    /// STALENESS IS CHECKED BEFORE THE THRESHOLD, and that ordering is the
    /// card. Checking the value first means a signal that stopped arriving is
    /// judged on its last-known number, which was fine -- that is exactly the
    /// green dashboard on a dead feed.
    [[nodiscard]] Health health(std::size_t i, Timestamp now) const noexcept {
        if (i >= N) { return Health::Unobserved; }
        const GaugeState& g = state_[i];
        if (!g.ever_observed) { return Health::Unobserved; }
        if ((now - g.last).raw() > spec_[i].stale_after.raw()) {
            return Health::Stale;
        }
        const GaugeSpec& s = spec_[i];
        if (s.sense == Sense::Above) {
            if (g.value >= s.critical_at) { return Health::Critical; }
            if (g.value >= s.warn_at)     { return Health::Warn; }
        } else {
            if (g.value <= s.critical_at) { return Health::Critical; }
            if (g.value <= s.warn_at)     { return Health::Warn; }
        }
        return Health::Ok;
    }

    /// Returns the FIRST gauge at the worst level, so the reported name is
    /// stable across calls rather than depending on iteration luck.
    [[nodiscard]] Worst worst(Timestamp now) const noexcept {
        Worst w{};
        w.name = N > 0 ? spec_[0].name : "";
        for (std::size_t i = 0; i < N; ++i) {
            const Health h = health(i, now);
            if (static_cast<std::uint8_t>(h)
                > static_cast<std::uint8_t>(w.health)) {
                w.health = h;
                w.index = i;
                w.name = spec_[i].name;
            }
        }
        return w;
    }

    [[nodiscard]] const GaugeState& state(std::size_t i) const noexcept {
        return state_[i < N ? i : 0];
    }
    [[nodiscard]] const GaugeSpec& spec(std::size_t i) const noexcept {
        return spec_[i < N ? i : 0];
    }
    [[nodiscard]] static constexpr std::size_t size() noexcept { return N; }

private:
    std::array<GaugeSpec, N> spec_{};
    std::array<GaugeState, N> state_{};
};

} // namespace altair
