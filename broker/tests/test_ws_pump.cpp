// broker/ws_pump.hpp against a local WebSocket server (plain TCP, loopback):
// the four ways a feed goes quiet or ends, and that each one returns
// promptly instead of waiting in a read for a frame that never comes.
//   * a server that sends three frames and then nothing at all  -> Idle
//   * a server that sends only heartbeats                         -> Deadline
//     (the old loop checked its deadline only after a tick frame)
//   * a silent server and a stop request from another thread      -> Stopped
//   * a server that closes                                        -> Closed
//
// No check description here may contain the substring "F" "AIL" joined.

#include <broker/ws_pump.hpp>

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) { ++failures; }
}

namespace net = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = net::ip::tcp;
using clock_type = std::chrono::steady_clock;

enum class Mode { ThreeThenSilent, Heartbeats, Silent, CloseAfterTwo };

/// One connection served on its own thread, then the thread ends.
struct Server {
    net::io_context io;
    tcp::acceptor acc{io, tcp::endpoint(net::ip::make_address("127.0.0.1"), 0)};
    std::thread th;
    std::atomic<bool> quit{false};

    explicit Server(Mode m) {
        th = std::thread([this, m] {
            boost::system::error_code ec;
            tcp::socket s(io);
            acc.accept(s, ec);
            if (ec) return;
            websocket::stream<tcp::socket> ws(std::move(s));
            ws.accept(ec);
            if (ec) return;
            ws.binary(true);
            const unsigned char tick[8] = {0, 1, 0, 4, 1, 2, 3, 4};
            const unsigned char beat[1] = {0};
            beast::flat_buffer b;
            switch (m) {
            case Mode::ThreeThenSilent:
                for (int k = 0; k < 3; ++k) ws.write(net::buffer(tick), ec);
                ws.read(b, ec);   // answers the client's close, or ends when it drops
                break;
            case Mode::Heartbeats:
                while (!quit.load() && !ec) {
                    ws.write(net::buffer(beat), ec);
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                break;
            case Mode::Silent:
                ws.read(b, ec);
                break;
            case Mode::CloseAfterTwo:
                for (int k = 0; k < 2; ++k) ws.write(net::buffer(tick), ec);
                ws.close(websocket::close_code::normal, ec);
                break;
            }
        });
    }
    ~Server() {
        quit.store(true);
        boost::system::error_code ec;
        acc.close(ec);
        if (th.joinable()) th.join();
    }
    unsigned short port() { return acc.local_endpoint().port(); }
};

struct Result {
    altair::ws_pump::End end{};
    int frames = 0, beats = 0;
    double seconds = 0.0;
};

template <class Stop>
Result pump(unsigned short port, std::chrono::milliseconds run_for, std::chrono::milliseconds idle, Stop&& stop) {
    Result r;
    net::io_context ioc;
    websocket::stream<beast::tcp_stream> ws(ioc);
    boost::system::error_code ec;
    beast::get_lowest_layer(ws).connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), port), ec);
    if (ec) return r;
    ws.handshake("127.0.0.1", "/", ec);
    if (ec) return r;
    const auto t0 = clock_type::now();
    r.end = altair::ws_pump::run(ioc, ws, t0 + run_for, idle, stop, [&](const unsigned char*, std::size_t n, bool binary) {
        if (binary && n <= 1) ++r.beats; else ++r.frames;
    });
    r.seconds = std::chrono::duration<double>(clock_type::now() - t0).count();
    return r;
}

}  // namespace

int main() {
    using altair::ws_pump::End;
    std::printf("websocket pump\n");
    const auto never = [] { return false; };
    {
        Server s(Mode::ThreeThenSilent);
        const auto r = pump(s.port(), std::chrono::seconds(30), std::chrono::milliseconds(300), never);
        check(r.end == End::Idle && r.frames == 3, "three frames, then silence: ends Idle (the connection is dead)");
        check(r.seconds < 2.0, "and promptly, not after the 30-second run");
    }
    {
        Server s(Mode::Heartbeats);
        const auto r = pump(s.port(), std::chrono::milliseconds(400), std::chrono::seconds(5), never);
        check(r.end == End::Deadline && r.beats > 0 && r.frames == 0, "heartbeats only: the deadline still ends it");
        check(r.seconds < 2.0, "within the deadline plus the close grace");
    }
    {
        Server s(Mode::Silent);
        std::atomic<bool> stop{false};
        std::thread t([&] { std::this_thread::sleep_for(std::chrono::milliseconds(200)); stop.store(true); });
        const auto r = pump(s.port(), std::chrono::seconds(30), std::chrono::seconds(30), [&] { return stop.load(); });
        t.join();
        check(r.end == End::Stopped && r.seconds < 2.0, "a silent feed: a stop request from another thread is seen at once");
    }
    {
        Server s(Mode::CloseAfterTwo);
        const auto r = pump(s.port(), std::chrono::seconds(30), std::chrono::seconds(30), never);
        check(r.end == End::Closed && r.frames == 2, "the server closes: ends Closed, both frames delivered");
    }
    std::printf("%s\n", failures == 0 ? "all websocket pump checks passed" : "websocket pump checks did not pass");
    return failures == 0 ? 0 : 1;
}
