#pragma once

// net/user_tcp.hpp -- a TCP client that runs in ALTAIR's own thread.
//
// With kernel bypass there is no kernel TCP for the bypass connection: this
// is it. One active-open connection (ALTAIR -> FYERS), transport-agnostic:
// frames go out through a callback (AF_XDP's XskQueue::send, or DPDK later)
// and come in through on_frame(). Nothing here blocks or sleeps; the owner
// busy-polls and calls tick() with the time for retransmits.
//
// What it implements (RFC 9293, client side):
//   - three-way handshake with an MSS option; no window scaling, SACK or
//     timestamps are offered, so the peer uses none (a 64 KiB window: ample
//     for a market-data feed, whose bursts are far smaller)
//   - in-order delivery through TcpReassembler (reordering, duplicates)
//   - an ACK for every segment that carries data or a FIN
//   - sending within the peer's window, go-back-N retransmit on timeout with
//     exponential backoff (RTO 200 ms, SYN 1 s), and a fast retransmit on
//     three duplicate ACKs
//   - FIN in both directions, RST in, RST out for a segment with no connection
//
// Frames carry the next hop's MAC (the VPC gateway on EC2, the peer itself on
// a veth) and are built with correct IPv4 and TCP checksums.

#include <net/packet.hpp>
#include <net/tcp_reassembly.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace altair::net {

struct TcpEndpoints {
    std::array<std::uint8_t, 6> local_mac{};
    std::array<std::uint8_t, 6> next_hop_mac{};
    std::uint32_t local_ip = 0;     // network order
    std::uint32_t remote_ip = 0;    // network order
    std::uint16_t local_port = 0;   // host order
    std::uint16_t remote_port = 0;  // host order
    std::uint16_t mss = 1460;
};

enum class TcpState : std::uint8_t { Closed, SynSent, Established, FinWait, CloseWait, LastAck, Failed };

inline const char* state_name(TcpState s) noexcept {
    switch (s) {
    case TcpState::Closed: return "closed";
    case TcpState::SynSent: return "SYN sent";
    case TcpState::Established: return "established";
    case TcpState::FinWait: return "FIN sent";
    case TcpState::CloseWait: return "peer closed";
    case TcpState::LastAck: return "last ACK";
    case TcpState::Failed: return "failed";
    }
    return "?";
}

struct TcpStats {
    std::uint64_t segments_out = 0;
    std::uint64_t segments_in = 0;
    std::uint64_t retransmits = 0;
    std::uint64_t fast_retransmits = 0;
    std::uint64_t bytes_sent = 0;
    std::uint64_t bytes_acked = 0;
    std::uint64_t window_stalls = 0;
};

/// Write one Ethernet + IPv4 + TCP frame into `out` (>= 54 + options + len
/// bytes); returns its length. `options` must be a multiple of 4 bytes.
inline std::size_t build_tcp_frame(std::uint8_t* out, const TcpEndpoints& e, std::uint16_t ip_id, std::uint32_t seq,
                                   std::uint32_t ack, std::uint8_t flags, std::uint16_t window,
                                   const std::uint8_t* options, std::size_t options_len, const std::uint8_t* payload,
                                   std::size_t len) noexcept {
    auto put16 = [](std::uint8_t* p, std::uint16_t v) {
        p[0] = static_cast<std::uint8_t>(v >> 8);
        p[1] = static_cast<std::uint8_t>(v);
    };
    auto put32 = [&](std::uint8_t* p, std::uint32_t v) {
        put16(p, static_cast<std::uint16_t>(v >> 16));
        put16(p + 2, static_cast<std::uint16_t>(v));
    };
    std::memcpy(out, e.next_hop_mac.data(), 6);
    std::memcpy(out + 6, e.local_mac.data(), 6);
    put16(out + 12, kEtherIpv4);
    std::uint8_t* ip = out + kEthHeader;
    const std::size_t tcp_len = 20 + options_len + len;
    const std::size_t total = 20 + tcp_len;
    ip[0] = 0x45;
    ip[1] = 0;
    put16(ip + 2, static_cast<std::uint16_t>(total));
    put16(ip + 4, ip_id);
    put16(ip + 6, 0x4000);   // DF
    ip[8] = 64;
    ip[9] = kIpProtoTcp;
    ip[10] = ip[11] = 0;
    std::memcpy(ip + 12, &e.local_ip, 4);
    std::memcpy(ip + 16, &e.remote_ip, 4);
    put16(ip + 10, csum_fold(csum_add(0, ip, 20)));
    std::uint8_t* tcp = ip + 20;
    put16(tcp, e.local_port);
    put16(tcp + 2, e.remote_port);
    put32(tcp + 4, seq);
    put32(tcp + 8, ack);
    tcp[12] = static_cast<std::uint8_t>(((20 + options_len) / 4) << 4);
    tcp[13] = flags;
    put16(tcp + 14, window);
    tcp[16] = tcp[17] = 0;
    tcp[18] = tcp[19] = 0;
    if (options_len) std::memcpy(tcp + 20, options, options_len);
    if (len) std::memcpy(tcp + 20 + options_len, payload, len);
    std::uint32_t acc = csum_add(0, ip + 12, 8);
    acc += kIpProtoTcp;
    acc += static_cast<std::uint32_t>(tcp_len);
    acc = csum_add(acc, tcp, tcp_len);
    put16(tcp + 16, csum_fold(acc));
    return kEthHeader + total;
}

/// Send: bool(const uint8_t* frame, size_t len). Deliver: void(const uint8_t*, size_t).
template <typename Send, typename Deliver>
class UserTcp {
public:
    static constexpr std::uint64_t kRtoNs = 200'000'000;      // 200 ms
    static constexpr std::uint64_t kSynRtoNs = 1'000'000'000; // 1 s
    static constexpr int kMaxRetries = 8;
    static constexpr std::uint16_t kRecvWindow = 65535;
    static constexpr std::size_t kMaxFrame = 2048;

    UserTcp(const TcpEndpoints& e, Send send, Deliver deliver) : e_(e), send_(send), deliver_(deliver) {}

    TcpState state() const noexcept { return state_; }
    const TcpStats& stats() const noexcept { return stats_; }
    const ReassemblyStats& rx_stats() const noexcept { return rx_.stats(); }
    bool established() const noexcept { return state_ == TcpState::Established; }
    /// Bytes handed to send() that the peer has not acknowledged yet.
    std::size_t unacked() const noexcept { return outq_.size(); }

    /// Begin the handshake. `isn` is the initial sequence number (random).
    void connect(std::uint32_t isn, std::uint64_t now_ns) {
        iss_ = isn;
        snd_una_ = snd_nxt_ = isn;
        state_ = TcpState::SynSent;
        send_syn();
        arm(now_ns, kSynRtoNs);
    }

    /// Queue bytes for the peer; they go out as the peer's window allows.
    void send(const std::uint8_t* p, std::size_t n, std::uint64_t now_ns) {
        if (state_ != TcpState::Established && state_ != TcpState::CloseWait) return;
        outq_.insert(outq_.end(), p, p + n);
        push(now_ns);
    }

    /// Send a FIN once everything queued is out.
    void close(std::uint64_t now_ns) {
        if (state_ == TcpState::Established) {
            fin_wanted_ = true;
            state_ = TcpState::FinWait;
        } else if (state_ == TcpState::CloseWait) {
            fin_wanted_ = true;
            state_ = TcpState::LastAck;
        }
        push(now_ns);
    }

    /// A frame of this connection (the caller has matched the 4-tuple).
    void on_segment(const TcpSegment& s, std::uint64_t now_ns) {
        ++stats_.segments_in;
        if (s.flags & tcp_flag::rst) {
            if (state_ != TcpState::SynSent || ((s.flags & tcp_flag::ack) && s.ack == snd_nxt_)) state_ = TcpState::Failed;
            return;
        }
        if (state_ == TcpState::SynSent) {
            if ((s.flags & (tcp_flag::syn | tcp_flag::ack)) != (tcp_flag::syn | tcp_flag::ack) || s.ack != iss_ + 1) {
                if (s.flags & tcp_flag::ack) send_rst(s.ack);
                return;
            }
            snd_una_ = snd_nxt_ = s.ack;
            snd_wnd_ = s.window;
            rx_.start(s.seq + 1);
            state_ = TcpState::Established;
            retries_ = 0;
            rto_at_ = 0;
            send_ack();
            push(now_ns);
            return;
        }
        if (state_ == TcpState::Closed || state_ == TcpState::Failed) return;
        if (s.flags & tcp_flag::syn) {   // a retransmitted SYN-ACK: our ACK was lost
            send_ack();
            return;
        }
        if (s.flags & tcp_flag::ack) on_ack(s.ack, s.window, s.payload_len == 0 && !(s.flags & tcp_flag::fin), now_ns);
        if (s.payload_len || (s.flags & tcp_flag::fin)) {
            const bool was_closed = rx_.closed();
            rx_.on_segment(s.seq, s.payload, s.payload_len, (s.flags & tcp_flag::fin) != 0,
                           [&](const std::uint8_t* p, std::size_t n) { deliver_(p, n); });
            if (rx_.closed() && !was_closed) {
                if (state_ == TcpState::Established) state_ = TcpState::CloseWait;
                else if (state_ == TcpState::FinWait && fin_acked_) state_ = TcpState::Closed;
            }
            send_ack();
        }
    }

    /// Retransmit what has waited past its deadline. Call it often.
    void tick(std::uint64_t now_ns) {
        if (rto_at_ == 0 || now_ns < rto_at_) return;
        if (++retries_ > kMaxRetries) {
            state_ = TcpState::Failed;
            rto_at_ = 0;
            return;
        }
        ++stats_.retransmits;
        const std::uint64_t base = state_ == TcpState::SynSent ? kSynRtoNs : kRtoNs;
        if (state_ == TcpState::SynSent) {
            send_syn();
        } else {
            snd_nxt_ = snd_una_;   // go back N
            fin_sent_ = false;
            push(now_ns, /*rearm=*/false);
        }
        arm(now_ns, base << std::min(retries_, 6));
    }

private:
    void arm(std::uint64_t now_ns, std::uint64_t after) { rto_at_ = now_ns + after; }

    bool emit(std::uint32_t seq, std::uint8_t flags, const std::uint8_t* opts, std::size_t olen,
              const std::uint8_t* payload, std::size_t len) {
        const std::size_t n = build_tcp_frame(frame_.data(), e_, ip_id_++, seq, rx_.started() ? rx_.next() : 0, flags,
                                              kRecvWindow, opts, olen, payload, len);
        ++stats_.segments_out;
        return send_(frame_.data(), n);
    }

    void send_syn() {
        const std::uint8_t mss[4] = {2, 4, static_cast<std::uint8_t>(e_.mss >> 8), static_cast<std::uint8_t>(e_.mss)};
        emit(iss_, tcp_flag::syn, mss, sizeof mss, nullptr, 0);
        snd_nxt_ = iss_ + 1;
    }
    void send_ack() { emit(snd_nxt_, tcp_flag::ack, nullptr, 0, nullptr, 0); }
    void send_rst(std::uint32_t seq) { emit(seq, tcp_flag::rst, nullptr, 0, nullptr, 0); }

    // Send new data (and the FIN) as far as the peer's window reaches.
    void push(std::uint64_t now_ns, bool rearm = true) {
        if (state_ == TcpState::SynSent || state_ == TcpState::Failed || state_ == TcpState::Closed) return;
        const std::uint32_t mss = e_.mss;
        bool sent = false;
        for (;;) {
            const std::uint32_t in_flight = snd_nxt_ - snd_una_;
            if (in_flight >= outq_.size()) break;
            if (in_flight >= snd_wnd_) {
                ++stats_.window_stalls;
                break;
            }
            const std::uint32_t room = snd_wnd_ - in_flight;
            const std::uint32_t n = std::min<std::uint32_t>({mss, room, static_cast<std::uint32_t>(outq_.size() - in_flight)});
            if (!emit(snd_nxt_, tcp_flag::ack | tcp_flag::psh, nullptr, 0, outq_.data() + in_flight, n)) break;
            snd_nxt_ += n;
            stats_.bytes_sent += n;
            sent = true;
        }
        if (fin_wanted_ && !fin_sent_ && snd_nxt_ - snd_una_ == outq_.size()) {
            emit(snd_nxt_, tcp_flag::fin | tcp_flag::ack, nullptr, 0, nullptr, 0);
            snd_nxt_ += 1;
            fin_sent_ = true;
            sent = true;
        }
        if (sent && rearm && rto_at_ == 0) arm(now_ns, kRtoNs);
    }

    void on_ack(std::uint32_t ack, std::uint16_t window, bool pure, std::uint64_t now_ns) {
        const std::int32_t advance = seq_diff(ack, snd_una_);
        if (advance < 0 || seq_diff(ack, snd_nxt_) > 0) return;   // old, or for data never sent
        snd_wnd_ = window;
        if (advance == 0) {
            if (pure && snd_nxt_ != snd_una_ && ++dup_acks_ == 3) {   // fast retransmit
                ++stats_.fast_retransmits;
                snd_nxt_ = snd_una_;
                fin_sent_ = false;
                push(now_ns, false);
            }
            if (!pure) dup_acks_ = 0;
            push(now_ns);
            return;
        }
        dup_acks_ = 0;
        std::size_t data = static_cast<std::size_t>(advance);
        if (fin_sent_ && ack == snd_nxt_) {   // the FIN's own sequence number
            fin_acked_ = true;
            data -= 1;
            if (state_ == TcpState::LastAck) state_ = TcpState::Closed;
            else if (state_ == TcpState::FinWait && rx_.closed()) state_ = TcpState::Closed;
        }
        data = std::min(data, outq_.size());
        outq_.erase(outq_.begin(), outq_.begin() + static_cast<std::ptrdiff_t>(data));
        stats_.bytes_acked += data;
        snd_una_ = ack;
        retries_ = 0;
        rto_at_ = 0;
        if (snd_nxt_ != snd_una_) arm(now_ns, kRtoNs);
        push(now_ns);
    }

    TcpEndpoints e_;
    Send send_;
    Deliver deliver_;
    TcpState state_ = TcpState::Closed;
    TcpReassembler<64, 2048> rx_;
    std::vector<std::uint8_t> outq_;   // from snd_una_: sent-but-unacked, then not yet sent
    std::array<std::uint8_t, kMaxFrame> frame_{};
    std::uint32_t iss_ = 0, snd_una_ = 0, snd_nxt_ = 0;
    std::uint32_t snd_wnd_ = 0;
    std::uint64_t rto_at_ = 0;
    int retries_ = 0;
    int dup_acks_ = 0;
    std::uint16_t ip_id_ = 1;
    bool fin_wanted_ = false, fin_sent_ = false, fin_acked_ = false;
    TcpStats stats_{};
};

} // namespace altair::net
