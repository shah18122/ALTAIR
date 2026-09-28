// DEBT-01 acceptance tests: exact paise formatting across the int64 domain.

#include "../format.hpp"

#include <QString>

#include <cstdint>
#include <cstdio>
#include <limits>

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

void test_zero_and_sub_rupee_signs()
{
    using altair::ui::format_paise;
    check(format_paise(0) == QStringLiteral("0.00"),
          "zero has no implicit sign");
    check(format_paise(0, true) == QStringLiteral("+0.00"),
          "zero accepts an explicit positive sign");
    check(format_paise(5) == QStringLiteral("0.05"),
          "five paise keeps two fraction digits");
    check(format_paise(-5) == QStringLiteral("-0.05"),
          "negative five paise keeps its sign");
    check(format_paise(99) == QStringLiteral("0.99"),
          "ninety-nine paise stays below one rupee");
    check(format_paise(-99) == QStringLiteral("-0.99"),
          "negative ninety-nine paise stays below one rupee");
}

void test_rupee_boundary()
{
    using altair::ui::format_paise;
    check(format_paise(100) == QStringLiteral("1.00"),
          "one hundred paise is one rupee");
    check(format_paise(101) == QStringLiteral("1.01"),
          "one rupee and one paisa is exact");
    check(format_paise(-100) == QStringLiteral("-1.00"),
          "negative one hundred paise is negative one rupee");
    check(format_paise(-101) == QStringLiteral("-1.01"),
          "negative one rupee and one paisa is exact");
}

void test_indian_grouping()
{
    using altair::ui::format_paise;
    check(format_paise(99'999) == QStringLiteral("999.99"),
          "three rupee digits have no separator");
    check(format_paise(100'000) == QStringLiteral("1,000.00"),
          "four rupee digits start grouping");
    check(format_paise(12'345'678) == QStringLiteral("1,23,456.78"),
          "positive lakh grouping is exact");
    check(format_paise(-12'345'678) == QStringLiteral("-1,23,456.78"),
          "negative lakh grouping is exact");
    check(format_paise(12'345'678, true) == QStringLiteral("+1,23,456.78"),
          "explicit sign preserves lakh grouping");
    check(format_paise(10'000'000) == QStringLiteral("1,00,000.00"),
          "one lakh uses Indian grouping");
    check(format_paise(1'000'000'000) == QStringLiteral("1,00,00,000.00"),
          "one crore uses Indian grouping");
    check(format_paise(-1'000'000'000) == QStringLiteral("-1,00,00,000.00"),
          "negative one crore uses Indian grouping");
}

void test_int64_extremes()
{
    using altair::ui::format_paise;
    constexpr std::int64_t maximum = std::numeric_limits<std::int64_t>::max();
    constexpr std::int64_t minimum = std::numeric_limits<std::int64_t>::min();

    check(format_paise(maximum)
              == QStringLiteral("92,23,37,20,36,85,47,758.07"),
          "INT64_MAX is formatted exactly");
    check(format_paise(maximum, true)
              == QStringLiteral("+92,23,37,20,36,85,47,758.07"),
          "INT64_MAX accepts an explicit sign");
    check(format_paise(minimum + 1)
              == QStringLiteral("-92,23,37,20,36,85,47,758.07"),
          "INT64_MIN plus one remains exact");

    const QString minimum_text = format_paise(minimum);
    check(minimum_text == QStringLiteral("-92,23,37,20,36,85,47,758.08"),
          "INT64_MIN is formatted exactly");
    check(format_paise(minimum, true) == minimum_text,
          "negative INT64_MIN ignores explicit positive sign");
    check(minimum_text.count(QLatin1Char('-')) == 1,
          "INT64_MIN output contains exactly one minus sign");
    check(!minimum_text.contains(QStringLiteral(".-")),
          "INT64_MIN fraction contains no second minus sign");
}

} // namespace

int main()
{
    std::printf("DEBT-01 -- exact paise formatting\n");
    test_zero_and_sub_rupee_signs();
    test_rupee_boundary();
    test_indian_grouping();
    test_int64_extremes();
    std::printf("\n%s\n", failures == 0 ? "all checks passed"
                                        : "checks did not pass");
    return failures == 0 ? 0 : 1;
}