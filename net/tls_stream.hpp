#pragma once

// net/tls_stream.hpp -- TLS over ALTAIR's own TCP, with OpenSSL in memory.
//
// The kernel-bypass TCP has no socket for OpenSSL to read, so TLS runs on two
// memory BIOs: ciphertext the TCP delivers goes in with feed(), plaintext the
// application writes comes out as ciphertext through drain_ciphertext() for
// the TCP to send. The certificate is verified against the system CA store
// and the host name (SNI + hostname check), exactly as a normal client would.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_st SSL;
typedef struct bio_st BIO;

namespace altair::net {

class TlsClient {
public:
    struct Options {
        std::string host;          // SNI and the name the certificate must carry
        std::string ca_file;       // empty: the system store
        bool verify = true;        // false only for tests against a self-signed server
    };

    static std::unique_ptr<TlsClient> create(const Options& o, std::string& err);
    ~TlsClient();
    TlsClient(const TlsClient&) = delete;
    TlsClient& operator=(const TlsClient&) = delete;

    /// Start (or continue) the handshake. True once it is complete.
    bool handshake();
    bool ready() const noexcept { return ready_; }
    bool failed() const noexcept { return failed_; }
    const std::string& error() const noexcept { return error_; }
    std::string cipher() const;

    /// Ciphertext from the TCP stream.
    void feed(const std::uint8_t* p, std::size_t n);
    /// Ciphertext waiting to go to the TCP stream (appended to `out`).
    std::size_t drain_ciphertext(std::vector<std::uint8_t>& out);
    /// Encrypt application bytes (after the handshake).
    bool write(const std::uint8_t* p, std::size_t n);
    /// Decrypt whatever is available into `out` (appended). Returns bytes read.
    std::size_t read(std::vector<std::uint8_t>& out);

private:
    TlsClient() = default;
    bool check(int r, const char* what);

    SSL_CTX* ctx_ = nullptr;
    SSL* ssl_ = nullptr;
    BIO* in_ = nullptr;    // TCP -> OpenSSL
    BIO* out_ = nullptr;   // OpenSSL -> TCP
    bool ready_ = false;
    bool failed_ = false;
    std::string error_;
};

} // namespace altair::net
