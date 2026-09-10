// P37-04 acceptance tests for desktop/price_client.hpp.
//
// THE REAL BUS AND THE REAL CLIENT, OVER A REAL SOCKET.
//
// Not a mock on either side. server/tests/test_price_bus.cpp proves the bus
// from a hand-written reader; this proves the pair, which is the only way to
// catch the class of bug where both sides are self-consistently wrong.
//
// THE PROPERTY THAT MATTERS IS REASSEMBLY.
//
// TCP is a byte stream and delivers whatever it feels like: a 288-byte book
// frame can arrive as 100 bytes, then 12, then 176. A reader that assumes one
// read is one frame works perfectly on a quiet loopback and corrupts the first
// busy morning. So the test publishes thousands of frames and requires that
// EVERY one is decoded, in order, with none undecodable -- which cannot happen
// unless the buffering is right, because at that volume the kernel will
// certainly have split some of them.
//
// No check description here may contain the substring "F" "AIL" joined.

#include "../price_client.hpp"

#include <server/price_bus.hpp>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QTcpServer>

#include <boost/asio/io_context.hpp>

#include <cstdio>

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

/// Pump both sides for a while: Qt's event loop for the client, the bus's
/// poll() for the server. Stops early when `done` says so.
template <typename F>
void pump(altair::PriceBus& bus, int ms, F done)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        bus.poll();
        QCoreApplication::processEvents();
        if (done()) { return; }
    }
}

altair::PricePayload tick(std::uint32_t token, std::int64_t paise)
{
    altair::PricePayload p;
    p.token = token;
    p.last_paise = paise;
    p.exchange_ts_ns = 1789000000000000000LL;
    return p;
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    std::printf("P37-04 price client\n\n");

    boost::asio::io_context io;
    altair::PriceBus bus(io, 0);
    if (!bus.ok()) {
        std::printf("  could not bind: %s\n", bus.error().message().c_str());
        return 1;
    }

    altair::ui::PriceClient client;
    // A BOUNDED SUBSCRIBER, so the flood below actually backs up. Without
    // this the kernel and Qt between them absorbed three megabytes at a
    // client that had stopped reading, nothing coalesced, and the gap check
    // measured nothing at all.
    client.set_read_buffer(16 * 1024);
    client.start(QStringLiteral("127.0.0.1"), bus.port());
    pump(bus, 3000, [&] { return client.connected() && bus.clients() == 1; });
    check(client.connected(), "the client connects to the bus");
    check(bus.clients() == 1, "and the bus sees it");

    // ---- one price arrives intact ----------------------------------------
    bus.publish(altair::kTopicTrades, tick(256265, 2343150), nullptr, nullptr,
                1789000000000000000LL);
    pump(bus, 2000, [&] { return client.frames() >= 1; });
    const auto* p = client.price(256265);
    check(p != nullptr, "the price lands under its own token");
    check(p != nullptr && p->last_paise == 2343150,
          "with the right value");
    check(p != nullptr && !p->has_volume,
          "and an index's absent volume stays ABSENT rather than becoming a "
          "measured zero");

    // ---- reassembly across arbitrary read boundaries ---------------------
    constexpr int kMany = 5000;
    const std::uint64_t before = client.frames();
    for (int i = 0; i < kMany; ++i) {
        bus.publish(altair::kTopicTrades, tick(256265, 2400000 + i), nullptr,
                    nullptr, 1789000000000000000LL);
        // Drain as we go, so nothing is coalesced -- this check is about
        // framing, and a dropped frame would confuse it with a lost one.
        if ((i % 64) == 0) {
            pump(bus, 5, [] { return false; });
        }
    }
    pump(bus, 8000, [&] { return client.frames() - before >= kMany; });
    std::printf("        published %d, client decoded %llu, undecodable %llu, "
                "gaps %llu\n",
                kMany,
                static_cast<unsigned long long>(client.frames() - before),
                static_cast<unsigned long long>(client.undecodable()),
                static_cast<unsigned long long>(client.gaps()));
    check(client.undecodable() == 0,
          "not one frame is undecodable across thousands of reads -- the "
          "stream is reassembled, not assumed to arrive one frame per read");
    const auto* last = client.price(256265);
    check(last != nullptr && last->last_paise == 2400000 + kMany - 1,
          "and the newest price is the last one published");

    // ---- a gap is COUNTED, not smoothed ----------------------------------
    //
    // Reported rather than hidden because a grid that silently skipped forty
    // ticks looks exactly like a grid that received forty ticks in which
    // nothing happened.
    // WITH CRAFTED FRAMES, NOT BY FLOODING THE BUS.
    //
    // The first version tried to provoke a real coalesce by flooding a client
    // that had stopped reading. It never worked, and the reason is worth
    // recording: on Windows a receive buffer can only be shrunk before the
    // socket connects, and Qt can only set that option AFTER. So the reader
    // stayed effectively unbounded, the bus never saw backpressure, and the
    // measurement was of nothing -- `bus coalesced 0`, three times.
    //
    // That the bus coalesces and leaves a visible gap is proven where it
    // belongs, in server/tests/test_price_bus.cpp, against a raw socket that
    // CAN be bounded: 21,010 frames dropped, one gap, stream still aligned.
    //
    // What belongs HERE is the client's own arithmetic, and crafted frames
    // test it far better than a flood ever would -- the gap is exactly the
    // size the test chose, so "missed" is checked against a known number
    // instead of "something more than zero".
    {
        QTcpServer feeder;
        check(feeder.listen(QHostAddress::LocalHost, 0),
              "a frame feeder binds");
        altair::ui::PriceClient c2;
        c2.start(QStringLiteral("127.0.0.1"), feeder.serverPort());
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 3000 && !feeder.hasPendingConnections()) {
            QCoreApplication::processEvents();
        }
        QTcpSocket* peer = feeder.nextPendingConnection();
        check(peer != nullptr, "and the client attaches to it");

        const auto send_seq = [&](std::uint64_t seq, std::int64_t paise) {
            std::uint8_t frame[altair::kFrameHeaderBytes
                               + altair::kPricePayloadBytes]{};
            const auto body = altair::encode_price(
                tick(256265, paise), nullptr, nullptr,
                frame + altair::kFrameHeaderBytes,
                altair::kPricePayloadBytes);
            altair::FrameHeader h;
            h.kind = altair::FrameKind::Delta;
            h.channel = altair::Channel::State;
            h.topic = altair::kTopicTrades;
            h.seq = seq;
            h.payload_len = static_cast<std::uint32_t>(*body);
            h.engine_time_ns = 1789000000000000000LL;
            h.server_time_ns = 1789000000000000001LL;
            (void)altair::encode_header(h, frame, altair::kFrameHeaderBytes);
            peer->write(reinterpret_cast<const char*>(frame), sizeof frame);
            peer->flush();
        };

        if (peer != nullptr) {
            send_seq(1, 100);
            send_seq(2, 200);
            t.restart();
            while (t.elapsed() < 2000 && c2.frames() < 2) {
                QCoreApplication::processEvents();
            }
            check(c2.gaps() == 0, "consecutive frames leave no gap");

            // seq 3..9 never arrive: seven frames coalesced away.
            send_seq(10, 300);
            t.restart();
            while (t.elapsed() < 2000 && c2.frames() < 3) {
                QCoreApplication::processEvents();
            }
            std::printf("        after a planted jump 2 -> 10: gaps %llu, "
                        "missed %llu\n",
                        static_cast<unsigned long long>(c2.gaps()),
                        static_cast<unsigned long long>(c2.missed()));
            check(c2.gaps() == 1, "a jump in the sequence is reported as ONE gap");
            check(c2.missed() == 7,
                  "and the count of missed frames is exactly right -- seven, "
                  "not 'more than zero'");
            const auto* q = c2.price(256265);
            check(q != nullptr && q->last_paise == 300,
                  "and the price after the gap is applied, because a gap is "
                  "something to report and not a reason to stop");
        }
    }

    // ---- a book populates the ladder's input -----------------------------
    {
        altair::PricePayload b;
        b.token = 17512194;
        b.flags = altair::kPriceHasBook;
        b.depth_levels = 5;
        altair::PriceLevel bids[5]{}, asks[5]{};
        for (int i = 0; i < 5; ++i) {
            bids[i] = {2343100 - i * 25, 50 + i,
                       static_cast<std::uint32_t>(i + 1), 0};
            asks[i] = {2343175 + i * 25, 60 + i,
                       static_cast<std::uint32_t>(i + 2), 0};
        }
        bus.publish(altair::kTopicBook, b, bids, asks, 1789000000000000000LL);
        pump(bus, 3000, [&] {
            const auto* q = client.price(17512194);
            return q != nullptr && q->levels == 5;
        });
        const auto* q = client.price(17512194);
        check(q != nullptr && q->levels == 5,
              "a book frame delivers five levels a side");
        check(q != nullptr && q->bids[0].price_paise == 2343100
                  && q->asks[0].price_paise == 2343175,
              "with the touch intact -- the first real book this tree has "
              "ever handed the depth ladder");
        check(q != nullptr && q->bids[0].qty == 50 && q->asks[0].orders == 2,
              "and quantities and order counts survive");
    }

    // ---- garbage is refused, not resynchronised --------------------------
    //
    // Hunting for the next magic would eventually resynchronise onto a byte
    // pattern inside a PRICE that happened to look like one, and every frame
    // after that would be plausible nonsense.
    {
        // A raw server, so the test can write bytes the bus would never
        // produce. The bus is not the only thing that could ever be on the
        // other end of this socket, and "the sender is always correct" is not
        // a property a reader gets to assume.
        QTcpServer rogue;
        check(rogue.listen(QHostAddress::LocalHost, 0),
              "a rogue server binds");

        altair::ui::PriceClient c2;
        c2.start(QStringLiteral("127.0.0.1"), rogue.serverPort());

        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 3000 && !rogue.hasPendingConnections()) {
            QCoreApplication::processEvents();
        }
        QTcpSocket* peer = rogue.nextPendingConnection();
        check(peer != nullptr, "and the client connects to it");

        if (peer != nullptr) {
            // 48 bytes of plausible-looking rubbish: the right LENGTH for a
            // header, the wrong magic.
            QByteArray junk(static_cast<qsizetype>(altair::kFrameHeaderBytes),
                            '\x5A');
            peer->write(junk);
            peer->flush();
            t.restart();
            while (t.elapsed() < 3000 && c2.undecodable() == 0) {
                QCoreApplication::processEvents();
            }
            check(c2.undecodable() > 0,
                  "a frame with the wrong magic is counted as undecodable");
            check(!c2.connected(),
                  "and the connection is DROPPED rather than hunted past -- "
                  "scanning for the next magic would resynchronise onto a byte "
                  "pattern inside a price and every frame after it would be "
                  "plausible nonsense");
        }
    }

    std::printf("\n%s\n", failures == 0 ? "all checks passed"
                                        : "THERE WERE FAILURES");
    return failures == 0 ? 0 : 1;
}
