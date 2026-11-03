// models/depth_features.hpp -- five-level order-book depth as forecast features.
//
// FROM BOOKS TO MINUTES TO BARS, NEVER BACKWARDS.
//
// 1. DepthMinuteBuilder takes every book of ONE instrument (5 levels from the
//    FYERS data socket or up to 50 from TBT; only the top five are read) and
//    keeps, per exchange minute, the SUMS needed to average later: touch OBI,
//    5-level OBI, 5-level weighted OBI (w_i = exp(-0.25 i), as book/flow.hpp),
//    spread in bps, the multi-level Cont-Kukanov-Stoikov OFI over the five
//    levels and its absolute sum, and the last microprice offset from the mid
//    in bps. A minute is stamped at its END, the moment it is known.
// 2. The recorder's nightly decode writes those minutes to depth_1m.csv
//    (app/training_store.hpp), the near future's under
//    data/store/depth_bars/fut/<inst>/1m/<day>.csv.
// 3. depth_for_bars() turns the minutes into one row of kDepthFeatures per
//    model bar. The bar ending at E uses ONLY minutes whose end is in
//    (E - width, E]: nothing from after the bar it describes.
//
// THE DATA LIMIT, SAID ONCE. FYERS serves no historical depth. Every depth
// row comes from ALTAIR's own recorder tapes, so depth history begins on the
// first recorded day and has holes wherever the recorder did not run. An
// index (NIFTY 50, NIFTY BANK) has no order book at all; its depth is the
// near-month future's book.
//
// HONEST EXPECTATION. features/book_flow.hpp's warning stands: book imbalance
// decays in well under a second. Averaged over a 15- or 60-minute bar it may
// carry nothing. The forecast fit therefore reports the returns-only score on
// the same rows beside the depth score, so the data decides.
//
// THREADING: one writer per builder, no locks, no allocation per book except
// when a finished minute is appended to the caller's vector.

#pragma once

#include <book/flow.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace altair::depth {

inline constexpr std::size_t kLevels = 5;
inline constexpr std::size_t kDepthFeatures = 6;
inline constexpr std::int64_t kMinuteNs = 60'000'000'000LL;
inline constexpr double kWeightDecay = 0.25;

/// Names of the bar features, in column order.
inline constexpr std::array<const char*, kDepthFeatures> kFeatureNames{
    "obi_touch", "obi_5", "obi_5_weighted", "microprice_offset_bps", "spread_bps", "ofi_5_norm"};

/// One minute of one instrument's books. Sums, so minutes add into bars.
struct DepthMinute {
    std::int64_t end_ns = 0;          ///< UTC ns, the minute's END
    std::uint32_t books = 0;          ///< tradable books seen in the minute
    double obi1_sum = 0.0, obi5_sum = 0.0, obi5w_sum = 0.0, spread_bps_sum = 0.0;
    double micro_bps_last = 0.0;      ///< (microprice - mid) / mid, bps, last book
    double ofi5 = 0.0, ofi5_abs = 0.0;   ///< sum of e and of |e|, units
};

/// A price level: paise and units. Any type with .px and .qty works too.
struct BookRung {
    std::int64_t px = 0, qty = 0;
};

[[nodiscard]] constexpr std::int64_t minute_end(std::int64_t ns) noexcept {
    // Floor division, so a negative stamp cannot round toward zero.
    const std::int64_t q = ns / kMinuteNs - ((ns % kMinuteNs) < 0 ? 1 : 0);
    return (q + 1) * kMinuteNs;
}

class DepthMinuteBuilder {
public:
    /// A book: `nb` bids and `na` asks, best first. Books at or before the
    /// last one's time are refused (false): the feed clock never runs back.
    template <class L>
    bool on_book(const L* bids, std::size_t nb, const L* asks, std::size_t na, std::int64_t ns,
                 std::vector<DepthMinute>& finished) {
        if (ns <= 0 || (last_ns_ > 0 && ns < last_ns_)) return false;
        last_ns_ = ns;
        const std::int64_t end = minute_end(ns);
        if (have_cur_ && end != cur_.end_ns) {
            if (cur_.books > 0) finished.push_back(cur_);
            have_cur_ = false;
        }
        if (!have_cur_) { cur_ = DepthMinute{}; cur_.end_ns = end; have_cur_ = true; }
        Snap s;
        s.n = 0;
        // RULE 11: deliberate truncation to the five-level book -- the features
        // are DEFINED on levels 1-5 (a 50-level TBT book is read as its top
        // five), so deeper levels are out of scope, not lost.
        const std::size_t n = std::min({nb, na, kLevels});
        for (std::size_t k = 0; k < n; ++k) {
            if (bids[k].px <= 0 || asks[k].px <= 0 || bids[k].qty < 0 || asks[k].qty < 0) break;
            if (k > 0 && (bids[k].px >= bids[k - 1].px || asks[k].px <= asks[k - 1].px)) break;
            s.bp[k] = bids[k].px; s.bq[k] = bids[k].qty; s.ap[k] = asks[k].px; s.aq[k] = asks[k].qty;
            s.n = k + 1;
        }
        // Untradable (empty, one-sided, locked, crossed): no reading, and no
        // OFI measured against it from either side.
        if (s.n == 0 || s.ap[0] <= s.bp[0]) { have_prev_ = false; return true; }
        const auto o1 = flow::imbalance(s.bq[0], s.aq[0]);
        std::int64_t b5 = 0, a5 = 0;
        double bw = 0.0, aw = 0.0;
        for (std::size_t k = 0; k < s.n; ++k) {
            b5 += s.bq[k]; a5 += s.aq[k];
            const double w = std::exp(-kWeightDecay * static_cast<double>(k));
            bw += w * static_cast<double>(s.bq[k]);
            aw += w * static_cast<double>(s.aq[k]);
        }
        const auto o5 = flow::imbalance(b5, a5);
        const auto micro = flow::microprice(s.bp[0], s.bq[0], s.ap[0], s.aq[0]);
        if (!o1 || !o5 || !micro || !(bw + aw > 0.0)) { have_prev_ = false; return true; }
        const double mid = 0.5 * static_cast<double>(s.bp[0] + s.ap[0]);
        ++cur_.books;
        cur_.obi1_sum += *o1;
        cur_.obi5_sum += *o5;
        cur_.obi5w_sum += (bw - aw) / (bw + aw);
        cur_.spread_bps_sum += 1e4 * static_cast<double>(s.ap[0] - s.bp[0]) / mid;
        cur_.micro_bps_last = 1e4 * (micro->value() - mid) / mid;
        if (have_prev_) {
            const std::size_t m = std::min(prev_.n, s.n);
            double e = 0.0;
            for (std::size_t k = 0; k < m; ++k)
                e += static_cast<double>(flow::ofi_level(prev_.bp[k], prev_.bq[k], prev_.ap[k], prev_.aq[k],
                                                         s.bp[k], s.bq[k], s.ap[k], s.aq[k]));
            cur_.ofi5 += e;
            cur_.ofi5_abs += std::fabs(e);
        }
        prev_ = s;
        have_prev_ = true;
        return true;
    }

    /// The stream broke (disconnect, sequence gap): the next book is not
    /// diffed against the last one seen.
    void break_stream() noexcept { have_prev_ = false; }

    /// The minute in progress, if it has a reading.
    [[nodiscard]] std::optional<DepthMinute> current() const noexcept {
        if (have_cur_ && cur_.books > 0) return cur_;
        return std::nullopt;
    }

    /// End of input: the minute in progress is finished too.
    void flush(std::vector<DepthMinute>& finished) {
        if (have_cur_ && cur_.books > 0) finished.push_back(cur_);
        have_cur_ = false;
    }

private:
    struct Snap {
        std::size_t n = 0;
        std::array<std::int64_t, kLevels> bp{}, bq{}, ap{}, aq{};
    };
    DepthMinute cur_{};
    Snap prev_{};
    bool have_cur_ = false, have_prev_ = false;
    std::int64_t last_ns_ = 0;
};

using DepthBar = std::array<double, kDepthFeatures>;

[[nodiscard]] inline DepthBar missing_bar() noexcept {
    DepthBar b;
    b.fill(std::numeric_limits<double>::quiet_NaN());
    return b;
}

/// Adds one minute into running sums for a bar.
struct BarSums {
    std::uint32_t books = 0, minutes = 0;
    double obi1 = 0.0, obi5 = 0.0, obi5w = 0.0, spread = 0.0, micro_last = 0.0, ofi = 0.0, ofi_abs = 0.0;
    void add(const DepthMinute& m) noexcept {
        if (m.books == 0) return;
        ++minutes;
        books += m.books;
        obi1 += m.obi1_sum; obi5 += m.obi5_sum; obi5w += m.obi5w_sum; spread += m.spread_bps_sum;
        micro_last = m.micro_bps_last;
        ofi += m.ofi5; ofi_abs += m.ofi5_abs;
    }
    /// The bar row, or all-NaN when fewer than `min_minutes` minutes had a book.
    [[nodiscard]] DepthBar row(std::uint32_t min_minutes) const noexcept {
        if (books == 0 || minutes < std::max<std::uint32_t>(1, min_minutes)) return missing_bar();
        const double n = static_cast<double>(books);
        return {obi1 / n, obi5 / n, obi5w / n, micro_last, spread / n, ofi_abs > 0.0 ? ofi / ofi_abs : 0.0};
    }
};

/// Half the bar's minutes must have a book, or the bar has no depth reading.
[[nodiscard]] constexpr std::uint32_t min_minutes_for(std::int64_t width_ns) noexcept {
    const std::int64_t m = width_ns / kMinuteNs;
    return static_cast<std::uint32_t>(m <= 1 ? 1 : m / 2);
}

/// One row per bar: the minutes with end in (ends[i] - width, ends[i]].
/// `minutes` must be in increasing end order (unordered input returns empty).
/// A bar no minute covers well enough is all-NaN, never zeros.
[[nodiscard]] inline std::vector<DepthBar> depth_for_bars(std::span<const DepthMinute> minutes,
                                                          std::span<const std::int64_t> ends,
                                                          std::int64_t width_ns) {
    std::vector<DepthBar> out;
    if (width_ns < kMinuteNs || width_ns % kMinuteNs != 0) return out;
    for (std::size_t i = 1; i < minutes.size(); ++i)
        if (minutes[i].end_ns <= minutes[i - 1].end_ns) return out;
    out.reserve(ends.size());
    const auto need = min_minutes_for(width_ns);
    std::size_t lo = 0;
    for (const std::int64_t e : ends) {
        while (lo < minutes.size() && minutes[lo].end_ns <= e - width_ns) ++lo;
        BarSums s;
        for (std::size_t j = lo; j < minutes.size() && minutes[j].end_ns <= e; ++j) s.add(minutes[j]);
        out.push_back(s.row(need));
    }
    return out;
}

// ---- the CSV ---------------------------------------------------------------------

inline constexpr std::string_view kCsvHeader =
    "time_end,fyers,books,obi1_sum,obi5_sum,obi5w_sum,spread_bps_sum,micro_bps_last,ofi5,ofi5_abs";

namespace detail {
inline constexpr std::int64_t kIstNs = 19'800'000'000'000LL;

[[nodiscard]] constexpr std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) noexcept {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

[[nodiscard]] inline bool int_at(std::string_view s, std::size_t at, std::size_t n, int& out) {
    if (at + n > s.size()) return false;
    const auto r = std::from_chars(s.data() + at, s.data() + at + n, out);
    return r.ec == std::errc{} && r.ptr == s.data() + at + n;
}
} // namespace detail

/// "YYYY-MM-DDTHH:MM:SS+05:30" (IST, as every ALTAIR file) to UTC ns.
[[nodiscard]] inline std::optional<std::int64_t> parse_ist(std::string_view s) {
    if (s.size() != 25 || s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' || s[16] != ':' ||
        s.substr(19) != "+05:30")
        return std::nullopt;
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    if (!detail::int_at(s, 0, 4, y) || !detail::int_at(s, 5, 2, mo) || !detail::int_at(s, 8, 2, d) ||
        !detail::int_at(s, 11, 2, h) || !detail::int_at(s, 14, 2, mi) || !detail::int_at(s, 17, 2, se))
        return std::nullopt;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || se > 59) return std::nullopt;
    const std::int64_t days = detail::days_from_civil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d));
    const std::int64_t local = ((days * 24 + h) * 60 + mi) * 60 + se;
    return local * 1'000'000'000LL - detail::kIstNs;
}

[[nodiscard]] inline std::string format_ist(std::int64_t utc_ns) {
    const std::int64_t s = (utc_ns + detail::kIstNs) / 1'000'000'000LL;
    std::int64_t days = s / 86400, rem = s % 86400;
    if (rem < 0) { rem += 86400; --days; }
    // civil_from_days
    const std::int64_t z = days + 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400 + (m <= 2);
    char buf[40];
    std::snprintf(buf, sizeof buf, "%04lld-%02u-%02uT%02lld:%02lld:%02lld+05:30", static_cast<long long>(y), m, d,
                  static_cast<long long>(rem / 3600), static_cast<long long>(rem / 60 % 60),
                  static_cast<long long>(rem % 60));
    return buf;
}

[[nodiscard]] inline std::string csv_row(const DepthMinute& m, std::string_view fyers) {
    char buf[256];
    std::snprintf(buf, sizeof buf, ",%u,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g\n", m.books, m.obi1_sum, m.obi5_sum,
                  m.obi5w_sum, m.spread_bps_sum, m.micro_bps_last, m.ofi5, m.ofi5_abs);
    return format_ist(m.end_ns) + "," + std::string(fyers) + buf;
}

/// One data row of depth_1m.csv; empty for a malformed row. `fyers` gets the symbol.
[[nodiscard]] inline std::optional<DepthMinute> parse_csv_row(std::string_view line, std::string* fyers = nullptr) {
    std::array<std::string_view, 10> f{};
    std::size_t k = 0, at = 0;
    while (k < f.size()) {
        const std::size_t c = line.find(',', at);
        f[k++] = line.substr(at, c == std::string_view::npos ? std::string_view::npos : c - at);
        if (c == std::string_view::npos) break;
        at = c + 1;
    }
    if (k != f.size() || line.find(',', at) != std::string_view::npos) return std::nullopt;
    while (!f[9].empty() && (f[9].back() == '\r' || f[9].back() == '\n')) f[9].remove_suffix(1);
    const auto t = parse_ist(f[0]);
    if (!t || *t % kMinuteNs != 0) return std::nullopt;
    DepthMinute m;
    m.end_ns = *t;
    const auto u = std::from_chars(f[2].data(), f[2].data() + f[2].size(), m.books);
    if (u.ec != std::errc{} || u.ptr != f[2].data() + f[2].size()) return std::nullopt;
    double* v[7] = {&m.obi1_sum, &m.obi5_sum, &m.obi5w_sum, &m.spread_bps_sum, &m.micro_bps_last, &m.ofi5, &m.ofi5_abs};
    for (std::size_t i = 0; i < 7; ++i) {
        const std::string text(f[3 + i]);
        char* end = nullptr;
        *v[i] = std::strtod(text.c_str(), &end);
        if (text.empty() || end != text.c_str() + text.size() || !std::isfinite(*v[i])) return std::nullopt;
    }
    if (m.books == 0 || m.ofi5_abs < 0.0) return std::nullopt;
    if (fyers) *fyers = std::string(f[1]);
    return m;
}

} // namespace altair::depth
