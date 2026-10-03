// live/latency.hpp -- how long things take, by percentile.
//
// A mean hides the moments that matter: a model decision that usually takes
// 4 ms and once a day takes 400 is the one that fills late. So latencies are
// kept as a log-bucketed histogram -- 32 buckets per power of two, about 3 %
// resolution, from a nanosecond to centuries, 2048 counters (16 KiB) -- and
// read back as p50 / p99 / p99.9 and the exact max.
// A quantile is reported as its bucket's upper edge: never optimistic.
//
// Not thread-safe: one histogram, one writer.

#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <string>

namespace altair::live {

class LatencyHistogram {
public:
    static constexpr int kSub = 32;                          ///< buckets per power of two
    static constexpr int kSubBits = 5;                       ///< log2(kSub)
    static constexpr std::size_t kBuckets = 64 * kSub;

    void record(std::int64_t ns) noexcept {
        const std::uint64_t v = ns > 0 ? static_cast<std::uint64_t>(ns) : 0u;
        ++counts_[index(v)];
        ++n_;
        sum_ += static_cast<double>(v);
        if (v > max_) max_ = v;
    }

    [[nodiscard]] std::uint64_t count() const noexcept { return n_; }
    [[nodiscard]] std::uint64_t max() const noexcept { return max_; }
    [[nodiscard]] double mean() const noexcept { return n_ > 0 ? sum_ / static_cast<double>(n_) : 0.0; }

    /// The value at or below which a fraction `q` of the samples fall
    /// (rounded up to the bucket's upper edge, never above the max). 0 if empty.
    [[nodiscard]] std::uint64_t quantile(double q) const noexcept {
        if (n_ == 0) return 0;
        const double want = q * static_cast<double>(n_);
        std::uint64_t seen = 0;
        for (std::size_t i = 0; i < kBuckets; ++i) {
            seen += counts_[i];
            if (static_cast<double>(seen) >= want && counts_[i] > 0) {
                // RULE 11: the bucket's upper edge overstates a sample by at most one
                // sub-bucket (~3 %); the largest sample is exact, so the edge is cut
                // to it -- the true quantile is never above the max, only the edge was.
                const std::uint64_t hi = upper(i);
                return hi < max_ ? hi : max_;
            }
        }
        return max_;
    }

    void merge(const LatencyHistogram& o) noexcept {
        for (std::size_t i = 0; i < kBuckets; ++i) counts_[i] += o.counts_[i];
        n_ += o.n_;
        sum_ += o.sum_;
        if (o.max_ > max_) max_ = o.max_;
    }

    /// {"n": .., "p50": .., "p99": .., "p999": .., "max": ..} in `unit_ns` units (1000: microseconds).
    [[nodiscard]] std::string json(double unit_ns) const {
        char b[256];
        std::snprintf(b, sizeof b, "{\"n\": %llu, \"p50\": %.3f, \"p99\": %.3f, \"p999\": %.3f, \"max\": %.3f, \"mean\": %.3f}",
                      static_cast<unsigned long long>(n_), static_cast<double>(quantile(0.5)) / unit_ns,
                      static_cast<double>(quantile(0.99)) / unit_ns, static_cast<double>(quantile(0.999)) / unit_ns,
                      static_cast<double>(max_) / unit_ns, mean() / unit_ns);
        return b;
    }
    /// "p50 1.2 / p99 3.4 / p99.9 5.6 / max 7.8 <unit> (n)".
    [[nodiscard]] std::string text(double unit_ns, const char* unit) const {
        char b[200];
        std::snprintf(b, sizeof b, "p50 %.2f / p99 %.2f / p99.9 %.2f / max %.2f %s (%llu)",
                      static_cast<double>(quantile(0.5)) / unit_ns, static_cast<double>(quantile(0.99)) / unit_ns,
                      static_cast<double>(quantile(0.999)) / unit_ns, static_cast<double>(max_) / unit_ns, unit,
                      static_cast<unsigned long long>(n_));
        return b;
    }

private:
    /// Values below kSub are their own bucket; above, the top kSubBits bits
    /// after the leading one choose among kSub buckets of that power of two.
    [[nodiscard]] static std::size_t index(std::uint64_t v) noexcept {
        if (v < static_cast<std::uint64_t>(kSub)) return static_cast<std::size_t>(v);
        const int e = 63 - std::countl_zero(v);   // v in [2^e, 2^(e+1))
        const std::uint64_t sub = (v >> (e - kSubBits)) & static_cast<std::uint64_t>(kSub - 1);
        return static_cast<std::size_t>((e - kSubBits + 1) * kSub) + static_cast<std::size_t>(sub);
    }
    /// The largest value that lands in bucket `i`.
    [[nodiscard]] static std::uint64_t upper(std::size_t i) noexcept {
        if (i < static_cast<std::size_t>(kSub)) return i;
        const int e = static_cast<int>(i / kSub) + kSubBits - 1;
        const std::uint64_t sub = i % kSub;
        const std::uint64_t lo = (std::uint64_t{1} << e) + (sub << (e - kSubBits));
        return lo + (std::uint64_t{1} << (e - kSubBits)) - 1;
    }

    std::array<std::uint64_t, kBuckets> counts_{};
    std::uint64_t n_ = 0, max_ = 0;
    double sum_ = 0.0;
};

} // namespace altair::live
