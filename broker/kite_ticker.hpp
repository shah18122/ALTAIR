// broker/kite_ticker.hpp -- the WebSocket tick feed. The transport half.
//
// P32-07. `feed/kite_decoder.hpp` has turned Kite's binary frames into Tick and
// DepthUpdate structs since P2-02, with every byte offset read out of
// Zerodha's own client and forty checks behind it. Nothing had ever handed it a
// frame off a socket. This is the other half, and it is the same split
// `broker/kite_quote.hpp` and `app/kite_quote_main.cpp` already have: the
// PARSER was written first deliberately, because fetching is a socket and a
// header while parsing is where the bugs are.
//
// WHY THIS IS A WEBSOCKET AND NOT A POLL.
//
// CLAUDE.md's physics section is unambiguous: order-book imbalance decays in
// 10-200 ms, the predecessor polled at 500 ms, and it could not even in
// principle see the thing it was trying to trade. That is a Nyquist limit, not
// a tuning problem, and no amount of polling fixes it. It is also the entire
// reason the engine is event-driven.
//
// `https_client.hpp` says so in its own header -- "the tick feed is a
// WebSocket and gets its own transport" -- and this is that transport. It
// deliberately does not grow out of the HTTP client: a form POST made a few
// times a day and a socket carrying every tick of the session have almost
// nothing in common except TLS.
//
// THE CREDENTIAL IS IN THE URL, WHICH IS UNUSUAL AND IS KITE'S DESIGN.
//
// Kite authenticates the ticker with `api_key` and `access_token` as QUERY
// PARAMETERS, not a header. That is their protocol and there is no alternative
// form. Two consequences this file takes seriously:
//
//   * The URL is never logged, never printed, and never put in an error
//     message. `redact_ws_url` exists for the one place a URL has to be shown.
//   * It is only ever sent over a verified TLS connection. An unverified
//     handshake here would put a live trading token on the wire in the clear,
//     so the verification is the same as https_client's and is equally not
//     optional: peer verification on, platform roots loaded explicitly
//     because Windows leaves OpenSSL's store empty, SNI set, and the
//     certificate checked against the host rather than merely being valid.
//
// THIS FILE DECODES NOTHING.
//
// Bytes in, bytes out. It hands a buffer and a receive timestamp to whatever
// the caller passes, and the caller passes it to feed/kite_decoder.hpp. That
// keeps rule 6 available: the same bytes go through the same decoder whether
// they arrived on this socket or out of a replay file, and if the two paths
// ever diverge the backtest is a lie.
//
// AND IT TAKES THE CLOCK AS A PARAMETER.
//
// `recv_ts` is supplied by the caller for the same reason the decoder takes
// one: a file that reads the wall clock cannot be replayed, and rule 10 wants
// every decision reproducible.

#pragma once

// The Windows header order, and every undef, is load bearing for exactly the
// reasons broker/https_client.hpp sets out at length: WinSock2 before
// windows.h, NOMINMAX before anything that includes core/types, and the
// wincrypt macros undefined before OpenSSL's types of the same names appear.
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
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace altair {

inline constexpr const char* kKiteTickerHost = "ws.kite.trade";

enum class TickerError : std::uint8_t {
    /// No api_key or no access_token was supplied.
    NoCredential,
    /// DNS lookup failed.
    ResolveFailed,
    /// TCP connect failed.
    ConnectFailed,
    /// TLS handshake failed, including a certificate that did not verify or
    /// was not for this host. NEVER downgraded and never retried unverified.
    TlsFailed,
    /// The WebSocket upgrade was refused. On this endpoint that is almost
    /// always a token that has expired -- they are daily.
    UpgradeFailed,
    /// Nothing to subscribe to.
    NoInstruments,
    /// The socket failed after it was established.
    TransportFailed,
    /// The read timed out with no frame at all.
    Idle,
    /// Anything the transport threw that does not fit above.
    Unknown
};

[[nodiscard]] inline const char* ticker_error_text(TickerError e) noexcept {
    switch (e) {
    case TickerError::NoCredential:    return "no api_key or access_token";
    case TickerError::ResolveFailed:   return "DNS lookup failed";
    case TickerError::ConnectFailed:   return "TCP connect failed";
    case TickerError::TlsFailed:       return "TLS handshake failed";
    case TickerError::UpgradeFailed:
        return "the WebSocket upgrade was refused -- the access token is "
               "daily and has very likely expired";
    case TickerError::NoInstruments:   return "nothing to subscribe to";
    case TickerError::TransportFailed: return "the socket failed";
    case TickerError::Idle:            return "no frame arrived";
    case TickerError::Unknown:         return "unknown transport failure";
    }
    return "unknown";
}

/// Which fields Kite sends per tick.
///
/// NOT AN OPTIMISATION KNOB. `Full` is the only mode that carries the five
/// levels of depth, and depth is the entire reason for a tick feed rather than
/// a quote poll -- see the Nyquist note above. `Ltp` exists because a large
/// subscription in Full mode is a lot of bandwidth for instruments nobody is
/// modelling the book of.
enum class TickerMode : std::uint8_t { Ltp, Quote, Full };

[[nodiscard]] inline const char* ticker_mode_name(TickerMode m) noexcept {
    switch (m) {
    case TickerMode::Ltp:   return "ltp";
    case TickerMode::Quote: return "quote";
    case TickerMode::Full:  return "full";
    }
    return "full";
}

/// The URL with the credential removed, for the one place a URL is shown.
///
/// KEEPS THE LENGTH of what it hides, the same way redact_request_token does,
/// so a truncated or doubled token is still visible as wrong without the value
/// ever being printed.
[[nodiscard]] inline std::string redact_ws_url(std::string_view url) {
    std::string out;
    out.reserve(url.size());
    std::size_t i = 0;
    while (i < url.size()) {
        // Find "api_key=" or "access_token=" and mask the value after it.
        const auto masked = [&](std::string_view key) -> bool {
            if (url.compare(i, key.size(), key) != 0) { return false; }
            out.append(key);
            i += key.size();
            std::size_t n = 0;
            while (i < url.size() && url[i] != '&') { ++i; ++n; }
            out.append(n, '*');
            return true;
        };
        if (masked("api_key=") || masked("access_token=")) { continue; }
        out.push_back(url[i]);
        ++i;
    }
    return out;
}

/// Build the ticker URL. Separated so it is testable without a socket.
[[nodiscard]] inline std::expected<std::string, TickerError>
kite_ticker_target(std::string_view api_key, std::string_view access_token) {
    if (api_key.empty() || access_token.empty()) {
        return std::unexpected(TickerError::NoCredential);
    }
    std::string t = "/?api_key=";
    t.append(api_key);
    t.append("&access_token=");
    t.append(access_token);
    return t;
}

/// The two JSON control messages. Text frames; the data comes back binary.
[[nodiscard]] inline std::string
kite_subscribe_message(const std::vector<std::uint32_t>& tokens) {
    std::string m = "{\"a\":\"subscribe\",\"v\":[";
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (i != 0) { m.push_back(','); }
        m.append(std::to_string(tokens[i]));
    }
    m.append("]}");
    return m;
}

[[nodiscard]] inline std::string
kite_mode_message(TickerMode mode, const std::vector<std::uint32_t>& tokens) {
    std::string m = "{\"a\":\"mode\",\"v\":[\"";
    m.append(ticker_mode_name(mode));
    m.append("\",[");
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (i != 0) { m.push_back(','); }
        m.append(std::to_string(tokens[i]));
    }
    m.append("]]}");
    return m;
}

/// What a session did, for the caller to report.
struct TickerStats {
    std::size_t binary_frames = 0;
    std::size_t text_frames = 0;      ///< order updates and errors, as JSON
    std::size_t bytes = 0;
    /// A one-byte binary frame is Kite's HEARTBEAT and is not a tick.
    ///
    /// Counted separately and deliberately: a feed delivering nothing but
    /// heartbeats is CONNECTED AND SILENT, which looks identical to a healthy
    /// feed in every counter that lumps the two together -- and is exactly
    /// what a subscription to a closed market looks like.
    std::size_t heartbeats = 0;
};

/// Connect, subscribe, and pump frames into `on_frame` until `should_stop`
/// says otherwise or the deadline passes.
///
/// `on_frame(const unsigned char* data, std::size_t len)` is called for every
/// BINARY frame that is not a heartbeat. It is not called for text frames:
/// those are order updates and error JSON, which belong to oms/ rather than to
/// the tick path, and quietly feeding them to a binary decoder would produce
/// garbage ticks rather than an error.
///
/// THIS FUNCTION BLOCKS. It is a tool's main loop, not something to call from
/// a UI thread -- and `desktop/` cannot call it in any case, because it may
/// not link broker/.
[[nodiscard]] inline std::expected<TickerStats, TickerError>
kite_ticker_run(std::string_view api_key, std::string_view access_token,
                const std::vector<std::uint32_t>& tokens, TickerMode mode,
                const std::function<void(const unsigned char*, std::size_t)>&
                    on_frame,
                const std::function<bool()>& should_stop,
                std::chrono::seconds read_timeout = std::chrono::seconds{60})
{
    namespace beast = boost::beast;
    namespace websocket = beast::websocket;
    namespace net = boost::asio;
    namespace ssl = net::ssl;
    using tcp = net::ip::tcp;

    if (tokens.empty()) { return std::unexpected(TickerError::NoInstruments); }
    const auto target = kite_ticker_target(api_key, access_token);
    if (!target) { return std::unexpected(target.error()); }

    try {
        net::io_context ioc;
        ssl::context ctx{ssl::context::tls_client};

        // Identical to https_client's, and for the identical reason: the
        // default is OFF, the failure is invisible, and this connection
        // carries a live trading token.
        ctx.set_verify_mode(ssl::verify_peer);
        ctx.set_default_verify_paths();
        if (detail::load_platform_roots(ctx.native_handle()) == 0) {
            return std::unexpected(TickerError::TlsFailed);
        }

        tcp::resolver resolver{ioc};
        websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws{ioc, ctx};

        const std::string host{kKiteTickerHost};
        if (!SSL_set_tlsext_host_name(ws.next_layer().native_handle(),
                                      host.c_str())) {
            return std::unexpected(TickerError::TlsFailed);
        }
        ws.next_layer().set_verify_callback(ssl::host_name_verification(host));

        boost::system::error_code ec;
        const auto results = resolver.resolve(host, "443", ec);
        if (ec) { return std::unexpected(TickerError::ResolveFailed); }

        beast::get_lowest_layer(ws).expires_after(std::chrono::seconds{20});
        beast::get_lowest_layer(ws).connect(results, ec);
        if (ec) { return std::unexpected(TickerError::ConnectFailed); }

        beast::get_lowest_layer(ws).expires_after(std::chrono::seconds{20});
        ws.next_layer().handshake(ssl::stream_base::client, ec);
        if (ec) { return std::unexpected(TickerError::TlsFailed); }

        // The timeout policy moves to the WebSocket layer once it owns the
        // stream; leaving the tcp_stream timer armed would abort the socket
        // mid-session at whatever the last expires_after said.
        beast::get_lowest_layer(ws).expires_never();
        ws.set_option(websocket::stream_base::timeout::suggested(
            beast::role_type::client));
        ws.binary(true);

        ws.handshake(host, *target, ec);
        if (ec) { return std::unexpected(TickerError::UpgradeFailed); }

        // Subscribe, then set the mode. Both are TEXT frames on a socket
        // whose data direction is binary, so the write option is flipped for
        // the control messages and flipped back.
        ws.text(true);
        ws.write(net::buffer(kite_subscribe_message(tokens)), ec);
        if (ec) { return std::unexpected(TickerError::TransportFailed); }
        ws.write(net::buffer(kite_mode_message(mode, tokens)), ec);
        if (ec) { return std::unexpected(TickerError::TransportFailed); }
        ws.text(false);

        TickerStats st{};
        beast::flat_buffer buf;
        const auto deadline = std::chrono::steady_clock::now() + read_timeout;

        while (!should_stop()) {
            buf.clear();
            ws.read(buf, ec);
            if (ec == websocket::error::closed) { break; }
            if (ec) {
                // A timeout with frames already delivered is a normal end to
                // a bounded run; with none it is a feed that never spoke.
                if (st.binary_frames == 0 && st.heartbeats == 0) {
                    return std::unexpected(TickerError::Idle);
                }
                break;
            }

            const auto* p =
                static_cast<const unsigned char*>(buf.data().data());
            const std::size_t n = buf.size();
            st.bytes += n;

            if (!ws.got_binary()) {
                // Order updates and error JSON. Not ticks, and deliberately
                // not handed to the binary decoder.
                ++st.text_frames;
                continue;
            }
            // KITE'S HEARTBEAT IS A ONE-BYTE BINARY FRAME. The decoder would
            // read its first two bytes as a packet count and run off the end
            // of a one-byte buffer -- it returns ShortFrame rather than
            // crashing, but counting a heartbeat as a failed decode makes a
            // healthy idle feed look broken.
            if (n <= 1) {
                ++st.heartbeats;
                continue;
            }
            ++st.binary_frames;
            on_frame(p, n);

            if (std::chrono::steady_clock::now() >= deadline) { break; }
        }

        ws.close(websocket::close_code::normal, ec);
        return st;
    } catch (const boost::system::system_error&) {
        return std::unexpected(TickerError::TransportFailed);
    } catch (...) {
        return std::unexpected(TickerError::Unknown);
    }
}

} // namespace altair
