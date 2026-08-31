// broker/sha256.hpp — SHA-256, because the Kite session checksum needs one.
//
// P2-10a. Written out rather than pulled in: the only consumer is a 96-byte
// hash computed once per trading day, vcpkg is still blocked (LEDGER blocker
// 7), and taking a dependency for this would be a poor trade.
//
// FIPS 180-4. Verified against the NIST vectors in the tests — including the
// two multi-block cases, because a SHA-256 that is wrong only past 55 bytes
// looks perfectly correct on "abc" and then fails on every real input.
//
// NOT a general-purpose crypto library. It hashes a byte range and that is
// all. Do not grow it into one.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace altair {

inline constexpr std::size_t kSha256Bytes = 32;
inline constexpr std::size_t kSha256HexChars = 64;

struct Sha256Digest {
    std::uint8_t b[kSha256Bytes];
};

namespace detail {

inline constexpr std::uint32_t kSha256K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

[[nodiscard]] constexpr std::uint32_t rotr32(std::uint32_t x, unsigned n) noexcept {
    return (x >> n) | (x << (32u - n));
}

} // namespace detail

/// Streaming SHA-256. Allocates nothing.
class Sha256 {
public:
    constexpr Sha256() noexcept { reset(); }

    constexpr void reset() noexcept {
        h_[0] = 0x6a09e667u; h_[1] = 0xbb67ae85u;
        h_[2] = 0x3c6ef372u; h_[3] = 0xa54ff53au;
        h_[4] = 0x510e527fu; h_[5] = 0x9b05688cu;
        h_[6] = 0x1f83d9abu; h_[7] = 0x5be0cd19u;
        len_ = 0;
        buf_used_ = 0;
    }

    void update(const void* data, std::size_t n) noexcept {
        const auto* p = static_cast<const std::uint8_t*>(data);
        len_ += static_cast<std::uint64_t>(n);
        while (n > 0) {
            const std::size_t take = (64 - buf_used_ < n) ? (64 - buf_used_) : n;
            std::memcpy(buf_ + buf_used_, p, take);
            buf_used_ += take;
            p += take;
            n -= take;
            if (buf_used_ == 64) {
                compress(buf_);
                buf_used_ = 0;
            }
        }
    }

    void update(const char* s) noexcept {
        update(s, std::strlen(s));
    }

    /// Finish and return the digest. The object is left unusable until reset().
    [[nodiscard]] Sha256Digest finish() noexcept {
        // FIPS 180-4 padding: 0x80, zeros, then the LENGTH IN BITS as a
        // big-endian 64-bit value. Bits, not bytes — a byte count here gives a
        // hash that is self-consistent and matches nothing in the world.
        const std::uint64_t bits = len_ * 8u;
        std::uint8_t pad = 0x80u;
        update(&pad, 1);
        pad = 0x00u;
        while (buf_used_ != 56) {
            update(&pad, 1);
        }
        std::uint8_t tail[8];
        for (int i = 0; i < 8; ++i) {
            tail[i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
        }
        // Bypass update() so the length is not counted into itself.
        std::memcpy(buf_ + 56, tail, 8);
        compress(buf_);
        buf_used_ = 0;

        Sha256Digest d{};
        for (int i = 0; i < 8; ++i) {
            d.b[i * 4 + 0] = static_cast<std::uint8_t>(h_[i] >> 24);
            d.b[i * 4 + 1] = static_cast<std::uint8_t>(h_[i] >> 16);
            d.b[i * 4 + 2] = static_cast<std::uint8_t>(h_[i] >> 8);
            d.b[i * 4 + 3] = static_cast<std::uint8_t>(h_[i]);
        }
        return d;
    }

private:
    void compress(const std::uint8_t* block) noexcept {
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(block[i * 4 + 0]) << 24)
                 | (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16)
                 | (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8)
                 |  static_cast<std::uint32_t>(block[i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = detail::rotr32(w[i - 15], 7)
                                   ^ detail::rotr32(w[i - 15], 18)
                                   ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = detail::rotr32(w[i - 2], 17)
                                   ^ detail::rotr32(w[i - 2], 19)
                                   ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
        std::uint32_t e = h_[4], f = h_[5], g = h_[6], hh = h_[7];

        for (int i = 0; i < 64; ++i) {
            const std::uint32_t S1 = detail::rotr32(e, 6) ^ detail::rotr32(e, 11)
                                   ^ detail::rotr32(e, 25);
            const std::uint32_t ch = (e & f) ^ ((~e) & g);
            const std::uint32_t t1 = hh + S1 + ch + detail::kSha256K[i] + w[i];
            const std::uint32_t S0 = detail::rotr32(a, 2) ^ detail::rotr32(a, 13)
                                   ^ detail::rotr32(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t t2 = S0 + maj;
            hh = g; g = f; f = e;
            e = d + t1;
            d = c; c = b; b = a;
            a = t1 + t2;
        }

        h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
        h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += hh;
    }

    std::uint32_t h_[8]{};
    std::uint8_t buf_[64]{};
    std::size_t buf_used_ = 0;
    std::uint64_t len_ = 0;
};

/// One-shot. UNIT: none.
[[nodiscard]] inline Sha256Digest sha256(const void* data, std::size_t n) noexcept {
    Sha256 s;
    s.update(data, n);
    return s.finish();
}

/// Lowercase hex, NUL-terminated. `out` must hold kSha256HexChars + 1 bytes.
/// Lowercase because that is what `fmt.Sprintf("%x", ...)` produces, and Kite
/// compares the string.
inline void sha256_hex(const Sha256Digest& d, char* out) noexcept {
    static constexpr char kHex[] = "0123456789abcdef";
    for (std::size_t i = 0; i < kSha256Bytes; ++i) {
        out[i * 2 + 0] = kHex[(d.b[i] >> 4) & 0x0Fu];
        out[i * 2 + 1] = kHex[d.b[i] & 0x0Fu];
    }
    out[kSha256HexChars] = '\0';
}

} // namespace altair
