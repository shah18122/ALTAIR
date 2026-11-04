#pragma once

// net/websocket.hpp -- a WebSocket client (RFC 6455) over any byte stream.
//
// The bypass path has no Beast/asio socket under it, so this does the framing
// itself, transport-agnostic and allocation-light: the caller feeds the bytes
// TLS decrypted with feed(), and sends whatever the client queues in out().
//
//   - the HTTP/1.1 upgrade request (Host, path, optional Authorization) and a
//     check of the 101 answer, including Sec-WebSocket-Accept
//   - client frames are masked (the RFC requires it), server frames are not
//   - fragmented messages are joined; ping is answered with pong; a close is
//     answered and ends the session
//
// FYERS's HSM feed is binary frames; the 50-level book is text frames out.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/evp.h>
#include <openssl/sha.h>

namespace altair::net {

namespace ws_op {
inline constexpr std::uint8_t cont = 0x0, text = 0x1, binary = 0x2, close = 0x8, ping = 0x9, pong = 0xA;
}

inline std::string base64(const std::uint8_t* p, std::size_t n) {
    std::string out(4 * ((n + 2) / 3), '\0');
    const int len = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), p, static_cast<int>(n));
    out.resize(static_cast<std::size_t>(len));
    return out;
}

/// The Sec-WebSocket-Accept a server must answer for `key`.
inline std::string ws_accept_for(std::string_view key) {
    std::string s(key);
    s += "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::uint8_t digest[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(s.data()), s.size(), digest);
    return base64(digest, sizeof digest);
}

/// Append one client frame (FIN set, masked) to `out`.
inline void ws_encode(std::vector<std::uint8_t>& out, std::uint8_t opcode, const std::uint8_t* p, std::size_t n,
                      std::array<std::uint8_t, 4> mask) {
    out.push_back(static_cast<std::uint8_t>(0x80 | opcode));
    if (n < 126) {
        out.push_back(static_cast<std::uint8_t>(0x80 | n));
    } else if (n <= 0xFFFF) {
        out.push_back(0x80 | 126);
        out.push_back(static_cast<std::uint8_t>(n >> 8));
        out.push_back(static_cast<std::uint8_t>(n));
    } else {
        out.push_back(0x80 | 127);
        for (int i = 7; i >= 0; --i) out.push_back(static_cast<std::uint8_t>(std::uint64_t{n} >> (8 * i)));
    }
    out.insert(out.end(), mask.begin(), mask.end());
    const std::size_t at = out.size();
    out.resize(at + n);
    for (std::size_t i = 0; i < n; ++i) out[at + i] = static_cast<std::uint8_t>(p[i] ^ mask[i & 3]);
}

class WebSocketClient {
public:
    enum class State : std::uint8_t { Upgrading, Open, Closed, Failed };

    struct Options {
        std::string host;
        std::string path = "/";
        std::string authorization;   // sent as the Authorization header; never logged
        std::size_t max_message = 16u << 20;
    };

    explicit WebSocketClient(Options o, std::uint64_t seed = std::random_device{}())
        : o_(std::move(o)), rng_(seed) {
        std::array<std::uint8_t, 16> k{};
        for (auto& b : k) b = static_cast<std::uint8_t>(rng_());
        key_ = base64(k.data(), k.size());
        std::string req = "GET " + o_.path + " HTTP/1.1\r\nHost: " + o_.host +
                          "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key_ +
                          "\r\nSec-WebSocket-Version: 13\r\n";
        if (!o_.authorization.empty()) req += "Authorization: " + o_.authorization + "\r\n";
        req += "\r\n";
        out_.assign(req.begin(), req.end());
    }

    State state() const noexcept { return state_; }
    bool open() const noexcept { return state_ == State::Open; }
    const std::string& error() const noexcept { return error_; }
    const std::string& key() const noexcept { return key_; }

    /// Bytes waiting for the transport; the caller sends them and clears it.
    std::vector<std::uint8_t>& out() noexcept { return out_; }

    void send(std::uint8_t opcode, const std::uint8_t* p, std::size_t n) {
        if (state_ != State::Open) return;
        ws_encode(out_, opcode, p, n, mask());
    }
    void send_binary(const std::uint8_t* p, std::size_t n) { send(ws_op::binary, p, n); }
    void send_text(std::string_view s) { send(ws_op::text, reinterpret_cast<const std::uint8_t*>(s.data()), s.size()); }
    void close(std::uint16_t code = 1000) {
        if (state_ != State::Open) return;
        const std::uint8_t c[2] = {static_cast<std::uint8_t>(code >> 8), static_cast<std::uint8_t>(code)};
        ws_encode(out_, ws_op::close, c, 2, mask());
        state_ = State::Closed;
    }

    /// Stream bytes in. on_message(opcode, data, size) runs for every complete
    /// text or binary message.
    template <typename OnMessage>
    void feed(const std::uint8_t* p, std::size_t n, OnMessage&& on_message) {
        if (state_ == State::Failed) return;
        in_.insert(in_.end(), p, p + n);
        std::size_t used = 0;
        if (state_ == State::Upgrading) {
            used = upgrade();
            if (state_ != State::Open) {
                if (state_ == State::Upgrading) return;   // not all of the answer yet
                in_.clear();
                return;
            }
        }
        for (;;) {
            const std::size_t k = frame(used, on_message);
            if (k == 0) break;
            used += k;
        }
        in_.erase(in_.begin(), in_.begin() + static_cast<std::ptrdiff_t>(used));
    }

private:
    std::array<std::uint8_t, 4> mask() {
        const std::uint32_t r = static_cast<std::uint32_t>(rng_());
        return {static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(r >> 8), static_cast<std::uint8_t>(r >> 16),
                static_cast<std::uint8_t>(r >> 24)};
    }

    void fail(std::string why) {
        state_ = State::Failed;
        error_ = std::move(why);
    }

    static std::string lower(std::string_view s) {
        std::string o(s);
        for (auto& c : o) c = static_cast<char>((c >= 'A' && c <= 'Z') ? c + 32 : c);
        return o;
    }

    // The 101 answer; returns the bytes it used (0 while incomplete).
    std::size_t upgrade() {
        const std::string_view all(reinterpret_cast<const char*>(in_.data()), in_.size());
        const auto end = all.find("\r\n\r\n");
        if (end == std::string_view::npos) {
            if (in_.size() > 16384) fail("the upgrade answer is too long");
            return 0;
        }
        const std::string_view head = all.substr(0, end);
        const auto eol = head.find("\r\n");
        const std::string_view status = head.substr(0, eol);
        if (status.substr(0, 12) != "HTTP/1.1 101") {
            fail("upgrade refused: " + std::string(status));
            return 0;
        }
        std::string accept;
        bool upgrade_ok = false;
        for (std::size_t at = eol + 2; at < head.size();) {
            auto nl = head.find("\r\n", at);
            if (nl == std::string_view::npos) nl = head.size();
            const std::string_view line = head.substr(at, nl - at);
            const auto colon = line.find(':');
            if (colon != std::string_view::npos) {
                const std::string name = lower(line.substr(0, colon));
                std::string_view value = line.substr(colon + 1);
                while (!value.empty() && value.front() == ' ') value.remove_prefix(1);
                if (name == "sec-websocket-accept") accept = std::string(value);
                if (name == "upgrade" && lower(value) == "websocket") upgrade_ok = true;
            }
            at = nl + 2;
        }
        if (!upgrade_ok || accept != ws_accept_for(key_)) {
            fail("the server's upgrade answer is not a valid WebSocket accept");
            return 0;
        }
        state_ = State::Open;
        return end + 4;
    }

    // One frame starting at in_[at]; returns its size (0 while incomplete).
    template <typename OnMessage>
    std::size_t frame(std::size_t at, OnMessage& on_message) {
        if (state_ != State::Open) return 0;
        const std::size_t avail = in_.size() - at;
        if (avail < 2) return 0;
        const std::uint8_t* f = in_.data() + at;
        const bool fin = (f[0] & 0x80) != 0;
        const std::uint8_t op = f[0] & 0x0F;
        const bool masked = (f[1] & 0x80) != 0;
        std::uint64_t len = f[1] & 0x7F;
        std::size_t hdr = 2;
        if (len == 126) {
            if (avail < 4) return 0;
            len = (std::uint64_t{f[2]} << 8) | f[3];
            hdr = 4;
        } else if (len == 127) {
            if (avail < 10) return 0;
            len = 0;
            for (int i = 0; i < 8; ++i) len = (len << 8) | f[2 + i];
            hdr = 10;
        }
        if (masked) {
            fail("the server masked a frame");
            return 0;
        }
        if (len > o_.max_message) {
            fail("a frame larger than the limit");
            return 0;
        }
        if (avail < hdr + len) return 0;
        const std::uint8_t* payload = f + hdr;
        const std::size_t n = static_cast<std::size_t>(len);
        switch (op) {
        case ws_op::ping:
            ws_encode(out_, ws_op::pong, payload, n, mask());
            break;
        case ws_op::pong:
            break;
        case ws_op::close:
            if (state_ == State::Open) {
                ws_encode(out_, ws_op::close, payload, n >= 2 ? 2 : 0, mask());
                state_ = State::Closed;
            }
            break;
        case ws_op::text:
        case ws_op::binary:
        case ws_op::cont:
            if (op != ws_op::cont) {
                msg_op_ = op;
                msg_.clear();
            } else if (msg_op_ == 0) {
                fail("a continuation with no message");
                return 0;
            }
            if (fin && op != ws_op::cont) {
                on_message(op, payload, n);   // the common case: no copy
            } else {
                msg_.insert(msg_.end(), payload, payload + n);
                if (msg_.size() > o_.max_message) {
                    fail("a message larger than the limit");
                    return 0;
                }
                if (fin) {
                    on_message(msg_op_, msg_.data(), msg_.size());
                    msg_.clear();
                    msg_op_ = 0;
                }
            }
            break;
        default:
            fail("an unknown opcode");
            return 0;
        }
        return hdr + n;
    }

    Options o_;
    std::mt19937_64 rng_;
    std::string key_;
    State state_ = State::Upgrading;
    std::string error_;
    std::vector<std::uint8_t> in_, out_, msg_;
    std::uint8_t msg_op_ = 0;
};

} // namespace altair::net
