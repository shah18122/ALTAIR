// broker/ws_pump.hpp -- read a WebSocket until told to stop, without ever
// blocking past the moment it is told.
//
// A synchronous ws.read() blocks until a frame arrives. The ticker used to
// check its stop flag and its deadline only BETWEEN frames -- and the
// deadline only after a tick frame -- so a feed sending nothing but
// heartbeats, or a TCP connection that died without a FIN, kept the thread in
// read() indefinitely: Ctrl-C, the session's end time and a watchlist change
// all waited for a frame that might never come.
//
// This runs the read asynchronously and the io_context in short slices, so
// between slices it can see:
//   * the caller's stop request            -> Stopped
//   * the deadline                         -> Deadline
//   * `idle_limit` with no frame at all,
//     not even a heartbeat                 -> Idle (the connection is dead)
//   * the server closing                   -> Closed
//   * a transport error                    -> Error
// and on the first three it closes the socket politely (a close frame, with
// a short grace) and then forcibly, so it returns within about a second.
//
// Templated on the stream so the test can drive it over plain TCP against a
// local server; the ticker uses it over TLS.

#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include <chrono>
#include <cstddef>

namespace altair::ws_pump {

enum class End : unsigned char { Stopped, Deadline, Idle, Closed, Error };

[[nodiscard]] inline const char* end_text(End e) noexcept {
    switch (e) {
    case End::Stopped: return "stopped";
    case End::Deadline: return "deadline";
    case End::Idle: return "idle";
    case End::Closed: return "closed by the server";
    case End::Error: return "transport error";
    }
    return "?";
}

/// `on_message(const unsigned char* data, std::size_t len, bool binary)` for
/// every frame. `ws` must already be connected and upgraded, on `ioc`.
template <class Ws, class ShouldStop, class OnMessage>
End run(boost::asio::io_context& ioc, Ws& ws, std::chrono::steady_clock::time_point deadline,
        std::chrono::milliseconds idle_limit, ShouldStop&& should_stop, OnMessage&& on_message,
        boost::system::error_code* last_error = nullptr) {
    namespace beast = boost::beast;
    namespace websocket = beast::websocket;
    using clock = std::chrono::steady_clock;
    constexpr auto kSlice = std::chrono::milliseconds(50);

    beast::flat_buffer buf;
    bool reading = false, done = false;
    boost::system::error_code rec;
    auto last_rx = clock::now();
    End end = End::Stopped;
    for (;;) {
        if (!reading) {
            buf.clear();
            done = false;
            reading = true;
            ws.async_read(buf, [&done, &rec](boost::system::error_code e, std::size_t) { rec = e; done = true; });
        }
        ioc.restart();
        ioc.run_for(kSlice);
        if (done) {
            reading = false;
            if (rec == websocket::error::closed) return End::Closed;
            if (rec) { if (last_error) *last_error = rec; return End::Error; }
            last_rx = clock::now();
            on_message(static_cast<const unsigned char*>(buf.data().data()), buf.size(), ws.got_binary());
        }
        // Checked after EVERY slice and every frame, heartbeats included.
        const auto now = clock::now();
        if (should_stop()) { end = End::Stopped; break; }
        if (now >= deadline) { end = End::Deadline; break; }
        if (idle_limit.count() > 0 && now - last_rx > idle_limit) { end = End::Idle; break; }
    }
    // A read is outstanding. Close politely -- Beast allows a close alongside
    // a pending read, which then completes -- and give it a moment.
    bool closed = false;
    if (end != End::Idle) {
        ws.async_close(websocket::close_code::normal, [&closed](boost::system::error_code) { closed = true; });
        const auto grace = clock::now() + std::chrono::seconds(1);
        while ((!closed || !done) && clock::now() < grace) {
            ioc.restart();
            ioc.run_for(kSlice);
        }
    }
    if (!closed || !done) {
        // A dead peer will not answer: tear the socket down and let the
        // pending operations finish with operation_aborted.
        boost::system::error_code ec;
        beast::get_lowest_layer(ws).socket().close(ec);
        ioc.restart();
        ioc.run_for(std::chrono::milliseconds(200));
    }
    return end;
}

} // namespace altair::ws_pump
