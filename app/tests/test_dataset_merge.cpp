// app/tests/test_dataset_merge.cpp -- CX02-A2.
//
// Findings C14-005 (the rewrite could lose a month), C14-006 (no coverage
// check), C14-007 (`--force` erased rows outside the range) and C14-016
// (substring --symbols, atoi --days).
//
// Each test names the defect it pins. The filesystem tests run in a fresh
// directory under the system temp path and remove it afterwards; nothing here
// reads or writes dataset/.
//
// No check description here may contain the substring FAIL.

#include <app/dataset_merge.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %-4s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) { ++failures; }
}

namespace ds = altair::dataset;
namespace fs = std::filesystem;

constexpr std::int64_t kMinNs = 60'000'000'000LL;

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void spit(const fs::path& p, const std::string& s) {
    std::ofstream out(p, std::ios::trunc);
    out << s;
}

/// A candle stamped at 00:00 IST on `date`, which is how Kite stamps a daily.
altair::RawCandle day_candle(const char* date, double close) {
    std::int64_t d = 0;
    (void)altair::parse_date(date, d);
    altair::RawCandle c;
    c.ts_ns = d * ds::kDayNs - altair::detail::kIstOffsetNs;
    c.open = c.high = c.low = c.close = close;
    return c;
}

// ── 1 ────────────────────────────────────────────────────────────────────
void replace_checked_writes_and_never_removes_first(const fs::path& dir) {
    std::printf("\n1 replace_checked_writes_and_never_removes_first\n");
    const fs::path target = dir / "2026-09.csv";
    spit(target, "time,open\nold\n");
    const auto ok = ds::replace_file_checked(target, "time,open\nnew\nrows\n");
    check(ok.has_value(), "an existing file is replaced");
    check(slurp(target) == "time,open\nnew\nrows\n",
          "and holds exactly the new bytes");
    bool temp_left = false;
    for (const auto& entry : fs::directory_iterator(dir)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("2026-09.csv.tmp.", 0) == 0) { temp_left = true; }
    }
    check(!temp_left, "with no uniquely named temp left behind");

    // The temp cannot be created: the would-be parent is a regular file.
    const fs::path blocked_parent = dir / "not_a_directory";
    spit(blocked_parent, "parent stays intact\n");
    const auto blocked = ds::replace_file_checked(
        blocked_parent / "keep.csv", "time,open\nlost\n");
    check(!blocked && blocked.error() == ds::ReplaceError::OpenTemp,
          "a temp that cannot be opened is reported");
    check(slurp(blocked_parent) == "parent stays intact\n",
          "and the obstructing path is byte-identical");

    // The rename cannot succeed: the target is a non-empty directory. The
    // old code removed the target FIRST and ignored the rename's result.
    const fs::path locked = dir / "locked.csv";
    fs::create_directory(locked);
    spit(locked / "inside.txt", "x");
    const auto ren = ds::replace_file_checked(locked, "time,open\nnew\n");
    check(!ren && ren.error() == ds::ReplaceError::Rename,
          "a rename that fails is reported, not ignored");
    check(fs::is_directory(locked) && fs::exists(locked / "inside.txt"),
          "the target is untouched");
    temp_left = false;
    for (const auto& entry : fs::directory_iterator(dir)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("locked.csv.tmp.", 0) == 0) { temp_left = true; }
    }
    check(!temp_left,
          "and the temp is cleaned up rather than left as an orphan");
}

// ── 1b ───────────────────────────────────────────────────────────────────
void concurrent_replacements_use_private_temps(const fs::path& dir) {
    std::printf("\n1b concurrent_replacements_use_private_temps\n");
    const fs::path target = dir / "concurrent.csv";
    spit(target, "initial\n");
    const std::vector<std::string> payloads{
        std::string(128 * 1024, 'A'), std::string(128 * 1024, 'B'),
        std::string(128 * 1024, 'C'), std::string(128 * 1024, 'D')};
    std::atomic<int> errors{0};
    std::vector<std::thread> writers;
    for (const std::string& payload : payloads) {
        writers.emplace_back([&target, payload, &errors] {
            for (int i = 0; i < 4; ++i) {
                const auto written = ds::replace_file_checked(target, payload);
                if (!written) { errors.fetch_add(1, std::memory_order_relaxed); }
            }
        });
    }
    for (std::thread& writer : writers) { writer.join(); }
    check(errors.load(std::memory_order_relaxed) == 0,
          "concurrent writers each publish through an independent temp file");
    const std::string final = slurp(target);
    bool is_complete_payload = false;
    for (const std::string& payload : payloads) {
        is_complete_payload = is_complete_payload || final == payload;
    }
    check(is_complete_payload,
          "the target is one complete writer payload, never a mixed or partial file");
    bool orphan = false;
    const std::string prefix = "concurrent.csv.tmp.";
    for (const auto& entry : fs::directory_iterator(dir)) {
        const std::string name = entry.path().filename().string();
        if (name.compare(0, prefix.size(), prefix) == 0) { orphan = true; }
    }
    check(!orphan, "all uniquely named temporary files were removed after publish");
}

// ── 2 ────────────────────────────────────────────────────────────────────
void load_keeps_headerless_first_row_and_per_file_headers() {
    std::printf("\n2 load_keeps_headerless_first_row_and_per_file_headers\n");
    ds::Series s;
    std::istringstream bare("2026-09-08,1,2,0.5,1.5,\n2026-09-09,1,2,0.5,1.6,\n");
    ds::load_file(s, "all.csv", bare, ds::kDayNs);
    const ds::StoredFile& f = s.files["all.csv"];
    check(f.rows.size() == 2,
          "a headerless file keeps BOTH rows -- the first used to be dropped"
          " on every run");
    check(f.header.empty() && ds::render(f) ==
              "2026-09-08,1,2,0.5,1.5,\n2026-09-09,1,2,0.5,1.6,\n",
          "and is written back without a header it never had");

    ds::Series m;
    std::istringstream a("time,open,high,low,close,volume\n"
                         "2026-08-31T15:29:00+05:30,1,1,1,1,\n");
    std::istringstream b("time,open,high,low,close,volume,oi\n"
                         "2026-09-01T09:15:00+05:30,1,1,1,1,,7\n");
    ds::load_file(m, "2026-08.csv", a, kMinNs);
    ds::load_file(m, "2026-09.csv", b, kMinNs);
    check(m.files["2026-08.csv"].header == "time,open,high,low,close,volume"
              && m.files["2026-09.csv"].header
                     == "time,open,high,low,close,volume,oi",
          "each file keeps its own header -- one series-wide header used to be"
          " written into all of them");
    std::istringstream crlf("time,open\r\n2026-09-08,1,\r\n");
    ds::Series c;
    ds::load_file(c, "all.csv", crlf, ds::kDayNs);
    check(c.files["all.csv"].rows.begin()->second == "2026-09-08,1,",
          "a CRLF row is stored without its carriage return");
}

// ── 3 ────────────────────────────────────────────────────────────────────
void only_changed_files_are_rewritten() {
    std::printf("\n3 only_changed_files_are_rewritten\n");
    ds::Series s;
    std::istringstream aug("time,open,high,low,close,volume\n"
                           "2026-08-31T15:29:00+05:30,10,10,10,10,\n");
    std::istringstream sep("time,open,high,low,close,volume\n"
                           "2026-09-01T09:15:00+05:30,20,20,20,20,\n");
    std::istringstream vendor("time,open,high,low,close,volume\n"
                              "2014-12-31T15:29:00+05:30,5,5,5,5,\n");
    ds::load_file(s, "2026-08.csv", aug, kMinNs);
    ds::load_file(s, "2026-09.csv", sep, kMinNs);
    ds::load_file(s, "vendor_pre2015.csv", vendor, kMinNs);
    check(s.single_file == "vendor_pre2015.csv",
          "(a series with a non-month file routes new rows to it, as before)");
    // A month series proper, without the vendor file:
    ds::Series p;
    std::istringstream aug2("time,open,high,low,close,volume\n"
                            "2026-08-31T15:29:00+05:30,10,10,10,10,\n");
    std::istringstream sep2("time,open,high,low,close,volume\n"
                            "2026-09-01T09:15:00+05:30,20,20,20,20,\n");
    ds::load_file(p, "2026-08.csv", aug2, kMinNs);
    ds::load_file(p, "2026-09.csv", sep2, kMinNs);

    check(ds::merge_bar(p, "2026-09-01T09:16:00+05:30", ",21,21,21,21,", 21.0,
                        kMinNs) == ds::MergeResult::Added,
          "a new September bar is added");
    check(p.files["2026-09.csv"].dirty && !p.files["2026-08.csv"].dirty,
          "only September needs writing -- every file in the series used to be"
          " rewritten");
    check(ds::merge_bar(p, "2026-08-31T15:29:00+05:30", ",10,10,10,10,", 10.0,
                        kMinNs) == ds::MergeResult::Same
              && !p.files["2026-08.csv"].dirty,
          "an identical re-fetched bar changes nothing");
}

// ── 4 ────────────────────────────────────────────────────────────────────
void material_change_replaces_and_rounding_does_not() {
    std::printf("\n4 material_change_replaces_and_rounding_does_not\n");
    ds::Series s;
    std::istringstream vix("time,open,high,low,close,volume\n"
                           "2026-09-04,10.8,10.8,10.8,10.80,\n"
                           "2026-09-05,24123.5,24123.5,24123.5,24123.5,\n");
    ds::load_file(s, "all.csv", vix, ds::kDayNs);
    check(ds::merge_bar(s, "2026-09-04T00:00:00+05:30",
                        ",10.68,10.68,10.68,10.68,", 10.68, ds::kDayNs)
              == ds::MergeResult::Replaced,
          "a close frozen mid-session (110 bp away) is replaced by the settled"
          " one");
    check(ds::merge_bar(s, "2026-09-05T00:00:00+05:30",
                        ",24123.45,24123.45,24123.45,24123.45,", 24123.45,
                        ds::kDayNs) == ds::MergeResult::Rounding,
          "a bar the old %g writer rounded (0.02 bp) is LEFT ALONE -- repairing"
          " stored history is a confirmed refetch, not a side effect");
    check(s.files["all.csv"].rows["2026-09-05"]
              == "2026-09-05,24123.5,24123.5,24123.5,24123.5,",
          "so the stored row is exactly as it was");
    check(s.files["all.csv"].rows["2026-09-04"] == "2026-09-04,10.68,10.68,10.68,10.68,",
          "and the replaced row keeps the series' date-only stamp");

}

// ── CX02-A2c named acceptance checks ─────────────────────────────────────
void zero_price_is_refused() {
    std::printf("\nCX02-A2c zero_price_is_refused\n");
    const altair::RawCandle zero = day_candle("2026-09-06", 0.0);
    std::string prices = "unchanged";
    check(!ds::append_prices(prices, zero) && prices == "unchanged",
          "a zero fetched OHLC value refuses serialization without changing output");

    ds::Series empty;
    check(ds::merge_bar(empty, "2026-09-06T00:00:00+05:30",
                        ",0,0,0,0,", 0.0, ds::kDayNs)
              == ds::MergeResult::Unusable
              && empty.files.empty(),
          "even a direct merge cannot add a new zero-close bar");

    ds::Series stored;
    std::istringstream row("time,open,high,low,close,volume\n"
                           "2026-09-06,10,10,10,10,\n");
    ds::load_file(stored, "all.csv", row, ds::kDayNs);
    check(ds::merge_bar(stored, "2026-09-06T00:00:00+05:30",
                        ",0,0,0,0,", 0.0, ds::kDayNs)
              == ds::MergeResult::Unusable
              && !stored.files["all.csv"].dirty,
          "a zero fresh close cannot match or replace a valid stored close");
}

void unparseable_close_blocks_rather_than_matching() {
    std::printf("\nCX02-A2c unparseable_close_blocks_rather_than_matching\n");
    ds::Series stored;
    std::istringstream row("time,open,high,low,close,volume\n"
                           "2026-09-06,1,1,1,nan,\n");
    ds::load_file(stored, "all.csv", row, ds::kDayNs);
    const auto& file = stored.files["all.csv"];
    check(!ds::close_of(file.rows.at("2026-09-06"))
              && file.unkeyed == 1 && !ds::writable(file),
          "a non-finite stored close is classified as unreadable and blocks rewriting");
    check(ds::merge_bar(stored, "2026-09-06T00:00:00+05:30",
                        ",2,2,2,2,", 2.0, ds::kDayNs)
              == ds::MergeResult::BlockedFile
              && !stored.files["all.csv"].dirty,
          "the fresh row is not silently dropped as Same or used to rewrite the file");
}

void series_update_lock_serializes_full_updates(const fs::path& dir) {
    std::printf("\nCX02-A2c series_update_lock_serializes_full_updates\n");
    const fs::path lock_path = dir / ".series_update_test.lock";
    std::atomic<bool> contender_acquired{false};
    std::thread contender;
    bool first_acquired = false;
    {
        ds::UpdateStateLock first;
        first_acquired = first.acquire(lock_path);
        if (first_acquired) {
            contender = std::thread([&] {
                ds::UpdateStateLock second;
                contender_acquired.store(second.acquire(lock_path),
                                         std::memory_order_release);
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            check(!contender_acquired.load(std::memory_order_acquire),
                  "a second updater cannot enter while the first holds the series lock");
        }
    }
    if (contender.joinable()) { contender.join(); }
    check(first_acquired && contender_acquired.load(std::memory_order_acquire),
          "the waiting updater proceeds after the first releases the whole-series lock");
}

void empty_answer_over_trading_days_is_not_nothing_new(const fs::path& dir) {
    std::printf("\nCX02-A2c empty_answer_over_trading_days_is_not_nothing_new\n");
    const fs::path state = dir / ".kite_update_empty_answers_test";
    int incomplete_attempts = 0;
    std::uint64_t persisted_count = 0;
    for (int attempt = 0; attempt < 3; ++attempt) {
        const ds::Coverage coverage =
            ds::judge_coverage({}, "2026-09-14", "2026-09-16", 5);
        if (coverage == ds::Coverage::Incomplete) {
            ++incomplete_attempts;
            const auto updated = ds::update_empty_answer_streak(
                state, ds::EmptyAnswerEvent::TradingWindowEmpty);
            check(updated && *updated == static_cast<std::uint64_t>(attempt + 1),
                  "the per-series count survives the next update invocation");
            if (updated) { persisted_count = *updated; }
        }
    }
    check(incomplete_attempts == 3,
          "each repeated empty response spanning Mon-Wed remains an explicit failure");
    check(persisted_count == 3,
          "the updater has a durable count to print for consecutive empty answers");
    std::atomic<int> state_errors{0};
    std::vector<std::thread> increments;
    for (int i = 0; i < 8; ++i) {
        increments.emplace_back([&state, &state_errors] {
            const auto updated = ds::update_empty_answer_streak(
                state, ds::EmptyAnswerEvent::TradingWindowEmpty);
            if (!updated) { state_errors.fetch_add(1, std::memory_order_relaxed); }
        });
    }
    for (std::thread& increment : increments) { increment.join(); }
    check(state_errors.load(std::memory_order_relaxed) == 0,
          "parallel updater invocations can update the streak state");
    const auto weekend = ds::update_empty_answer_streak(
        state, ds::EmptyAnswerEvent::NoTradingSession);
    check(weekend && *weekend == 11
              && ds::judge_coverage({}, "2026-09-12", "2026-09-13", 5)
                     == ds::Coverage::NothingNew,
          "the serialized count survives parallel updates and an empty weekend");
    const auto resumed = ds::update_empty_answer_streak(
        state, ds::EmptyAnswerEvent::NonemptyResponse);
    check(resumed && *resumed == 0 && slurp(state) == "0\n",
          "a nonempty response resets the persisted consecutive-empty count");
}

void future_last_stamp_refuses_rather_than_clamping() {
    std::printf("\nCX02-A2c future_last_stamp_refuses_rather_than_clamping\n");
    std::int64_t today = 0;
    (void)altair::parse_date("2026-09-19", today);
    const auto future = ds::update_from_day(
        "2026-09-20T09:15:00+05:30", today, 400);
    check(!future && future.error() == ds::UpdateWindowError::FutureLastStamp,
          "a future stored date is rejected rather than becoming a zero-width request");
    const auto empty = ds::update_from_day({}, today, 400);
    check(empty && *empty == today - 400,
          "an empty series still uses its validated lookback");
    const auto existing = ds::update_from_day(
        "2026-09-18T15:29:00+05:30", today, 400);
    std::int64_t expected = 0;
    (void)altair::parse_date("2026-09-18", expected);
    check(existing && *existing == expected,
          "an existing series resumes from the latest stored day");
    const auto invalid = ds::update_from_day("2026-02-31", today, 400);
    check(!invalid && invalid.error() == ds::UpdateWindowError::InvalidLastStamp,
          "an impossible stored date is refused instead of falling back silently");
}

// ── 5 ────────────────────────────────────────────────────────────────────
void unkeyed_or_duplicate_rows_block_the_rewrite() {
    std::printf("\n5 unkeyed_or_duplicate_rows_block_the_rewrite\n");
    ds::Series s;
    std::istringstream bad("time,open,high,low,close,volume\n"
                           "2026-09-08,1,1,1,1,\n"
                           "garbage,1\n");
    ds::load_file(s, "all.csv", bad, ds::kDayNs);
    check(s.files["all.csv"].unkeyed == 1, "an unkeyable row is counted");
    check(ds::merge_bar(s, "2026-09-09T00:00:00+05:30", ",2,2,2,2,", 2.0,
                        ds::kDayNs) == ds::MergeResult::BlockedFile
              && !s.files["all.csv"].dirty,
          "and the file is never rewritten, which would drop that row");

    ds::Series d;
    std::istringstream dup("time,open,high,low,close,volume\n"
                           "2026-09-08,1,1,1,1,\n"
                           "2026-09-08,1,1,1,1.5,\n");
    ds::load_file(d, "all.csv", dup, ds::kDayNs);
    check(d.files["all.csv"].duplicate_keys == 1
              && ds::merge_bar(d, "2026-09-09T00:00:00+05:30", ",2,2,2,2,",
                               2.0, ds::kDayNs) == ds::MergeResult::BlockedFile,
          "the same bar twice in one file blocks a rewrite too");
}

// ── 6 ────────────────────────────────────────────────────────────────────
void incomplete_coverage_merges_nothing() {
    std::printf("\n6 incomplete_coverage_merges_nothing\n");
    // 60 calendar days asked for, candles for only the first 10.
    std::vector<altair::RawCandle> got;
    std::int64_t d0 = 0;
    (void)altair::parse_date("2026-07-01", d0);
    for (std::int64_t i = 0; i < 10; ++i) {
        got.push_back(day_candle(altair::date_string(d0 + i).c_str(), 100.0));
    }
    check(ds::judge_coverage(got, "2026-07-01", "2026-08-29", 5)
              == ds::Coverage::Incomplete,
          "a window Kite truncated is INCOMPLETE -- the old updater merged it");
    check(ds::judge_coverage({}, "2026-07-01", "2026-08-29", 5)
              == ds::Coverage::Incomplete,
          "an empty answer to a 60-day question is incomplete");
    std::vector<altair::RawCandle> full;
    for (std::int64_t i = 0; i < 60; ++i) {
        full.push_back(day_candle(altair::date_string(d0 + i).c_str(), 100.0));
    }
    check(ds::judge_coverage(full, "2026-07-01", "2026-08-29", 5)
              == ds::Coverage::Complete,
          "and a full answer is complete");
    // Ends inside tolerance (first Jul 6, window Jul 1..4), so the ORDER check
    // is the one that answers.
    std::vector<altair::RawCandle> backwards{full[5], full[1]};
    check(ds::judge_coverage(backwards, "2026-07-01", "2026-07-04", 5)
              == ds::Coverage::Malformed,
          "candles out of order are malformed");
}

// ── 7 ────────────────────────────────────────────────────────────────────
void prices_are_exact_and_nan_is_refused() {
    std::printf("\n7 prices_are_exact_and_nan_is_refused\n");
    altair::RawCandle c = day_candle("2026-09-08", 24123.45);
    c.high = 24150.1;
    c.low = 24100.05;
    std::string s;
    check(ds::append_prices(s, c)
              && s == ",24123.45,24150.1,24100.05,24123.45,",
          "every price is written exactly -- %g wrote 24123.5");
    c.low = std::numeric_limits<double>::quiet_NaN();
    std::string t = "unchanged";
    check(!ds::append_prices(t, c) && t == "unchanged",
          "a NaN price refuses the candle and leaves the row untouched");
    const altair::RawCandle z = day_candle("2026-09-08", 0.0);
    std::string zs = "unchanged";
    check(!ds::append_prices(zs, z) && zs == "unchanged",
          "and so does a ZERO price -- a halted or malformed candle is not a"
          " bar that traded at nothing");
}

// ── 8 ────────────────────────────────────────────────────────────────────
void force_refuses_a_partial_month() {
    std::printf("\n8 force_refuses_a_partial_month\n");
    std::int64_t today = 0;
    (void)altair::parse_date("2026-09-16", today);
    check(!ds::force_covers_month("2024-09", "2024-09-15", "2024-10-31", today),
          "--from 2024-09-15 --force on a full September is REFUSED -- it"
          " erased September 1-14");
    check(ds::force_covers_month("2024-09", "2024-09-01", "2024-10-31", today),
          "a window covering the whole month may replace it");
    check(!ds::force_covers_month("2024-02", "2024-02-01", "2024-02-28", today),
          "a leap February needs the 29th");
    check(ds::force_covers_month("2026-09", "2026-09-01", "2026-09-16", today),
          "the month in progress needs only up to today");
    check(!ds::force_covers_month("2026-09", "2026-09-01", "2026-09-15", today),
          "and not less");
    check(!ds::force_covers_month("2027-05", "2020-01-01", "2026-09-16", today),
          "force_refuses_a_month_the_window_does_not_reach: a month that has"
          " not started must still be covered to its last day -- clamping to"
          " today let --force erase a file the window never touched");
}

// ── 9 ────────────────────────────────────────────────────────────────────
void symbols_are_exact_and_days_are_numbers() {
    std::printf("\n9 symbols_are_exact_and_days_are_numbers\n");
    const std::vector<std::string_view> known{"nifty", "banknifty", "indiavix"};
    const auto b = ds::select_names("banknifty", known);
    check(b.picked.size() == 1 && b.picked[0] == 1 && b.unknown.empty(),
          "--symbols banknifty selects banknifty ONLY -- it used to select"
          " nifty as well");
    const auto two = ds::select_names("indiavix, nifty", known);
    check(two.picked.size() == 2 && two.picked[0] == 0 && two.picked[1] == 2,
          "a list selects each named series once");
    const auto typo = ds::select_names("nifty,niftyy", known);
    check(typo.unknown.size() == 1 && typo.unknown[0] == "niftyy",
          "an unknown name is reported, not ignored");
    check(ds::parse_count("400", 1, 20000) == 400, "--days 400 is 400");
    check(!ds::parse_count("40x", 1, 20000) && !ds::parse_count("x", 1, 20000)
              && !ds::parse_count("0", 1, 20000)
              && !ds::parse_count("-5", 1, 20000),
          "and 40x, x, 0 and -5 are refused rather than read as 40 or 0");
}

} // namespace

int main() {
    std::printf("CX02-A2 -- dataset merge and replacement\n");
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path dir = fs::temp_directory_path()
                         / ("altair_cx02_merge_" + std::to_string(stamp));
    std::error_code ec;
    fs::create_directories(dir, ec);
    check(!ec && fs::is_directory(dir), "a scratch directory exists");

    replace_checked_writes_and_never_removes_first(dir);
    concurrent_replacements_use_private_temps(dir);
    load_keeps_headerless_first_row_and_per_file_headers();
    only_changed_files_are_rewritten();
    material_change_replaces_and_rounding_does_not();
    zero_price_is_refused();
    unparseable_close_blocks_rather_than_matching();
    series_update_lock_serializes_full_updates(dir);
    empty_answer_over_trading_days_is_not_nothing_new(dir);
    future_last_stamp_refuses_rather_than_clamping();
    unkeyed_or_duplicate_rows_block_the_rewrite();
    incomplete_coverage_merges_nothing();
    prices_are_exact_and_nan_is_refused();
    force_refuses_a_partial_month();
    symbols_are_exact_and_days_are_numbers();

    fs::remove_all(dir, ec);
    std::printf("\n%s -- %d failing check(s)\n",
                failures == 0 ? "PASS" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
