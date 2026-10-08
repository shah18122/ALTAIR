// broker/fyers_data_socket.hpp -- the TLS WebSocket that carries the FYERS
// data feed (wss://socket.fyers.in/hsm/v1-5/prod).
//
// Transport only. It knows nothing about the HSM binary protocol: the caller
// supplies the opening frames and the keep-alive bytes, and decides what to
// send back after each frame (feed/fyers_hsm.hpp builds all of them). That
// keeps the protocol testable without a socket and this file free of it.
//
// Unlike Kite's ticker, FYERS needs traffic from the client while the feed is
// running (an application ping every ten seconds and periodic
// acknowledgements), so this is a small single-threaded asio loop: one read
// always pending, one serialized write queue, one 1-second timer.
//
// THIS FUNCTION BLOCKS for up to `run_for`. It is a helper process's main
// loop. desktop/ cannot call it: the UI may not link broker/.
#pragma once

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <windows.h>
#  include <wincrypt.h>
#  undef X509_NAME
#  undef X509_EXTENSIONS
#  undef PKCS7_ISSUER_AND_SERIAL
#  undef PKCS7_SIGNER_INFO
#  undef OCSP_REQUEST
#  undef OCSP_RESPONSE
#endif

#include <broker/https_client.hpp>   // detail::load_platform_roots

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace altair {

inline constexpr const char* kFyersDataSocketHost = "socket.fyers.in";
inline constexpr const char* kFyersDataSocketPath = "/hsm/v1-5/prod";

enum class FyersSocketError : unsigned char {
    NoOpeningFrames,
    TlsFailed,
    ResolveFailed,
    ConnectFailed,
    UpgradeFailed,
    TransportFailed,
    /// Connected but not one binary frame arrived within the idle limit.
    Idle,
    Unknown,
};

[[nodiscard]] inline const char* fyers_socket_error_text(FyersSocketError e) noexcept {
    switch (e) {
    case FyersSocketError::NoOpeningFrames: return "nothing to send";
    case FyersSocketError::TlsFailed:       return "TLS handshake failed";
    case FyersSocketError::ResolveFailed:   return "DNS resolve failed";
    case FyersSocketError::ConnectFailed:   return "TCP connect failed";
    case FyersSocketError::UpgradeFailed:   return "WebSocket upgrade refused";
    case FyersSocketError::TransportFailed: return "connection dropped";
    case FyersSocketError::Idle:            return "connected, but FYERS sent nothing";
    case FyersSocketError::Unknown:         return "unknown failure";
    }
    return "?";
}

struct FyersSocketStats {
    std::size_t binary_frames = 0;
    std::size_t text_frames = 0;
    std::size_t bytes = 0;
    std::size_t frames_sent = 0;
    std::size_t pings = 0;
    bool closed_by_server = false;  ///< the server ended the session
    bool interrupted = false;       ///< Ctrl+C / SIGTERM
};

using FyersFrame = std::vector<std::uint8_t>;

/// Where and how to connect. The defaults are the HSM feed; the 50-level
/// book (feed/fyers_tbt.hpp) is another host, an `authorization` header on
/// the upgrade, and JSON text frames out.
struct FyersSocketOptions {
    std::string host = kFyersDataSocketHost;
    std::string path = kFyersDataSocketPath;
    std::string authorization;             ///< "appid:token"; never logged
    bool text = false;                     ///< frames sent are text, not binary
    std::function<bool()> stop;            ///< checked every second: true ends the session
};

/// Connect, send `opening`, then pump. For every binary message
/// `on_frame(data, size, replies)` runs on this thread; frames it appends to
/// `replies` are sent in order, and returning false ends the session cleanly.
/// `ping` is sent every `ping_every` while connected.
[[nodiscard]] inline std::expected<FyersSocketStats, FyersSocketError>
fyers_data_socket_run(
    const std::vector<FyersFrame>& opening,
    const std::function<bool(const std::uint8_t*, std::size_t, std::vector<FyersFrame>&)>& on_frame,
    const FyersFrame& ping,
    std::chrono::seconds ping_every,
    std::chrono::seconds run_for,
    std::chrono::seconds idle_limit = std::chrono::seconds{30},
    const FyersSocketOptions& options = {})
{
    namespace beast = boost::beast;
    namespace websocket = beast::websocket;
    namespace net = boost::asio;
    namespace ssl = net::ssl;
    using tcp = net::ip::tcp;
    using clock = std::chrono::steady_clock;

    if (opening.empty()) return std::unexpected(FyersSocketError::NoOpeningFrames);

    try {
        net::io_context ioc;
        ssl::context ctx{ssl::context::tls_client};
        // Same policy as https_client and the Kite ticker: verification is
        // on, and the socket carries a live trading credential.
        ctx.set_verify_mode(ssl::verify_peer);
        ctx.set_default_verify_paths();
        if (detail::load_platform_roots(ctx.native_handle()) == 0)
            return std::unexpected(FyersSocketError::TlsFailed);

        tcp::resolver resolver{ioc};
        websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws{ioc, ctx};
        const std::string host{options.host};
        if (!SSL_set_tlsext_host_name(ws.next_layer().native_handle(), host.c_str()))
            return std::unexpected(FyersSocketError::TlsFailed);
        ws.next_layer().set_verify_callback(ssl::host_name_verification(host));

        boost::system::error_code ec;
        const auto results = resolver.resolve(host, "443", ec);
        if (ec) return std::unexpected(FyersSocketError::ResolveFailed);
        beast::get_lowest_layer(ws).expires_after(std::chrono::seconds{20});
        beast::get_lowest_layer(ws).connect(results, ec);
        if (ec) return std::unexpected(FyersSocketError::ConnectFailed);
        beast::get_lowest_layer(ws).expires_after(std::chrono::seconds{20});
        ws.next_layer().handshake(ssl::stream_base::client, ec);
        if (ec) return std::unexpected(FyersSocketError::TlsFailed);
        beast::get_lowest_layer(ws).expires_never();
        ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
        ws.binary(!options.text);
        if (!options.authorization.empty()) {
            const std::string auth = options.authorization;
            ws.set_option(websocket::stream_base::decorator([auth](websocket::request_type& req) {
                req.set(beast::http::field::authorization, auth);
            }));
        }
        ws.handshake(host, options.path, ec);
        if (ec) return std::unexpected(FyersSocketError::UpgradeFailed);

        FyersSocketStats st{};
        std::optional<FyersSocketError> failure;
        std::deque<FyersFrame> outq;
        bool writing = false;
        bool closing = false;
        bool finished = false;
        beast::flat_buffer buf;
        net::steady_timer timer{ioc};
        net::steady_timer guard{ioc};
        net::signal_set signals{ioc, SIGINT, SIGTERM};
        const auto start = clock::now();
        const auto deadline = start + run_for;
        auto next_ping = start + ping_every;

        std::function<void()> do_close, request_close, write_next, read_next, tick;

        const auto finish = [&] {
            finished = true;
            timer.cancel();
            guard.cancel();
            boost::system::error_code ignore;
            signals.cancel(ignore);
        };
        do_close = [&] {
            ws.async_close(websocket::close_code::normal, [](boost::system::error_code) {});
            // A server that never answers the close must not hold us forever.
            guard.expires_after(std::chrono::seconds{5});
            guard.async_wait([&](boost::system::error_code e) {
                if (e || finished) return;
                boost::system::error_code ignore;
                beast::get_lowest_layer(ws).socket().close(ignore);
            });
        };
        request_close = [&] {
            if (closing) return;
            closing = true;
            timer.cancel();
            if (!writing) do_close();   // otherwise the write completion closes
        };
        write_next = [&] {
            if (outq.empty()) { writing = false; if (closing) do_close(); return; }
            if (closing) { outq.clear(); writing = false; do_close(); return; }
            writing = true;
            ws.async_write(net::buffer(outq.front()),
                [&](boost::system::error_code e, std::size_t) {
                    if (e) {
                        writing = false;
                        if (!closing) failure = FyersSocketError::TransportFailed;
                        outq.clear();
                        request_close();
                        return;
                    }
                    ++st.frames_sent;
                    outq.pop_front();
                    write_next();
                });
        };
        const auto send = [&](FyersFrame f) {
            if (closing) return;
            outq.push_back(std::move(f));
            if (!writing) write_next();
        };
        read_next = [&] {
            ws.async_read(buf, [&](boost::system::error_code e, std::size_t n) {
                if (e) {
                    if (!closing) {
                        if (e == websocket::error::closed) st.closed_by_server = true;
                        else failure = FyersSocketError::TransportFailed;
                    }
                    closing = true;
                    finish();
                    return;
                }
                st.bytes += n;
                if (!ws.got_binary()) {
                    ++st.text_frames;
                } else if (!closing) {
                    ++st.binary_frames;
                    std::vector<FyersFrame> replies;
                    const bool keep = on_frame(
                        static_cast<const std::uint8_t*>(buf.data().data()), buf.size(), replies);
                    for (auto& r : replies) send(std::move(r));
                    if (!keep) request_close();
                }
                buf.consume(buf.size());
                read_next();
            });
        };
        tick = [&] {
            timer.expires_after(std::chrono::seconds{1});
            timer.async_wait([&](boost::system::error_code e) {
                if (e || closing) return;
                const auto now = clock::now();
                if (now >= deadline || (options.stop && options.stop())) { request_close(); return; }
                if (st.binary_frames == 0 && now - start >= idle_limit) {
                    failure = FyersSocketError::Idle;
                    request_close();
                    return;
                }
                if (now >= next_ping) {
                    send(ping);
                    ++st.pings;
                    next_ping = now + ping_every;
                }
                tick();
            });
        };

        signals.async_wait([&](boost::system::error_code e, int) {
            if (e) return;
            st.interrupted = true;
            request_close();
        });
        for (const auto& f : opening) send(f);
        read_next();
        tick();
        ioc.run();

        if (failure) return std::unexpected(*failure);
        return st;
    } catch (const boost::system::system_error&) {
        return std::unexpected(FyersSocketError::TransportFailed);
    } catch (...) {
        return std::unexpected(FyersSocketError::Unknown);
    }
}

} // namespace altair
