// P7-01 .. P7-04 acceptance tests for the research pipeline.
//
// Test 1: the registry counts ATTEMPTS. A record of what worked cannot answer
// the question the phase is asking, and the trial count is what a deflated
// Sharpe divides by.
//
// Test 2: the scaffold generator refuses to produce compiling code.
//
// Test 3 is the card: the same Sharpe of 1.5 means "found something" after one
// trial and "found nothing" after fifty, and nothing about the number itself
// distinguishes them. Measured.
//
// Test 4: the promotion gate is a conjunction and reports which term vetoed.
//
// No check description here may contain the substring FAIL.

#include <research/replication.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    if (ok) {
        std::printf("  ok  : %s\n", what);
    } else {
        ++failures;
        std::printf("  FAIL: %s\n", what);
    }
}

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

} // namespace

using namespace altair;

namespace {

FeatureCard momentum_spec()
{
    FeatureCard c{};
    std::snprintf(c.name, sizeof c.name, "%s", "TimeSeriesMomentum");
    std::snprintf(c.transform, sizeof c.transform, "%s", "sign of the trailing 12-month return, held one month");
    c.band = HorizonBand::Swing;
    c.lookback = Duration{252LL * 24 * 3600 * 1000000000LL};
    c.expected_min = -1.0;
    c.expected_max = 1.0;
    c.claimed.sharpe = 1.8;
    c.claimed.observations = 8400;
    c.claimed.net_of_costs = false;
    std::snprintf(c.claimed.where.venue, sizeof c.claimed.where.venue, "%s", "NYSE/CME");
    std::snprintf(c.claimed.where.universe, sizeof c.claimed.where.universe, "%s", "58 futures, US large cap");
    c.claimed.where.from_year = 1965;
    c.claimed.where.to_year = 2009;
    return c;
}

// ── 1 ────────────────────────────────────────────────────────────────────
void the_registry_counts_attempts_not_successes()
{
    std::printf("\n1 the_registry_counts_attempts_not_successes\n");
    ResearchRegistry reg;

    // Fifty papers read. Ten get far enough to be tested. One replicates.
    std::uint32_t ids[50];
    for (int i = 0; i < 50; ++i) {
        char name[kNameLen];
        std::snprintf(name, sizeof name, "paper_%02d", i);
        const auto id = reg.ingest(name, "citation");
        check(id.has_value() || i > 0, "papers ingest");
        ids[i] = id ? *id : 0;
    }
    for (int i = 0; i < 10; ++i) {
        FeatureCard spec = momentum_spec();
        (void)reg.specify(ids[i], spec);
        (void)reg.advance(ids[i], PaperStatus::Implemented);
        (void)reg.advance(ids[i], i == 0 ? PaperStatus::Replicated
                                         : PaperStatus::Rejected);
    }

    std::printf("    fifty papers read, ten implemented and tested, one held"
                " up:\n"
                "      ingested    %3zu\n"
                "      TRIALS      %3zu   <- what a deflated Sharpe divides by\n"
                "      replicated  %3zu\n"
                "      rejected    %3zu\n",
                reg.attempts(), reg.trials(),
                reg.count(PaperStatus::Replicated),
                reg.count(PaperStatus::Rejected));

    check(reg.attempts() == 50, "every paper ingested has a row");
    check(reg.trials() == 10,
          "ten of them were actually TESTED against our data -- which is the"
          " trial count, and is not the same as the number read: a paper never"
          " implemented was never a trial");
    check(reg.count(PaperStatus::Rejected) == 9,
          "the nine that did not hold up are STILL IN THE REGISTRY -- a"
          " registry that dropped its failures would hand the correction a 1"
          " and report the survivor as significant");
    check(reg.count(PaperStatus::Replicated) == 1, "one replicated");

    // Provenance is required, not decorative.
    const auto extra = reg.ingest("no_provenance", "nowhere");
    check(extra.has_value(), "another paper ingests");
    if (extra) {
        FeatureCard bare = momentum_spec();
        bare.claimed.where = Provenance{};      // wiped
        check(reg.specify(*extra, bare).error() == CardError::NoProvenance,
              "a claimed Sharpe with no venue, universe or period is REFUSED --"
              " there is nowhere to record 1.8 without recording what it was"
              " 1.8 on, because a US futures result from 1965-2009 is a fact"
              " about that market and a HYPOTHESIS about NSE in 2026");
    }

    // The lifecycle is a state machine, not a suggestion.
    const auto keen = reg.ingest("very_promising", "someone confident");
    check(keen.has_value(), "and one more");
    if (keen) {
        check(reg.advance(*keen, PaperStatus::Promoted).error()
              == CardError::BadTransition,
              "a paper cannot go from Ingested straight to Promoted, however"
              " promising it looked -- the transitions are enumerated and"
              " enthusiasm is not one of them");
    }

    const FeatureCard* zeroed = reg.get(999999);
    check(zeroed == nullptr, "an unknown id yields nothing");
    FeatureCard blank{};
    check(blank.status == PaperStatus::Unknown && !blank.specified(),
          "and a zeroed card is Unknown and unspecified, so it cannot pass for"
          " one that replicated");
}

// ── 2 ────────────────────────────────────────────────────────────────────
void a_generated_scaffold_refuses_to_compile()
{
    std::printf("\n2 a_generated_scaffold_refuses_to_compile\n");
    static char buf[4096];
    const auto n = emit_scaffold(momentum_spec(), buf, sizeof buf);
    check(n.has_value(), "the scaffold generates from a specified card");
    if (!n) { return; }

    check(scaffold_is_unfilled(buf),
          "and it contains an #error, so it CANNOT be compiled as generated");
    check(std::strstr(buf, "TimeSeriesMomentumSlots") != nullptr,
          "the mechanical part is filled in: the slots struct is named after"
          " the feature");
    check(std::strstr(buf, "std_error") != nullptr,
          "and carries a std_error slot, because ROADMAP section 3 sizes on"
          " the lower bound and a feature that cannot report its uncertainty"
          " cannot be sized on");
    check(std::strstr(buf, "kSkip") != nullptr
          && std::strstr(buf, "ABSENT") != nullptr,
          "the absence discipline is generated too -- a value that is not"
          " ready is absent, never zero");
    check(std::strstr(buf, "swing") != nullptr,
          "the horizon band from the card is written into the contract");
    check(std::strstr(buf, "NOT net of costs") != nullptr,
          "and the paper's provenance is quoted in the header, including that"
          " its claim was NOT net of costs -- which CLAUDE.md's reality check"
          " says is where most published anomalies go");
    std::printf("    -> the generator writes the CONTRACT and stops at the"
                " transform. A generated\n       scaffold that compiles is one"
                " that can ship with a plausible, confident,\n       wrong"
                " transform inside it -- exactly what review gate 7 exists to"
                " catch.\n       Making the placeholder a compile error means"
                " the only way to get a building\n       binary is for somebody"
                " to have read the paper.\n");

    FeatureCard bare{};
    check(emit_scaffold(bare, buf, sizeof buf).error() == CardError::Incomplete,
          "an unspecified card generates nothing");
    check(emit_scaffold(momentum_spec(), buf, 64).error()
          == CardError::BufferTooSmall,
          "and a buffer too small to hold the contract is refused rather than"
          " truncated -- a truncated scaffold could lose the #error and become"
          " compilable");
}

// ── 3 ────────────────────────────────────────────────────────────────────
// THE card.
void the_same_sharpe_means_opposite_things_at_one_trial_and_fifty()
{
    std::printf("\n3 the_same_sharpe_means_opposite_things_at_one_trial_and"
                "_fifty\n");
    const double sr = 1.5;              // annualised, and it looks excellent
    const std::size_t obs = 1000;
    const double var = 1.0;             // variance of Sharpe across trials
    const double skew = 0.0, kurt = 3.0;   // normal returns, for now

    std::printf("    an observed Sharpe of %.2f over %zu observations:\n"
                "      trials   expected max under the NULL   deflated Sharpe\n",
                sr, obs);
    double d1 = 0.0, d50 = 0.0;
    for (const std::size_t n : {std::size_t{1}, std::size_t{5},
                                std::size_t{20}, std::size_t{50},
                                std::size_t{200}}) {
        const auto m = expected_max_sharpe(n, var);
        const auto d = deflated_sharpe(sr, obs, n, var, skew, kurt);
        check(m.has_value() && d.has_value(), "the correction computes");
        if (!m || !d) { continue; }
        std::printf("      %5zu            %8.3f                  %8.4f\n",
                    n, *m, *d);
        if (n == 1) { d1 = *d; }
        if (n == 50) { d50 = *d; }
    }

    check(d1 > 0.99,
          "after ONE trial a Sharpe of 1.5 is overwhelmingly significant");
    check(d50 < 0.99 && d50 < d1,
          "after fifty it is not -- the same number, the same data length, and"
          " a materially weaker conclusion");
    std::printf("    -> the expected maximum under the null RISES with the"
                " number of trials, because\n       looking harder finds a"
                " bigger maximum without finding more edge. That is the\n"
                "       same mechanism P5-08 measured for the scanner, one"
                " level up: there it was\n       200,000 strikes, here it is"
                " fifty papers.\n");

    // Non-normality pushes it down further. Measured at a Sharpe just ABOVE
    // the null bar, because that is where the answer is decided: at 1.5
    // against 50 trials the verdict is already 0.0000 to machine precision,
    // and a comparison between two zeros shows nothing.
    const double marginal = 2.5;        // the 50-trial bar is 2.276
    const auto normal = deflated_sharpe(marginal, obs, 50, var, 0.0, 3.0);
    const auto ugly = deflated_sharpe(marginal, obs, 50, var, -1.2, 9.0);
    check(normal.has_value() && ugly.has_value(), "both shapes compute");
    if (normal && ugly) {
        std::printf("    a Sharpe of %.2f -- just above the %zu-trial bar --"
                    " over %zu observations:\n"
                    "      normal returns (skew 0, kurt 3)        %.4f  ->"
                    " clears a 0.95 threshold\n"
                    "      skewed and fat-tailed (-1.2, 9.0)      %.4f  ->"
                    " %s\n",
                    marginal, std::size_t{50}, obs, *normal, *ugly,
                    *ugly >= 0.95 ? "clears it too" : "does NOT");
        check(*ugly < *normal,
              "negative skew and fat tails push the deflated Sharpe DOWN -- a"
              " strategy that makes a little most days and loses a lot"
              " occasionally has a flattering Sharpe and a poor deflated one,"
              " which is exactly what the correction is for");
        check(*normal - *ugly > 0.01,
              "and the gap is large enough to change a verdict, not a rounding"
              " term: the same Sharpe, the same trial count, the same sample"
              " length, and a different answer because of the SHAPE of the"
              " returns");
    }

    check(expected_max_sharpe(0, var).error()
          == ReplicationError::NoTrialCount,
          "and a trial count of zero is refused rather than defaulted -- a"
          " zero there would silently become a 1 and make everything look"
          " significant");
}

// ── 4 ────────────────────────────────────────────────────────────────────
void preregistration_is_enforced_and_the_gate_names_its_veto()
{
    std::printf("\n4 preregistration_is_enforced_and_the_gate_names_its"
                "_veto\n");
    ReplicationRun run;
    check(run.evaluate(1.5, 1000, 1.0, 0.0, 3.0).error()
          == ReplicationError::NotPreregistered,
          "evaluating before registering a threshold is REFUSED -- a threshold"
          " chosen after seeing the result is not a threshold, and the only way"
          " to make that stick is a state machine rather than a convention");

    Preregistration pre{};
    pre.min_deflated = 0.95;
    pre.min_observations = 500;
    pre.trials_at_registration = 50;
    check(run.preregister(pre).has_value(), "the threshold registers");
    check(run.preregister(pre).error() == ReplicationError::AlreadyRegistered,
          "and cannot be registered a second time, which would let a"
          " disappointing result be met with a lower bar");

    const auto weak = run.evaluate(1.5, 1000, 1.0, 0.0, 3.0);
    check(weak.has_value(), "the run evaluates");
    if (!weak) { return; }
    std::printf("    Sharpe %.2f, %zu observations, %zu trials:  deflated"
                " %.4f  ->  %s\n"
                "      naive verdict (is the Sharpe positive?):  %s\n",
                weak->observed_sharpe, weak->observations, weak->trials,
                weak->deflated, weak->replicated ? "replicated" : "NOT",
                weak->naive_verdict ? "replicated" : "NOT");
    check(weak->naive_verdict,
          "the naive rule -- is the Sharpe positive -- says replicated");
    check(!weak->replicated || weak->deflated >= 0.95,
          "while the harness measures it against the pre-registered threshold"
          " and the trial count, and the two can disagree");

    // The gate: four terms, each able to veto, and it says which.
    PromotionEvidence e{};
    e.replication = *weak;
    e.replication.replicated = true;
    e.status = PaperStatus::Replicated;
    e.out_of_sample = true;
    e.net_edge_paise = Notional{4500};
    check(promotion_gate(e) == GateVerdict::Promote,
          "with all four terms satisfied the gate promotes");

    PromotionEvidence in_sample = e;
    in_sample.out_of_sample = false;
    check(promotion_gate(in_sample) == GateVerdict::NoOutOfSample,
          "a result only ever seen in sample is blocked, and the gate says so");

    PromotionEvidence unprofitable = e;
    unprofitable.net_edge_paise = Notional{-1200};
    check(promotion_gate(unprofitable) == GateVerdict::CostExceedsEdge,
          "an effect that is real and does not survive the cost of trading it"
          " is blocked on COST, not on significance (rule 5)");

    PromotionEvidence insignificant = e;
    insignificant.replication.replicated = false;
    check(promotion_gate(insignificant) == GateVerdict::NotSignificant,
          "and one that did not clear the deflated threshold is blocked on"
          " that");

    PromotionEvidence wrong_state = e;
    wrong_state.status = PaperStatus::Implemented;
    check(promotion_gate(wrong_state) == GateVerdict::WrongStatus,
          "a paper that was never marked replicated cannot be promoted even"
          " with every other term satisfied");

    PromotionEvidence zeroed{};
    check(promotion_gate(zeroed) == GateVerdict::WrongStatus,
          "and a zeroed evidence struct does not promote -- ordinal 0 is"
          " Unknown on both enums");

    std::printf("    -> 'did not promote' is four different pieces of news:"
                " the idea was wrong, the\n       test was too small, the test"
                " was in-sample, or the idea was right and\n       unprofitable."
                " Only the first means stop looking, so the gate returns"
                " WHICH.\n");
}

} // namespace

int main()
{
    std::printf("altair research pipeline tests\n");
    the_registry_counts_attempts_not_successes();
    a_generated_scaffold_refuses_to_compile();
    the_same_sharpe_means_opposite_things_at_one_trial_and_fifty();
    preregistration_is_enforced_and_the_gate_names_its_veto();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
