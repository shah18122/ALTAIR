#pragma once

// net/tcp_reassembly.hpp -- the receive half of a userspace TCP connection.
//
// Segments arrive from AF_XDP / DPDK in whatever order the network delivers
// them. This hands the stream's bytes to a sink strictly in order, exactly
// once: retransmitted and overlapping bytes are trimmed, segments that arrive
// early wait in a fixed set of slots (no allocation), and anything that does
// not fit is dropped so the sender retransmits it -- the same thing a kernel
// does when its receive queue is full.
//
// Sequence numbers wrap at 2^32; every comparison is the signed difference
// (RFC 1982 serial arithmetic), so the wrap is invisible.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace altair::net {

/// a - b as a signed distance on the 2^32 sequence circle.
inline std::int32_t seq_diff(std::uint32_t a, std::uint32_t b) noexcept {
    return static_cast<std::int32_t>(a - b);
}

struct ReassemblyStats {
    std::uint64_t delivered_bytes = 0;
    std::uint64_t duplicate_bytes = 0;   // already delivered (retransmits, overlaps)
    std::uint64_t queued_segments = 0;   // arrived early and waited in a slot
    std::uint64_t dropped_segments = 0;  // early, but no slot (or too big): sender retransmits
};

template <std::size_t Slots = 32, std::size_t SlotBytes = 2048>
class TcpReassembler {
public:
    /// Begin at the first byte the peer will send (its ISN + 1).
    void start(std::uint32_t next_seq) noexcept {
        nxt_ = next_seq;
        started_ = true;
        fin_ = false;
        closed_ = false;
        for (auto& s : slots_) s.used = false;
    }

    bool started() const noexcept { return started_; }
    bool closed() const noexcept { return closed_; }
    /// The next byte expected: what an ACK carries.
    std::uint32_t next() const noexcept { return nxt_; }
    const ReassemblyStats& stats() const noexcept { return stats_; }

    /// Feed one segment's payload. `sink(const uint8_t*, size_t)` receives the
    /// in-order bytes. Returns the number of bytes delivered by this call.
    template <typename Sink>
    std::size_t on_segment(std::uint32_t seq, const std::uint8_t* data, std::size_t len, bool fin,
                           Sink&& sink) noexcept {
        if (!started_ || closed_) return 0;
        if (fin) {
            fin_ = true;
            fin_seq_ = seq + static_cast<std::uint32_t>(len);
        }
        std::size_t out = 0;
        const std::int32_t d = seq_diff(seq, nxt_);
        if (d > 0) {
            queue(seq, data, len);
        } else {
            out += deliver(seq, data, len, sink);
            out += drain(sink);
        }
        if (fin_ && nxt_ == fin_seq_) {
            closed_ = true;
            nxt_ += 1;   // the FIN takes one sequence number
        }
        return out;
    }

private:
    struct Slot {
        bool used = false;
        std::uint32_t seq = 0;
        std::uint16_t len = 0;
        std::array<std::uint8_t, SlotBytes> bytes{};
    };

    template <typename Sink>
    std::size_t deliver(std::uint32_t seq, const std::uint8_t* data, std::size_t len, Sink& sink) noexcept {
        const std::int32_t d = seq_diff(seq, nxt_);   // <= 0 here
        const std::size_t skip = static_cast<std::size_t>(-static_cast<std::int64_t>(d));
        if (skip >= len) {
            stats_.duplicate_bytes += len;
            return 0;
        }
        stats_.duplicate_bytes += skip;
        const std::size_t n = len - skip;
        sink(data + skip, n);
        nxt_ += static_cast<std::uint32_t>(n);
        stats_.delivered_bytes += n;
        return n;
    }

    void queue(std::uint32_t seq, const std::uint8_t* data, std::size_t len) noexcept {
        if (len == 0) return;
        if (len > SlotBytes) {
            ++stats_.dropped_segments;
            return;
        }
        Slot* free_slot = nullptr;
        for (auto& s : slots_) {
            if (s.used && s.seq == seq && s.len >= len) return;   // already waiting
            if (!s.used && !free_slot) free_slot = &s;
        }
        if (!free_slot) {
            ++stats_.dropped_segments;
            return;
        }
        free_slot->used = true;
        free_slot->seq = seq;
        free_slot->len = static_cast<std::uint16_t>(len);
        std::memcpy(free_slot->bytes.data(), data, len);
        ++stats_.queued_segments;
    }

    template <typename Sink>
    std::size_t drain(Sink& sink) noexcept {
        std::size_t out = 0;
        for (bool progress = true; progress;) {
            progress = false;
            for (auto& s : slots_) {
                if (!s.used || seq_diff(s.seq, nxt_) > 0) continue;
                s.used = false;
                out += deliver(s.seq, s.bytes.data(), s.len, sink);
                progress = true;
            }
        }
        return out;
    }

    std::array<Slot, Slots> slots_{};
    std::uint32_t nxt_ = 0;
    std::uint32_t fin_seq_ = 0;
    bool started_ = false;
    bool fin_ = false;
    bool closed_ = false;
    ReassemblyStats stats_{};
};

} // namespace altair::net
