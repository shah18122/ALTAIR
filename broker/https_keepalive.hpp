// broker/https_keepalive.hpp -- one TLS connection to a FYERS host, opened once and kept open.
//
// broker/https_client.hpp opens a NEW connection for every call: it loads the
// trust store, resolves the host, connects (one round trip), does the TLS
// handshake (one more) and then sends. Measured from the Mumbai server that is
// ~1.5 ms + ~6.4 ms before the order's first byte leaves, on every order. This
// keeps the connection: the order is written into a socket that is already
// connected and already encrypted, so what is left is the request's own round
// trip to FYERS and FYERS' own time.
//
// SAFETY OF A KEPT CONNECTION. The failure that matters is an order written
// into a connection the far end has already closed: the write "succeeds" into
// the kernel and the read then fails, and nobody knows whether FYERS saw it.
// So, before every request:
//   * a connection idle for longer than `max_idle` is replaced (servers and
//     Cloudflare close idle keep-alives; this stays well inside their limits);
//   * the socket is polled with a zero timeout: on a quiet HTTP/1.1 connection
//     nothing may arrive between our requests, so ANY readable byte (a FIN, a
//     TLS close_notify, an error) means it is dying, and it is replaced.
// A replacement happens before a single byte of the request is written, so it
// is always safe. Once the request is written, a failure is reported as a
// transport failure exactly as https_client reports it -- never retried here:
// the router treats it as uncertain and asks the order book (oms/fyers_adapter.hpp).
//
// TLS VERIFICATION is the same as https_client.hpp: verify_peer, the system
// trust store, SNI and a host-name check. The TLS session is kept, so a
// reconnect resumes it (one round trip less) when the server allows.
//
// One thread. Not thread-safe: the router owns its connections.

#pragma once

#include <broker/https_client.hpp>
#include <live/latency.hpp>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>

#include <poll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

namespace altair {

class HttpsKeepAlive {
public:
    struct Stats {
        std::uint64_t requests = 0, connects = 0, resumed = 0, replaced_stale = 0, replaced_idle = 0, failures = 0;
        live::LatencyHistogram rtt;       ///< request written -> response read, on a warm connection
        live::LatencyHistogram connect;   ///< resolve + TCP connect + TLS handshake
    };

    /// `ca_file`: tests only (a self-signed server); empty is the system store.
    explicit HttpsKeepAlive(std::string host, std::chrono::seconds max_idle = std::chrono::seconds{50},
                            unsigned short port = 443, const std::string& ca_file = {})
        : host_(std::move(host)), port_(std::to_string(port)), max_idle_(max_idle), ctx_(boost::asio::ssl::context::tls_client) {
        ctx_.set_verify_mode(boost::asio::ssl::verify_peer);
        if (ca_file.empty()) ctx_.set_default_verify_paths();
        else ctx_.load_verify_file(ca_file);
        ctx_ok_ = detail::load_platform_roots(ctx_.native_handle()) != 0;
    }
    ~HttpsKeepAlive() {
        close();
        if (session_ != nullptr) SSL_SESSION_free(session_);
    }
    HttpsKeepAlive(const HttpsKeepAlive&) = delete;
    HttpsKeepAlive& operator=(const HttpsKeepAlive&) = delete;

    [[nodiscard]] bool connected() const noexcept { return stream_ != nullptr; }
    [[nodiscard]] const Stats& stats() const noexcept { return st_; }
    [[nodiscard]] std::chrono::steady_clock::duration idle() const noexcept {
        return std::chrono::steady_clock::now() - last_use_;
    }

    /// Connect now (at start, on arming), so the first order does not pay for it.
    /// Best followed by one cheap GET, which also takes in the TLS session tickets.
    bool warm(std::chrono::seconds timeout = std::chrono::seconds{10}) {
        return usable() || open(timeout);
    }

    /// One request on the kept connection. `method` GET, POST or DELETE; the
    /// headers are https_fyers_request's (version: 3, the authorization).
    [[nodiscard]] std::expected<HttpResponse, HttpError>
    request(std::string_view method, std::string_view target, std::string_view body, std::string_view authorization,
            std::chrono::seconds timeout = std::chrono::seconds{10}) {
        namespace beast = boost::beast;
        namespace http = beast::http;
        const http::verb verb = method == "GET" ? http::verb::get
                              : method == "POST" ? http::verb::post
                              : method == "DELETE" ? http::verb::delete_
                              : http::verb::unknown;
        if (verb == http::verb::unknown) return std::unexpected(HttpError::Unknown);
        // Replaced, if at all, BEFORE anything is written: always safe.
        if (!usable() && !open(timeout)) { ++st_.failures; return std::unexpected(last_error_); }
        try {
            http::request<http::string_body> req{verb, std::string{target}, 11};
            req.set(http::field::host, host_);
            req.set(http::field::user_agent, "altair/0.1");
            req.set(http::field::content_type, "application/json");
            req.set(http::field::connection, "keep-alive");
            req.set(http::field::authorization, std::string{authorization});
            req.set("version", "3");
            if (verb != http::verb::get) req.body() = std::string{body};
            req.prepare_payload();
            boost::system::error_code ec;
            const auto t0 = std::chrono::steady_clock::now();
            beast::get_lowest_layer(*stream_).expires_after(timeout);
            http::async_write(*stream_, req, [&ec](boost::system::error_code e, std::size_t) { ec = e; });
            run();
            if (ec) { close(); ++st_.failures; return std::unexpected(HttpError::TransportFailed); }
            http::response_parser<http::string_body> parser;
            parser.body_limit(8ull * 1024 * 1024);
            beast::get_lowest_layer(*stream_).expires_after(timeout);
            http::async_read(*stream_, buffer_, parser, [&ec](boost::system::error_code e, std::size_t) { ec = e; });
            run();
            if (ec) { close(); ++st_.failures; return std::unexpected(HttpError::TransportFailed); }
            const auto t1 = std::chrono::steady_clock::now();
            st_.rtt.record(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
            ++st_.requests;
            ++on_conn_;
            last_use_ = t1;
            auto res = parser.release();
            HttpResponse out{};
            out.status = res.result_int();
            out.body = std::move(res.body());
            keep_session();
            if (!res.keep_alive()) close();   // the server said Connection: close
            return out;
        } catch (const std::exception&) {
            close();
            ++st_.failures;
            return std::unexpected(HttpError::Unknown);
        }
    }

    /// Drop the connection (a new login, shutdown). The next request reconnects.
    void close() noexcept {
        if (!stream_) return;
        boost::system::error_code ec;
        beast_lowest().socket().close(ec);   // no close_notify round trip: nothing waits on it
        stream_.reset();
        buffer_.clear();
    }

    /// `{"requests":..,"connects":..,"resumed":..,"stale":..,"idle":..,"failures":..,"rtt_us":{..},"connect_us":{..}}`.
    [[nodiscard]] std::string json() const {
        return "{\"host\": \"" + host_ + "\", \"requests\": " + std::to_string(st_.requests) + ", \"connects\": "
             + std::to_string(st_.connects) + ", \"resumed\": " + std::to_string(st_.resumed) + ", \"replaced_stale\": "
             + std::to_string(st_.replaced_stale) + ", \"replaced_idle\": " + std::to_string(st_.replaced_idle)
             + ", \"failures\": " + std::to_string(st_.failures) + ", \"rtt_us\": " + st_.rtt.json(1e3)
             + ", \"connect_us\": " + st_.connect.json(1e3) + "}";
    }

private:
    using Stream = boost::beast::ssl_stream<boost::beast::tcp_stream>;

    boost::beast::tcp_stream& beast_lowest() { return boost::beast::get_lowest_layer(*stream_); }

    /// Runs the queued operation to its end. Asynchronous, because Beast's
    /// timeouts (expires_after) bound asynchronous operations only: a blocking
    /// read on a FYERS that never answers would hang the router. The loop
    /// polls rather than sleeping in epoll: the router's thread is on its own
    /// core, and the reply is picked up without a scheduler wake-up.
    /// TCP_QUICKACK is set again on every pass (Linux clears it after a
    /// read): a server that writes its reply in pieces under Nagle waits for
    /// our ACK, and a delayed ACK there cost 40 ms a reply in the test.
    void run() {
        ioc_.restart();
        const int fd = stream_ ? beast_lowest().socket().native_handle() : -1;
        while (ioc_.poll() > 0 || !ioc_.stopped()) {
            if (fd >= 0) { const int one = 1; (void)::setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &one, sizeof one); }
        }
    }

    /// Connected, not idle past the limit, and nothing waiting to be read.
    bool usable() {
        if (!stream_) return false;
        if (idle() > max_idle_) { ++st_.replaced_idle; close(); return false; }
        const int fd = beast_lowest().socket().native_handle();
        pollfd p{};
        p.fd = fd;
        p.events = POLLIN;
        const int r = ::poll(&p, 1, 0);
        bool stale = r < 0 || buffer_.size() != 0 || (p.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0;
        if (!stale && r > 0) {
            if (on_conn_ == 0) {
                // Before the first reply, TLS 1.3 session tickets may be waiting
                // (OpenSSL reads them with the first reply): only a FIN is stale.
                char c = 0;
                stale = ::recv(fd, &c, 1, MSG_PEEK | MSG_DONTWAIT) == 0;
            } else {
                stale = true;   // after a reply nothing may arrive unasked
            }
        }
        if (stale) { ++st_.replaced_stale; close(); return false; }
        return true;
    }

    bool open(std::chrono::seconds timeout) {
        namespace beast = boost::beast;
        namespace net = boost::asio;
        close();
        if (!ctx_ok_) { last_error_ = HttpError::TlsFailed; return false; }
        const auto t0 = std::chrono::steady_clock::now();
        try {
            auto s = std::make_unique<Stream>(ioc_, ctx_);
            if (!SSL_set_tlsext_host_name(s->native_handle(), host_.c_str())) { last_error_ = HttpError::TlsFailed; return false; }
            s->set_verify_callback(net::ssl::host_name_verification(host_));
            if (session_ != nullptr) SSL_set_session(s->native_handle(), session_);
            boost::system::error_code ec;
            net::ip::tcp::resolver resolver{ioc_};
            const auto results = resolver.resolve(host_, port_, ec);
            if (ec) { last_error_ = HttpError::ResolveFailed; return false; }
            beast::get_lowest_layer(*s).expires_after(timeout);
            beast::get_lowest_layer(*s).async_connect(results, [&ec](boost::system::error_code e, const auto&) { ec = e; });
            run();
            if (ec) { last_error_ = HttpError::ConnectFailed; return false; }
            beast::get_lowest_layer(*s).socket().set_option(net::ip::tcp::no_delay(true), ec);
            ec = {};
            beast::get_lowest_layer(*s).expires_after(timeout);
            s->async_handshake(net::ssl::stream_base::client, [&ec](boost::system::error_code e) { ec = e; });
            run();
            if (ec) {
                // A stale resumed session is the likeliest cause of a refusal: forget it.
                if (session_ != nullptr) { SSL_SESSION_free(session_); session_ = nullptr; }
                last_error_ = HttpError::TlsFailed;
                return false;
            }
            if (SSL_session_reused(s->native_handle()) == 1) ++st_.resumed;
            stream_ = std::move(s);
            on_conn_ = 0;
            ++st_.connects;
            const auto t1 = std::chrono::steady_clock::now();
            st_.connect.record(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
            last_use_ = t1;
            return true;
        } catch (const std::exception&) {
            last_error_ = HttpError::Unknown;
            return false;
        }
    }

    /// TLS 1.3 sends its session tickets after the handshake: take one after a reply.
    void keep_session() {
        if (!stream_) return;
        SSL_SESSION* s = SSL_get1_session(stream_->native_handle());
        if (s == nullptr) return;
        if (SSL_SESSION_is_resumable(s) != 1) { SSL_SESSION_free(s); return; }
        if (session_ != nullptr) SSL_SESSION_free(session_);
        session_ = s;
    }

    std::string host_, port_;
    std::chrono::seconds max_idle_;
    boost::asio::io_context ioc_;
    boost::asio::ssl::context ctx_;
    bool ctx_ok_ = false;
    std::unique_ptr<Stream> stream_;
    boost::beast::flat_buffer buffer_;
    SSL_SESSION* session_ = nullptr;
    HttpError last_error_ = HttpError::Unknown;
    std::chrono::steady_clock::time_point last_use_{};
    std::uint64_t on_conn_ = 0;   ///< replies read on this connection
    Stats st_;
};

} // namespace altair
