// P10-01, P10-02 and P10-03 acceptance tests.
//
// Test 1 is the card: a fundamental keyed on its period end hands you earnings
// six weeks before anyone had them. Measured in days and in what it is worth.
//
// Test 2: a negative denominator makes a multiple meaningless, and it sorts
// FIRST.
//
// Test 3: FCFF at WACC and FCFE at the cost of equity, and what mixing them
// costs. Plus how much of a DCF is the perpetuity.
//
// No check description here may contain the substring FAIL.

#include <models/dcf.hpp>
#include <strategies/fundamentals.hpp>

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

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

constexpr std::int64_t kDay = 24LL * 3600 * 1000000000LL;

} // namespace

using namespace altair;

namespace {

Fundamental row(std::uint32_t id, std::int64_t period_day,
                std::int64_t filed_day, std::uint8_t rev,
                std::int64_t net_income)
{
    Fundamental f{};
    f.instrument = id;
    f.sector = 1;
    f.period_end = Timestamp{period_day * kDay};
    f.filed = Timestamp{filed_day * kDay};
    f.revision = rev;
    f.revenue = Notional{10'000'000'00};
    f.ebitda = Notional{2'000'000'00};
    f.net_income = Notional{net_income};
    f.equity = Notional{8'000'000'00};
    f.total_debt = Notional{3'000'000'00};
    f.cash = Notional{1'000'000'00};
    f.free_cash_flow = Notional{900'000'00};
    f.shares_outstanding = Qty{1'000'000};
    return f;
}

// ── 1 ────────────────────────────────────────────────────────────────────
// THE card.
void a_fundamental_keyed_on_its_period_end_is_six_weeks_of_hindsight()
{
    std::printf("\n1 a_fundamental_keyed_on_its_period_end_is_six_weeks_of"
                "_hindsight\n");
    FundamentalStore store;
    // Q2 ends on day 273 (30 September). SEBI allows 45 days; this one filed
    // on day 318, which is ordinary.
    const auto q2 = row(101, 273, 318, 0, 1'200'000'00);
    check(store.insert(q2).has_value(), "the Q2 filing inserts");

    std::printf("    Q2 period ends day %lld, FILED day %lld -- a reporting lag"
                " of %.0f days\n",
                static_cast<long long>(273), static_cast<long long>(318),
                FundamentalStore::reporting_lag_days(q2));
    check(near(FundamentalStore::reporting_lag_days(q2), 45.0, 0.5),
          "forty-five days between the quarter ending and the number being"
          " published -- which is the SEBI limit and the ordinary case, not an"
          " outlier");

    // The whole point: asking on any day in that window gets nothing.
    check(store.as_of(101, Timestamp{280 * kDay}).error()
          == FundamentalError::NotYetFiled,
          "asked on day 280, a week after the quarter ended, the store returns"
          " NOTHING -- because nobody had the number");
    check(store.as_of(101, Timestamp{317 * kDay}).error()
          == FundamentalError::NotYetFiled,
          "and still nothing the day before it was filed");
    const auto after = store.as_of(101, Timestamp{318 * kDay});
    check(after.has_value() && after->net_income.raw() == 1'200'000'00,
          "on the filing day it appears, and not before");
    std::printf("    -> a database keyed on PERIOD END would have served this"
                " on day 273. Every screen\n       built on it trades %.0f days"
                " of hindsight on the single most price-moving\n       number a"
                " company publishes, and the backtest looks extraordinary.\n",
                FundamentalStore::reporting_lag_days(q2));

    // A loader that put the period end in the filing column is caught at the
    // door.
    Fundamental bad = q2;
    bad.filed = Timestamp{273 * kDay - kDay};
    check(store.insert(bad).error() == FundamentalError::FiledBeforePeriod,
          "a row whose filing date precedes its period is REFUSED at insertion"
          " -- that is what a loader does when it puts the period end in the"
          " filing column, and it is the commonest ingest bug of the lot");
    Fundamental undated = q2;
    undated.filed = Timestamp{};
    check(store.insert(undated).error() == FundamentalError::NoFilingDate,
          "and a row with no filing date is rejected rather than defaulted to"
          " its period end, because that default IS the bug");

    // THE RESTATEMENT: a second datum, not a correction.
    const auto restated = row(101, 273, 318 + 400, 1, 700'000'00);
    check(store.insert(restated).has_value(), "the restatement inserts");
    const auto before_restate = store.as_of(101, Timestamp{400 * kDay});
    const auto after_restate = store.as_of(101, Timestamp{800 * kDay});
    check(before_restate.has_value()
          && before_restate->net_income.raw() == 1'200'000'00,
          "asked on day 400 the store returns the ORIGINAL figure, because"
          " that is what was on the record then");
    check(after_restate.has_value()
          && after_restate->net_income.raw() == 700'000'00,
          "and asked on day 800 it returns the restated one");
    std::printf("    the same quarter, restated from Rs %.0f lakh to Rs %.0f"
                " lakh 400 days later:\n"
                "      as of day 400 -> Rs %.0f lakh   (what was known)\n"
                "      as of day 800 -> Rs %.0f lakh   (what is known now)\n",
                1'200'000'00 / 100000.0, 700'000'00 / 100000.0,
                static_cast<double>(before_restate->net_income.raw()) / 100000.0,
                static_cast<double>(after_restate->net_income.raw()) / 100000.0);
    std::printf("    -> a vendor that OVERWRITES the original with the restated"
                " figure is worse than the\n       period-end bug, because it"
                " is invisible: the filing date is still right.\n");
}

// ── 2 ────────────────────────────────────────────────────────────────────
void a_negative_denominator_sorts_first()
{
    std::printf("\n2 a_negative_denominator_sorts_first\n");
    const Price px{50'000};             // Rs 500

    const auto profitable = row(1, 0, 1, 0, 500'000'00);
    const auto loss_making = row(2, 0, 1, 0, -300'000'00);

    const auto pe_good = price_to_earnings(px, profitable);
    const auto pe_bad = price_to_earnings(px, loss_making);
    check(pe_good.has_value(), "a profitable company has a P/E");
    check(!pe_bad && pe_bad.error() == FundamentalError::NonPositiveDenominator,
          "and a loss-making one is REFUSED rather than given a negative P/E");

    // What the negative P/E would have been, and where it sorts.
    const double eps_bad = -300'000'00.0 / 1'000'000.0;
    const double naive_pe = static_cast<double>(px.raw()) / eps_bad;
    std::printf("    price Rs %.2f:\n"
                "      profitable company   P/E %+8.2f\n"
                "      loss-making company  P/E %+8.2f   <- sorts FIRST"
                " ascending\n",
                static_cast<double>(px.raw()) / 100.0, *pe_good, naive_pe);
    check(naive_pe < *pe_good,
          "the loss-maker's negative P/E sorts ahead of every profitable"
          " company, so a 'cheapest first' screen returns exactly the names it"
          " was built to avoid -- and the ordering looks perfectly sensible");

    // The same trap on quality.
    Fundamental negative_book = profitable;
    negative_book.equity = Notional{-2'000'000'00};
    negative_book.net_income = Notional{-500'000'00};
    const auto roe = return_on_equity(negative_book);
    check(!roe && roe.error() == FundamentalError::NonPositiveDenominator,
          "and a loss on NEGATIVE book value is refused too -- its ROE comes"
          " out POSITIVE, so a quality screen would rank the most distressed"
          " name in the universe as the highest quality");
    std::printf("    -> a loss of Rs -%.0f lakh on equity of Rs -%.0f lakh"
                " gives an ROE of %+.3f.\n       Two negatives, one confident"
                " number, and it is at the top of the screen.\n",
                500'000'00 / 100000.0, 2'000'000'00 / 100000.0,
                -500'000'00.0 / -2'000'000'00.0);

    // Relative-to-sector needs a real sector.
    double peers[6] = {12.0, 15.0, 18.0, 22.0, 25.0, 31.0};
    const auto rel = relative_to_sector(12.0, peers, 6);
    check(rel.has_value(), "a six-name sector produces a median");
    if (rel) {
        std::printf("    a P/E of 12.0 against a sector median of %.1f from %zu"
                    " names: relative %.3f\n",
                    rel->sector_median, rel->sector_n, rel->relative);
        check(near(rel->sector_median, 20.0, 1e-9),
              "the median of six is the mean of the middle two");
        check(rel->relative < 1.0, "and a P/E below it is relatively cheap");
    }
    double three[3] = {12.0, 15.0, 18.0};
    check(relative_to_sector(12.0, three, 3).error()
          == FundamentalError::TooFewNames,
          "a sector of three is refused -- a median of three is one"
          " observation and a median of two is a mean, so the count travels"
          " with every relative score");
}

// ── 3 ────────────────────────────────────────────────────────────────────
void fcff_takes_wacc_and_fcfe_takes_the_cost_of_equity()
{
    std::printf("\n3 fcff_takes_wacc_and_fcfe_takes_the_cost_of_equity\n");
    GrowthProfile g{};
    g.high_years = 5;
    g.high_growth = 0.14;
    g.fade_years = 5;
    g.terminal_growth = 0.045;

    const double ke = 0.125, kd = 0.085, tax = 0.25;
    const double e = 8'000'000'00.0, d = 3'000'000'00.0;
    const auto w = wacc(e, d, ke, kd, tax);
    check(w.has_value(), "the WACC computes");
    if (!w) { return; }

    const double fcff = 900'000'00.0;
    const auto ev = dcf_fcff(fcff, g, *w);
    const auto mixed = dcf_fcff(fcff, g, ke);   // THE ERROR: FCFF at Ke
    check(ev.has_value() && mixed.has_value(), "both discountings run");
    if (!ev || !mixed) { return; }

    const double eq_correct = equity_from_enterprise(*ev, Notional{3'000'000'00},
                                                     Notional{1'000'000'00});
    const double eq_mixed = equity_from_enterprise(*mixed,
                                                   Notional{3'000'000'00},
                                                   Notional{1'000'000'00});
    std::printf("    WACC %.4f against a cost of equity of %.4f (%.0f%%"
                " debt-funded):\n"
                "      FCFF at WACC          enterprise Rs %12.0f  equity"
                " Rs %12.0f\n"
                "      FCFF at Ke  (WRONG)   enterprise Rs %12.0f  equity"
                " Rs %12.0f\n"
                "      the mistake is worth %.1f%% of the equity value\n",
                *w, ke, 100.0 * d / (d + e),
                ev->value(), eq_correct, mixed->value(), eq_mixed,
                100.0 * (eq_correct - eq_mixed) / eq_correct);
    check(*w < ke,
          "the WACC is below the cost of equity, because the debt leg is"
          " cheaper and carries a tax shield");
    check(eq_correct > eq_mixed,
          "so discounting FCFF at the cost of equity UNDERSTATES the value --"
          " by roughly the tax shield, and by more the more leveraged the"
          " company is");
    check((eq_correct - eq_mixed) / eq_correct > 0.05,
          "and it is not a rounding difference: both numbers have the right"
          " units, the right order of magnitude and a confident air");
    std::printf("    -> which is why the parameters are named `wacc` and"
                " `cost_of_equity` rather than\n       `discount_rate`, and why"
                " EnterpriseValue and EquityValue are different types:\n"
                "       equity_from_enterprise has no EquityValue overload, so"
                " double-subtracting\n       the debt FCFE already paid is a"
                " compile error.\n");

    // THE TERMINAL VALUE IS MOST OF THE ANSWER.
    std::printf("    the ten explicit years contribute Rs %.0f and the"
                " perpetuity Rs %.0f\n"
                "      TERMINAL SHARE %.1f%%\n",
                ev->dcf.pv_explicit, ev->dcf.pv_terminal,
                100.0 * ev->dcf.terminal_share);
    check(ev->dcf.terminal_share > 0.5,
          "more than half the valuation is the perpetuity -- so a DCF is"
          " mostly an assumption about year eleven onward, dressed as a"
          " ten-year forecast");
    const auto sens = terminal_growth_sensitivity(fcff, g, *w);
    check(sens.has_value(), "the sensitivity computes");
    if (sens) {
        std::printf("      and 50bp on the terminal growth rate moves the whole"
                    " valuation by %.1f%%\n", 100.0 * *sens * 50.0);
        check(std::fabs(*sens) * 50.0 > 0.03,
              "fifty basis points of an unobservable perpetual growth rate"
              " moves the answer more than the ten years of detailed modelling"
              " that precede it, which is what the terminal share was already"
              " saying");
    }

    // g >= r is refused before anything is computed.
    GrowthProfile runaway = g;
    runaway.terminal_growth = *w + 0.01;
    check(dcf_fcff(fcff, runaway, *w).error()
          == DcfError::GrowthExceedsDiscount,
          "a terminal growth above the discount rate is REFUSED -- the Gordon"
          " perpetuity is infinite at equality and negative above it, and a"
          " large negative enterprise value looks like a bug somewhere else"
          " entirely");

    // FCFE goes to equity directly, with no debt subtraction.
    const auto eqv = dcf_fcfe(700'000'00.0, g, ke);
    check(eqv.has_value(), "FCFE discounts at the cost of equity");
    if (eqv) {
        std::printf("    FCFE Rs %.0f at Ke %.3f -> equity Rs %.0f directly,"
                    " no debt subtraction\n",
                    700'000'00.0, ke, eqv->value());
        check(eqv->value() > 0.0, "and gives a positive equity value");
    }
}

} // namespace

int main()
{
    std::printf("altair fundamentals, DCF and screening tests\n");
    a_fundamental_keyed_on_its_period_end_is_six_weeks_of_hindsight();
    a_negative_denominator_sorts_first();
    fcff_takes_wacc_and_fcfe_takes_the_cost_of_equity();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
