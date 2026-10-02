// The price service's shared bus (app/live_feed_sources.hpp): one owner
// thread, sources pushing through a ring.
//   * a late joiner gets the board as SNAPSHOT frames at the current
//     sequence, and nobody else sees them (they used to be republished to
//     every subscriber as fresh trades);
//   * every pushed update is published: the ring makes the source wait rather
//     than drop, and what reaches a subscriber plus what the bus coalesced for
//     it accounts for every frame;
//   * the source and the owner thread share no lock (run under TSan: label
//     `concurrency`).
//
// No check description here may contain the substring "F" "AIL" joined.

#include <app/live_feed_sources.hpp>
#include <server/price_payload.hpp>
#include <server/protocol.hpp>

#include <boost/asio/ip/tcp.hpp>

#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) { ++failures; }
}

namespace ip = boost::asio::ip;

struct Frame {
    altair::FrameHeader h;
    altair::PricePayload p;
};

/// Read whole frames until `quiet` passes with nothing new (or `want` frames).
std::vector<Frame> read_frames(ip::tcp::socket& s, std::vector<std::uint8_t>& buf, std::size_t want,
                               std::chrono::milliseconds quiet) {
    std::vector<Frame> out;
    std::uint8_t chunk[65536];
    auto last = std::chrono::steady_clock::now();
    for (;;) {
        boost::system::error_code ec;
        const std::size_t n = s.read_some(boost::asio::buffer(chunk), ec);
        if (!ec && n > 0) { buf.insert(buf.end(), chunk, chunk + n); last = std::chrono::steady_clock::now(); }
        std::size_t at = 0;
        while (buf.size() - at >= altair::kFrameHeaderBytes) {
            const auto h = altair::decode_header(buf.data() + at, buf.size() - at);
            if (!h) return out;
            const std::size_t need = altair::kFrameHeaderBytes + h->payload_len;
            if (buf.size() - at < need) break;
            Frame f{*h, {}};
            if (h->topic == altair::kTopicTrades)
                if (const auto d = altair::decode_price(buf.data() + at + altair::kFrameHeaderBytes, h->payload_len)) f.p = d->payload;
            out.push_back(f);
            at += need;
        }
        buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(at));
        if (want > 0 && out.size() >= want) return out;
        if (std::chrono::steady_clock::now() - last > quiet) return out;
        if (ec == boost::asio::error::would_block || ec == boost::asio::error::try_again) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        else if (ec) return out;
    }
}

template <class Pred>
bool wait_for(Pred&& p) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!p() && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return p();
}

altair::PricePayload px(std::uint32_t tok, std::int64_t paise) {
    altair::PricePayload p;
    p.token = tok;
    p.last_paise = paise;
    p.exchange_ts_ns = 1;
    return p;
}

}  // namespace

int main() {
    using namespace altair;
    std::printf("shared price bus\n");
    boost::asio::io_context io;
    PriceBus bus(io, 0);
    if (!bus.ok()) { std::printf("  could not bind\n"); return 1; }
    const auto port = bus.port();
    {
        live_sources::SharedBus shared(bus);
        boost::asio::io_context cio;
        ip::tcp::socket a(cio), b(cio);
        boost::system::error_code ec;
        a.connect(ip::tcp::endpoint(ip::make_address("127.0.0.1"), port), ec);
        a.non_blocking(true, ec);
        check(wait_for([&] { return shared.clients() == 1; }), "the first subscriber is accepted by the owner thread");
        std::vector<std::uint8_t> abuf, bbuf;

        for (std::uint32_t t = 1; t <= 3; ++t) shared.trade(px(t, 10000 * t), 1);
        shared.drain();
        auto fa = read_frames(a, abuf, 3, std::chrono::milliseconds(500));
        bool deltas = fa.size() == 3;
        for (std::size_t i = 0; i < fa.size(); ++i) deltas = deltas && fa[i].h.kind == FrameKind::Delta && fa[i].h.seq == i + 1;
        check(deltas, "three trades reach it as deltas, sequence 1, 2, 3");

        b.connect(ip::tcp::endpoint(ip::make_address("127.0.0.1"), port), ec);
        b.non_blocking(true, ec);
        check(wait_for([&] { return shared.clients() == 2 && shared.snapshots_sent() == 2; }), "a second subscriber joins");
        auto fb = read_frames(b, bbuf, 3, std::chrono::milliseconds(500));
        bool snaps = fb.size() == 3;
        for (std::size_t i = 0; i < fb.size(); ++i)
            snaps = snaps && fb[i].h.kind == FrameKind::Snapshot && fb[i].h.seq == 3 && fb[i].p.token == i + 1
                 && fb[i].p.last_paise == 10000 * static_cast<std::int64_t>(i + 1);
        check(snaps, "the joiner gets the board as snapshots, by token, at the current sequence");
        const auto again = read_frames(a, abuf, 0, std::chrono::milliseconds(200));
        check(again.empty(), "the running subscriber sees none of them: no replayed trade lands in its stream");

        shared.trade(px(2, 20500), 2);
        shared.drain();
        fa = read_frames(a, abuf, 1, std::chrono::milliseconds(500));
        fb = read_frames(b, bbuf, 1, std::chrono::milliseconds(500));
        check(fa.size() == 1 && fb.size() == 1 && fa[0].h.seq == 4 && fb[0].h.seq == 4 && fb[0].h.kind == FrameKind::Delta,
              "the next trade is sequence 4 to both: the joiner's stream continues from its baseline");

        // A burst from a source thread, several rings' worth, while a reader drains.
        b.close(ec);   // only `a` reads now, so everything coalesced was coalesced for it
        check(wait_for([&] { shared.trade(px(9, 1), 3); shared.drain(); return shared.clients() == 1; }),
              "a subscriber that leaves is dropped");
        fa = read_frames(a, abuf, 0, std::chrono::milliseconds(300));
        const std::uint64_t base = fa.empty() ? 4 : fa.back().h.seq;
        const std::uint64_t coalesced_before = shared.coalesced();
        constexpr std::uint64_t kBurst = 20000;
        std::vector<Frame> got;
        std::thread reader([&] { got = read_frames(a, abuf, kBurst, std::chrono::milliseconds(1500)); });
        std::thread source([&] {
            for (std::uint64_t k = 0; k < kBurst; ++k) shared.trade(px(static_cast<std::uint32_t>(1 + k % 50), 100 + static_cast<std::int64_t>(k)), 3);
        });
        source.join();
        shared.drain();
        reader.join();
        std::uint64_t last = base;
        bool ordered = true;
        for (const auto& f : got) { ordered = ordered && f.h.seq > last; last = f.h.seq; }
        check(shared.trades() == base + kBurst, "every update a source pushed was taken: none dropped at the ring");
        check(ordered && last == base + kBurst, "delivered in order, ending at the last sequence");
        check(wait_for([&] { return got.size() + (shared.coalesced() - coalesced_before) == kBurst; }),
              "every frame is delivered or counted as coalesced for that reader, exactly");
        std::printf("    burst: %zu delivered to the reader, ring waits %llu\n", got.size(),
                    static_cast<unsigned long long>(shared.waits()));
    }
    std::printf("%s\n", failures == 0 ? "all shared bus checks passed" : "shared bus checks did not pass");
    return failures == 0 ? 0 : 1;
}
