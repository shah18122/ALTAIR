#pragma once

// P0-10 — the session file: a SKELETON tick format.
//
// P2-06/07 defines the real mmap'd columnar tick store. This exists so the
// Phase 0 exit criterion is runnable today, and so a real capture drops in
// behind the same reader when there is one.
//
// Layout: a 32-byte header, then `count` ReplayTick records back to back.
// Nothing here allocates; the caller supplies every buffer.

#include <feed/replay.hpp>
#include <types/units.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>

namespace altair {

// ─────────────────────────────────────────────────────────────────────────
// Why a session file could not be read or written.
// ─────────────────────────────────────────────────────────────────────────
enum class SessionError : std::uint8_t {
    OpenFailed,       // the path could not be opened
    ShortRead,        // the file ended mid-record
    BadMagic,         // not an Altair session file
    BadVersion,       // a format this build does not know
    BadTickSize,      // ReplayTick changed size since the file was written
    TooManyTicks,     // more ticks than the caller's buffer holds
    WriteFailed       // the write did not complete
};

/// Magic at the head of every session file: "ALTAIRTK".
inline constexpr char kSessionMagic[8] = {'A','L','T','A','I','R','T','K'};

/// Format version this build reads and writes. UNIT: none.
inline constexpr std::uint32_t kSessionVersion = 1;

// ─────────────────────────────────────────────────────────────────────────
// SessionHeader — 32 bytes, then `count` ReplayTick records back to back.
// ─────────────────────────────────────────────────────────────────────────
struct SessionHeader {
    char magic[8];
    /// kSessionVersion at write time. UNIT: none.
    std::uint32_t version;
    /// sizeof(ReplayTick) at write time. UNIT: bytes. A mismatch means the
    /// struct changed and the file cannot be trusted — reject, never reinterpret.
    std::uint32_t tick_size;
    /// Records following the header. UNIT: ticks.
    std::uint64_t count;
    std::uint64_t reserved;
};

static_assert(sizeof(SessionHeader) == 32, "SessionHeader must be 32 bytes");
static_assert(std::is_trivially_copyable_v<SessionHeader>,
              "SessionHeader is written raw");

namespace detail {

/// std::fopen, portably. MSVC deprecates fopen and warns at /W4, which gate 1
/// treats as a failure; fopen_s is the Annex K spelling it wants. Handled here
/// once rather than suppressed with _CRT_SECURE_NO_WARNINGS, because the
/// warning is real advice everywhere except in a build that has already
/// checked its inputs.
/// UNIT: none. PRECONDITION: path and mode non-null. Returns nullptr on failure.
[[nodiscard]] inline std::FILE* session_fopen(const char* path,
                                              const char* mode) noexcept {
#if defined(_MSC_VER)
    std::FILE* f = nullptr;
    if (::fopen_s(&f, path, mode) != 0) {
        return nullptr;
    }
    return f;
#else
    return std::fopen(path, mode);
#endif
}

} // namespace detail

/// Write a session. UNIT: none.
/// PRECONDITION: `ticks` holds `count` records; `path` is writable.
/// Returns OpenFailed or WriteFailed. Never partially succeeds silently:
/// a failed write leaves an error, and the caller must not trust the file.
[[nodiscard]] inline std::expected<void, SessionError>
write_session(const char* path, const ReplayTick* ticks, std::size_t count) noexcept {
    if (path == nullptr || (ticks == nullptr && count != 0)) {
        return std::unexpected(SessionError::OpenFailed);
    }
    std::FILE* f = detail::session_fopen(path, "wb");
    if (f == nullptr) {
        return std::unexpected(SessionError::OpenFailed);
    }

    SessionHeader h{};
    std::memcpy(h.magic, kSessionMagic, sizeof(h.magic));
    h.version = kSessionVersion;
    h.tick_size = static_cast<std::uint32_t>(sizeof(ReplayTick));
    h.count = static_cast<std::uint64_t>(count);
    h.reserved = 0;

    if (std::fwrite(&h, sizeof(h), 1, f) != 1) {
        (void)std::fclose(f);
        return std::unexpected(SessionError::WriteFailed);
    }
    if (count != 0 && std::fwrite(ticks, sizeof(ReplayTick), count, f) != count) {
        (void)std::fclose(f);
        return std::unexpected(SessionError::WriteFailed);
    }
    if (std::fclose(f) != 0) {
        return std::unexpected(SessionError::WriteFailed);
    }
    return {};
}

/// Read a session into a caller-supplied buffer. UNIT: none.
/// PRECONDITION: `out` holds at least `capacity` records.
/// Returns the number of ticks read, or BadMagic / BadVersion / BadTickSize /
/// ShortRead / TooManyTicks / OpenFailed. Allocates nothing.
[[nodiscard]] inline std::expected<std::size_t, SessionError>
read_session(const char* path, ReplayTick* out, std::size_t capacity) noexcept {
    if (path == nullptr || out == nullptr) {
        return std::unexpected(SessionError::OpenFailed);
    }
    std::FILE* f = detail::session_fopen(path, "rb");
    if (f == nullptr) {
        return std::unexpected(SessionError::OpenFailed);
    }

    // Validation order matters: a truncated file must not be reported as a bad
    // magic, and a wrong tick_size must be caught before any record is read.
    SessionHeader h{};
    if (std::fread(&h, sizeof(h), 1, f) != 1) {
        (void)std::fclose(f);
        return std::unexpected(SessionError::ShortRead);
    }
    if (std::memcmp(h.magic, kSessionMagic, sizeof(h.magic)) != 0) {
        (void)std::fclose(f);
        return std::unexpected(SessionError::BadMagic);
    }
    if (h.version != kSessionVersion) {
        (void)std::fclose(f);
        return std::unexpected(SessionError::BadVersion);
    }
    // Not paranoia: if a later card changes ReplayTick, every existing file
    // becomes garbage that would otherwise be silently reinterpreted.
    if (h.tick_size != static_cast<std::uint32_t>(sizeof(ReplayTick))) {
        (void)std::fclose(f);
        return std::unexpected(SessionError::BadTickSize);
    }
    if (h.count > static_cast<std::uint64_t>(capacity)) {
        (void)std::fclose(f);
        return std::unexpected(SessionError::TooManyTicks);
    }

    const std::size_t n = static_cast<std::size_t>(h.count);
    if (n != 0 && std::fread(out, sizeof(ReplayTick), n, f) != n) {
        (void)std::fclose(f);
        return std::unexpected(SessionError::ShortRead);
    }
    (void)std::fclose(f);
    return n;
}

/// Fill `out` with a deterministic synthetic session. UNIT: none.
/// PRECONDITION: `out` holds `count` records.
/// The same `seed` and `count` always produce the same session on every box,
/// so a replay is reproducible — it uses a fixed LCG, never std::random.
/// Timestamps start at `start` and advance 1 ms per tick; seqno starts at 1.
/// This exists because there is no captured tick data yet, NOT because
/// synthetic data is a substitute for it.
inline void generate_session(ReplayTick* out, std::size_t count,
                             Timestamp start, std::uint64_t seed) noexcept {
    if (out == nullptr) {
        return;
    }
    std::uint64_t rng = seed * 6364136223846793005ull + 1442695040888963407ull;
    auto next = [&rng]() noexcept -> std::uint64_t {
        rng = rng * 6364136223846793005ull + 1442695040888963407ull;
        return rng >> 33;
    };

    std::int64_t px = 2'500'000;   // Rs 25,000.00 in paise
    for (std::size_t i = 0; i < count; ++i) {
        // A bounded random walk that never reaches zero.
        const std::int64_t step = static_cast<std::int64_t>(next() % 201) - 100;
        px += step;
        if (px < 1) {
            px = 1;
        }

        ReplayTick t{};
        t.ts = start + duration::millis(static_cast<std::int64_t>(i));
        t.seqno = static_cast<std::uint64_t>(i) + 1;
        t.token = 256265;                       // one instrument, as a token
        t.reserved = 0;
        t.last = Price{px};
        t.qty = Qty{static_cast<std::int64_t>(next() % 100) + 1};
        out[i] = t;
    }
}

} // namespace altair
