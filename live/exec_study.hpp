// live/exec_study.hpp -- what each paper fill cost, what came after it, and
// whether waiting at the touch would have worked: execution labels from a
// session tape (live/tape.hpp) and the fills that session made.
//
// The tape is replayed into a model-free engine, used only as the market's
// state; each fill sets probes that read that state "as of" a time T -- after
// every frame stamped <= T, before the first stamped later:
//
//   * IMPLEMENTATION SHORTFALL: side x (fill price - mid at the decision) /
//     mid, in bp. Positive is cost: the spread crossed plus the move during
//     the latency.
//   * MARKOUTS: side x (mid at fill + h - fill price) / fill price, in bp, for
//     h = 1, 5, 30, 60 and 300 s. Positive: the market moved our way after we
//     dealt; a markout that turns negative with h is adverse selection.
//   * PASSIVE FILL: had the order instead joined the near touch at the
//     decision (a buy at the bid), would it have filled within h = 1, 5, 30,
//     60 s? Filled when a print goes THROUGH that price, or prints AT it add up
//     to the queue ahead (the touch quantity at the decision) plus the order.
//     Cancellations ahead are not seen, so this is a LOWER bound on the fill
//     probability -- the queue only ever shrinks faster than modelled.
//   * the spread at the decision, and the queue ahead.
//
// A label whose horizon runs past the tape's end is unknown (NaN / -1), never
// zero. Timestamps are the feed's (the engine's clock: a trade's exchange
// time, else the frame's engine time), made monotone.

#pragma once

#include <live/engine.hpp>
#include <live/feed_consumer.hpp>
#include <live/report.hpp>
#include <server/price_payload.hpp>
#include <server/protocol.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <queue>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace altair::live::execution {

inline constexpr std::array<std::int64_t, 5> kMarkoutSeconds{1, 5, 30, 60, 300};
inline constexpr std::array<std::int64_t, 4> kPassiveSeconds{1, 5, 30, 60};

struct FillLabel {
    report::JournalFill fill;
    double decision_mid = report::kNaN, decision_spread_bp = report::kNaN, fill_mid = report::kNaN;
    double shortfall_bp = report::kNaN;
    std::array<double, kMarkoutSeconds.size()> markout_bp{};
    double passive_price = report::kNaN;         ///< rupees: the near touch at the decision
    std::int64_t queue_ahead = -1;               ///< units at that touch then; -1 unknown
    std::array<int, kPassiveSeconds.size()> passive_filled{};   ///< 1, 0, or -1 unknown
    FillLabel() {
        markout_bp.fill(report::kNaN);
        passive_filled.fill(-1);
    }
};

class ExecStudy {
public:
    ExecStudy(const std::vector<LiveInstrument>& universe, std::vector<report::JournalFill> fills)
        : engine_(universe, nullptr), consumer_(engine_) {
        labels_.resize(fills.size());
        passive_.resize(fills.size());
        for (std::size_t i = 0; i < fills.size(); ++i) {
            labels_[i].fill = std::move(fills[i]);
            const auto& f = labels_[i].fill;
            const std::int64_t decided = f.submit_ns > 0 && f.submit_ns <= f.ns ? f.submit_ns : f.ns;
            probes_.push({decided, i, kDecision});
            probes_.push({f.ns, i, kAtFill});
            for (std::size_t h = 0; h < kMarkoutSeconds.size(); ++h)
                probes_.push({f.ns + kMarkoutSeconds[h] * 1'000'000'000LL, i, static_cast<int>(kMarkout0 + h)});
        }
    }

    /// One whole frame off the tape, in order.
    void on_frame(const FrameHeader& h, const std::uint8_t* body) {
        std::int64_t t = h.engine_time_ns;
        std::optional<DecodedPrice> trade;
        if (h.topic == kTopicTrades && h.kind == FrameKind::Delta) {
            if (auto d = decode_price(body, h.payload_len)) trade = std::move(*d);
            if (trade && trade->payload.exchange_ts_ns > 0) t = trade->payload.exchange_ts_ns;
        }
        if (t > 0) {
            now_ = std::max(now_, t);
            if (first_ == 0) first_ = now_;
            // The state as of every probe due before this frame's time.
            while (!probes_.empty() && probes_.top().due < now_) {
                sample(probes_.top());
                probes_.pop();
            }
        }
        if (trade) on_print(trade->payload, now_);
        (void)consumer_.on_frame(h, body);
    }
    void on_reconnect() { consumer_.on_reconnect(); }

    /// The labels, once the tape is done: what the tape never reached is unknown.
    [[nodiscard]] std::vector<FillLabel> finish() {
        while (!probes_.empty()) {
            if (probes_.top().due <= now_) sample(probes_.top());   // due at the tape's last instant
            probes_.pop();
        }
        for (std::size_t i = 0; i < labels_.size(); ++i) {
            const Passive& p = passive_[i];
            if (!p.armed) continue;
            for (std::size_t k = 0; k < kPassiveSeconds.size(); ++k) {
                const std::int64_t until = p.from + kPassiveSeconds[k] * 1'000'000'000LL;
                if (p.filled_at > 0 && p.filled_at <= until) labels_[i].passive_filled[k] = 1;
                else if (now_ >= until) labels_[i].passive_filled[k] = 0;
            }
        }
        return labels_;
    }
    /// The feed time the tape covered.
    [[nodiscard]] std::pair<std::int64_t, std::int64_t> span() const noexcept { return {first_, now_}; }

private:
    enum : int { kDecision = 0, kAtFill = 1, kMarkout0 = 2 };
    struct Probe {
        std::int64_t due = 0;
        std::size_t fill = 0;
        int what = 0;
        bool operator>(const Probe& o) const noexcept { return due != o.due ? due > o.due : (fill != o.fill ? fill > o.fill : what > o.what); }
    };
    struct Passive {
        bool armed = false;
        int side = 0;
        std::uint32_t token = 0;
        std::int64_t price = 0;        ///< paise
        std::int64_t need = 0;         ///< queue ahead + the order
        std::int64_t at_price = 0;     ///< printed at the price since
        std::int64_t from = 0, filled_at = 0;
    };

    [[nodiscard]] double mid(std::uint32_t token) const { return engine_.mid(token); }

    void sample(const Probe& p) {
        // Before this tape began, the state is another session's: unknown here.
        if (p.due < first_) return;
        FillLabel& l = labels_[p.fill];
        const auto& f = l.fill;
        const double m = mid(f.token);
        if (p.what == kDecision) {
            const LiveTop t = engine_.top(f.token);
            if (m > 0.0) {
                l.decision_mid = m;
                l.shortfall_bp = static_cast<double>(f.side) * (f.price - m) / m * 1e4;
            }
            if (t.bid > 0 && t.ask > t.bid && m > 0.0) l.decision_spread_bp = static_cast<double>(t.ask - t.bid) / 100.0 / m * 1e4;
            const std::int64_t touch = f.side > 0 ? t.bid : t.ask;
            const std::int64_t queue = f.side > 0 ? t.bid_qty : t.ask_qty;
            if (touch > 0) {
                l.passive_price = static_cast<double>(touch) / 100.0;
                l.queue_ahead = queue;
                Passive& q = passive_[p.fill];
                q.armed = true; q.side = f.side; q.token = f.token; q.price = touch; q.need = queue + f.qty; q.from = p.due;
                watching_[f.token].push_back(p.fill);
            }
        } else if (p.what == kAtFill) {
            if (m > 0.0) l.fill_mid = m;
        } else {
            const auto h = static_cast<std::size_t>(p.what - kMarkout0);
            if (m > 0.0 && f.price > 0.0) l.markout_bp[h] = static_cast<double>(f.side) * (m - f.price) / f.price * 1e4;
        }
    }

    void on_print(const PricePayload& pr, std::int64_t t) {
        if (pr.last_paise <= 0) return;
        const auto it = watching_.find(pr.token);
        if (it == watching_.end()) return;
        auto& ids = it->second;
        for (std::size_t k = 0; k < ids.size();) {
            Passive& q = passive_[ids[k]];
            if (t <= q.from) { ++k; continue; }
            // A buy resting at the bid fills when a print goes below it, or
            // when prints at it have used up the queue ahead and the order.
            const bool through = q.side > 0 ? pr.last_paise < q.price : pr.last_paise > q.price;
            if (pr.last_paise == q.price) q.at_price += std::max<std::int64_t>(pr.last_qty, 0);
            if (through || (q.at_price >= q.need && q.need > 0)) {
                q.filled_at = t;
                ids[k] = ids.back();   // filled: stop watching it
                ids.pop_back();
                continue;
            }
            ++k;
        }
    }

    LiveEngine engine_;
    LiveFeedConsumer consumer_;
    std::vector<FillLabel> labels_;
    std::vector<Passive> passive_;
    std::priority_queue<Probe, std::vector<Probe>, std::greater<Probe>> probes_;
    std::unordered_map<std::uint32_t, std::vector<std::size_t>> watching_;   ///< passive orders not yet filled, by token
    std::int64_t now_ = 0, first_ = 0;
};

} // namespace altair::live::execution
