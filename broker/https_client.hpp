// broker/https_client.hpp -- one HTTPS POST, and nothing else.
//
// P2-10c. The piece that was missing while `broker/tools/kite_login.py` stood
// in: Altair's C++ had no HTTP client, so the one call in the Kite handshake
// could not be made from the engine. It can now.
//
// DELIBERATELY MINIMAL. This is a form POST to one known host, used a handful
// of times a day, on a path where latency does not matter. It is not a general
// HTTP client and should not grow into one: the tick feed is a WebSocket and
// gets its own transport, and anything else that wants to talk HTTP should
// justify itself rather than reach for this.
//
// TLS VERIFICATION IS ON, AND THE HOSTNAME IS CHECKED.
//
// Both, explicitly, because the default in every C++ TLS wrapper is to do
// neither and the failure is silent: an unverified connection completes
// perfectly and looks identical to a verified one right up until somebody is
// on the path. This carries an API secret and a live trading token, so:
//
//   * `set_verify_mode(verify_peer)` -- reject an untrusted chain.
//   * `set_default_verify_paths()` -- use the system trust store.
//   * SNI via SSL_set_tlsext_host_name -- without it the server cannot pick
//     the right certificate and will hand back the wrong one.
//   * `set_verify_callback(host_name_verification(host))` -- the certificate
//     must actually be FOR this host. Chain validity alone is not identity;
//     a valid certificate for another domain would otherwise pass.
//
// THE SECRET NEVER TOUCHES THIS FILE. The caller passes a body that has
// already been assembled, and `broker/kite_session.hpp` computes the checksum.
// Nothing here logs a body, and the response is returned rather than printed.

#pragma once

// Windows headers FIRST, in this exact order, and every line of it is load
// bearing:
//
//   * WIN32_LEAN_AND_MEAN and winsock2.h before windows.h. Plain windows.h
//     drags in WinSock 1.x, and Boost.Asio needs WinSock2 -- getting this
//     wrong is a "WinSock.h has already been included" error from inside
//     asio, which names neither this file nor the real cause.
//   * NOMINMAX. windows.h defines min and max as MACROS, which then eat
//     `std::numeric_limits<T>::max()` in core/types/units.hpp -- an error
//     reported against units.hpp, a file that has nothing to do with any of
//     this and has compiled cleanly for every other card.
//   * The undefs after wincrypt.h. Windows defines X509_NAME, OCSP_REQUEST
//     and friends as MACROS, and OpenSSL uses those same names as TYPES, so
//     leaving them defined produces a wall of unrelated errors deep in Beast.
//
// Three separate collisions, and not one of their error messages points here.
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

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace altair {

enum class HttpError : std::uint8_t {
    /// DNS lookup failed.
    ResolveFailed,
    /// TCP connect failed.
    ConnectFailed,
    /// The TLS handshake failed -- including because the certificate did not
    /// verify or was not for this host. NOT retried, and NOT downgraded.
    TlsFailed,
    /// The request could not be written or the response could not be read.
    TransportFailed,
    /// Anything the transport threw that does not fit above.
    Unknown
};

struct HttpResponse {
    /// HTTP status. 200 on success.
    unsigned status = 0;
    /// Response body, verbatim. Never logged by this file.
    std::string body;
};

namespace detail {

/// Load the Windows ROOT certificate store into an OpenSSL context.
///
/// WITHOUT THIS, EVERY HTTPS CALL ON WINDOWS FAILS. `set_default_verify_paths`
/// makes OpenSSL look for a Unix-style CA bundle at a compiled-in path that
/// does not exist on Windows, so the trust store comes up EMPTY and every
/// certificate is untrusted. Measured before this was added: the exchange
/// returned TlsFailed against a host curl reaches without complaint.
///
/// The instinct at that point is to turn verification off. That would have
/// "worked" immediately and shipped an unauthenticated TLS connection carrying
/// an API secret and a live trading token. The failure being LOUD is the
/// system behaving correctly; the fix is to give OpenSSL the trust store
/// Windows already has, not to stop checking.
///
/// Returns how many roots were added. Zero means the store could not be read,
/// which the caller treats as fatal rather than continuing unverified.
[[nodiscard]] inline int load_platform_roots(::SSL_CTX* ssl_ctx) noexcept {
#if defined(_WIN32)
    HCERTSTORE store = ::CertOpenSystemStoreA(0, "ROOT");
    if (store == nullptr) { return 0; }
    ::X509_STORE* x_store = ::SSL_CTX_get_cert_store(ssl_ctx);
    if (x_store == nullptr) { ::CertCloseStore(store, 0); return 0; }

    int added = 0;
    PCCERT_CONTEXT ctx = nullptr;
    while ((ctx = ::CertEnumCertificatesInStore(store, ctx)) != nullptr) {
        const unsigned char* p = ctx->pbCertEncoded;
        ::X509* x = ::d2i_X509(nullptr, &p,
                               static_cast<long>(ctx->cbCertEncoded));
        if (x == nullptr) { continue; }
        // A duplicate is not an error: the store legitimately carries the
        // same root more than once, and treating that as a failure would
        // reject a healthy machine.
        if (::X509_STORE_add_cert(x_store, x) == 1) { ++added; }
        ::X509_free(x);
    }
    ::CertCloseStore(store, 0);
    return added;
#else
    // Everywhere else the compiled-in paths are correct.
    (void)ssl_ctx;
    return 1;
#endif
}

} // namespace detail

/// POST an `application/x-www-form-urlencoded` body over TLS.
///
/// `host` without scheme (for example "api.kite.trade"), `target` the path
/// (for example "/session/token"). `timeout` applies to each transport
/// operation, not to the call as a whole.
///
/// A completed exchange is a success WHATEVER the status; only transport
/// failures take the error path. Callers check `status`.
[[nodiscard]] inline std::expected<HttpResponse, HttpError>
https_post_form(std::string_view host, std::string_view target,
                std::string_view body,
                std::string_view api_version = "3",
                std::chrono::seconds timeout = std::chrono::seconds{20})
{
    namespace beast = boost::beast;
    namespace http = beast::http;
    namespace net = boost::asio;
    namespace ssl = net::ssl;
    using tcp = net::ip::tcp;

    try {
        net::io_context ioc;
        ssl::context ctx{ssl::context::tls_client};

        // Verification, on. See the header note -- the default is off and the
        // failure is invisible.
        ctx.set_verify_mode(ssl::verify_peer);
        ctx.set_default_verify_paths();
        // And on Windows that call leaves the store EMPTY, so the platform
        // roots are loaded explicitly. Zero roots is fatal: continuing would
        // mean verifying against nothing, which is not verifying.
        if (detail::load_platform_roots(ctx.native_handle()) == 0) {
            return std::unexpected(HttpError::TlsFailed);
        }

        tcp::resolver resolver{ioc};
        beast::ssl_stream<beast::tcp_stream> stream{ioc, ctx};

        const std::string host_s{host};
        // SNI. Without it the server cannot select the right certificate.
        if (!SSL_set_tlsext_host_name(stream.native_handle(), host_s.c_str())) {
            return std::unexpected(HttpError::TlsFailed);
        }
        // And the certificate must be FOR this host, not merely valid.
        stream.set_verify_callback(ssl::host_name_verification(host_s));

        boost::system::error_code ec;
        const auto results = resolver.resolve(host_s, "443", ec);
        if (ec) { return std::unexpected(HttpError::ResolveFailed); }

        beast::get_lowest_layer(stream).expires_after(timeout);
        beast::get_lowest_layer(stream).connect(results, ec);
        if (ec) { return std::unexpected(HttpError::ConnectFailed); }

        beast::get_lowest_layer(stream).expires_after(timeout);
        stream.handshake(ssl::stream_base::client, ec);
        if (ec) { return std::unexpected(HttpError::TlsFailed); }

        http::request<http::string_body> req{http::verb::post,
                                             std::string{target}, 11};
        req.set(http::field::host, host_s);
        req.set(http::field::user_agent, "altair/0.1");
        req.set(http::field::content_type,
                "application/x-www-form-urlencoded");
        if (!api_version.empty())
            req.set("X-Kite-Version", std::string{api_version});
        req.body() = std::string{body};
        req.prepare_payload();

        beast::get_lowest_layer(stream).expires_after(timeout);
        http::write(stream, req, ec);
        if (ec) { return std::unexpected(HttpError::TransportFailed); }

        beast::flat_buffer buffer;
        http::response<http::string_body> res;
        beast::get_lowest_layer(stream).expires_after(timeout);
        http::read(stream, buffer, res, ec);
        if (ec) { return std::unexpected(HttpError::TransportFailed); }

        HttpResponse out{};
        out.status = res.result_int();
        out.body = res.body();

        // Shutdown is best-effort. Many servers close without the TLS
        // close_notify, and treating that as a failure would turn every
        // successful call into an error.
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds{5});
        boost::system::error_code shut;
        stream.shutdown(shut);

        // A completed HTTP exchange is a SUCCESS of this function, whatever
        // the status. Only transport failures take the error path.
        //
        // That split is deliberate: Kite puts a machine-readable reason in the
        // body of a 400, and std::expected cannot carry a body on the error
        // side. Folding a 400 into an error code would turn "your
        // request_token is expired" into "it did not work". Callers check
        // `status`; `HttpError` means the exchange never happened at all.
        return out;
    } catch (const std::exception&) {
        return std::unexpected(HttpError::Unknown);
    }
}

/// POST an `application/json` body over TLS.
///
/// FYERS v3's authorization-code exchange is JSON rather than a form. This
/// remains a narrow sibling of `https_post_form`: no cookies, redirects,
/// retries, or generic broker API surface are added here.
[[nodiscard]] inline std::expected<HttpResponse, HttpError>
https_post_json(std::string_view host, std::string_view target,
                std::string_view body,
                std::chrono::seconds timeout = std::chrono::seconds{20})
{
    namespace beast = boost::beast;
    namespace http = beast::http;
    namespace net = boost::asio;
    namespace ssl = net::ssl;
    using tcp = net::ip::tcp;

    try {
        net::io_context ioc;
        ssl::context ctx{ssl::context::tls_client};
        ctx.set_verify_mode(ssl::verify_peer);
        ctx.set_default_verify_paths();
        if (detail::load_platform_roots(ctx.native_handle()) == 0) {
            return std::unexpected(HttpError::TlsFailed);
        }

        tcp::resolver resolver{ioc};
        beast::ssl_stream<beast::tcp_stream> stream{ioc, ctx};
        const std::string host_s{host};
        if (!SSL_set_tlsext_host_name(stream.native_handle(), host_s.c_str())) {
            return std::unexpected(HttpError::TlsFailed);
        }
        stream.set_verify_callback(ssl::host_name_verification(host_s));

        boost::system::error_code ec;
        const auto results = resolver.resolve(host_s, "443", ec);
        if (ec) { return std::unexpected(HttpError::ResolveFailed); }
        beast::get_lowest_layer(stream).expires_after(timeout);
        beast::get_lowest_layer(stream).connect(results, ec);
        if (ec) { return std::unexpected(HttpError::ConnectFailed); }
        beast::get_lowest_layer(stream).expires_after(timeout);
        stream.handshake(ssl::stream_base::client, ec);
        if (ec) { return std::unexpected(HttpError::TlsFailed); }

        http::request<http::string_body> req{http::verb::post,
                                             std::string{target}, 11};
        req.set(http::field::host, host_s);
        req.set(http::field::user_agent, "altair/0.1");
        req.set(http::field::content_type, "application/json");
        req.body() = std::string{body};
        req.prepare_payload();
        beast::get_lowest_layer(stream).expires_after(timeout);
        http::write(stream, req, ec);
        if (ec) { return std::unexpected(HttpError::TransportFailed); }

        beast::flat_buffer buffer;
        http::response_parser<http::string_body> parser;
        parser.body_limit(1ull * 1024 * 1024);
        beast::get_lowest_layer(stream).expires_after(timeout);
        http::read(stream, buffer, parser, ec);
        if (ec) { return std::unexpected(HttpError::TransportFailed); }
        auto res = parser.release();
        HttpResponse out{};
        out.status = res.result_int();
        out.body = res.body();

        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds{5});
        boost::system::error_code shut;
        stream.shutdown(shut);
        return out;
    } catch (const std::exception&) {
        return std::unexpected(HttpError::Unknown);
    }
}

/// GET over TLS with a Kite `Authorization` header.
///
/// P2-12b. Added because the historical candle API is a GET and this file had
/// only a form POST -- it was written for the session exchange, which is the
/// one call Altair made until now.
///
/// THE AUTHORIZATION HEADER CARRIES A LIVE TRADING CREDENTIAL.
///
/// Kite's scheme is `Authorization: token <api_key>:<access_token>`, and the
/// access_token in it is valid until the next morning and can place orders.
/// So it is taken as a parameter and NEVER logged, never put in the target,
/// never in a query string. The caller builds it from
/// `data/kite_session.json`; nothing here reads a file or an environment
/// variable, which keeps the set of code that touches a credential small
/// enough to audit.
///
/// Same success contract as the POST: a completed exchange is a success
/// whatever the status, because Kite puts the machine-readable reason in the
/// body of a 400 and folding that into an error code would turn "your token
/// expired" into "it did not work".
///
/// NOT EXERCISED AGAINST THE LIVE API. There are no credentials on this
/// machine's test path, so this compiles and has never made a real call. That
/// is stated here rather than discovered later.
[[nodiscard]] inline std::expected<HttpResponse, HttpError>
https_get_auth(std::string_view host, std::string_view target,
               std::string_view authorization,
               std::string_view api_version = "3",
               std::chrono::seconds timeout = std::chrono::seconds{20},
               std::uint64_t max_body_bytes = 64ull * 1024 * 1024)
{
    namespace beast = boost::beast;
    namespace http = beast::http;
    namespace net = boost::asio;
    namespace ssl = net::ssl;
    using tcp = net::ip::tcp;

    try {
        net::io_context ioc;
        ssl::context ctx{ssl::context::tls_client};
        ctx.set_verify_mode(ssl::verify_peer);
        ctx.set_default_verify_paths();
        if (detail::load_platform_roots(ctx.native_handle()) == 0) {
            return std::unexpected(HttpError::TlsFailed);
        }

        tcp::resolver resolver{ioc};
        beast::ssl_stream<beast::tcp_stream> stream{ioc, ctx};

        const std::string host_s{host};
        if (!SSL_set_tlsext_host_name(stream.native_handle(), host_s.c_str())) {
            return std::unexpected(HttpError::TlsFailed);
        }
        stream.set_verify_callback(ssl::host_name_verification(host_s));

        boost::system::error_code ec;
        const auto results = resolver.resolve(host_s, "443", ec);
        if (ec) { return std::unexpected(HttpError::ResolveFailed); }

        beast::get_lowest_layer(stream).expires_after(timeout);
        beast::get_lowest_layer(stream).connect(results, ec);
        if (ec) { return std::unexpected(HttpError::ConnectFailed); }

        beast::get_lowest_layer(stream).expires_after(timeout);
        stream.handshake(ssl::stream_base::client, ec);
        if (ec) { return std::unexpected(HttpError::TlsFailed); }

        http::request<http::string_body> req{http::verb::get,
                                             std::string{target}, 11};
        req.set(http::field::host, host_s);
        req.set(http::field::user_agent, "altair/0.1");
        req.set(http::field::authorization, std::string{authorization});
        if (!api_version.empty())
            req.set("X-Kite-Version", std::string{api_version});
        req.prepare_payload();

        beast::get_lowest_layer(stream).expires_after(timeout);
        http::write(stream, req, ec);
        if (ec) { return std::unexpected(HttpError::TransportFailed); }

        beast::flat_buffer buffer;
        // A PARSER, NOT A BARE RESPONSE, BECAUSE OF THE BODY LIMIT.
        //
        // `http::read` into a `response<string_body>` builds a parser with
        // Beast's DEFAULT body limit of 8 MB, and exceeding it fails as a
        // generic transport error -- no status, no body, nothing naming the
        // size. Kite's full instrument master is larger than that (every
        // option strike on every expiry), so the first attempt to download it
        // returned "TRANSPORT FAILED" and looked like a network fault.
        //
        // The limit is raised, not removed. An unbounded body is a memory
        // exhaustion waiting for a server that misbehaves, and this function
        // is reachable from an engine process.
        http::response_parser<http::string_body> parser;
        parser.body_limit(max_body_bytes);
        beast::get_lowest_layer(stream).expires_after(timeout);
        http::read(stream, buffer, parser, ec);
        if (ec) { return std::unexpected(HttpError::TransportFailed); }
        auto res = parser.release();

        HttpResponse out{};
        out.status = res.result_int();
        out.body = res.body();

        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds{5});
        boost::system::error_code shut;
        stream.shutdown(shut);
        return out;
    } catch (const std::exception&) {
        return std::unexpected(HttpError::Unknown);
    }
}

} // namespace altair
