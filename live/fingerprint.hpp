// live/fingerprint.hpp -- a 64-bit FNV-1a digest, for pinning what a session
// was built from: the history it read, the fitted models, the config. Not a
// security hash: it tells two runs apart, it does not resist an adversary.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace altair::live {

class Fingerprint {
public:
    Fingerprint& bytes(const void* p, std::size_t n) noexcept {
        const auto* b = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n; ++i) { h_ ^= b[i]; h_ *= 0x100000001b3ull; }
        return *this;
    }
    Fingerprint& text(std::string_view s) noexcept { return bytes(s.data(), s.size()).bytes("\x1f", 1); }
    Fingerprint& i64(std::int64_t v) noexcept { return bytes(&v, sizeof v); }
    /// The exact bit pattern: two doubles that print alike but differ, differ here.
    Fingerprint& f64(double v) noexcept {
        std::uint64_t u = 0;
        std::memcpy(&u, &v, sizeof u);
        return bytes(&u, sizeof u);
    }
    [[nodiscard]] std::uint64_t value() const noexcept { return h_; }
    [[nodiscard]] std::string hex() const {
        char b[20];
        std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(h_));
        return b;
    }

private:
    std::uint64_t h_ = 0xcbf29ce484222325ull;
};

} // namespace altair::live
