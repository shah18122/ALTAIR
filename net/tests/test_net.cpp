// net/packet.hpp and net/tcp_reassembly.hpp. Plain main(), like the rest of core.

#include <net/packet.hpp>
#include <net/tcp_reassembly.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

using namespace altair::net;
using Bytes = std::vector<std::uint8_t>;

void put16(Bytes& b, std::size_t at, std::uint16_t v) {
    b[at] = static_cast<std::uint8_t>(v >> 8);
    b[at + 1] = static_cast<std::uint8_t>(v);
}
void put32(Bytes& b, std::size_t at, std::uint32_t v) {
    put16(b, at, static_cast<std::uint16_t>(v >> 16));
    put16(b, at + 2, static_cast<std::uint16_t>(v));
}

// An Ethernet + IPv4 + TCP frame with correct checksums, the way a NIC hands it over.
Bytes frame(std::uint32_t seq, const std::string& payload, std::uint8_t flags = tcp_flag::ack | tcp_flag::psh,
            bool vlan = false, std::size_t tcp_options = 0) {
    const std::size_t l2 = vlan ? 18 : 14;
    const std::size_t tcp_hdr = 20 + tcp_options;
    const std::size_t total = 20 + tcp_hdr + payload.size();
    Bytes b(l2 + total, 0);
    if (vlan) {
        put16(b, 12, kEtherVlan);
        put16(b, 14, 7);   // VLAN id 7
        put16(b, 16, kEtherIpv4);
    } else {
        put16(b, 12, kEtherIpv4);
    }
    std::uint8_t* ip = b.data() + l2;
    ip[0] = 0x45;
    put16(b, l2 + 2, static_cast<std::uint16_t>(total));
    put16(b, l2 + 6, 0x4000);   // DF
    ip[8] = 64;
    ip[9] = kIpProtoTcp;
    ip[12] = 104; ip[13] = 18; ip[14] = 4; ip[15] = 135;   // 104.18.4.135 (FYERS, behind Cloudflare)
    ip[16] = 172; ip[17] = 31; ip[18] = 44; ip[19] = 249;
    put16(b, l2 + 10, csum_fold(csum_add(0, ip, 20)));
    const std::size_t t = l2 + 20;
    put16(b, t, 443);
    put16(b, t + 2, 50000);
    put32(b, t + 4, seq);
    put32(b, t + 8, 777);
    b[t + 12] = static_cast<std::uint8_t>((tcp_hdr / 4) << 4);
    b[t + 13] = flags;
    put16(b, t + 14, 65535);
    std::copy(payload.begin(), payload.end(), b.begin() + static_cast<std::ptrdiff_t>(t + tcp_hdr));
    TcpSegment s{};
    if (parse_frame(b.data(), b.size(), s) == Parse::Ok) put16(b, t + 16, tcp_checksum(s));
    return b;
}

void test_parse() {
    std::puts("packet parsing");
    Bytes f = frame(1000, "hello");
    TcpSegment s{};
    check(parse_frame(f.data(), f.size(), s) == Parse::Ok, "a TCP/IPv4 frame parses");
    check(s.seq == 1000 && s.ack == 777 && s.window == 65535, "seq, ack and window in host order");
    check(s.payload_len == 5 && std::string(reinterpret_cast<const char*>(s.payload), 5) == "hello",
          "the payload points at the bytes, no copy");
    std::uint32_t fyers = 0;
    check(parse_ipv4("104.18.4.135", fyers) && s.flow.src_ip == fyers, "source address in network order");
    check(s.flow.src_port == load_raw16(reinterpret_cast<const std::uint8_t*>("\x01\xbb")), "source port 443");
    check(ipv4_checksum_ok(s) && tcp_checksum_ok(s), "IPv4 and TCP checksums verify");

    Bytes bad = f;
    bad.back() ^= 0x01;
    TcpSegment sb{};
    check(parse_frame(bad.data(), bad.size(), sb) == Parse::Ok && !tcp_checksum_ok(sb), "a flipped payload bit fails the TCP checksum");

    Bytes odd = frame(1, "abc");
    TcpSegment so{};
    check(parse_frame(odd.data(), odd.size(), so) == Parse::Ok && tcp_checksum_ok(so), "odd-length payload checksums");

    Bytes v = frame(5, "vlan", tcp_flag::ack, true);
    TcpSegment sv{};
    check(parse_frame(v.data(), v.size(), sv) == Parse::Ok && sv.payload_len == 4, "one 802.1Q tag is skipped");

    Bytes o = frame(9, "opts", tcp_flag::ack, false, 12);
    TcpSegment sopt{};
    check(parse_frame(o.data(), o.size(), sopt) == Parse::Ok && sopt.payload_len == 4 && tcp_checksum_ok(sopt),
          "TCP options (timestamps) are skipped");

    check(parse_frame(f.data(), 10, s) == Parse::Truncated, "a runt frame is truncated");
    check(parse_frame(f.data(), f.size() - 1, s) == Parse::Truncated, "a frame shorter than its IP length is truncated");

    Bytes frag = f;
    put16(frag, 14 + 6, 0x2000);   // MF
    check(parse_frame(frag.data(), frag.size(), s) == Parse::Fragment, "a fragment is refused");

    Bytes udp = f;
    udp[14 + 9] = 17;
    check(parse_frame(udp.data(), udp.size(), s) == Parse::NotTcp, "UDP is not TCP");

    Bytes arp = f;
    put16(arp, 12, 0x0806);
    check(parse_frame(arp.data(), arp.size(), s) == Parse::NotIpv4, "ARP is not IPv4");

    Bytes ihl = f;
    ihl[14] = 0x44;
    check(parse_frame(ihl.data(), ihl.size(), s) == Parse::BadHeader, "IHL < 5 is a bad header");

    std::uint32_t a = 0;
    check(!parse_ipv4("256.1.1.1", a) && !parse_ipv4("1.2.3", a) && !parse_ipv4("1.2.3.4.5", a) && !parse_ipv4("a.b.c.d", a),
          "malformed addresses are refused");
}

struct Collect {
    std::string* out;
    void operator()(const std::uint8_t* p, std::size_t n) const { out->append(reinterpret_cast<const char*>(p), n); }
};

void test_reassembly() {
    std::puts("TCP reassembly");
    {
        TcpReassembler<> r;
        std::string got;
        Collect c{&got};
        check(r.on_segment(100, reinterpret_cast<const std::uint8_t*>("x"), 1, false, c) == 0 && got.empty(),
              "nothing is delivered before start()");
        r.start(100);
        r.on_segment(100, reinterpret_cast<const std::uint8_t*>("abc"), 3, false, c);
        r.on_segment(103, reinterpret_cast<const std::uint8_t*>("def"), 3, false, c);
        check(got == "abcdef" && r.next() == 106, "in-order segments are delivered at once");
    }
    {
        TcpReassembler<> r;
        r.start(0);
        std::string got;
        Collect c{&got};
        r.on_segment(6, reinterpret_cast<const std::uint8_t*>("ghi"), 3, false, c);
        r.on_segment(3, reinterpret_cast<const std::uint8_t*>("def"), 3, false, c);
        check(got.empty() && r.stats().queued_segments == 2, "early segments wait");
        r.on_segment(0, reinterpret_cast<const std::uint8_t*>("abc"), 3, false, c);
        check(got == "abcdefghi" && r.next() == 9, "the gap fills and the waiting segments follow in order");
    }
    {
        TcpReassembler<> r;
        r.start(0);
        std::string got;
        Collect c{&got};
        r.on_segment(0, reinterpret_cast<const std::uint8_t*>("abcd"), 4, false, c);
        r.on_segment(0, reinterpret_cast<const std::uint8_t*>("abcd"), 4, false, c);
        r.on_segment(2, reinterpret_cast<const std::uint8_t*>("cdef"), 4, false, c);
        check(got == "abcdef" && r.stats().duplicate_bytes == 6, "retransmits and overlaps are trimmed, delivered once");
    }
    {
        TcpReassembler<> r;
        const std::uint32_t isn = 0xFFFFFFFCu;
        r.start(isn);
        std::string got;
        Collect c{&got};
        r.on_segment(isn + 4, reinterpret_cast<const std::uint8_t*>("efgh"), 4, false, c);
        r.on_segment(isn, reinterpret_cast<const std::uint8_t*>("abcd"), 4, false, c);
        check(got == "abcdefgh" && r.next() == 4, "the 2^32 sequence wrap is seamless");
    }
    {
        TcpReassembler<2, 16> r;
        r.start(0);
        std::string got;
        Collect c{&got};
        r.on_segment(10, reinterpret_cast<const std::uint8_t*>("a"), 1, false, c);
        r.on_segment(20, reinterpret_cast<const std::uint8_t*>("b"), 1, false, c);
        r.on_segment(30, reinterpret_cast<const std::uint8_t*>("c"), 1, false, c);
        std::uint8_t big[32] = {};
        r.on_segment(40, big, sizeof big, false, c);
        check(r.stats().dropped_segments == 2, "no free slot, or too big for one: dropped for the sender to resend");
    }
    {
        TcpReassembler<> r;
        r.start(0);
        std::string got;
        Collect c{&got};
        r.on_segment(3, reinterpret_cast<const std::uint8_t*>("de"), 2, true, c);
        check(!r.closed(), "an early FIN does not close");
        r.on_segment(0, reinterpret_cast<const std::uint8_t*>("abc"), 3, false, c);
        check(got == "abcde" && r.closed() && r.next() == 6, "the FIN closes once its bytes are in, and takes one number");
    }
    {
        // Every permutation the network could produce: random order, duplicates,
        // overlapping re-segmentation, against the stream itself.
        std::mt19937 rng(20261104);
        int bad = 0;
        for (int round = 0; round < 200; ++round) {
            std::string stream(4000, '\0');
            for (auto& ch : stream) ch = static_cast<char>('a' + rng() % 26);
            struct Seg { std::uint32_t at; std::uint32_t len; };
            std::vector<Seg> segs;
            for (std::uint32_t at = 0; at < stream.size();) {
                const std::uint32_t len = std::min<std::uint32_t>(static_cast<std::uint32_t>(1 + rng() % 300), static_cast<std::uint32_t>(stream.size()) - at);
                segs.push_back({at, len});
                if (rng() % 4 == 0) segs.push_back({at, len});   // a retransmit
                if (rng() % 5 == 0 && at > 10) segs.push_back({at - 10, std::min<std::uint32_t>(len + 10, 300)});
                at += len;
            }
            std::shuffle(segs.begin(), segs.end(), rng);
            const auto isn = static_cast<std::uint32_t>(rng());
            TcpReassembler<64, 512> r;
            r.start(isn);
            std::string got;
            Collect c{&got};
            // Pass until everything is in: a dropped segment comes round again, as a retransmit would.
            for (int pass = 0; pass < 50 && got.size() < stream.size(); ++pass)
                for (const Seg& s : segs) {
                    const std::uint32_t len = std::min<std::uint32_t>(s.len, static_cast<std::uint32_t>(stream.size()) - s.at);
                    r.on_segment(isn + s.at, reinterpret_cast<const std::uint8_t*>(stream.data() + s.at), len, false, c);
                }
            if (got != stream) ++bad;
        }
        check(bad == 0, "200 shuffled, duplicated, overlapping streams come out byte-exact");
    }
}

} // namespace

int main() {
    test_parse();
    test_reassembly();
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "passed", failures);
    return failures ? 1 : 0;
}
