// P8-13 acceptance tests -- against the REAL 35-year daily NIFTY series.
//
// Test 1 is the card: quantile boundaries taken from the whole sample are
// look-ahead, and the number of state labels that change when the boundaries
// are computed from the past only is measured.
//
// Test 2: a transition probability from a handful of observations is not a
// probability, and the thin cells are counted rather than presented.
//
// Test 3: THE QUESTION THAT MATTERS. Does the chain beat "tomorrow is drawn
// from the unconditional distribution"? Measured on the real series, with a
// SHUFFLED control that has the same marginal distribution and no temporal
// structure -- so a rejection means dependence rather than a big sample.
//
// Test 4: an all-zero row stays all-zero rather than becoming uniform.
//
// No check description here may contain the substring FAIL.

#include <models/markov.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#ifndef ALTAIR_DATASET_DIR
#  define ALTAIR_DATASET_DIR "dataset"
#endif

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

using namespace altair;

/// Close prices from a `time,open,high,low,close,volume` file.
///
/// Deliberately minimal: this test is about the model, and pulling in the Qt
/// loader from desktop/ would make an engine test depend on the UI.
std::vector<double> load_closes(const std::string& path)
{
    std::vector<double> out;
    std::ifstream f(path);
    if (!f) {
        return out;
    }
    std::string line;
    bool header = true;
    while (std::getline(f, line)) {
        if (header) {
            header = false;
            if (line.rfind("time", 0) == 0) {
                continue;
            }
        }
        std::size_t col = 0;
        std::size_t start = 0;
        std::string close;
        for (std::size_t i = 0; i <= line.size(); ++i) {
            if (i == line.size() || line[i] == ',') {
                if (col == 4) {
                    close = line.substr(start, i - start);
                    break;
                }
                ++col;
                start = i + 1;
            }
        }
        if (!close.empty()) {
            out.push_back(std::atof(close.c_str()));
        }
    }
    return out;
}

std::string dataset(const char* rel)
{
    return std::string(ALTAIR_DATASET_DIR) + "/" + rel;
}

constexpr std::size_t kStates = 5;
constexpr std::size_t kWarmup = 500;   // two years of daily bars

} // namespace

// ---------------------------------------------------------------------------

static std::vector<double> g_returns;

static void load_once()
{
    const auto closes = load_closes(dataset("spot/nifty/1d/all.csv"));
    g_returns = log_returns(closes);
    std::printf("    loaded %zu daily closes -> %zu log returns\n",
                closes.size(), g_returns.size());
}

static void test_boundaries_are_the_lookahead()
{
    std::printf("\n[1] quantile boundaries from the whole sample are look-ahead\n");

    check(g_returns.size() > 5000,
          "the real 35-year daily series is loaded and long enough to fit a"
          " five-state chain");
    if (g_returns.size() < 1000) {
        return;
    }

    // The contaminated labelling: cut points from every return, including the
    // ones that had not happened.
    std::vector<double> all = g_returns;
    const auto full = quantile_boundaries(all, kStates);
    check(full.has_value(), "full-sample boundaries are computable");
    if (!full) {
        return;
    }

    // The honest labelling: cut points from the past only, recomputed each
    // step.
    const auto honest = expanding_states(g_returns, kStates, kWarmup);
    check(honest.has_value(), "and so is the expanding labelling");
    if (!honest) {
        return;
    }

    std::size_t disagree = 0;
    for (std::size_t i = 0; i < honest->size(); ++i) {
        const std::size_t contaminated = full->classify(g_returns[kWarmup + i]);
        if (contaminated != (*honest)[i]) {
            ++disagree;
        }
    }
    const double pct = 100.0 * static_cast<double>(disagree)
                     / static_cast<double>(honest->size());
    std::printf("    of %zu labelled days, %zu get a DIFFERENT state when the"
                " boundaries come from the past only (%.1f%%)\n",
                honest->size(), disagree, pct);

    check(disagree > 0,
          "the two labellings disagree, so the full-sample cut points were"
          " carrying information from days that had not happened -- and the"
          " line that computes them reads like data preparation, not like"
          " modelling");
    check(pct < 50.0,
          "while most days agree, which is what makes this survive review: the"
          " contaminated model is mostly right, and wrong exactly at the"
          " boundaries where every regime call is made");
}

static void test_thin_cells_are_counted()
{
    std::printf("\n[2] a transition probability from a few observations\n");

    const auto m = fit_expanding(g_returns, kStates, kWarmup);
    check(m.has_value(), "the chain fits");
    if (!m) {
        return;
    }
    std::printf("    %zu transitions over %zu states: %zu cells, thinnest"
                " populated cell holds %u, %zu cells are empty\n",
                m->transitions, m->states, m->states * m->states,
                m->min_count(), m->empty_cells());

    check(m->transitions > 5000,
          "the chain is fitted on thousands of transitions, which is what makes"
          " a five-state model defensible on daily data at all");
    check(m->min_count() > 0,
          "and the thinnest populated cell is reported beside the probability"
          " it produced, so a 0.167 that came from one observation in six is"
          " visible as one");

    // Rows sum to one, or are all zero. Never uniform-by-default.
    for (std::size_t i = 0; i < m->states; ++i) {
        double sum = 0.0;
        for (std::size_t j = 0; j < m->states; ++j) {
            sum += m->p[i][j];
        }
        check(m->row_total[i] == 0 ? sum == 0.0 : std::fabs(sum - 1.0) < 1e-9,
              "each row is a distribution or is empty");
    }

    const auto s = stationary(*m);
    const double total = std::accumulate(s.begin(), s.end(), 0.0);
    std::printf("    stationary distribution:");
    for (double x : s) {
        std::printf(" %.3f", x);
    }
    std::printf("  (sums to %.6f)\n", total);
    check(std::fabs(total - 1.0) < 1e-6,
          "the stationary distribution sums to one, so the chain is ergodic"
          " over the states it actually visits");
}

static void test_is_it_markov_at_all()
{
    std::printf("\n[3] does the chain beat the unconditional distribution?\n");

    const auto real = fit_expanding(g_returns, kStates, kWarmup);
    check(real.has_value(), "the real series fits");
    if (!real) {
        return;
    }
    const IndependenceTest rt = independence_chi_square(*real);

    // THE CONTROL. Shuffling destroys every temporal relationship while
    // leaving the marginal distribution exactly as it was. A statistic that
    // fires on both is measuring sample size, not structure.
    std::vector<double> shuffled = g_returns;
    std::mt19937_64 rng(20260903);
    std::shuffle(shuffled.begin(), shuffled.end(), rng);
    const auto control = fit_expanding(shuffled, kStates, kWarmup);
    check(control.has_value(), "and so does a shuffled control");
    if (!control) {
        return;
    }
    const IndependenceTest ct = independence_chi_square(*control);

    std::printf("    real series : chi2 = %8.2f  df = %zu  crit(5%%) = %.2f"
                "  -> %s\n",
                rt.chi_square, rt.degrees_of_freedom, rt.critical_5pct,
                rt.rejects_independence ? "REJECTS independence"
                                        : "does not reject");
    std::printf("    shuffled    : chi2 = %8.2f  df = %zu  crit(5%%) = %.2f"
                "  -> %s\n",
                ct.chi_square, ct.degrees_of_freedom, ct.critical_5pct,
                ct.rejects_independence ? "REJECTS independence"
                                        : "does not reject");
    std::printf("    thin cells (expected count < 5): real %zu, shuffled %zu\n",
                rt.thin_cells, ct.thin_cells);

    check(rt.degrees_of_freedom == (kStates - 1) * (kStates - 1),
          "the degrees of freedom are (k-1)^2, as a test of independence in a"
          " k-by-k contingency table requires");

    // The finding is whatever it is. Both outcomes are reported, and neither
    // is asserted -- a test that demanded a rejection would be a test that
    // demanded the model work.
    if (rt.rejects_independence && !ct.rejects_independence) {
        std::printf("    -> the real series carries serial dependence the"
                    " shuffle destroys: there is something here\n");
    } else if (rt.rejects_independence && ct.rejects_independence) {
        std::printf("    -> BOTH reject, so the statistic is responding to"
                    " sample size rather than to structure\n");
    } else {
        std::printf("    -> the chain does not beat the unconditional"
                    " distribution; a regime label from it is decoration\n");
    }
    check(true,
          "and whichever it is, it is REPORTED -- a fitted transition matrix"
          " always looks like structure, and this is the only thing that says"
          " whether it is");
}

static void test_an_empty_row_stays_empty()
{
    std::printf("\n[4] an unobserved row is empty, not uniform\n");

    // Three states, but nothing ever enters state 2.
    std::vector<std::size_t> seq{0, 1, 0, 1, 1, 0, 0, 1};
    const auto m = fit_from_states(seq, 3);
    check(m.has_value(), "the chain fits over three declared states");
    if (!m) {
        return;
    }
    check(m->row_total[2] == 0, "state 2 was never visited");
    double row2 = 0.0;
    for (std::size_t j = 0; j < 3; ++j) {
        row2 += m->p[2][j];
    }
    check(row2 == 0.0,
          "and its row is all zero rather than a uniform 1/3 -- a uniform row"
          " is a claim that every transition out of a state nobody has seen is"
          " equally likely, which is a claim from no evidence at all");

    std::vector<double> flat(10, 1.0);
    check(!quantile_boundaries(flat, 3).has_value(),
          "and a series with no variation is refused rather than fitted into"
          " states nothing can land in");
}


// P33-03. THE CRITICAL TABLE STOPPED ONE SHORT OF THE BIGGEST CHAIN.
//
// A k-state chain tests independence with (k-1)^2 degrees of freedom, so
// kMaxStates = 9 needs df 64 -- and the table ended at 63. The lookup returned
// 0.0, `rejects_independence` requires a positive critical value, and a
// nine-state chain therefore could never reject, whatever the data said.
//
// Silent, and in the worse direction: a test that always answers "no
// structure" on a model built to find structure looks exactly like an honest
// negative result, and this tree is full of honest negative results.
void the_largest_chain_can_still_reject()
{
    std::printf("\nN the_largest_chain_can_still_reject\n");

    for (std::size_t k = 2; k <= kMaxStates; ++k) {
        const std::size_t df = (k - 1) * (k - 1);
        const double c = chi2_critical_5pct(df);
        std::printf("    %zu states -> df %2zu -> critical %7.3f%s\n",
                    k, df, c, c > 0.0 ? "" : "   <- UNAVAILABLE");
        check(c > 0.0,
              "every state count the API allows has a critical value -- a "
              "missing one silently disables the test rather than failing it");
    }

    // Spot-checked against a computed inverse regularised incomplete gamma,
    // not copied from a printed table. P23-08's ks_critical was wrong by a
    // factor of 27 because a critical value was taken on trust.
    check(std::fabs(chi2_critical_5pct(1) - 3.841) < 0.001,
          "df 1 is 3.841");
    check(std::fabs(chi2_critical_5pct(16) - 26.296) < 0.001,
          "df 16, a five-state chain, is 26.296");
    check(std::fabs(chi2_critical_5pct(64) - 83.675) < 0.001,
          "and df 64, a NINE-state chain, is 83.675 -- the entry that was "
          "missing");

    // A nine-state chain with real structure must now actually reject. Built
    // so the answer is known before the test: the next state is the current
    // one two times in three.
    std::vector<std::size_t> seq;
    std::uint64_t s = 12345u;
    std::size_t cur = 0;
    for (int i = 0; i < 20000; ++i) {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        const double u =
            static_cast<double>((s >> 11) & 0xFFFFFFFFull) / 4294967296.0;
        if (u > 0.667) { cur = static_cast<std::size_t>(u * 9.0) % 9; }
        seq.push_back(cur);
    }
    const auto m = fit_from_states(seq, 9);
    check(m.has_value(), "a nine-state chain fits");
    if (m) {
        const auto t = independence_chi_square(*m);
        std::printf("    nine states, sticky chain: chi2 %.1f vs critical "
                    "%.1f, df %zu\n",
                    t.chi_square, t.critical_5pct, t.degrees_of_freedom);
        check(t.critical_5pct > 0.0,
              "the nine-state critical value is available");
        check(t.rejects_independence,
              "and a chain that stays put two times in three REJECTS "
              "independence -- which it could not do at all before");
    }
}

int main()
{
    std::printf("P8-13 -- a Markov regime chain on the real daily NIFTY\n");
    load_once();
    if (g_returns.empty()) {
        std::printf("  dataset not found at %s -- nothing measured\n",
                    ALTAIR_DATASET_DIR);
        return 1;
    }
    test_boundaries_are_the_lookahead();
    test_thin_cells_are_counted();
    test_is_it_markov_at_all();
    test_an_empty_row_stays_empty();

    the_largest_chain_can_still_reject();

    std::printf("\n%s\n", failures == 0 ? "all checks passed"
                                        : "checks did not pass");
    return failures == 0 ? 0 : 1;
}
