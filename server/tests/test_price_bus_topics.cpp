// The price bus with all three topics interleaved: trades, books and the
// quote topic. Each topic keeps its own unbroken sequence, the quote frame
// arrives intact, and an unknown topic is refused rather than written past the
// end of the sequence table (which is what happened before the quote topic had
// a slot).
//
// No check description here may contain the substring "F" "AIL" joined.

#include <server/price_bus.hpp>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>

#include <chrono>
#include <cstdio>
#include <map>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) { ++failures; }
}

}  // namespace

int main() {
    using namespace altair;
    namespace ip = boost::asio::ip;
    std::printf("price bus topics\n");
    boost::asio::io_context io;
    PriceBus bus(io, 0);
    if (!bus.ok()) { std::printf("  could not bind\n"); return 1; }

    boost::asio::io_context cio;
    ip::tcp::socket client(cio);
    boost::system::error_code ec;
    client.connect(ip::tcp::endpoint(ip::make_address("127.0.0.1"), bus.port()), ec);
    check(!ec, "a subscriber connects");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (bus.clients() != 1 && std::chrono::steady_clock::now() < deadline) {
        bus.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(bus.clients() == 1, "and is accepted");

    constexpr int kEach = 5;
    PriceLevel bids[kMaxDepthLevels]{}, asks[kMaxDepthLevels]{};
    for (std::size_t i = 0; i < kMaxDepthLevels; ++i) {
        bids[i] = PriceLevel{100000 - 5 * static_cast<std::int64_t>(i), 65, 1, 0};
        asks[i] = PriceLevel{100005 + 5 * static_cast<std::int64_t>(i), 65, 1, 0};
    }
    for (int k = 0; k < kEach; ++k) {
        PricePayload t;
        t.token = 256265;
        t.last_paise = 2300000 + k;
        bus.publish(kTopicTrades, t, nullptr, nullptr, 1);
        QuotePayload q;
        q.token = 256265;
        q.flags = kQuoteHasPrevClose;
        q.prev_close = 2290000 + k;
        bus.publish_quote(q, 1);
        PricePayload b;
        b.token = 12468226;
        b.flags = kPriceHasBook;
        b.depth_levels = kMaxDepthLevels;
        bus.publish(kTopicBook, b, bids, asks, 1);
    }
    PricePayload stray;
    stray.token = 1;
    bus.publish(7, stray, nullptr, nullptr, 1);   // no such topic
    for (int k = 0; k < 50 && bus.pending() > 0; ++k) {
        bus.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::map<std::uint32_t, std::vector<std::uint64_t>> seqs;
    std::vector<std::int64_t> prev_closes;
    for (int n = 0; n < 3 * kEach; ++n) {
        std::uint8_t head[kFrameHeaderBytes];
        boost::asio::read(client, boost::asio::buffer(head), ec);
        if (ec) { break; }
        const auto h = decode_header(head, sizeof head);
        if (!h) { break; }
        std::vector<std::uint8_t> body(h->payload_len);
        boost::asio::read(client, boost::asio::buffer(body), ec);
        if (ec) { break; }
        seqs[h->topic].push_back(h->seq);
        if (h->topic == kTopicQuote) {
            const auto q = decode_quote(body.data(), body.size());
            if (q) { prev_closes.push_back(q->prev_close); }
        }
    }
    const auto run = [&seqs](std::uint32_t topic) {
        const auto& v = seqs[topic];
        if (v.size() != kEach) { return false; }
        for (std::size_t i = 0; i < v.size(); ++i)
            if (v[i] != i + 1) { return false; }
        return true;
    };
    check(run(kTopicTrades), "trades run 1..5");
    check(run(kTopicBook), "books run 1..5");
    check(run(kTopicQuote), "quotes run 1..5 -- their own sequence, not a neighbour's");
    check(prev_closes.size() == kEach && prev_closes.front() == 2290000 && prev_closes.back() == 2290004,
          "quote frames decode intact");
    check(seqs.size() == 3, "an unknown topic is refused, not sent");
    std::printf("%s\n", failures == 0 ? "all price bus topic checks passed" : "price bus topic checks did not pass");
    return failures == 0 ? 0 : 1;
}
