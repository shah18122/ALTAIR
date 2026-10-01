// live/bars.hpp -- ticks into one-minute bars on the IST session grid.
//
// The live models decide on bar closes, the same bars the research ran on
// (09:15 to 15:30 IST, labelled by their start). A bar is CLOSED when the
// engine clock -- the feed's own trade stamps, never the wall clock -- moves
// past its minute, whether or not the instrument traded again: an illiquid
// strike's 10:42 bar closes at 10:43 even if its next trade is at 11:05.
//
// A minute with no trade has no bar. Gaps are not filled with the previous
// close: a bar that never traded is not a measurement of anything.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace altair::live {

inline constexpr int kLiveOpenMinute = 9 * 60 + 15;    ///< 09:15 IST
inline constexpr int kLiveCloseMinute = 15 * 60 + 30;  ///< 15:30 IST

/// IST minute-of-day and IST day of a UTC nanosecond stamp.
[[nodiscard]] constexpr std::int64_t live_ist_minute_index(std::int64_t ns) noexcept {
    const std::int64_t s = ns / 1'000'000'000LL + 19800;
    return (s >= 0 ? s : s - 59) / 60;
}
[[nodiscard]] constexpr int live_minute_of_day(std::int64_t minute_index) noexcept {
    return static_cast<int>(((minute_index % 1440) + 1440) % 1440);
}
[[nodiscard]] constexpr std::int64_t live_day_of(std::int64_t minute_index) noexcept {
    return (minute_index >= 0 ? minute_index : minute_index - 1439) / 1440;
}

struct LiveBar {
    std::int64_t minute = 0;   ///< IST minute index of the bar's START
    double o = 0, h = 0, l = 0, c = 0;
    std::int64_t volume = 0;   ///< traded in the bar (cumulative-volume difference)
    int ticks = 0;
    [[nodiscard]] int start_of_day() const noexcept { return live_minute_of_day(minute); }
    [[nodiscard]] int close_of_day() const noexcept { return live_minute_of_day(minute) + 1; }
};

/// One-minute bars for many instruments. Feed trades with feed(); move the
/// clock with advance(); closed bars come back through the callback.
class LiveBarBuilder {
public:
    /// A trade at `price` (rupees), stamped `ns`, with the day's cumulative
    /// volume when the instrument has one (-1 when it does not).
    template <class OnClosed>
    void feed(std::uint32_t token, double price, std::int64_t ns, std::int64_t cum_volume, OnClosed&& closed) {
        if (!(price > 0.0)) return;
        const std::int64_t m = live_ist_minute_index(ns);
        auto& s = slots_[token];
        if (s.open && m > s.bar.minute) {
            closed(token, s.bar);
            today_[token].push_back(s.bar);
            s.open = false;
        }
        if (s.open && m < s.bar.minute) return;   // a late print for a closed minute: kept out, not rewritten
        if (!s.open) {
            s.bar = LiveBar{m, price, price, price, price, 0, 0};
            s.open = true;
        }
        LiveBar& b = s.bar;
        b.h = std::max(b.h, price);
        b.l = std::min(b.l, price);
        b.c = price;
        ++b.ticks;
        if (cum_volume >= 0) {
            if (s.last_volume >= 0 && cum_volume >= s.last_volume) b.volume += cum_volume - s.last_volume;
            s.last_volume = cum_volume;
        }
    }

    /// The clock reached `ns`: close every open bar of an earlier minute.
    template <class OnClosed>
    void advance(std::int64_t ns, OnClosed&& closed) {
        const std::int64_t m = live_ist_minute_index(ns);
        for (auto& [tok, s] : slots_) {
            if (s.open && m > s.bar.minute) {
                closed(tok, s.bar);
                today_[tok].push_back(s.bar);
                s.open = false;
            }
        }
    }

    /// Today's closed one-minute bars for `token`, oldest first.
    [[nodiscard]] const std::vector<LiveBar>& bars(std::uint32_t token) const {
        static const std::vector<LiveBar> none;
        const auto it = today_.find(token);
        return it == today_.end() ? none : it->second;
    }

    /// Forget the day's bars (a new session).
    void new_day() {
        today_.clear();
        for (auto& [tok, s] : slots_) { s.open = false; s.last_volume = -1; }
    }

private:
    struct Slot {
        LiveBar bar{};
        bool open = false;
        std::int64_t last_volume = -1;
    };
    std::unordered_map<std::uint32_t, Slot> slots_;
    std::unordered_map<std::uint32_t, std::vector<LiveBar>> today_;
};

/// One-minute bars into `n`-minute bars on the session grid (09:15, 09:20, ...),
/// complete or not; a bucket with no one-minute bar is absent.
[[nodiscard]] inline std::vector<LiveBar> live_aggregate(const std::vector<LiveBar>& one, int n) {
    std::vector<LiveBar> out;
    for (const LiveBar& b : one) {
        const int mod = b.start_of_day();
        if (mod < kLiveOpenMinute) continue;
        const std::int64_t bucket = b.minute - ((mod - kLiveOpenMinute) % n);
        if (out.empty() || out.back().minute != bucket) {
            out.push_back(LiveBar{bucket, b.o, b.h, b.l, b.c, b.volume, b.ticks});
            continue;
        }
        LiveBar& a = out.back();
        a.h = std::max(a.h, b.h);
        a.l = std::min(a.l, b.l);
        a.c = b.c;
        a.volume += b.volume;
        a.ticks += b.ticks;
    }
    return out;
}

} // namespace altair::live
