// P5-08 acceptance tests for strategies/oppty_log.hpp.
//
// Test 1 is the card: four completely different states all present as "no
// actionable opportunities today", and a log that records only winners
// collapses them into an empty file. The conclusion drawn from an empty file
// is always the wrong one of the four.
//
// Test 3 is the sampling: a fixed ring that overwrites reports the close of
// the session and one that stops when full reports the open. Both are measured
// against the reservoir, which reports the session.
//
// Test 4 is the winner's curse: the best of 200,000 noisy observations looks
// like a discovery and is not.
//
// No check description here may contain the substring FAIL.

#include <strategies/oppty_log.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>

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

double rupees(std::int64_t p) { return static_cast<double>(p) / 100.0; }

struct Lcg {
    std::uint64_t s;
    double uniform()
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>((s >> 11) & ((1ULL << 53) - 1))
               / static_cast<double>(1ULL << 53);
    }
    double normal()
    {
        const double u1 = uniform();
        const double u2 = uniform();
        return std::sqrt(-2.0 * std::log(u1 + 1e-300))
               * std::cos(6.283185307179586 * u2);
    }
};

} // namespace

using namespace altair;

namespace {

Observation obs(OpportunityKind k, std::int64_t gross, std::int64_t cost,
                Executability ex, std::int64_t floor_paise,
                std::int64_t ts_ns = 0)
{
    Observation o{};
    o.ts = Timestamp{ts_ns};
    o.kind = k;
    o.executability = ex;
    o.gross = Notional{gross};
    o.cost = Notional{cost};
    o.net = Notional{gross - cost};
    o.verdict = classify(o.gross, o.cost, ex, floor_paise);
    return o;
}

// ── 1 ────────────────────────────────────────────────────────────────────
// THE card.
void four_reasons_for_no_opportunities_look_identical_in_a_winners_log()
{
    std::printf("\n1 four_reasons_for_no_opportunities_look_identical_in_a"
                "_winners_log\n");
    const std::int64_t floor_ = 100;        // Rs 1.00 of mispricing

    OpportunityLog<64> log;
    // A session in which nothing was tradable, for four different reasons.
    for (int i = 0; i < 400; ++i) {         // prices were simply right
        (void)log.push(obs(OpportunityKind::Parity, 40, 900,
                           Executability::Executable, floor_));
    }
    for (int i = 0; i < 250; ++i) {         // real edge, bigger bill
        (void)log.push(obs(OpportunityKind::Box, 30'000, 42'000,
                           Executability::Executable, floor_));
    }
    for (int i = 0; i < 120; ++i) {         // real edge, cannot be placed
        (void)log.push(obs(OpportunityKind::CashFutures, 90'000, 40'000,
                           Executability::ShortCashUnavailable, floor_));
    }
    for (int i = 0; i < 60; ++i) {          // never saw both legs at once
        (void)log.push(obs(OpportunityKind::CrossVenue, 5'000, 600,
                           Executability::QuotesTooFarApart, floor_));
    }

    const SessionReport& r = log.report();
    std::printf("    a session with ZERO actionable opportunities:\n"
                "      observed          %6llu\n"
                "      NoMispricing      %6llu   -> the prices were right;"
                " stop looking\n"
                "      CostExceedsEdge   %6llu   -> edge was real, bill was"
                " bigger; look at cheaper structures\n"
                "      Unreachable       %6llu   -> edge cleared the bill and"
                " cannot be placed; look at the ACCOUNT\n"
                "      StaleQuotes       %6llu   -> we never saw both legs at"
                " once; look at the FEED\n"
                "      Actionable        %6llu\n",
                static_cast<unsigned long long>(r.observed),
                static_cast<unsigned long long>(r.count(OpportunityVerdict::NoMispricing)),
                static_cast<unsigned long long>(r.count(OpportunityVerdict::CostExceedsEdge)),
                static_cast<unsigned long long>(r.count(OpportunityVerdict::Unreachable)),
                static_cast<unsigned long long>(r.count(OpportunityVerdict::StaleQuotes)),
                static_cast<unsigned long long>(r.count(OpportunityVerdict::Actionable)));

    check(r.count(OpportunityVerdict::Actionable) == 0,
          "not one opportunity was actionable all session");
    check(r.observed == 830,
          "and yet 830 observations were recorded -- a log of winners would"
          " hold zero rows and the whole session would read as 'nothing"
          " happened'");
    check(r.count(OpportunityVerdict::NoMispricing) == 400
          && r.count(OpportunityVerdict::CostExceedsEdge) == 250
          && r.count(OpportunityVerdict::Unreachable) == 120
          && r.count(OpportunityVerdict::StaleQuotes) == 60,
          "the four reasons are separated exactly, and they point at four"
          " different places: the market, the cost schedule, the account, and"
          " the feed");
    std::printf("    -> these four are the entire content of the phase's exit"
                " criterion. Collapsed\n       into one empty file they read as"
                " the first of them, which is the only one\n       of the four"
                " that means give up.\n");

    check(r.count(OpportunityVerdict::Unreachable) > 0,
          "the Unreachable bucket in particular is not an error state -- 120"
          " times the edge cleared the bill and P5-05's short-cash constraint"
          " was what stopped it, which is a fact about the account and not"
          " about the market");

    // Ordering: unreachable is decided BEFORE the cost comparison.
    const Observation u = obs(OpportunityKind::CashFutures, 90'000, 40'000,
                              Executability::ShortCashUnavailable, floor_);
    check(u.verdict == OpportunityVerdict::Unreachable && u.net.raw() > 0,
          "an unreachable observation with a POSITIVE net is filed as"
          " Unreachable, not as Actionable -- and not as CostExceedsEdge"
          " either, which would send someone to the cost schedule to fix a"
          " problem that lives in the account");
}

// ── 2 ────────────────────────────────────────────────────────────────────
void gross_lives_in_the_log_and_nowhere_on_a_decision_type()
{
    std::printf("\n2 gross_lives_in_the_log_and_nowhere_on_a_decision_type\n");
    OpportunityLog<16> log;
    (void)log.push(obs(OpportunityKind::Box, 30'000, 42'000,
                       Executability::Executable, 100));
    const SessionReport& r = log.report();
    std::printf("    gross Rs %.2f, cost Rs %.2f, net Rs %.2f\n",
                rupees(r.gross_total.raw()), rupees(r.cost_total.raw()),
                rupees(r.net_total.raw()));
    check(r.gross_total.raw() == 30'000 && r.cost_total.raw() == 42'000,
          "the log keeps gross and cost SEPARATELY, which is what makes 'is"
          " there edge before costs?' answerable at all");
    check(r.net_total.raw() == r.gross_total.raw() - r.cost_total.raw(),
          "and net is their difference exactly, in paise");
    std::printf("    -> rule 5 is not violated by this: BasisOpportunity,"
                " ParityOpportunity and\n       BoxOpportunity have no"
                " gross-edge accessor, and nothing in oms/ includes\n       this"
                " header. The log is a research artefact and is not on the path"
                " to an\n       order.\n");

    // A zeroed row cannot pass for anything.
    Observation zeroed{};
    check(zeroed.kind == OpportunityKind::Unknown
          && zeroed.verdict == OpportunityVerdict::Unknown
          && zeroed.executability == Executability::Unknown,
          "and a zeroed observation is Unknown in all three enums, so an"
          " uninitialised row cannot be counted as a cash-futures observation"
          " that was actionable");
}

// ── 3 ────────────────────────────────────────────────────────────────────
void a_ring_that_overwrites_samples_the_end_of_the_session()
{
    std::printf("\n3 a_ring_that_overwrites_samples_the_end_of_the_session\n");
    constexpr std::size_t kCap = 200;
    constexpr int kSession = 20'000;

    // A session whose character CHANGES: the first half is quiet and the
    // second half is dislocated. Any sampling bias shows up as a wrong
    // estimate of how often the session was dislocated.
    OpportunityLog<kCap> log(12345);
    int keep_first = 0, keep_last = 0;
    for (int i = 0; i < kSession; ++i) {
        const bool late = i >= kSession / 2;
        (void)log.push(obs(late ? OpportunityKind::Box
                                : OpportunityKind::Parity,
                           late ? 50'000 : 40, late ? 20'000 : 900,
                           Executability::Executable, 100,
                           static_cast<std::int64_t>(i)));
    }
    for (std::size_t i = 0; i < log.row_count(); ++i) {
        if (log.rows()[i].ts.ns_since_epoch() < kSession / 2) { ++keep_first; }
        else { ++keep_last; }
    }
    const double frac_late = static_cast<double>(keep_last)
                           / static_cast<double>(log.row_count());
    std::printf("    %d observations, half quiet then half dislocated,"
                " %zu rows kept:\n"
                "      reservoir sample is %d early / %d late  ->  %.1f%% late"
                " (truth 50.0%%)\n"
                "      a ring that OVERWRITES would keep %zu late / 0 early "
                " ->  100.0%% late\n"
                "      a log that STOPS when full would keep 0 late / %zu"
                " early ->    0.0%% late\n",
                kSession, log.row_count(), keep_first, keep_last,
                100.0 * frac_late, kCap, kCap);

    check(frac_late > 0.40 && frac_late < 0.60,
          "the reservoir's retained rows are close to half from each half of"
          " the session, which is the truth");
    check(keep_first > 0 && keep_last > 0,
          "so both regimes survive into the sample -- an overwriting ring"
          " would report a session that was dislocated throughout, and a log"
          " that stopped when full would report one that never was");

    const SessionReport& r = log.report();
    check(r.observed == kSession,
          "and the AGGREGATES cover every observation regardless of what was"
          " retained -- the report is the session, the rows are a draw from"
          " it");
    check(r.count(OpportunityVerdict::Actionable) == kSession / 2,
          "so the actionable count is exact at 10,000, not estimated from 200"
          " sampled rows");
    check(log.row_count() == kCap,
          "while the sample stays at its fixed capacity, with no allocation"
          " anywhere");
}

// ── 4 ────────────────────────────────────────────────────────────────────
void the_best_of_many_noisy_observations_is_not_a_discovery()
{
    std::printf("\n4 the_best_of_many_noisy_observations_is_not_a_discovery\n");
    // A session with NO edge whatsoever: every observation is pure noise with
    // a mean of zero. Exactly what a scanner sees on a market with no
    // arbitrage in it, which per CLAUDE.md is the expected finding.
    constexpr int kScans = 200'000;
    const double sd_paise = 5'000.0;        // Rs 50 of measurement noise
    OpportunityLog<64> log(999);
    Lcg g{0xC0FFEE};
    for (int i = 0; i < kScans; ++i) {
        const auto n = static_cast<std::int64_t>(g.normal() * sd_paise);
        Observation o{};
        o.kind = OpportunityKind::Butterfly;
        o.executability = Executability::Executable;
        o.gross = Notional{n};
        o.cost = Notional{0};
        o.net = Notional{n};
        o.verdict = classify(o.gross, o.cost, o.executability, 100);
        (void)log.push(o);
    }

    const SessionReport& r = log.report();
    const double e = expected_extreme_from_noise(r.observed, r.net_sd());
    std::printf("    %d observations of PURE NOISE, mean %.2f, sd Rs %.2f:\n"
                "      best net edge seen        Rs %8.2f\n"
                "      best of %d pure-noise draws  Rs %8.2f  (expected)\n"
                "      ratio                        %8.2f\n",
                kScans, r.net_mean, r.net_sd() / 100.0,
                rupees(r.best_net.raw()), kScans, e / 100.0,
                r.best_over_noise());

    check(std::fabs(r.net_mean) < 0.05 * r.net_sd(),
          "the session has no edge in it at all -- the mean net is a small"
          " fraction of a standard deviation");
    check(r.best_net.raw() > 4 * static_cast<std::int64_t>(r.net_sd()),
          "and yet the BEST observation is over four standard deviations from"
          " the mean, which in isolation reads as an unmissable opportunity");
    check(r.best_over_noise() > 0.75 && r.best_over_noise() < 1.35,
          "against the expected best of this many noise draws it is right"
          " where it should be -- so the ratio, not the sigma count, is the"
          " number that says whether anything was found");
    std::printf("    -> scan four structures across a strike ladder every tick"
                " and a day is hundreds\n       of thousands of observations."
                " The largest edge among them is the one with\n       the most"
                " measurement error. The report carries `observed` so the best"
                " can be\n       read against it rather than celebrated.\n");

    // Sanity: the calibration responds to n, not just to sd.
    check(expected_extreme_from_noise(1'000'000, sd_paise)
          > expected_extreme_from_noise(1'000, sd_paise),
          "and the bar rises with the number of observations, which is the"
          " entire mechanism -- looking harder finds a bigger maximum with no"
          " more edge behind it");
    check(expected_extreme_from_noise(1, sd_paise) == 0.0
          && expected_extreme_from_noise(1'000, 0.0) == 0.0,
          "with no observations to choose between, or no spread to choose"
          " within, the calibration is zero rather than a small number that"
          " would look like a real threshold");
}

} // namespace

int main()
{
    std::printf("altair arbitrage opportunity log tests\n");
    four_reasons_for_no_opportunities_look_identical_in_a_winners_log();
    gross_lives_in_the_log_and_nowhere_on_a_decision_type();
    a_ring_that_overwrites_samples_the_end_of_the_session();
    the_best_of_many_noisy_observations_is_not_a_discovery();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
