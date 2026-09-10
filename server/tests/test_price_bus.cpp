// P37-02 acceptance tests for server/price_bus.hpp.
//
// A REAL SOCKET, ON LOOPBACK, WITH NO KITE ANYWHERE.
//
// The bus is the piece that cannot be proved by reasoning: it has a socket, a
// queue, and a policy for what happens when the reader stops. So this binds an
// ephemeral port, connects to it, and reads bytes back out.
//
// THE CHECK THAT MATTERS IS THE THIRD ONE.
//
// A subscriber that stops reading must not stall the feed, and the frames it
// misses must be VISIBLE to it rather than silently skipped. Anyone can write
// a queue that drops; the property worth testing is that after dropping, the
// stream is still frame-aligned and the sequence number jumps -- so the reader
// learns it missed something instead of quietly carrying on with a gap in its
// picture of the market. A bus that dropped bytes rather than whole frames
// would leave the reader mid-header and every later frame would be garbage
// that still parsed as a number.
//
// No check description here may contain the substring "F" "AIL" joined.

#include <server/price_bus.hpp>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    if (ok) {
        std::printf("  ok  : %s\n", what);
    } else {
        ++failures;
        std::printf("  FAIL: %s\n", what);
    }
}

altair::PricePayload tick(std::uint32_t token, std::int64_t paise,
                          std::int64_t ts)
{
    altair::PricePayload p;
    p.token = token;
    p.last_paise = paise;
    p.exchange_ts_ns = ts;
    return p;
}

}  // namespace

int main()
{
    std::printf("P37-02 price bus\n\n");
    namespace ip = boost::asio::ip;

    boost::asio::io_context io;
    altair::PriceBus bus(io, 0);          // 0 = let the OS choose
    if (!bus.ok()) {
        std::printf("  could not bind: %s\n", bus.error().message().c_str());
        return 1;
    }
    check(bus.port() != 0, "the bus binds an ephemeral loopback port");
    std::printf("        listening on 127.0.0.1:%u\n",
                static_cast<unsigned>(bus.port()));

    boost::asio::io_context cio;
    ip::tcp::socket client(cio);
    boost::system::error_code ec;
    // A SMALL RECEIVE BUFFER, BECAUSE THE POINT IS TO SIMULATE A READER THAT
    // HAS STOPPED, and Windows will not let one stop by itself.
    //
    // The bus bounds its own send buffer, but loopback receive buffers
    // auto-tune upward and absorbed the entire flood twice: 32,766 frames,
    // 2.9 MB, nothing coalesced, because the kernel on the CLIENT side was
    // willing to hold all of it. Capping it here makes the backlog land where
    // the test is actually looking. This is scaffolding for the scenario, not
    // a property of the bus.
    client.open(ip::tcp::v4(), ec);
    client.set_option(boost::asio::socket_base::receive_buffer_size(8 * 1024),
                      ec);
    client.connect(ip::tcp::endpoint(ip::make_address("127.0.0.1"),
                                     bus.port()), ec);
    check(!ec, "a subscriber connects");
    bus.poll();
    check(bus.clients() == 1, "and the bus sees exactly one client");

    // ---- frames arrive intact, in order ----------------------------------
    constexpr int kN = 20;
    for (int i = 0; i < kN; ++i) {
        bus.publish(altair::kTopicTrades,
                    tick(256265, 2343150 + i, 1789000000000000000LL + i),
                    nullptr, nullptr, 1789000000000000000LL + i);
    }
    // Give the loopback stack a moment, then push whatever is left.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    bus.poll();

    std::vector<std::uint8_t> buf(
        static_cast<std::size_t>(kN)
        * (altair::kFrameHeaderBytes + altair::kPricePayloadBytes));
    boost::asio::read(client, boost::asio::buffer(buf), ec);
    check(!ec, "every published frame arrives");

    bool ordered = true, correct = true;
    std::size_t at = 0;
    for (int i = 0; i < kN && ordered && correct; ++i) {
        const auto h = altair::decode_header(buf.data() + at,
                                             buf.size() - at);
        if (!h || h->seq != static_cast<std::uint64_t>(i + 1)) {
            ordered = false;
            break;
        }
        if (h->topic != altair::kTopicTrades
            || h->channel != altair::Channel::State) {
            correct = false;
            break;
        }
        const auto p = altair::decode_price(
            buf.data() + at + altair::kFrameHeaderBytes, h->payload_len);
        if (!p || p->payload.token != 256265
            || p->payload.last_paise != 2343150 + i) {
            correct = false;
            break;
        }
        // ENGINE TIME IS THE TICK'S, SERVER TIME IS NOW. On a replay these
        // differ by years, which is what makes a replay visibly a replay.
        if (h->engine_time_ns != 1789000000000000000LL + i
            || h->server_time_ns <= h->engine_time_ns) {
            correct = false;
            break;
        }
        at += altair::kFrameHeaderBytes + h->payload_len;
    }
    check(ordered, "sequence numbers run 1..N with no gap and no repeat");
    check(correct,
          "and every frame carries the right topic, token, price and clocks");

    // ---- a reader that stops must not stall the feed ---------------------
    //
    // Nothing is read from `client` below. The socket buffer fills, the outbox
    // fills, and the cap starts dropping. The feed must keep running.
    const std::uint64_t before = bus.coalesced();
    const std::size_t frame_bytes =
        altair::kFrameHeaderBytes + altair::kPricePayloadBytes;
    const int flood =
        static_cast<int>(altair::kOutboxBytesCap / frame_bytes) * 3;
    for (int i = 0; i < flood; ++i) {
        bus.publish(altair::kTopicTrades,
                    tick(256265, 3000000 + i, 1789000000000000000LL),
                    nullptr, nullptr, 1789000000000000000LL);
    }
    bus.poll();
    std::printf("        flooded %d frames at a reader that stopped; "
                "coalesced %llu\n",
                flood,
                static_cast<unsigned long long>(bus.coalesced() - before));
    check(bus.coalesced() > before,
          "a subscriber that stops reading has its oldest frames dropped");
    check(bus.clients() == 1,
          "and it is NOT disconnected -- a slow reader is not a dead one");

    // Now drain and confirm the survivor stream is still frame-aligned, and
    // that the gap is visible.
    // DRAIN EVERYTHING, and look for the gap anywhere in the stream.
    //
    // The first draft looked for it at the START and did not find it, which
    // was the test being wrong rather than the bus. What a resuming reader
    // actually sees is: the frames already sitting in the kernel buffers,
    // in order; then the jump; then the newest frames. The gap is in the
    // middle, exactly where it should be -- the bus dropped from the middle
    // of the queue, not from the front of what had already been handed to the
    // kernel.
    // NON-BLOCKING FROM HERE. The first draft left the client socket blocking
    // and read_some() simply never returned once the stream ran dry -- the
    // test hung rather than reporting anything, which is the least useful
    // possible outcome for a check about a reader that has stopped.
    client.non_blocking(true, ec);

    std::vector<std::uint8_t> tail(8u * 1024u * 1024u);
    std::size_t got = 0;
    int quiet = 0;
    for (int spin = 0; spin < 20000 && got < tail.size(); ++spin) {
        boost::system::error_code rec;
        const std::size_t n = client.read_some(
            boost::asio::buffer(tail.data() + got, tail.size() - got), rec);
        got += n;
        bus.poll();
        if (rec && rec != boost::asio::error::would_block
            && rec != boost::asio::error::try_again) {
            break;                                   // closed or a real error
        }
        // The bus refills the socket on poll(), so "nothing this time" is not
        // "nothing left". Stop only after several consecutive empty reads.
        quiet = (n == 0) ? quiet + 1 : 0;
        if (quiet > 50) { break; }
    }

    bool wellformed = got >= frame_bytes;
    std::size_t frames = 0, gaps = 0;
    std::uint64_t first_seq = 0, last_seq = 0, biggest_jump = 0;
    std::size_t off = 0;
    while (off + altair::kFrameHeaderBytes <= got) {
        const auto h = altair::decode_header(tail.data() + off, got - off);
        if (!h) { wellformed = false; break; }
        if (off + altair::kFrameHeaderBytes + h->payload_len > got) { break; }
        if (frames == 0) {
            first_seq = h->seq;
        } else if (h->seq != last_seq + 1) {
            // A jump FORWARD is a coalesce and is the point. A repeat or a
            // step backwards would be corruption.
            if (h->seq <= last_seq) { wellformed = false; break; }
            ++gaps;
            biggest_jump = std::max(biggest_jump, h->seq - last_seq - 1);
        }
        last_seq = h->seq;
        ++frames;
        off += altair::kFrameHeaderBytes + h->payload_len;
    }
    std::printf("        drained %zu frames, seq %llu..%llu, %zu gap(s), "
                "biggest %llu\n",
                frames,
                static_cast<unsigned long long>(first_seq),
                static_cast<unsigned long long>(last_seq),
                gaps, static_cast<unsigned long long>(biggest_jump));
    check(wellformed && frames > 0,
          "after coalescing every frame is still well-formed and the sequence "
          "never repeats or goes backwards -- whole frames were dropped, not "
          "bytes");
    check(gaps > 0,
          "and the sequence JUMPS somewhere in the stream, so the reader can "
          "see it missed something rather than quietly carrying a gap");
    check(last_seq == static_cast<std::uint64_t>(kN) + flood,
          "the LAST frame is the newest one published -- coalescing kept the "
          "current price, which is what a State channel is for");

    // ---- a disconnecting client is reaped --------------------------------
    client.close(ec);
    for (int i = 0; i < 50; ++i) {
        bus.publish(altair::kTopicTrades, tick(256265, 1, 1), nullptr,
                    nullptr, 1);
        bus.poll();
        if (bus.clients() == 0) { break; }
    }
    check(bus.clients() == 0, "a disconnected subscriber is reaped");
    check(bus.dropped_clients() >= 1, "and the departure is counted");

    std::printf("\n%s\n", failures == 0 ? "all checks passed"
                                        : "THERE WERE FAILURES");
    return failures == 0 ? 0 : 1;
}
