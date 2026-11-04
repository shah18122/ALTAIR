// net/tls_stream.cpp -- see tls_stream.hpp.

#include <net/tls_stream.hpp>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

namespace altair::net {

namespace {
std::string openssl_error() {
    const unsigned long e = ERR_get_error();
    if (!e) return "unknown TLS error";
    char buf[256];
    ERR_error_string_n(e, buf, sizeof buf);
    return buf;
}
} // namespace

std::unique_ptr<TlsClient> TlsClient::create(const Options& o, std::string& err) {
    std::unique_ptr<TlsClient> t(new TlsClient());
    t->ctx_ = SSL_CTX_new(TLS_client_method());
    if (!t->ctx_) {
        err = openssl_error();
        return nullptr;
    }
    SSL_CTX_set_min_proto_version(t->ctx_, TLS1_2_VERSION);
    if (o.verify) {
        SSL_CTX_set_verify(t->ctx_, SSL_VERIFY_PEER, nullptr);
        const int ok = o.ca_file.empty() ? SSL_CTX_set_default_verify_paths(t->ctx_)
                                         : SSL_CTX_load_verify_locations(t->ctx_, o.ca_file.c_str(), nullptr);
        if (ok != 1) {
            err = "cannot load CA certificates: " + openssl_error();
            return nullptr;
        }
    } else {
        SSL_CTX_set_verify(t->ctx_, SSL_VERIFY_NONE, nullptr);
    }
    t->ssl_ = SSL_new(t->ctx_);
    t->in_ = BIO_new(BIO_s_mem());
    t->out_ = BIO_new(BIO_s_mem());
    if (!t->ssl_ || !t->in_ || !t->out_) {
        err = openssl_error();
        return nullptr;
    }
    BIO_set_mem_eof_return(t->in_, -1);   // "no data yet", not end of stream
    SSL_set_bio(t->ssl_, t->in_, t->out_);   // the SSL owns both BIOs now
    SSL_set_connect_state(t->ssl_);
    if (!o.host.empty()) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"   // OpenSSL's macro
        SSL_set_tlsext_host_name(t->ssl_, o.host.c_str());
#pragma GCC diagnostic pop
        if (o.verify) SSL_set1_host(t->ssl_, o.host.c_str());
    }
    return t;
}

TlsClient::~TlsClient() {
    if (ssl_) SSL_free(ssl_);   // frees in_ and out_
    if (ctx_) SSL_CTX_free(ctx_);
}

bool TlsClient::check(int r, const char* what) {
    if (r > 0) return true;
    const int e = SSL_get_error(ssl_, r);
    if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) return false;
    failed_ = true;
    error_ = std::string(what) + ": " + openssl_error();
    if (SSL_get_verify_result(ssl_) != X509_V_OK)
        error_ += std::string(" (certificate: ") + X509_verify_cert_error_string(SSL_get_verify_result(ssl_)) + ")";
    return false;
}

bool TlsClient::handshake() {
    if (ready_ || failed_) return ready_;
    if (check(SSL_do_handshake(ssl_), "TLS handshake")) ready_ = true;
    return ready_;
}

std::string TlsClient::cipher() const {
    if (!ready_) return {};
    return std::string(SSL_get_version(ssl_)) + " " + SSL_get_cipher_name(ssl_);
}

void TlsClient::feed(const std::uint8_t* p, std::size_t n) {
    if (n) BIO_write(in_, p, static_cast<int>(n));
}

std::size_t TlsClient::drain_ciphertext(std::vector<std::uint8_t>& out) {
    std::size_t total = 0;
    std::uint8_t buf[16384];
    for (int n; (n = BIO_read(out_, buf, sizeof buf)) > 0;) {
        out.insert(out.end(), buf, buf + n);
        total += static_cast<std::size_t>(n);
    }
    return total;
}

bool TlsClient::write(const std::uint8_t* p, std::size_t n) {
    if (!ready_) return false;
    return check(SSL_write(ssl_, p, static_cast<int>(n)), "TLS write");
}

std::size_t TlsClient::read(std::vector<std::uint8_t>& out) {
    if (!ready_ && !handshake()) return 0;
    std::size_t total = 0;
    std::uint8_t buf[16384];
    for (;;) {
        const int n = SSL_read(ssl_, buf, sizeof buf);
        if (n <= 0) {
            const int e = SSL_get_error(ssl_, n);
            if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_ZERO_RETURN) check(n, "TLS read");
            break;
        }
        out.insert(out.end(), buf, buf + n);
        total += static_cast<std::size_t>(n);
    }
    return total;
}

} // namespace altair::net
