// live/feed_consumer.hpp -- price-bus frames into the live engine.
//
// The bus (server/price_bus.hpp) numbers every frame per topic. A Delta whose
// number jumps means frames were lost on the way -- the bus coalesced them
// for a slow reader, or the engine was disconnected while they went by. Lost
// QUOTES and BOOKS are state: the next one supersedes them. Lost TRADES are
// not: the bars they belonged to are now incomplete, so the engine is told
// (LiveEngine::on_trade_gap) and holds its models' decisions back until a
// clean minute has passed.
//
// SNAPSHOT frames are a late joiner's baseline: the last trade, quote and book
// of each instrument as they stood. They set prices and the sequence the next
// Delta continues from; a snapshot trade is NOT a trade now and never enters
// a bar or moves the clock.
//
// A RECONNECT is checked against what came before: if the topic's sequence
// moved on while the engine was away, those trades were missed; if it went
// backwards, the price service restarted and nobody can say what was missed.
// Either way it is a gap.

#pragma once

#include <live/engine.hpp>
#include <server/price_payload.hpp>
#include <server/protocol.hpp>
#include <server/quote_payload.hpp>

#include <cstdint>

namespace altair::live {

class LiveFeedConsumer {
public:
    explicit LiveFeedConsumer(LiveEngine& e) : e_(e) {}

    /// The connection dropped and a new one is about to start.
    void on_reconnect() noexcept {
        for (auto& t : topics_) t.rejoin = t.primed;
    }

    /// One whole frame (header decoded, `body` holding payload_len bytes).
    /// False when the payload did not decode.
    bool on_frame(const FrameHeader& h, const std::uint8_t* body) {
        ++frames_;
        if (!h.advances_seq() || h.topic == 0 || h.topic > kTopicQuote) return true;   // heartbeats, unknown topics
        const bool snapshot = h.kind == FrameKind::Snapshot;
        if (!sequence(h, snapshot)) return true;   // a duplicate
        if (h.topic == kTopicQuote) {
            const auto q = decode_quote(body, h.payload_len);
            if (!q) { ++bad_; return false; }
            e_.on_quote(*q, h.engine_time_ns);
            return true;
        }
        const auto d = decode_price(body, h.payload_len);
        if (!d) { ++bad_; return false; }
        if (h.topic == kTopicTrades) {
            if (snapshot) { e_.on_trade_snapshot(d->payload); ++snapshots_; }
            else e_.on_trade(d->payload, d->payload.exchange_ts_ns > 0 ? d->payload.exchange_ts_ns : h.engine_time_ns);
            return true;
        }
        // kTopicBook
        LiveLevel bids[kLiveDepth]{}, asks[kLiveDepth]{};
        const std::uint16_t n = d->payload.depth_levels;
        for (std::size_t k = 0; k < kLiveDepth && k < n; ++k) {
            bids[k] = LiveLevel{d->bids[k].price_paise, d->bids[k].qty};
            asks[k] = LiveLevel{d->asks[k].price_paise, d->asks[k].qty};
        }
        e_.on_book(d->payload.token, n, bids, asks, h.engine_time_ns);
        return true;
    }

    [[nodiscard]] std::uint64_t frames() const noexcept { return frames_; }
    [[nodiscard]] std::uint64_t bad() const noexcept { return bad_; }
    [[nodiscard]] std::uint64_t snapshots() const noexcept { return snapshots_; }
    /// Frames lost per topic (trades, book, quote), as the sequence says.
    [[nodiscard]] std::uint64_t missed(std::uint32_t topic) const noexcept {
        return topic <= kTopicQuote ? topics_[topic].missed : 0;
    }

private:
    struct Topic {
        std::uint64_t last = 0, missed = 0;
        bool primed = false, rejoin = false;
    };

    /// Book-keeping for one frame. False for a duplicate Delta (skip it).
    bool sequence(const FrameHeader& h, bool snapshot) {
        Topic& t = topics_[h.topic];
        if (!t.primed) {                       // the first frame this engine has seen
            t.primed = true;
            t.last = h.seq;
            return true;
        }
        if (t.rejoin) {
            t.rejoin = false;
            // A snapshot carries the sequence the stream is AT; a delta, one past it.
            const std::uint64_t at = snapshot ? h.seq : h.seq - 1;
            if (at < t.last) gap(h, 0);                     // the service restarted: unknown
            else if (at > t.last) gap(h, at - t.last);      // went by while away
            t.last = h.seq;
            return true;
        }
        if (snapshot) {                        // a baseline (only after a rejoin, in practice)
            if (h.seq > t.last) gap(h, h.seq - t.last);
            t.last = h.seq;
            return true;
        }
        if (h.seq <= t.last) return false;     // seen already
        if (h.seq > t.last + 1) gap(h, h.seq - t.last - 1);
        t.last = h.seq;
        return true;
    }

    void gap(const FrameHeader& h, std::uint64_t missed) {
        topics_[h.topic].missed += missed;
        if (h.topic == kTopicTrades) e_.on_trade_gap(h.engine_time_ns, missed);
    }

    LiveEngine& e_;
    Topic topics_[kTopicQuote + 1]{};
    std::uint64_t frames_ = 0, bad_ = 0, snapshots_ = 0;
};

} // namespace altair::live
