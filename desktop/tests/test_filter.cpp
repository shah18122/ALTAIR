// P11Q-02 acceptance tests.
//
// Test 1 is the card: sorting on the DISPLAY STRING is lexicographic, and the
// order it produces is measured against the order sorting on the raw paise
// produces. They differ, and the difference is the largest position landing in
// the middle of a list a trader reads top-down.
//
// Test 2: a money filter is typed in rupees and compared in paise.
//
// Test 3: blank is not zero -- a comparison never matches a blank, and
// "(Blanks)" is the only thing that does.
//
// Test 4: a proxy is a second index space, and token_at survives a re-sort.
//
// No check description here may contain the substring FAIL.

#include "../filter.hpp"
#include "../tick_model.hpp"

#include <QCoreApplication>
#include <QStringList>

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

using namespace altair;
using namespace altair::ui;

/// Four instruments spanning two orders of magnitude -- which is exactly the
/// spread that makes a lexicographic sort look plausible.
struct Seed {
    std::uint32_t token;
    const char* symbol;
    std::int64_t paise;
};
/// The fifth entry is load-bearing. With only the first four, a lexicographic
/// sort happens to produce the SAME order as a numeric one -- '1' < '2' < '5'
/// carries it, and the first draft of this test measured no difference and
/// concluded there was none.
///
/// A far-OTM option at Rs 9.50 breaks it, because "9.50" sorts AFTER
/// "52,089.54" as text while being the smallest number in the column. That is
/// not a contrived value; it is what a cheap weekly option costs.
constexpr Seed kSeeds[] = {
    {1, "NIFTY 50",           2'450'754},   // Rs 24,507.54
    {2, "BANKNIFTY",          5'208'954},   // Rs 52,089.54
    {3, "RELIANCE",             290'605},   // Rs  2,906.05
    {4, "NIFTY26SEP24500CE",     11'998},   // Rs    119.98
    {5, "NIFTY26SEP26000CE",        950},   // Rs      9.50
};

void seed_model(TickModel& m, bool leave_last_blank = false)
{
    for (const auto& s : kSeeds) {
        m.add_instrument(s.token, QString::fromUtf8(s.symbol));
    }
    std::uint64_t seq = 1;
    for (const auto& s : kSeeds) {
        if (leave_last_blank && s.token == 4) {
            continue;   // no tick ever arrives for this one
        }
        ReplayTick t{};
        t.ts = Timestamp{1'788'428'100'000'000'000LL
                         + static_cast<std::int64_t>(seq) * 1'000'000};
        t.seqno = seq++;
        t.token = s.token;
        t.last = Price{s.paise};
        t.qty = Qty{1};
        m.apply_tick(t);
    }
}

} // namespace

// ---------------------------------------------------------------------------

static void test_sort_is_not_lexicographic()
{
    std::printf("\n[1] sorting the display string is lexicographic\n");

    TickModel model;
    seed_model(model);

    // What sorting the DISPLAY strings gives -- which is what a naive proxy
    // with the default Qt::DisplayRole does.
    QStringList shown;
    for (int r = 0; r < model.rowCount(); ++r) {
        shown << model.index(r, TickModel::ColLast).data().toString();
    }
    QStringList lexicographic = shown;
    lexicographic.sort();

    FilterProxy proxy;
    proxy.setSourceModel(&model);
    proxy.sort(TickModel::ColLast, Qt::AscendingOrder);

    QStringList numeric;
    for (int r = 0; r < proxy.rowCount(); ++r) {
        numeric << proxy.index(r, TickModel::ColLast).data().toString();
    }

    std::printf("    as text  : %s\n",
                lexicographic.join(QStringLiteral("  ")).toUtf8().constData());
    std::printf("    as paise : %s\n",
                numeric.join(QStringLiteral("  ")).toUtf8().constData());

    check(lexicographic != numeric,
          "the two orders differ -- so a proxy left on the default display role"
          " is not merely imprecise, it produces a different list");

    check(numeric.first() == QStringLiteral("9.50")
              && numeric.last() == QStringLiteral("52,089.54"),
          "sorting on the raw int64 paise puts the cheapest first and the"
          " largest last, which is what ascending means");

    check(lexicographic.last() == QStringLiteral("9.50"),
          "as text, the SMALLEST number in the column sorts LAST -- '9' beats"
          " every leading digit, so the cheapest option lands at the bottom of"
          " an ascending list where the most expensive belongs");
    check(lexicographic.first() == QStringLiteral("119.98"),
          "and the order still starts plausibly, which is exactly why a"
          " lexicographic sort survives a glance at the top of the screen");
}

static void test_money_filter_is_typed_in_rupees()
{
    std::printf("\n[2] a money filter is typed in rupees, compared in paise\n");

    check(parse_rupees_to_paise(QStringLiteral("1000")) == 100'000,
          "one thousand rupees parses to a hundred thousand paise -- the"
          " conversion happens once, at the parse");
    check(parse_rupees_to_paise(QStringLiteral("1234.35")) == 123'435,
          "and the paise digits survive exactly, via integer arithmetic rather"
          " than Math.round(x * 100) on a double");
    check(parse_rupees_to_paise(QStringLiteral("-0.05")) == -5,
          "negatives included");
    check(parse_rupees_to_paise(QStringLiteral("1,00,000")) == 10'000'000,
          "and Indian grouping in the input is accepted, since that is how the"
          " number is displayed back");
    check(!parse_rupees_to_paise(QStringLiteral("1.234")).has_value(),
          "sub-paisa precision is REFUSED rather than rounded: a rounded filter"
          " BOUND silently changes which rows match and the user cannot see"
          " why");
    check(!parse_rupees_to_paise(QStringLiteral("abc")).has_value(),
          "and text is refused rather than treated as zero, which would apply"
          " a filter nobody asked for");

    TickModel model;
    seed_model(model);
    FilterProxy proxy;
    proxy.setSourceModel(&model);

    ColumnFilter f;
    f.cmp = Cmp::Greater;
    f.operand = *parse_rupees_to_paise(QStringLiteral("10000"));  // Rs 10,000
    proxy.set_filter(TickModel::ColLast, f);
    const int correct = proxy.rowCount();

    ColumnFilter literal;
    literal.cmp = Cmp::Greater;
    literal.operand = 10'000;          // the same number taken as PAISE
    proxy.set_filter(TickModel::ColLast, literal);
    const int wrong = proxy.rowCount();

    std::printf("    \"LTP > 10000\" meaning rupees: %d rows;  taken literally"
                " as paise: %d rows\n", correct, wrong);
    check(wrong > correct,
          "taking the typed number as paise selects everything over a hundred"
          " rupees -- a hundredfold wider filter that looks entirely plausible"
          " on screen");
}

static void test_blank_is_not_zero()
{
    std::printf("\n[3] a blank matches no comparison\n");

    TickModel model;
    seed_model(model, /*leave_last_blank=*/true);
    FilterProxy proxy;
    proxy.setSourceModel(&model);

    check(model.rowCount() == 5, "five instruments");
    check(model.index(3, TickModel::ColLast).data(TickModel::BlankRole).toBool(),
          "one of them never received a tick, so its LTP is BLANK -- not zero,"
          " and the model says which");

    ColumnFilter ge;
    ge.cmp = Cmp::GreaterOrEqual;
    ge.operand = 0;
    proxy.set_filter(TickModel::ColLast, ge);
    const int at_or_above = proxy.rowCount();

    ColumnFilter blanks_only;
    blanks_only.value_set_active = true;      // nothing in the allowed set
    blanks_only.allow_blanks = true;
    proxy.set_filter(TickModel::ColLast, blanks_only);
    const int blanks = proxy.rowCount();

    std::printf("    of %d rows: \">= 0\" gives %d, blanks-only gives %d\n",
                model.rowCount(), at_or_above, blanks);
    check(at_or_above == model.rowCount() - 1,
          "\">= 0\" does not sweep in the instrument that has not traded --"
          " otherwise \"show me everything at or above break-even\" quietly"
          " includes every symbol with no data");
    check(blanks == 1,
          "and the blank is reachable, through its own checkbox, so no row is"
          " unfilterable");
    check(at_or_above + blanks == model.rowCount(),
          "the two account for every row exactly, which is the same"
          " partition check P11-04 made in the web client");

    ColumnFilter no_blanks;
    no_blanks.allow_blanks = false;
    proxy.set_filter(TickModel::ColLast, no_blanks);
    check(proxy.rowCount() == model.rowCount() - 1,
          "and unticking (Blanks) alone is an active filter, rather than a"
          " no-op that leaves the header indicator lying");
}

static void test_proxy_is_a_second_index_space()
{
    std::printf("\n[4] a proxy is a second index space\n");

    TickModel model;
    seed_model(model);
    FilterProxy proxy;
    proxy.setSourceModel(&model);

    proxy.sort(TickModel::ColLast, Qt::AscendingOrder);
    const std::uint32_t top_ascending = proxy.token_at(0);

    proxy.sort(TickModel::ColLast, Qt::DescendingOrder);
    const std::uint32_t top_descending = proxy.token_at(0);

    std::printf("    proxy row 0 holds token %u ascending, token %u"
                " descending\n", top_ascending, top_descending);
    check(top_ascending != top_descending,
          "row 0 is a DIFFERENT INSTRUMENT after a sort -- which is the whole"
          " hazard: a row number captured before the sort addresses someone"
          " else after it");
    check(top_ascending == 5 && top_descending == 2,
          "and token_at resolves proxy row -> source row -> token in one step,"
          " so a caller never holds an index across the change");

    // Filtering moves it again.
    ColumnFilter f;
    f.cmp = Cmp::Less;
    f.operand = 1'000'000;                     // under Rs 10,000
    proxy.set_filter(TickModel::ColLast, f);
    std::printf("    after filtering to < Rs 10,000: %d rows, row 0 is token"
                " %u\n", proxy.rowCount(), proxy.token_at(0));
    check(proxy.rowCount() == 3,
          "the filter narrows the view and renumbers every row underneath it");
    check(proxy.active_filter_count() == 1,
          "and the active-filter count is reported, so the status bar can say"
          " the grid is showing a subset rather than leaving it to be noticed");
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    std::printf("P11Q-02 -- sorting and Excel-style filters\n");
    test_sort_is_not_lexicographic();
    test_money_filter_is_typed_in_rupees();
    test_blank_is_not_zero();
    test_proxy_is_a_second_index_space();

    std::printf("\n%s\n", failures == 0 ? "all checks passed"
                                        : "checks did not pass");
    return failures == 0 ? 0 : 1;
}
