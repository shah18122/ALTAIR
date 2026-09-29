// app/dataset_merge.hpp -- merging fetched bars into dataset/ without losing any.
//
// CX02-A2. Fixes C14-005, C14-006, C14-007 and the CLI half of C14-016.
//
// P34-01 wrote the merge inside altair_kite_update's main(), where nothing
// could test it, and the audit found five ways it lost stored data:
//
//   1. The rewrite never checked the stream, so a disk-full or an antivirus
//      lock mid-write renamed a TRUNCATED temp file over a good month.
//   2. It removed the target BEFORE renaming the temp over it. When the rename
//      then failed -- a reader holding the path is enough on Windows -- the
//      month vanished and its rows survived only in a `.tmp` no loader reads.
//   3. It rewrote EVERY file in the series whenever one bar changed, including
//      `vendor_pre2015.csv`, which it had no business touching.
//   4. A file with no header lost its first data row on every run: the first
//      line was taken as the header only if it began with 't', and otherwise
//      was neither header nor row.
//   5. One header, from whichever file was read last, was written into all.
//
// And it never called `verify_coverage`, so a window Kite truncated was merged
// as if complete -- the exact failure P2-12 exists to stop, and the fetcher
// beside it refuses.
//
// WHAT CHANGED, IN ONE SENTENCE EACH.
//
// A file is replaced by one checked write to a temp and one rename OVER the
// target, so the target is either the old bytes or the new ones. Only files a
// merge actually changed are written. A file this reader cannot fully key --
// an unparseable row, or the same bar twice -- is never rewritten, because
// rewriting it would drop what could not be keyed. Headers are per file. And a
// fetched window that does not cover what was asked merges nothing.
//
// What this does NOT do: repair precision already lost. A bar stored as
// 24123.5 against a fresh 24123.45 differs by 0.02 bp, which is "decimal
// places" under the 1 bp rule, and it stays. Rewriting stored history needs a
// login, a refetch and a confirmed scope (CX02-A3), not a side effect.

#pragma once

#include <app/price_text.hpp>
#include <broker/kite_historical.hpp>

#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <istream>
#include <limits>
#include <optional>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  include <fcntl.h>
#  include <io.h>
#  include <process.h>
#  include <share.h>
#  include <sys/stat.h>
#else
#  include <fcntl.h>
#  include <sys/file.h>
#  include <unistd.h>
#endif

namespace altair::dataset {

inline constexpr std::int64_t kDayNs = 86'400'000'000'000LL;

// ── replacing a file ─────────────────────────────────────────────────────

enum class ReplaceError : std::uint8_t {
    /// The temp file could not be created. The target was not touched.
    OpenTemp,
    /// A write or flush to the temp failed. The target was not touched.
    WriteTemp,
    /// Closing the temp failed, so its bytes are not known to be on disk.
    CloseTemp,
    /// The rename over the target failed. The target still holds its old
    /// bytes; the temp has been removed.
    Rename
};

[[nodiscard]] inline const char* to_string(ReplaceError e) noexcept {
    switch (e) {
    case ReplaceError::OpenTemp:  return "could not create the temp file";
    case ReplaceError::WriteTemp: return "write to the temp file failed";
    case ReplaceError::CloseTemp: return "closing the temp file failed";
    case ReplaceError::Rename:    return "rename over the target failed";
    }
    return "unknown";
}

/// Replace `path` with `bytes`, or leave it exactly as it was.
///
/// ONE rename OVER the target, never remove-then-rename. Each writer first
/// creates a unique temp with exclusive-create semantics, so concurrent
/// writers cannot share or truncate one another's temp. On POSIX that rename
/// is atomic; on Windows `std::filesystem::rename` replaces an existing file
/// in one call, and a failure leaves the old file in place. Either way there
/// is no instant at which the month does not exist.
///
/// Text mode, deliberately: every dataset/ file these tools wrote before was
/// written in text mode, and a rewrite should not change the line endings of
/// the rows it did not touch.
///
/// Not a durability guarantee against power loss -- no fsync. A crashed
/// PROCESS leaves the old or the new file; a crashed MACHINE may leave the new
/// file short. Recorded in prompts/cx02 as a deferred limit.
[[nodiscard]] inline std::expected<void, ReplaceError>
replace_file_checked(const std::filesystem::path& path, std::string_view bytes) {
    static std::atomic<std::uint64_t> sequence{0};
    const auto ticket = sequence.fetch_add(1, std::memory_order_relaxed);
    const auto clock = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
#if defined(_WIN32)
    const auto pid = static_cast<std::uint64_t>(_getpid());
#else
    const auto pid = static_cast<std::uint64_t>(::getpid());
#endif

    std::filesystem::path tmp;
    std::FILE* f = nullptr;
    for (std::uint64_t attempt = 0; attempt < 64; ++attempt) {
        tmp = path;
        tmp += ".tmp." + std::to_string(pid) + "." + std::to_string(clock)
               + "." + std::to_string(ticket + attempt);
        int open_error = 0;
#if defined(_WIN32)
        const errno_t rc = _wfopen_s(&f, tmp.c_str(), L"wx");
        open_error = static_cast<int>(rc);
#else
        f = std::fopen(tmp.c_str(), "wx");
        if (f == nullptr) { open_error = errno; }
#endif
        if (f != nullptr) { break; }
        if (open_error != EEXIST) {
            return std::unexpected(ReplaceError::OpenTemp);
        }
    }
    if (f == nullptr) { return std::unexpected(ReplaceError::OpenTemp); }

    std::error_code ec;
    const bool wrote =
        std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    const bool flushed = std::fflush(f) == 0;
    const bool closed = std::fclose(f) == 0;
    if (!wrote || !flushed) {
        std::filesystem::remove(tmp, ec);
        return std::unexpected(ReplaceError::WriteTemp);
    }
    if (!closed) {
        std::filesystem::remove(tmp, ec);
        return std::unexpected(ReplaceError::CloseTemp);
    }
    bool renamed = false;
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        ec.clear();
        std::filesystem::rename(tmp, path, ec);
        if (!ec) {
            renamed = true;
            break;
        }
        const bool transient =
            ec == std::errc::permission_denied
            || ec == std::errc::file_exists
            || ec == std::errc::device_or_resource_busy
            || ec == std::errc::resource_unavailable_try_again;
        if (!transient || attempt + 1 == 100) { break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!renamed) {
        std::error_code ignored;
        std::filesystem::remove(tmp, ignored);
        return std::unexpected(ReplaceError::Rename);
    }
    return {};
}

enum class EmptyAnswerEvent : std::uint8_t {
    /// The API answered with no candles for a window containing a weekday.
    TradingWindowEmpty,
    /// An empty answer over a weekend; it does not break an empty trading-day
    /// streak because it proves nothing about a session.
    NoTradingSession,
    /// Any non-empty valid API answer breaks the consecutive-empty streak.
    NonemptyResponse
};

enum class EmptyAnswerError : std::uint8_t {
    Lock,
    Read,
    Malformed,
    Overflow,
    Write
};

[[nodiscard]] inline const char* to_string(EmptyAnswerError e) noexcept {
    switch (e) {
    case EmptyAnswerError::Lock:      return "could not acquire the state lock";
    case EmptyAnswerError::Read:      return "could not read the state file";
    case EmptyAnswerError::Malformed: return "the state file is malformed";
    case EmptyAnswerError::Overflow:  return "the consecutive-empty counter overflowed";
    case EmptyAnswerError::Write:     return "could not publish the state file";
    }
    return "unknown";
}

/// A per-series lock. Windows' exclusive sharing and POSIX flock both release
/// automatically if the process exits, so there is no stale lock-file state.
class UpdateStateLock final {
public:
    UpdateStateLock() = default;
    UpdateStateLock(const UpdateStateLock&) = delete;
    UpdateStateLock& operator=(const UpdateStateLock&) = delete;

    ~UpdateStateLock() {
#if defined(_WIN32)
        if (fd_ >= 0) { (void)::_close(fd_); }
#else
        if (fd_ >= 0) {
            (void)::flock(fd_, LOCK_UN);
            (void)::close(fd_);
        }
#endif
    }

    [[nodiscard]] bool acquire(const std::filesystem::path& path) noexcept {
#if defined(_WIN32)
        constexpr unsigned attempts = 400;
        for (unsigned i = 0; i < attempts; ++i) {
            const errno_t rc = _wsopen_s(&fd_, path.c_str(),
                _O_CREAT | _O_RDWR | _O_BINARY, _SH_DENYRW,
                _S_IREAD | _S_IWRITE);
            if (rc == 0 && fd_ >= 0) { return true; }
            if (rc != EACCES && rc != EAGAIN) { return false; }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        return false;
#else
        fd_ = ::open(path.c_str(), O_CREAT | O_RDWR, 0666);
        return fd_ >= 0 && ::flock(fd_, LOCK_EX) == 0;
#endif
    }

private:
    int fd_ = -1;
};

/// Persist and return the consecutive empty trading-window response count.
/// `state_path` is a small sidecar beside the series CSVs. The advisory lock
/// serializes concurrent updater invocations; the count itself is published
/// through the same unique-temp, checked rename as dataset files.
[[nodiscard]] inline std::expected<std::uint64_t, EmptyAnswerError>
update_empty_answer_streak(const std::filesystem::path& state_path,
                           EmptyAnswerEvent event) {
    std::filesystem::path lock_path = state_path;
    lock_path += ".lock";
    UpdateStateLock lock;
    if (!lock.acquire(lock_path)) {
        return std::unexpected(EmptyAnswerError::Lock);
    }

    std::error_code ec;
    const bool exists = std::filesystem::exists(state_path, ec);
    if (ec) { return std::unexpected(EmptyAnswerError::Read); }
    std::uint64_t count = 0;
    if (exists) {
        std::ifstream in(state_path, std::ios::binary);
        if (!in) { return std::unexpected(EmptyAnswerError::Read); }
        std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
        if (in.bad()) { return std::unexpected(EmptyAnswerError::Read); }
        std::size_t first = text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            return std::unexpected(EmptyAnswerError::Malformed);
        }
        std::size_t last = text.find_last_not_of(" \t\r\n");
        const char* begin = text.data() + first;
        const char* end = text.data() + last + 1;
        const auto parsed = std::from_chars(begin, end, count);
        if (parsed.ec != std::errc{} || parsed.ptr != end) {
            return std::unexpected(EmptyAnswerError::Malformed);
        }
    }

    switch (event) {
    case EmptyAnswerEvent::TradingWindowEmpty:
        if (count == std::numeric_limits<std::uint64_t>::max()) {
            return std::unexpected(EmptyAnswerError::Overflow);
        }
        ++count;
        break;
    case EmptyAnswerEvent::NoTradingSession:
        break;
    case EmptyAnswerEvent::NonemptyResponse:
        count = 0;
        break;
    }

    if (count > 0 || exists) {
        const auto wr = replace_file_checked(state_path,
                                             std::to_string(count) + "\n");
        if (!wr) { return std::unexpected(EmptyAnswerError::Write); }
    }
    return count;
}

// ── what a bar is ────────────────────────────────────────────────────────

/// The key two stamps must share to be the SAME BAR.
///
/// dataset/spot/nifty/1d holds `2026-09-08` and Kite returns
/// `2026-09-08T00:00:00+05:30`. Keyed on the full string those are different
/// bars, and P34-01's first version added a duplicate of every re-fetched day.
/// A daily bar is identified by its DATE.
[[nodiscard]] inline std::string bar_key(const std::string& stamp,
                                         std::int64_t iv_ns) {
    return iv_ns >= kDayNs && stamp.size() >= 10 ? stamp.substr(0, 10) : stamp;
}

/// Is this a `YYYY-MM.csv` partition file?
[[nodiscard]] inline bool is_month_file(const std::string& name) {
    if (name.size() != 11 || name.compare(7, 4, ".csv") != 0) { return false; }
    for (std::size_t i : {0u, 1u, 2u, 3u, 5u, 6u}) {
        if (name[i] < '0' || name[i] > '9') { return false; }
    }
    return name[4] == '-';
}

/// Is the bar starting at `start_ns` finished, at `now_ns`?
///
/// A DAILY BAR IS NOT FINISHED AT MIDNIGHT. Stamped 00:00, it covers 09:15 to
/// 15:30 IST, so start + one day would call today's partial bar complete for
/// the whole afternoon.
[[nodiscard]] inline bool bar_is_complete(std::int64_t start_ns,
                                          std::int64_t iv_ns,
                                          std::int64_t now_ns) {
    if (iv_ns >= kDayNs) {
        const std::int64_t ist = start_ns + altair::detail::kIstOffsetNs;
        const std::int64_t day = ist / kDayNs;
        const std::int64_t settle =
            day * kDayNs + (15 * 3600 + 30 * 60) * 1'000'000'000LL
            - altair::detail::kIstOffsetNs;
        return now_ns >= settle;
    }
    return start_ns + iv_ns <= now_ns;
}

/// The close column of a stored row, for the 1 bp comparison only. Analytics,
/// not the ledger: a double is fine here.
///
/// CX02-A2c (R-AB-027). This used to answer 0.0 for a row it could not read --
/// too few fields, an empty close, `nan`, a legacy column order -- and 0.0 then
/// took the `Same` path, so an unreadable stored row silently beat the fetched
/// bar and nothing was printed. An unreadable close is now ABSENT, and every
/// caller has to decide what that means.
[[nodiscard]] inline std::optional<double> close_of(const std::string& line) {
    std::size_t at = 0;
    for (int f = 0; f < 4; ++f) {
        at = line.find(',', at);
        if (at == std::string::npos) { return std::nullopt; }
        ++at;
    }
    std::size_t end = line.find(',', at);
    if (end == std::string::npos) { end = line.size(); }
    while (end > at && (line[end - 1] == ' ' || line[end - 1] == '\r')) {
        --end;
    }
    if (end == at) { return std::nullopt; }
    double v = 0.0;
    if (!parse_exact_double(line.data() + at, line.data() + end, v)
        || !std::isfinite(v)) {
        return std::nullopt;
    }
    return v;
}

/// `,open,high,low,close,` for one candle, every price exact (CX02-A1a).
/// Volume is left for the caller. False, with `out` unchanged, if any price
/// is not writable -- a NaN from the API is a malformed answer, not a bar.
[[nodiscard]] inline bool append_prices(std::string& out, const RawCandle& c) {
    std::string s;
    char buf[kPriceTextMax];
    for (const double v : {c.open, c.high, c.low, c.close}) {
        const auto n = format_price(v, buf, sizeof buf);
        if (!n) { return false; }
        s += ',';
        s.append(buf, *n);
    }
    s += ',';
    out += s;
    return true;
}

// ── a series on disk ─────────────────────────────────────────────────────

struct StoredFile {
    /// Empty when the file has none -- and then none is written back.
    std::string header;
    /// Bar key -> the CSV row, verbatim.
    std::map<std::string, std::string> rows;
    /// Rows with no usable stamp. Any at all and the file is never rewritten.
    std::size_t unkeyed = 0;
    /// The same bar twice in one file. Likewise blocks a rewrite, because
    /// rewriting would silently keep one and drop the other.
    std::size_t duplicate_keys = 0;
    /// Read from disk, as opposed to created by a merge.
    bool loaded = false;
    /// Changed by a merge, so it needs writing.
    bool dirty = false;
};

[[nodiscard]] inline bool writable(const StoredFile& f) noexcept {
    return f.unkeyed == 0 && f.duplicate_keys == 0;
}

struct Series {
    std::map<std::string, StoredFile> files;
    /// Bar key -> the file it is stored in.
    std::map<std::string, std::string> where;
    std::string last_stamp;
    /// The header a NEW file gets: the one the series already uses.
    std::string header = "time,open,high,low,close,volume";
    /// `all.csv` when the series is stored as one file, else empty and new
    /// rows go to their month partition.
    std::string single_file;
    bool stamp_is_date_only = false;
    /// The same bar in two different files. Counted; the later file wins
    /// `where`, as it always did.
    std::size_t cross_file_duplicates = 0;
};

/// Read one stored file into the series.
inline void load_file(Series& s, const std::string& name, std::istream& in,
                      std::int64_t iv_ns) {
    StoredFile& f = s.files[name];
    f.loaded = true;
    if (!is_month_file(name) && (s.single_file.empty() || name == "all.csv")) {
        s.single_file = name;
    }
    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') { line.pop_back(); }
        if (line.empty()) { continue; }
        if (first) {
            first = false;
            // A stamp starts with a digit. Anything else on the first line is
            // a header; a digit means the file has none and this is a ROW.
            if (line[0] < '0' || line[0] > '9') {
                f.header = line;
                s.header = line;
                continue;
            }
        }
        const std::string stamp = line.substr(0, line.find(','));
        if (stamp.size() < 10) {
            ++f.unkeyed;
            continue;
        }
        // CX02-A2c (R-AB-027). A row whose CLOSE cannot be read is a row this
        // reader does not fully understand, exactly like one with no usable
        // stamp. It is kept, counted, and it blocks the rewrite.
        if (!close_of(line)) {
            ++f.unkeyed;
        }
        const std::string key = bar_key(stamp, iv_ns);
        if (f.rows.contains(key)) {
            ++f.duplicate_keys;
        } else if (const auto w = s.where.find(key);
                   w != s.where.end() && w->second != name) {
            ++s.cross_file_duplicates;
        }
        f.rows[key] = line;
        s.where[key] = name;
        if (stamp.size() == 10) { s.stamp_is_date_only = true; }
        // MAX ACROSS EVERY FILE, not the last one by name: banknifty/1d holds
        // all.csv and vendor_pre2015.csv, and the vendor file sorts LAST.
        if (stamp > s.last_stamp) { s.last_stamp = stamp; }
    }
}

enum class MergeResult : std::uint8_t {
    Added,
    /// Stored and fetched closes differ by more than 1 bp: the fresh bar wins.
    Replaced,
    /// They differ by decimal places only: the stored bar stays.
    Rounding,
    Same,
    /// The bar belongs in a file this reader cannot fully key. Not merged.
    BlockedFile,
    /// One of the two closes is unusable -- unreadable, or zero. Not merged,
    /// and deliberately NOT reported as `Same` (R-AB-026/027).
    Unusable
};

/// Merge one COMPLETE bar. `prices` is `,o,h,l,c,` from `append_prices`, and
/// the rest of the row (volume, oi) is whatever follows it -- empty here.
[[nodiscard]] inline MergeResult
merge_bar(Series& s, const std::string& full_stamp, std::string_view prices,
          double close, std::int64_t iv_ns) {
    if (!std::isfinite(close) || close <= 0.0) {
        return MergeResult::Unusable;
    }
    const std::string key = bar_key(full_stamp, iv_ns);
    // Written in the shape the series already uses. Mixing `2026-09-08` and
    // `2026-09-08T00:00:00+05:30` in one file makes every reader's stamp
    // parser the arbiter of what a bar is.
    const std::string stamp = (s.stamp_is_date_only && iv_ns >= kDayNs)
                                  ? full_stamp.substr(0, 10) : full_stamp;
    std::string line = stamp;
    line.append(prices);

    const auto w = s.where.find(key);
    const std::string file =
        w != s.where.end() ? w->second
        : (!s.single_file.empty() ? s.single_file
                                  : full_stamp.substr(0, 7) + ".csv");
    StoredFile& f = s.files[file];
    if (!writable(f)) { return MergeResult::BlockedFile; }
    if (!f.loaded && f.rows.empty() && f.header.empty()) {
        f.header = s.header;
    }

    const auto it = f.rows.find(key);
    if (it == f.rows.end()) {
        f.rows.emplace(key, std::move(line));
        s.where[key] = file;
        f.dirty = true;
        if (stamp > s.last_stamp) { s.last_stamp = stamp; }
        return MergeResult::Added;
    }
    // MATERIAL or DECIMAL PLACES. One basis point separates a bar captured
    // mid-session and frozen (India VIX 10.80 against a settled 10.68 is
    // 110 bp) from a re-fetch that only carries a different number of
    // decimals.
    // A comparison needs TWO usable closes. An unreadable stored close, or a
    // zero on either side, used to fall through to `Same`: the stored row won,
    // the fetched bar was dropped, and the run printed nothing at all.
    const auto a = close_of(it->second);
    const double b = close;
    if (!a || *a <= 0.0) {
        return MergeResult::Unusable;
    }
    const double d = std::fabs(10'000.0 * std::log(b / *a));
    if (d > 1.0) {
        it->second = std::move(line);
        f.dirty = true;
        return MergeResult::Replaced;
    }
    return d > 0.0 ? MergeResult::Rounding : MergeResult::Same;
}

/// The bytes a stored file is written back as.
[[nodiscard]] inline std::string render(const StoredFile& f) {
    std::string out;
    if (!f.header.empty()) {
        out += f.header;
        out += '\n';
    }
    for (const auto& [key, row] : f.rows) {
        out += row;
        out += '\n';
    }
    return out;
}

// ── did the API answer the question that was asked ───────────────────────

enum class Coverage : std::uint8_t {
    Complete,
    /// No candles, over a window that contained no WEEKDAY at all -- a
    /// weekend since the last stored bar, not a truncation.
    NothingNew,
    /// Candles missing from the window. Merge nothing.
    Incomplete,
    /// Unparseable window, or candles out of order.
    Malformed
};

enum class UpdateWindowError : std::uint8_t {
    InvalidLookback,
    InvalidLastStamp,
    FutureLastStamp
};

/// Choose the first day of an incremental update window. An empty series uses
/// the requested lookback; an existing series is topped up from its latest
/// stored day so a partial intraday session is not skipped. Refuse malformed
/// or future stored dates rather than silently falling back or clamping a
/// future stamp into an empty window.
[[nodiscard]] inline std::expected<std::int64_t, UpdateWindowError>
update_from_day(std::string_view last_stamp, std::int64_t today,
                int lookback_days) {
    if (lookback_days <= 0) {
        return std::unexpected(UpdateWindowError::InvalidLookback);
    }
    if (last_stamp.empty()) { return today - lookback_days; }
    if (last_stamp.size() < 10) {
        return std::unexpected(UpdateWindowError::InvalidLastStamp);
    }
    const std::string_view date = last_stamp.substr(0, 10);
    std::int64_t last_day = 0;
    if (!parse_date(date, last_day) || date_string(last_day) != date) {
        return std::unexpected(UpdateWindowError::InvalidLastStamp);
    }
    if (last_day > today) {
        return std::unexpected(UpdateWindowError::FutureLastStamp);
    }
    return last_day;
}

[[nodiscard]] inline Coverage
judge_coverage(const std::vector<RawCandle>& got, std::string_view from,
               std::string_view to, std::int64_t max_gap_days) {
    std::int64_t f = 0, t = 0;
    if (!parse_date(from, f) || !parse_date(to, t) || t < f) {
        return Coverage::Malformed;
    }
    if (got.empty()) {
        // CX02-A2c (R-AB-028). An empty answer is NothingNew only when the
        // window held no trading day at all. The window starts at the last
        // STORED day, so `t - f` was never more than the tolerance and every
        // empty answer passed -- a permanently broken series reported success
        // on every run, for ever.
        //
        // Exchange holidays are not known here, so a holiday stretch reports
        // Incomplete and prints. That is the safe direction: nothing merges
        // either way, and a refusal is visible.
        for (std::int64_t d = f; d <= t; ++d) {
            const std::int64_t dow = (d + 4) % 7;   // epoch day 0 was a Thursday
            if (dow != 0 && dow != 6) {
                return Coverage::Incomplete;
            }
        }
        return Coverage::NothingNew;
    }
    const auto r = verify_coverage(got, from, to, max_gap_days);
    if (r) { return Coverage::Complete; }
    return r.error() == HistError::Malformed ? Coverage::Malformed
                                             : Coverage::Incomplete;
}

/// `--force` on an existing month REPLACES it. Allowed only when the requested
/// window covers every day that file could hold, or rows outside the window
/// are deleted (C14-007: `--from 2024-09-15 --force` erased Sept 1-14).
///
/// `month` is `YYYY-MM`; `today` bounds a month still in progress, which
/// cannot hold rows after today.
[[nodiscard]] inline bool force_covers_month(std::string_view month,
                                             std::string_view from,
                                             std::string_view to,
                                             std::int64_t today) {
    if (month.size() != 7) { return false; }
    std::string first(month);
    first += "-01";
    std::int64_t m0 = 0, f = 0, t = 0;
    if (!parse_date(first, m0) || !parse_date(from, f) || !parse_date(to, t)) {
        return false;
    }
    // The last day of the month: step to the 28th, then forward until the
    // month changes.
    std::int64_t last = m0 + 27;
    while (date_string(last + 1).compare(0, 7, month) == 0) { ++last; }
    // RULE 11: not a capacity clamp -- proven, and ONLY for the month in
    // progress. A month that has ended must be covered to its last day, and so
    // must a month that has not started: a file for it can only hold
    // mis-stamped rows, and --force would erase them without the window ever
    // reaching them (R-AB-031). Clamping to today for every future month is
    // what made that possible.
    const bool in_progress = (m0 <= today && today <= last);
    const std::int64_t need_to = in_progress ? today : last;
    return f <= m0 && t >= need_to;
}

// ── command line ─────────────────────────────────────────────────────────

struct NameSelection {
    /// Indices into `known`, in `known` order, each at most once.
    std::vector<std::size_t> picked;
    /// Names that matched nothing. Non-empty means refuse the command.
    std::vector<std::string> unknown;
};

/// Resolve a comma list against known names. EXACT names: `--symbols
/// banknifty` used to select nifty as well, because it was a substring match.
[[nodiscard]] inline NameSelection
select_names(std::string_view list, const std::vector<std::string_view>& known) {
    NameSelection out;
    std::vector<bool> taken(known.size(), false);
    std::size_t at = 0;
    while (at <= list.size()) {
        const std::size_t comma = list.find(',', at);
        const std::size_t end = comma == std::string_view::npos ? list.size()
                                                                : comma;
        std::string_view name = list.substr(at, end - at);
        while (!name.empty() && name.front() == ' ') { name.remove_prefix(1); }
        while (!name.empty() && name.back() == ' ') { name.remove_suffix(1); }
        if (!name.empty()) {
            bool found = false;
            for (std::size_t i = 0; i < known.size(); ++i) {
                if (known[i] == name) {
                    taken[i] = true;
                    found = true;
                    break;
                }
            }
            if (!found) { out.unknown.emplace_back(name); }
        }
        if (comma == std::string_view::npos) { break; }
        at = comma + 1;
    }
    for (std::size_t i = 0; i < known.size(); ++i) {
        if (taken[i]) { out.picked.push_back(i); }
    }
    return out;
}

/// A whole number in [lo, hi], and nothing else. `atoi("40x")` is 40 and
/// `atoi("x")` is 0; neither is a day count.
[[nodiscard]] inline std::expected<int, std::errc>
parse_count(std::string_view s, int lo, int hi) {
    int v = 0;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) {
        return std::unexpected(std::errc::invalid_argument);
    }
    if (v < lo || v > hi) {
        return std::unexpected(std::errc::result_out_of_range);
    }
    return v;
}

} // namespace altair::dataset
