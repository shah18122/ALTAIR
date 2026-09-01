// research/feature_card.hpp -- paper ingest, the feature card, the registry,
// and the builder-scaffold generator.
//
// P7-01 and P7-02.
//
// THE REGISTRY COUNTS ATTEMPTS, NOT SUCCESSES.
//
// This is the card, and it is the same shape as P5-08's: a record of what
// worked cannot answer the question the phase is asking.
//
// Read fifty papers, implement ten, and one replicates at p < 0.05. That one
// is not a discovery -- it is what fifty draws from a null distribution look
// like. The correction requires knowing the number of trials, and the number
// of trials is exactly what a registry of successes has thrown away.
//
// So every paper that is ingested gets a row, and the row survives being
// rejected. `attempts()` is what P7-03's deflated Sharpe divides by, and a
// registry that quietly dropped its failures would hand it a 1 and report
// every fluke as significant.
//
// A PAPER'S CLAIMED RESULT IS EVIDENCE ABOUT THE PAPER'S MARKET.
//
// A US large-cap anomaly measured from 1963 to 1991 is a fact about US large
// caps between 1963 and 1991. It is a HYPOTHESIS about NSE in 2026, and the
// difference is the entire content of this phase. So the card carries the
// claimed effect size AND the venue, period and universe it was claimed on,
// as separate required fields -- there is nowhere to record "Sharpe 1.8"
// without also recording what it was 1.8 on.
//
// THE SCAFFOLD GENERATOR DOES NOT WRITE THE MATHS.
//
// It emits the CONTRACT: the signature, the horizon band, the warmup, the
// registry wiring -- everything mechanical, where a generator is reliable and
// a human is not. It stops at the transform and emits an `#error`.
//
// That is deliberate and it is structural. A generated scaffold that compiles
// is a scaffold that can be shipped with a plausible, confident, wrong
// transform inside it -- which is precisely the failure mode CLAUDE.md's
// review gate 7 exists for. Making the placeholder a compile error means the
// only way to get a building binary is for somebody to have read the paper.

#pragma once

#include <features/registry.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

inline constexpr std::size_t kMaxPapers = 256;
inline constexpr std::size_t kNameLen = 48;
inline constexpr std::size_t kTextLen = 160;

/// Where a paper is in its life. Ordinal 0 is Unknown so a zeroed row cannot
/// pass for one that has been replicated.
enum class PaperStatus : std::uint8_t {
    Unknown = 0,
    /// PDF is in research/papers/inbox and has an id. Nothing read yet.
    Ingested,
    /// A feature card exists: the claim has been written down in a form that
    /// can be tested.
    Specified,
    /// A C++ builder exists and computes something.
    Implemented,
    /// Tested on OUR data and it held. See P7-03 for what that requires.
    Replicated,
    /// Tested on our data and it did not hold. STAYS IN THE REGISTRY.
    Rejected,
    /// Through the P7-04 gate and live.
    Promoted,
    /// Was live, no longer is.
    Retired
};

[[nodiscard]] inline const char* status_name(PaperStatus s) noexcept {
    switch (s) {
        case PaperStatus::Unknown:     return "Unknown";
        case PaperStatus::Ingested:    return "Ingested";
        case PaperStatus::Specified:   return "Specified";
        case PaperStatus::Implemented: return "Implemented";
        case PaperStatus::Replicated:  return "Replicated";
        case PaperStatus::Rejected:    return "Rejected";
        case PaperStatus::Promoted:    return "Promoted";
        case PaperStatus::Retired:     return "Retired";
    }
    return "?";
}

enum class CardError : std::uint8_t {
    /// The registry is full.
    Full,
    /// A required field was empty.
    Incomplete,
    /// The claimed effect had no provenance -- no venue, period or universe.
    NoProvenance,
    /// The transition is not in the lifecycle.
    BadTransition,
    /// No such paper id.
    NotFound,
    /// The output buffer was too small for the scaffold.
    BufferTooSmall
};

/// Where a claimed result came from. All three fields are required.
///
/// Kept as its own struct so `ClaimedEffect` cannot be constructed without it.
/// A Sharpe with no provenance is a number somebody remembers reading.
struct Provenance {
    char venue[kNameLen] = {};       // "NYSE", "NSE", "LSE"
    char universe[kNameLen] = {};    // "US large cap", "NIFTY 50"
    std::int32_t from_year = 0;
    std::int32_t to_year = 0;

    [[nodiscard]] bool complete() const noexcept {
        return venue[0] != '\0' && universe[0] != '\0'
            && from_year > 1900 && to_year >= from_year;
    }
};

/// What the paper says it found.
struct ClaimedEffect {
    /// The paper's headline Sharpe, or whatever it reported converted to one.
    double sharpe = 0.0;
    /// How many observations it rested on. Needed to compare like with like:
    /// a Sharpe of 1.8 over 200 observations and over 20,000 are different
    /// claims.
    std::size_t observations = 0;
    /// Whether the paper's own result was net of transaction costs. Most are
    /// not, and CLAUDE.md's reality check says that is where most published
    /// anomalies go.
    bool net_of_costs = false;
    Provenance where{};
};

/// One paper, as a testable specification.
struct FeatureCard {
    std::uint32_t paper_id = 0;
    char name[kNameLen] = {};
    char citation[kTextLen] = {};
    /// The transform, in words. Quoted into the scaffold as a comment so the
    /// person filling in the maths is looking at the claim while doing it.
    char transform[kTextLen] = {};

    ClaimedEffect claimed{};

    /// The contract the builder must satisfy. Same vocabulary as P5-01, so a
    /// replicated feature drops into the existing registry rather than into a
    /// parallel one.
    HorizonBand band = HorizonBand::Unset;
    Duration lookback{0};
    double expected_min = 0.0;
    double expected_max = 0.0;

    PaperStatus status = PaperStatus::Unknown;

    [[nodiscard]] bool specified() const noexcept {
        return name[0] != '\0' && transform[0] != '\0'
            && band != HorizonBand::Unset && lookback.raw() > 0
            && expected_max > expected_min
            && claimed.observations > 0 && claimed.where.complete();
    }
};

namespace detail {

inline void copy_field(char* dst, const char* src, std::size_t cap) noexcept {
    std::size_t i = 0;
    if (src != nullptr) {
        for (; i + 1 < cap && src[i] != '\0'; ++i) { dst[i] = src[i]; }
    }
    dst[i] = '\0';
}

inline std::size_t append(char* out, std::size_t cap, std::size_t at,
                          const char* s) noexcept {
    std::size_t i = 0;
    while (s[i] != '\0' && at + i + 1 < cap) { out[at + i] = s[i]; ++i; }
    if (at + i < cap) { out[at + i] = '\0'; }
    return at + i;
}

} // namespace detail

/// The research registry. Fixed capacity, no allocation, and it never forgets.
class ResearchRegistry {
public:
    /// Ingest a paper. Nothing is claimed yet -- this is the row existing.
    [[nodiscard]] std::expected<std::uint32_t, CardError>
    ingest(const char* name, const char* citation) noexcept {
        if (n_ >= kMaxPapers) { return std::unexpected(CardError::Full); }
        if (name == nullptr || name[0] == '\0') {
            return std::unexpected(CardError::Incomplete);
        }
        FeatureCard c{};
        c.paper_id = next_id_++;
        detail::copy_field(c.name, name, kNameLen);
        detail::copy_field(c.citation, citation, kTextLen);
        c.status = PaperStatus::Ingested;
        cards_[n_++] = c;
        return c.paper_id;
    }

    /// Attach the specification. Refuses a claimed effect with no provenance:
    /// there is nowhere to record a Sharpe without recording what it was on.
    [[nodiscard]] std::expected<void, CardError>
    specify(std::uint32_t id, const FeatureCard& spec) noexcept {
        FeatureCard* c = find(id);
        if (c == nullptr) { return std::unexpected(CardError::NotFound); }
        if (!spec.claimed.where.complete()) {
            return std::unexpected(CardError::NoProvenance);
        }
        FeatureCard merged = spec;
        merged.paper_id = c->paper_id;
        detail::copy_field(merged.name, c->name, kNameLen);
        detail::copy_field(merged.citation, c->citation, kTextLen);
        if (!merged.specified()) {
            return std::unexpected(CardError::Incomplete);
        }
        merged.status = PaperStatus::Specified;
        *c = merged;
        return {};
    }

    /// Advance the lifecycle. The legal transitions are explicit, so a paper
    /// cannot go from Ingested straight to Promoted because somebody was
    /// confident.
    [[nodiscard]] std::expected<void, CardError>
    advance(std::uint32_t id, PaperStatus to) noexcept {
        FeatureCard* c = find(id);
        if (c == nullptr) { return std::unexpected(CardError::NotFound); }
        const PaperStatus from = c->status;
        const bool ok =
            (from == PaperStatus::Specified   && to == PaperStatus::Implemented)
         || (from == PaperStatus::Implemented && (to == PaperStatus::Replicated
                                               || to == PaperStatus::Rejected))
         || (from == PaperStatus::Replicated  && (to == PaperStatus::Promoted
                                               || to == PaperStatus::Rejected))
         || (from == PaperStatus::Promoted    && to == PaperStatus::Retired);
        if (!ok) { return std::unexpected(CardError::BadTransition); }
        c->status = to;
        return {};
    }

    /// EVERY paper ever ingested, including the rejected ones.
    ///
    /// This is the number P7-03's deflated Sharpe divides by. A registry that
    /// dropped its failures would hand it a 1 and report every fluke as
    /// significant.
    [[nodiscard]] std::size_t attempts() const noexcept { return n_; }

    /// How many reached at least `s`. Used for reporting, never for the
    /// multiple-comparison correction.
    [[nodiscard]] std::size_t count(PaperStatus s) const noexcept {
        std::size_t k = 0;
        for (std::size_t i = 0; i < n_; ++i) {
            if (cards_[i].status == s) { ++k; }
        }
        return k;
    }

    /// How many were actually TESTED against our data -- the trial count for
    /// a multiple-comparison correction, which is not the same as the number
    /// ingested. A paper read and never implemented was never a trial.
    [[nodiscard]] std::size_t trials() const noexcept {
        std::size_t k = 0;
        for (std::size_t i = 0; i < n_; ++i) {
            const PaperStatus s = cards_[i].status;
            if (s == PaperStatus::Replicated || s == PaperStatus::Rejected
                || s == PaperStatus::Promoted || s == PaperStatus::Retired) {
                ++k;
            }
        }
        return k;
    }

    [[nodiscard]] const FeatureCard* get(std::uint32_t id) const noexcept {
        for (std::size_t i = 0; i < n_; ++i) {
            if (cards_[i].paper_id == id) { return &cards_[i]; }
        }
        return nullptr;
    }
    [[nodiscard]] const FeatureCard* at(std::size_t i) const noexcept {
        return i < n_ ? &cards_[i] : nullptr;
    }

private:
    [[nodiscard]] FeatureCard* find(std::uint32_t id) noexcept {
        for (std::size_t i = 0; i < n_; ++i) {
            if (cards_[i].paper_id == id) { return &cards_[i]; }
        }
        return nullptr;
    }

    FeatureCard cards_[kMaxPapers] = {};
    std::size_t n_ = 0;
    std::uint32_t next_id_ = 1;
};

// ---------------------------------------------------------------------------
// P7-02: the scaffold generator
// ---------------------------------------------------------------------------

/// Emit a C++ builder scaffold for a specified card, into `out`.
///
/// Generates the CONTRACT and nothing else: includes, the slots struct, the
/// signature, the band and warmup assertions, the absence discipline. All of
/// that is mechanical, and a generator is more reliable at it than a person.
///
/// It stops at the transform and emits `#error`. A generated scaffold that
/// COMPILES is a scaffold that can be shipped with a plausible, confident,
/// wrong transform inside it -- which is exactly what review gate 7 exists to
/// catch. Making the placeholder a compile error means the only way to get a
/// building binary is for somebody to have read the paper.
[[nodiscard]] inline std::expected<std::size_t, CardError>
emit_scaffold(const FeatureCard& c, char* out, std::size_t cap) noexcept {
    if (out == nullptr || cap < 512) {
        return std::unexpected(CardError::BufferTooSmall);
    }
    if (!c.specified()) { return std::unexpected(CardError::Incomplete); }

    std::size_t at = 0;
    auto w = [&](const char* s) { at = detail::append(out, cap, at, s); };

    w("// GENERATED SCAFFOLD -- P7-02. Contract only; the transform is yours.\n");
    w("//\n// Feature: ");
    w(c.name);
    w("\n// Paper:   ");
    w(c.citation);
    w("\n// Claim:   ");
    w(c.transform);
    w("\n//\n// The paper's claimed effect was measured on ");
    w(c.claimed.where.universe);
    w(" / ");
    w(c.claimed.where.venue);
    w(",\n// which is a fact about that market and a HYPOTHESIS about ours.\n");
    w("// It was ");
    w(c.claimed.net_of_costs ? "net of costs." : "NOT net of costs.");
    w("\n\n#pragma once\n\n#include <features/vector.hpp>\n\n");
    w("namespace altair {\n\n");
    w("struct ");
    w(c.name);
    w("Slots {\n    FeatureIndex value = kSkip;\n");
    w("    /// The estimator's own standard error. Required, not optional:\n");
    w("    /// ROADMAP section 3 sizes on the lower bound of edge.\n");
    w("    FeatureIndex std_error = kSkip;\n};\n\n");
    w("/// Band: ");
    w(band_name(c.band));
    w(".  Warmup: from the card's lookback.\n");
    w("/// A value that is not ready is ABSENT, never zero.\n");
    w("[[nodiscard]] inline int build_");
    w(c.name);
    w("(const double* src, std::size_t n,\n");
    w("       const ");
    w(c.name);
    w("Slots& s, FeatureVector& out) noexcept {\n");
    w("    (void)src; (void)n; (void)s; (void)out;\n\n");
    w("#error \"P7-02 scaffold: implement the transform from the paper, then\"\n");
    w("#error \"delete these two lines. A scaffold that compiles is a scaffold\"\n");
    w("#error \"that can ship with a plausible wrong transform inside it.\"\n\n");
    w("    return 0;\n}\n\n} // namespace altair\n");
    return at;
}

/// Does a scaffold still contain its refusal-to-compile marker?
///
/// Exposed so a build check can assert that nothing under generated/ reached
/// the tree with the placeholder intact -- and so the test can prove the
/// marker is actually there rather than trusting the generator.
[[nodiscard]] inline bool scaffold_is_unfilled(const char* src) noexcept {
    if (src == nullptr) { return false; }
    for (std::size_t i = 0; src[i] != '\0'; ++i) {
        if (src[i] == '#' && src[i + 1] == 'e' && src[i + 2] == 'r'
            && src[i + 3] == 'r' && src[i + 4] == 'o' && src[i + 5] == 'r') {
            return true;
        }
    }
    return false;
}

} // namespace altair
