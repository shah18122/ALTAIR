// book/flow.hpp — VPIN and Kyle's lambda.
//
// P2-09b. The stateful half of the microstructure work: both estimators need
// history, where P2-09a's imbalance and microprice are pure functions of one
// book.
//
// ROADMAP 3 governs this file: MEASUREMENTS CARRY ERROR. Neither estimator
// returns a bare number. Each reports the sample it was computed from, and
// each returns EMPTY until it has enough — because an estimate from three
// observations is not a small number with a wide error bar, it is noise
// wearing the costume of a signal. "Size on the lower confidence bound of
// edge, never the point estimate" only means anything if the point estimate
// arrives with something to bound it.
//
// Decisions D1..D7 are fixed in prompts/P2-09b_flow.md.

#pragma once

#include <book/microstructure.hpp>
#include <feed/tick.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace altair {

/// Which side initiated a trade. Inferred, never told: the exchange does not
/// publish aggressor side on these feeds.
enum class TradeSide : std::uint8_t { Unknown, Buy, Sell };

/// Classify a trade against the book that preceded it.
///
/// D2: the LEE-READY rule, and it needs the PRIOR book. Classifying against
/// the book that already absorbed the trade is look-ahead — the very tick
/// being classified has moved the quote, and the answer becomes almost
/// tautological. The caller must pass the book as it stood BEFORE.
///
/// Above the mid is a buy, below is a sell, exactly at the mid is Unknown
/// rather than a coin flip: a tick-rule fallback would manufacture a side from
/// nothing, and at-mid prints are common enough that the invention would
/// matter.
[[nodiscard]] ALTAIR_HOT inline TradeSide
classify_trade(Price traded, const BookState& prior) noexcept {
    const auto m = mid(prior);
    if (!m) {
        return TradeSide::Unknown;
    }
    if (traded.raw() > m->raw()) {
        return TradeSide::Buy;
    }
    if (traded.raw() < m->raw()) {
        return TradeSide::Sell;
    }
    return TradeSide::Unknown;
}

// ─────────────────────────────────────────────────────────────────────────
// VPIN — volume-synchronised probability of informed trading.
//
// D1: VPIN is bucketed by VOLUME, not by time, and that is the whole point of
// it. Clock-time buckets sample a process whose activity varies by orders of
// magnitude across a session, so a fixed interval oversamples the quiet middle
// and undersamples the open. Volume buckets make each observation carry the
// same amount of trading.
// ─────────────────────────────────────────────────────────────────────────
inline constexpr std::size_t kMaxVpinBuckets = 64;

class Vpin {
public:
    struct Config {
        /// Volume that fills one bucket. UNIT: units. From config, never a
        /// literal — the right size is instrument-specific.
        std::int64_t bucket_volume = 0;
        /// Buckets averaged. UNIT: count.
        std::size_t window = 50;
        /// Buckets required before a value is reported at all. UNIT: count.
        std::size_t min_buckets = 50;
    };

    struct Reading {
        double      value;    // UNIT: dimensionless, [0, 1]
        std::size_t buckets;  // how many completed buckets it averages (D7)
    };

    explicit Vpin(Config cfg) noexcept : cfg_(clamp(cfg)) {}

    /// Add one classified trade. ALTAIR_HOT.
    ALTAIR_HOT void on_trade(TradeSide side, Qty volume) noexcept {
        if (volume.raw() <= 0 || cfg_.bucket_volume <= 0) {
            return;
        }
        std::int64_t left = volume.raw();
        while (left > 0) {
            const std::int64_t room = cfg_.bucket_volume - cur_volume_;
            const std::int64_t take = left < room ? left : room;
            // A trade larger than one bucket is SPLIT across buckets rather
            // than dropped or counted whole. A block print is exactly when
            // VPIN should move, and assigning it wholly to one bucket would
            // spike that bucket and starve the next.
            if (side == TradeSide::Buy) {
                cur_buy_ += take;
            } else if (side == TradeSide::Sell) {
                cur_sell_ += take;
            }
            // Unknown-side volume still FILLS the bucket but contributes to
            // neither side: it is volume that happened, and pretending it did
            // not would stretch the bucket over a longer stretch of tape.
            cur_volume_ += take;
            left -= take;
            if (cur_volume_ >= cfg_.bucket_volume) {
                close_bucket();
            }
        }
    }

    /// The current estimate. Empty until `min_buckets` have completed (D7).
    [[nodiscard]] std::optional<Reading> value() const noexcept {
        if (filled_ < cfg_.min_buckets || filled_ == 0) {
            return std::nullopt;
        }
        const std::size_t n = filled_ < cfg_.window ? filled_ : cfg_.window;
        double sum = 0.0;
        double vol = 0.0;
        for (std::size_t k = 0; k < n; ++k) {
            const std::size_t idx = (head_ + kMaxVpinBuckets - 1 - k) % kMaxVpinBuckets;
            sum += imbalance_[idx];
            vol += total_[idx];
        }
        if (!(vol > 0.0)) {
            return std::nullopt;
        }
        return Reading{sum / vol, n};
    }

    [[nodiscard]] std::size_t completed_buckets() const noexcept { return filled_; }
    [[nodiscard]] std::int64_t bucket_progress() const noexcept { return cur_volume_; }

    void reset() noexcept {
        head_ = 0;
        filled_ = 0;
        cur_buy_ = cur_sell_ = cur_volume_ = 0;
        for (std::size_t i = 0; i < kMaxVpinBuckets; ++i) {
            imbalance_[i] = 0.0;
            total_[i] = 0.0;
        }
    }

private:
    static Config clamp(Config c) noexcept {
        if (c.window > kMaxVpinBuckets) { c.window = kMaxVpinBuckets; }
        if (c.window == 0) { c.window = 1; }
        if (c.min_buckets > c.window) { c.min_buckets = c.window; }
        return c;
    }

    void close_bucket() noexcept {
        const double d = static_cast<double>(cur_buy_ - cur_sell_);
        imbalance_[head_] = d < 0.0 ? -d : d;          // |buy - sell|
        total_[head_] = static_cast<double>(cur_volume_);
        head_ = (head_ + 1) % kMaxVpinBuckets;
        if (filled_ < kMaxVpinBuckets) {
            ++filled_;
        }
        cur_buy_ = cur_sell_ = cur_volume_ = 0;
    }

    Config       cfg_;
    double       imbalance_[kMaxVpinBuckets]{};
    double       total_[kMaxVpinBuckets]{};
    std::size_t  head_ = 0;
    std::size_t  filled_ = 0;
    std::int64_t cur_buy_ = 0;
    std::int64_t cur_sell_ = 0;
    std::int64_t cur_volume_ = 0;
};

// ─────────────────────────────────────────────────────────────────────────
// Kyle's lambda — price impact per unit of signed order flow.
//
// The regression slope of dP on signed volume, through the origin:
//
//     lambda = sum(v * dP) / sum(v * v)
//
// D4: through the ORIGIN, with no intercept. An intercept here would be a
// price change that happens with zero order flow, which is not a thing this
// model claims exists. Fitting one would quietly absorb drift and flatter the
// slope.
//
// UNIT: paise per unit. It is a DIMENSIONED quantity, and the unit is the
// whole content of it — lambda times a size in units gives an expected move in
// paise, which is what a sizing rule needs.
// ─────────────────────────────────────────────────────────────────────────
class KyleLambda {
public:
    struct Config {
        /// Observations required before a slope is reported. UNIT: count.
        std::size_t min_samples = 30;
    };

    struct Reading {
        double      lambda;    // UNIT: paise per unit of signed volume
        std::size_t samples;   // D7 — never reported without its sample size
        double      r2;        // goodness of fit, [0, 1]
    };

    explicit KyleLambda(Config cfg) noexcept : cfg_(cfg) {}

    /// One observation: signed volume and the price change that followed it.
    /// ALTAIR_HOT. `signed_volume` is positive for buyer-initiated.
    ALTAIR_HOT void on_observation(std::int64_t signed_volume,
                                   Price price_change) noexcept {
        if (signed_volume == 0) {
            // A zero-flow observation contributes nothing to a
            // through-the-origin fit and would only inflate the sample count,
            // making the estimate look better supported than it is.
            return;
        }
        const double v = static_cast<double>(signed_volume);
        const double dp = static_cast<double>(price_change.raw());
        sum_vv_ += v * v;
        sum_vdp_ += v * dp;
        sum_dpdp_ += dp * dp;
        ++n_;
    }

    [[nodiscard]] std::optional<Reading> value() const noexcept {
        if (n_ < cfg_.min_samples || !(sum_vv_ > 0.0)) {
            return std::nullopt;
        }
        const double lam = sum_vdp_ / sum_vv_;
        // R-squared for a through-the-origin fit: explained over total.
        double r2 = 0.0;
        if (sum_dpdp_ > 0.0) {
            r2 = (lam * sum_vdp_) / sum_dpdp_;
            if (r2 < 0.0) { r2 = 0.0; }
            if (r2 > 1.0) { r2 = 1.0; }
        }
        return Reading{lam, n_, r2};
    }

    [[nodiscard]] std::size_t samples() const noexcept { return n_; }

    void reset() noexcept {
        sum_vv_ = sum_vdp_ = sum_dpdp_ = 0.0;
        n_ = 0;
    }

private:
    Config      cfg_;
    double      sum_vv_ = 0.0;
    double      sum_vdp_ = 0.0;
    double      sum_dpdp_ = 0.0;
    std::size_t n_ = 0;
};

} // namespace altair
