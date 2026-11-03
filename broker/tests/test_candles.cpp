// Tests for broker/candles.hpp: the IST date helpers and the coverage check
// every history fetch (FYERS) runs before a candle reaches dataset/.
//
// No check description here may contain the substring "F" "AIL" joined.

#include <broker/candles.hpp>

#include <cstdio>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) { ++failures; }
}

altair::RawCandle at_day(std::int64_t day) {
    altair::RawCandle c;
    // 09:15 IST on `day` = 03:45 UTC.
    c.ts_ns = day * altair::detail::kNsPerDay + (3 * 3600 + 45 * 60) * 1'000'000'000LL;
    c.open = c.high = c.low = c.close = 100.0;
    return c;
}

}  // namespace

int main() {
    std::printf("broker candles\n");
    std::int64_t d = 0;
    check(altair::parse_date("2026-08-31", d) && altair::date_string(d) == "2026-08-31",
          "a date round-trips through its day number");
    check(!altair::parse_date("2026-13-01", d) && !altair::parse_date("2026-8-31", d),
          "a malformed date is refused");

    std::int64_t aug31 = 0;
    (void)altair::parse_date("2026-08-31", aug31);
    const std::int64_t open_utc = aug31 * altair::detail::kNsPerDay + (3 * 3600 + 45 * 60) * 1'000'000'000LL;
    check(altair::format_ist(open_utc) == "2026-08-31T09:15:00+05:30",
          "03:45 UTC is written as 09:15 IST, colon offset");

    std::vector<altair::RawCandle> c;
    for (std::int64_t k = 0; k < 10; ++k) c.push_back(at_day(aug31 - 9 + k));
    const auto full = altair::verify_coverage(c, "2026-08-22", "2026-08-31", 4);
    check(full.has_value() && *full == 1, "contiguous candles cover the window");
    const auto trunc = altair::verify_coverage(c, "2026-07-01", "2026-08-31", 4);
    check(!trunc && trunc.error() == altair::HistError::IncompleteCoverage,
          "a window the candles do not reach is refused, not merged short");
    std::vector<altair::RawCandle> holes{at_day(aug31 - 20), at_day(aug31)};
    const auto gap = altair::verify_coverage(holes, "2026-08-11", "2026-08-31", 4);
    check(!gap && gap.error() == altair::HistError::IncompleteCoverage, "a fortnight's hole is refused");
    std::vector<altair::RawCandle> back{at_day(aug31), at_day(aug31 - 1)};
    const auto bad = altair::verify_coverage(back, "2026-08-30", "2026-08-31", 4);
    check(!bad && bad.error() == altair::HistError::Malformed, "candles out of order are refused");
    const std::vector<altair::RawCandle> none;
    check(!altair::verify_coverage(none, "2026-07-01", "2026-08-31", 4), "no candles is not coverage");
    check(!altair::verify_coverage(c, "2026-08-31", "2026-08-01", 4), "a reversed window is refused");

    std::printf("%s\n", failures == 0 ? "all candle checks passed" : "candle checks did not pass");
    return failures == 0 ? 0 : 1;
}
