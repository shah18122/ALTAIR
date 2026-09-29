// Tests for app/data_audit.hpp and app/xlsx_writer.hpp.

#include <app/data_audit.hpp>
#include <app/xlsx_writer.hpp>

#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>

namespace {

namespace da = altair::data_audit;
namespace xl = altair::xlsx;

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::printf("FAIL: %s\n", what); }
    else { std::printf("  ok  : %s\n", what); }
}

constexpr std::int64_t kDay = 20'725;   // 2026-09-29, a Tuesday

std::int64_t at(int hh, int mm, std::int64_t day = kDay) { return day * 86'400 + hh * 3600 + mm * 60; }

da::AuditSeries series_from(const std::string& csv, int tf, std::int64_t close = da::kSessionCloseMin) {
    da::AuditSeries s;
    s.tf = tf;
    s.session_close = close;
    std::istringstream in(csv);
    da::load_audit_csv(s, in, "test.csv");
    da::finish_audit_series(s);
    return s;
}

/// A full 1m session for one day, prices rising by one paisa a minute.
std::string minute_day(std::int64_t day, int end_min = 15 * 60 + 30, double base = 100.0) {
    std::string csv = "time,open,high,low,close,volume\n";
    for (int m = 9 * 60 + 15; m < end_min; ++m) {
        const double p = base + (m - 555) * 0.01;
        char line[160];
        std::snprintf(line, sizeof(line), "%s,%.2f,%.2f,%.2f,%.2f,10\n",
                      da::format_audit_time(day * 86'400 + m * 60, false).c_str(), p, p + 0.05, p - 0.05, p);
        csv += line;
    }
    return csv;
}

void test_time() {
    check(da::parse_audit_time("2026-09-29") == kDay * 86'400, "a date is IST midnight");
    check(da::parse_audit_time("2026-09-29T09:15:00+05:30") == at(9, 15), "IST offset");
    check(da::parse_audit_time("2026-09-29T03:45:00Z") == at(9, 15), "UTC 03:45 is 09:15 IST");
    check(da::parse_audit_time("2026-09-29T09:15") == at(9, 15), "no offset is IST");
    check(!da::parse_audit_time("2026-13-01").has_value() && !da::parse_audit_time("29-09-2026").has_value(),
          "bad dates are refused");
    check(da::format_audit_time(at(9, 15), false) == "2026-09-29T09:15:00+05:30"
              && da::format_audit_time(kDay * 86'400, true) == "2026-09-29",
          "formatting round-trips");
    check(da::audit_weekday(kDay) == 1, "2026-09-29 is a Tuesday");
}

void test_load() {
    const auto s = series_from(
        "close,time,open,low,high,volume,oi\n"
        "101,2026-09-29T09:16:00+05:30,100,99,102,,5\n"
        "100,2026-09-29T09:15:01+05:30,100,99,101,7,4\n"
        "oops,2026-09-29T09:17:00+05:30,1,1,1,1,1\n"
        "100,2026-09-29T09:15:00+05:30,100,99,101,7,4\n"
        "100,2026-09-29T09:18:00+05:30,100,99,101,7,4\n"
        "100,2026-09-29T09:18:00+05:30,100,98,101,7,4\n",
        1);
    check(s.rows_read == 6 && s.parse_errors == 1, "header order is read from the header; a bad row is counted");
    check(s.seconds_floored == 1, "a :01 stamp is floored and counted");
    check(s.duplicates == 1 && s.conflicts == 1, "an identical repeat is a duplicate; a different one a conflict");
    check(s.unsorted == 1, "a row earlier than its predecessor is counted as unsorted");
    check(s.bars.size() == 3 && s.bars[0].t == at(9, 15) && std::isnan(s.bars[1].v) && s.bars[1].oi == 5.0,
          "sorted, one bar per minute; empty volume stays absent");
    check(s.has_volume && s.has_oi, "volume and OI columns are detected");
}

void test_integrity() {
    const auto s = series_from(
        "time,open,high,low,close\n"
        "2026-09-29T09:15:00+05:30,100,101,99,100\n"
        "2026-09-29T09:17:00+05:30,100,101,99,100\n"
        "2026-09-29T15:35:00+05:30,100,101,99,100\n"
        "2026-09-29T09:25:00+05:30,100,99,98,100\n"
        "2026-10-03T09:15:00+05:30,100,101,99,100\n",
        5);
    const auto r = da::check_audit_series(s);
    check(r.off_grid == 1, "09:17 is off the 5m grid");
    check(r.outside_session == 1, "15:35 is outside the cash session");
    check(r.bad_ohlc == 1, "open above high is impossible OHLC");
    check(r.weekend_days == 1, "2026-10-03 is a Saturday");
    check(r.trading_days == 2 && r.short_days == 2, "both days are short of 75 five-minute bars");
    const auto fno = series_from("time,open,high,low,close\n2026-09-29T15:35:00+05:30,1,1,1,1\n", 5,
                                 da::kFnoSessionCloseMin);
    check(da::check_audit_series(fno).outside_session == 0, "15:35 is inside the F&O session (to 15:40)");
}

void test_aggregate_and_compare() {
    const auto m1 = series_from(minute_day(kDay), 1);
    check(m1.bars.size() == 375, "a full cash session is 375 minutes");
    const auto b60 = da::aggregate_audit(m1, 60);
    check(b60.size() == 7 && b60.back().bar.t == at(15, 15) && b60.back().expected == 15 && b60.back().complete(),
          "60m buckets anchor at 09:15; the 15:15 bucket holds 15 minutes");
    check(std::fabs(b60[0].bar.o - 100.0) < 1e-9 && std::fabs(b60[0].bar.c - 100.59) < 1e-9
              && std::fabs(b60[0].bar.h - 100.64) < 1e-9 && b60[0].bar.v == 600.0,
          "open first, close last, high max, volume summed");
    const auto d1 = da::aggregate_audit(m1, da::kDailyTf);
    check(d1.size() == 1 && d1[0].complete() && d1[0].expected == 375, "1d from 1m expects 375 bars");

    // Stored 60m: one exact, one within 1 bp, one mismatch, one extra bar.
    std::vector<da::AuditBar> stored;
    for (const auto& k : b60) stored.push_back(k.bar);
    stored[1].c += 0.006;                       // > half a paisa, < 1 bp
    stored[2].h += 1.0;                         // ~100 bp
    stored.push_back(stored.back());
    stored.back().t = at(9, 15, kDay + 1);      // only in stored
    std::vector<da::AuditBar> bars;
    std::vector<bool> complete;
    da::split_buckets(b60, bars, complete);
    const auto c = da::compare_audit("60m", stored, bars, complete);
    check(c.compared == 7 && c.exact_bars == 5 && c.rounding_bars == 1 && c.mismatch_bars == 1,
          "exact / within 1 bp / mismatch are classified");
    check(c.field_mismatch[1] == 1 && c.diffs.size() == 1 && c.diffs[0].bp > 90.0, "the mismatch names high");
    check(c.only_reference == 0, "the extra stored bar lies outside the overlap and is not called missing");

    // A day with a hole: its buckets are not comparable, not wrong.
    std::string holed = minute_day(kDay + 1);
    const std::string cut = da::format_audit_time(at(10, 0, kDay + 1), false);
    const std::size_t pos = holed.find(cut);
    holed.erase(pos, holed.find('\n', pos) - pos + 1);
    const auto m1h = series_from(holed, 1);
    const auto b60h = da::aggregate_audit(m1h, 60);
    check(!b60h[0].complete() && b60h[0].have == 59, "a missing minute makes its bucket incomplete");
    std::vector<da::AuditBar> stored_h;
    for (const auto& k : b60h) stored_h.push_back(k.bar);
    stored_h[0].c = 1.0;                        // would be a huge mismatch if compared
    da::split_buckets(b60h, bars, complete);
    const auto ch = da::compare_audit("60m", stored_h, bars, complete);
    check(ch.not_comparable == 1 && ch.mismatch_bars == 0, "an incomplete bucket is not compared");

    const auto fno = series_from(minute_day(kDay, 15 * 60 + 40), 1, da::kFnoSessionCloseMin);
    const auto f60 = da::aggregate_audit(fno, 60);
    check(f60.back().expected == 25 && f60.back().complete(), "F&O 15:15 bucket runs to 15:40");
}

void test_infer() {
    check(da::infer_audit_tf(series_from(minute_day(kDay), 1)) == 1, "1m spacing is inferred");
    const auto daily = series_from("time,open,high,low,close\n2026-09-29T05:30:00+05:30,1,1,1,1\n", da::kDailyTf);
    check(daily.bars.size() == 1 && daily.bars[0].t == kDay * 86'400, "a daily bar stamped 05:30 is keyed by its date");
    check(da::infer_audit_tf(series_from("time,open,high,low,close\n2026-09-28,1,1,1,1\n2026-09-29,1,1,1,1\n",
                                         da::kDailyTf)) == da::kDailyTf,
          "date-only rows are daily");
}

void test_xlsx() {
    check(xl::xlsx_detail::crc32("123456789") == 0xCBF43926u, "CRC-32 check value");
    check(xl::xlsx_detail::column_name(0) == "A" && xl::xlsx_detail::column_name(25) == "Z"
              && xl::xlsx_detail::column_name(26) == "AA" && xl::xlsx_detail::column_name(701) == "ZZ"
              && xl::xlsx_detail::column_name(702) == "AAA",
          "column letters");
    xl::XlsxSheet a;
    a.name = "Summary";
    a.rows.push_back({xl::XlsxCell::str("Name & <x>", true), xl::XlsxCell::num(1.5)});
    a.freeze_rows = 1;
    a.widths = {20};
    xl::XlsxSheet b = a;
    b.name = "spot_nifty";
    const auto book = xl::build_workbook({a, b});
    check(book && book->rfind("PK\x03\x04", 0) == 0, "a workbook is a ZIP");
    check(book && book->find("PK\x05\x06") != std::string::npos, "with an end-of-central-directory record");
    check(book && book->find("Name &amp; &lt;x&gt;") != std::string::npos, "cell text is XML-escaped");
    check(book && book->find("xl/worksheets/sheet2.xml") != std::string::npos, "one part per sheet");
    xl::XlsxSheet bad = a;
    bad.name = "a/b";
    check(xl::build_workbook({bad}).error() == xl::XlsxError::BadSheetName, "an invalid sheet name is refused");
    check(xl::build_workbook({a, a}).error() == xl::XlsxError::DuplicateSheetName, "duplicate names are refused");
}

} // namespace

int main() {
    std::printf("Data audit\n");
    test_time();
    test_load();
    test_integrity();
    test_aggregate_and_compare();
    test_infer();
    test_xlsx();
    std::printf("Data audit: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
