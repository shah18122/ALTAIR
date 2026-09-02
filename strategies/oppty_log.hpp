// strategies/oppty_log.hpp -- the arbitrage opportunity log and the
// post-cost session report.
//
// P5-08, and the card Phase 5 exists to reach. ROADMAP: "a full session
// logging every arbitrage opportunity with its post-cost edge. You will learn
// empirically whether the edge exists."
//
// A LOG OF WHAT PASSED CANNOT ANSWER THE QUESTION THE PHASE IS ASKING.
//
// This is the card. The natural thing to build is a log of actionable
// opportunities, and it is useless for the stated purpose: it can tell you how
// often you won and never why you did not. Four completely different states
// all present as "no actionable opportunities today":
//
//   NoMispricing      the prices were right. There is no edge to find.
//   CostExceedsEdge   the edge was real and the bill was larger.
//   Unreachable       the edge was real, the bill was smaller, and the trade
//                     cannot be placed (P5-05: no short cash in India).
//   StaleQuotes       we never actually saw both legs at once.
//
// The first says stop looking. The second says look at cheaper structures. The
// third says look at the account. The fourth says fix the feed. A log that
// records only winners collapses all four into an empty file, and the
// conclusion drawn from an empty file is always the first one.
//
// So EVERY observation is counted, including the ones that were never close,
// and the verdict is recorded with it.
//
// THE LOG RECORDS THE PRE-COST NUMBER. THE DECISION PATH NEVER SEES IT.
//
// Rule 5 says no strategy sees a pre-cost number, and none does: the scanners
// in P5-05 to P5-07 have no gross-edge accessor on their decision types. But
// "is there edge before costs?" is exactly the question the exit criterion
// asks, so the LOG carries gross, cost and net separately. The distinction is
// that this type is a research artefact and is not on the path to an order --
// nothing in oms/ includes this header, and nothing here includes oms/.
//
// A FULL LOG THAT OVERWRITES REPORTS A BIASED SAMPLE.
//
// A fixed ring that keeps the newest entries reports the close of the session;
// one that stops when full reports the open. Both are wrong in a way that
// looks like data. This keeps EXACT aggregate counts for every observation and
// a UNIFORM RESERVOIR SAMPLE of the detail, so the numbers in the report are
// the whole session and the rows kept for inspection are an unbiased draw from
// it. No allocation anywhere; the reservoir is a fixed array.
//
// THE BEST THING YOU SAW IS NOT THE BEST THING THERE IS.
//
// Scan four structures across a strike ladder on every tick and the day
// produces hundreds of thousands of observations. The largest net edge among
// them is the one with the most measurement error, not the most edge --
// selecting a maximum out of N noisy draws is a bias, and it grows with N.
//
// Measured on 200,000 observations of PURE NOISE, mean zero, Rs 50.21 of
// spread: the best observation of the session is Rs 229.06, which is 4.6
// standard deviations from the mean and reads in isolation as an unmissable
// opportunity. The expected best of 200,000 noise draws is Rs 222.52. The
// ratio is 1.03. Nothing was found.
//
// So `expected_extreme_from_noise` is what the observed best is reported
// against, and the RATIO rather than the sigma count is the number that says
// whether anything is there.
//
// Measured on the sampling too, on a session that is quiet for its first half
// and dislocated for its second: the reservoir keeps 109 early rows and 91
// late ones -- 45.5% late against a truth of 50%. An overwriting ring would
// report 100% late and a log that stopped when full would report 0%. Each of
// those is a confident, precise, wrong description of the day.

#pragma once

#include <core/types/units.hpp>
#include <strategies/basis.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

/// Which scanner produced the observation. Ordinal 0 is Unknown so a zeroed
/// row cannot pass for a cash-futures observation.
enum class OpportunityKind : std::uint8_t {
    Unknown = 0,
    CashFutures,
    CrossVenue,
    Parity,
    Box,
    Butterfly,
    FuturesCalendar,
    OptionCalendar
};
inline constexpr std::size_t kOpportunityKinds = 8;

/// Why this observation is or is not tradable. The whole point of the log.
enum class Verdict : std::uint8_t {
    Unknown = 0,
    /// Post-cost edge is positive and the trade can be placed.
    Actionable,
    /// There was a real mispricing and the bill was larger.
    CostExceedsEdge,
    /// The bill was smaller and the trade cannot be placed anyway.
    Unreachable,
    /// The prices were right. Nothing to explain.
    NoMispricing,
    /// The legs were not seen at the same instant.
    StaleQuotes,
    /// A leg had no two-sided quote.
    NoQuote
};
inline constexpr std::size_t kVerdicts = 7;

[[nodiscard]] inline const char* verdict_name(Verdict v) noexcept {
    switch (v) {
        case Verdict::Unknown:         return "Unknown";
        case Verdict::Actionable:      return "Actionable";
        case Verdict::CostExceedsEdge: return "CostExceedsEdge";
        case Verdict::Unreachable:     return "Unreachable";
        case Verdict::NoMispricing:    return "NoMispricing";
        case Verdict::StaleQuotes:     return "StaleQuotes";
        case Verdict::NoQuote:         return "NoQuote";
    }
    return "?";
}

[[nodiscard]] inline const char* kind_name(OpportunityKind k) noexcept {
    switch (k) {
        case OpportunityKind::Unknown:         return "Unknown";
        case OpportunityKind::CashFutures:     return "CashFutures";
        case OpportunityKind::CrossVenue:      return "CrossVenue";
        case OpportunityKind::Parity:          return "Parity";
        case OpportunityKind::Box:             return "Box";
        case OpportunityKind::Butterfly:       return "Butterfly";
        case OpportunityKind::FuturesCalendar: return "FuturesCalendar";
        case OpportunityKind::OptionCalendar:  return "OptionCalendar";
    }
    return "?";
}

/// One observation. GROSS IS PRESENT HERE and nowhere on a decision type.
struct Observation {
    Timestamp ts{};
    OpportunityKind kind = OpportunityKind::Unknown;
    Verdict verdict = Verdict::Unknown;
    Executability executability = Executability::Unknown;
    /// Instrument identity, from the spec store. Opaque here on purpose: the
    /// log does not need to know what a symbol means.
    std::uint32_t instrument = 0;
    Notional gross{};
    Notional cost{};
    Notional net{};
};

/// Decide the verdict from the three numbers and the executability.
///
/// Centralised so four scanners cannot each invent their own ordering. The
/// ORDER matters: unreachable is checked before the cost comparison, because
/// "we could not have done it" is a more informative answer than "it would not
/// have paid", and reporting the second when the first is true sends you to
/// look at the cost schedule for a problem that lives in the account.
[[nodiscard]] inline Verdict classify(Notional gross, Notional cost,
                                      Executability ex,
                                      std::int64_t noise_floor) noexcept {
    if (ex == Executability::NoQuote)          { return Verdict::NoQuote; }
    if (ex == Executability::QuotesTooFarApart){ return Verdict::StaleQuotes; }
    // Below the floor is NOT a mispricing. The floor is the caller's, in
    // paise, and it has no default: what counts as "no mispricing" depends on
    // the tick size and the instrument, which are spec-store facts (rule 1).
    const std::int64_t g = gross.raw() < 0 ? -gross.raw() : gross.raw();
    if (g <= noise_floor)                      { return Verdict::NoMispricing; }
    if (ex != Executability::Executable)       { return Verdict::Unreachable; }
    if (gross.raw() - cost.raw() <= 0)         { return Verdict::CostExceedsEdge; }
    return Verdict::Actionable;
}

/// What the largest of `n` independent noise observations looks like.
///
/// The standard extreme-value approximation for the maximum of n standard
/// normals, scaled by the observed spread. Not exact and does not need to be:
/// its job is to stop a 3-sigma best-of-200000 being read as a discovery, and
/// for that an approximation that is right to within a few percent is plenty.
[[nodiscard]] inline double expected_extreme_from_noise(std::uint64_t n,
                                                        double sd) noexcept {
    if (n < 2 || !(sd > 0.0)) { return 0.0; }
    const double ln_n = std::log(static_cast<double>(n));
    // sqrt(2 ln n) - (ln ln n + ln 4pi) / (2 sqrt(2 ln n)) -- the usual
    // second-order form; the correction matters at the n a scanner reaches.
    const double root = std::sqrt(2.0 * ln_n);
    const double corr = (std::log(ln_n) + std::log(4.0 * 3.14159265358979323846))
                        / (2.0 * root);
    return sd * (root - corr);
}

/// Exact aggregates for a session, plus the sample that produced them.
struct SessionReport {
    /// EVERY observation offered to the log, retained or not.
    std::uint64_t observed = 0;
    std::uint64_t by_verdict[kVerdicts] = {};
    std::uint64_t by_kind[kOpportunityKinds] = {};
    /// Actionable count broken out by kind, which is the table the exit
    /// criterion is actually asking for.
    std::uint64_t actionable_by_kind[kOpportunityKinds] = {};

    Notional gross_total{};
    Notional cost_total{};
    Notional net_total{};
    Notional best_net{};
    Notional worst_net{};

    /// Mean and standard deviation of net edge over every observation, in
    /// paise. Welford, so one pass and no accumulated cancellation.
    double net_mean = 0.0;
    double net_m2 = 0.0;

    std::uint64_t retained = 0;

    [[nodiscard]] double net_sd() const noexcept {
        if (observed < 2) { return 0.0; }
        return std::sqrt(net_m2 / static_cast<double>(observed - 1));
    }
    /// The best net edge seen, against what the best of this many pure-noise
    /// observations would have looked like. A ratio near 1 is not a discovery.
    [[nodiscard]] double best_over_noise() const noexcept {
        const double e = expected_extreme_from_noise(observed, net_sd());
        if (!(e > 0.0)) { return 0.0; }
        return static_cast<double>(best_net.raw()) / e;
    }
    [[nodiscard]] std::uint64_t count(Verdict v) const noexcept {
        return by_verdict[static_cast<std::size_t>(v)];
    }
    [[nodiscard]] std::uint64_t count(OpportunityKind k) const noexcept {
        return by_kind[static_cast<std::size_t>(k)];
    }
};

/// Fixed-capacity log: exact aggregates, uniformly sampled detail.
///
/// `push` is allocation-free and branch-light. Nothing here throws, nothing
/// grows, and the reservoir replacement is one multiply and a modulo.
template <std::size_t N>
class OpportunityLog {
public:
    explicit OpportunityLog(std::uint64_t seed = 0x9E3779B97F4A7C15ull) noexcept
        : rng_(seed != 0 ? seed : 0x9E3779B97F4A7C15ull) {}

    /// Record one observation. Returns true when it was retained in the
    /// sample; the aggregates are updated either way, which is the whole
    /// design -- the report is the SESSION and the rows are a draw from it.
    ALTAIR_HOT bool push(const Observation& o) noexcept {
        ++r_.observed;
        ++r_.by_verdict[static_cast<std::size_t>(o.verdict)];
        ++r_.by_kind[static_cast<std::size_t>(o.kind)];
        if (o.verdict == Verdict::Actionable) {
            ++r_.actionable_by_kind[static_cast<std::size_t>(o.kind)];
        }

        r_.gross_total = Notional{r_.gross_total.raw() + o.gross.raw()};
        r_.cost_total = Notional{r_.cost_total.raw() + o.cost.raw()};
        r_.net_total = Notional{r_.net_total.raw() + o.net.raw()};
        if (r_.observed == 1 || o.net.raw() > r_.best_net.raw()) {
            r_.best_net = o.net;
        }
        if (r_.observed == 1 || o.net.raw() < r_.worst_net.raw()) {
            r_.worst_net = o.net;
        }

        // Welford on net, in paise as a double. Exact to 2^53 paise, which is
        // Rs 90.07 lakh crore of single-observation edge -- not the case the
        // P0-01 debt is about.
        const double x = static_cast<double>(o.net.raw());
        const double d = x - r_.net_mean;
        r_.net_mean += d / static_cast<double>(r_.observed);
        r_.net_m2 += d * (x - r_.net_mean);

        // Reservoir. The first N are kept; after that observation number t is
        // kept with probability N/t, replacing a uniformly chosen row. The
        // retained set is then a uniform sample of the whole session rather
        // than of its beginning or its end.
        if (r_.retained < N) {
            rows_[r_.retained] = o;
            ++r_.retained;
            return true;
        }
        const std::uint64_t j = next() % r_.observed;
        if (j < N) {
            rows_[static_cast<std::size_t>(j)] = o;
            return true;
        }
        return false;
    }

    [[nodiscard]] const SessionReport& report() const noexcept { return r_; }
    [[nodiscard]] const Observation* rows() const noexcept { return rows_; }
    [[nodiscard]] std::size_t row_count() const noexcept {
        return static_cast<std::size_t>(r_.retained);
    }
    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return N; }

    void reset(std::uint64_t seed = 0x9E3779B97F4A7C15ull) noexcept {
        r_ = SessionReport{};
        rng_ = seed != 0 ? seed : 0x9E3779B97F4A7C15ull;
    }

private:
    [[nodiscard]] std::uint64_t next() noexcept {
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 7;
        rng_ ^= rng_ << 17;
        return rng_;
    }

    Observation rows_[N] = {};
    SessionReport r_{};
    std::uint64_t rng_;
};

} // namespace altair
