// feed/tick_store.hpp — the session tick store: writer and reader.
//
// P2-06 (writer) and P2-07 (reader), delivered together on purpose. A format
// written without a reader that proves it round-trips is a format nobody can
// trust, and the first time it is read back will be the day a backtest
// disagrees with a session and nobody can say which is wrong.
//
// RULE 6 IS THE POINT OF THIS FILE. The replay feed and the live feed must emit
// the SAME struct into the SAME pipeline. So the store persists `Tick` and
// `DepthUpdate` exactly as P2-01 defined them, byte for byte, and the reader
// hands back structs indistinguishable from the ones the decoder produced. If
// they ever diverge, the backtest is a lie.
//
// Decisions D1..D7 are fixed in prompts/P2-06_tick_store.md.

#pragma once

#include <feed/tick.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>

namespace altair {

/// Bumped whenever the FILE layout changes — separate from kTickWireVersion,
/// which describes the structs. A file can outlive several struct versions
/// only if both are recorded, so both are.
inline constexpr std::uint32_t kTickStoreVersion = 1;
inline constexpr std::uint64_t kTickStoreMagic = 0x414C5441'494B5354ull;  // "ALTAIKST"

enum class StoreError : std::uint8_t {
    NotOpen,
    OpenFailed,
    WriteFailed,
    ReadFailed,
    BadMagic,        // not an Altair tick store
    BadVersion,      // a file layout this build does not understand
    BadWireVersion,  // the structs inside are a shape this build cannot read
    Truncated,       // the file ends mid-record
    BadRecord        // an unknown record tag
};

/// What a record holds. One byte, first in every record, so a reader always
/// knows how many bytes follow before it reads them.
enum class RecordTag : std::uint8_t { Tick = 1, Depth = 2 };

/// File header. Fixed size, written once, checked on every open.
struct TickStoreHeader {
    std::uint64_t magic;
    std::uint32_t store_version;
    std::uint16_t wire_version;     // kTickWireVersion at write time (D2)
    std::uint16_t reserved;
    std::uint32_t tick_bytes;       // sizeof(Tick) at write time
    std::uint32_t depth_bytes;      // sizeof(DepthUpdate) at write time
    std::int64_t  session_date_ns;  // IST midnight of the session
    std::uint64_t reserved2[2];
};

static_assert(sizeof(TickStoreHeader) == 48);
static_assert(std::is_trivially_copyable_v<TickStoreHeader>);

// ─────────────────────────────────────────────────────────────────────────
// Writer
// ─────────────────────────────────────────────────────────────────────────
class TickStoreWriter {
public:
    struct Stats {
        std::uint64_t ticks = 0;
        std::uint64_t depths = 0;
        std::uint64_t bytes = 0;
    };

    TickStoreWriter() noexcept = default;
    TickStoreWriter(const TickStoreWriter&) = delete;
    TickStoreWriter& operator=(const TickStoreWriter&) = delete;
    ~TickStoreWriter() noexcept { (void)close(); }

    [[nodiscard]] std::expected<void, StoreError>
    open(const char* path, Timestamp session_date) noexcept {
        (void)close();
        f_ = fopen_portable(path, "wb");
        if (f_ == nullptr) {
            return std::unexpected(StoreError::OpenFailed);
        }
        TickStoreHeader h{};
        h.magic = kTickStoreMagic;
        h.store_version = kTickStoreVersion;
        // D2: the STRUCT shape is recorded beside the FILE shape. A reader
        // built against a different Tick must refuse rather than reinterpret
        // 64 bytes that no longer mean what they did.
        h.wire_version = kTickWireVersion;
        h.tick_bytes = static_cast<std::uint32_t>(sizeof(Tick));
        h.depth_bytes = static_cast<std::uint32_t>(sizeof(DepthUpdate));
        h.session_date_ns = session_date.ns_since_epoch();
        if (std::fwrite(&h, sizeof(h), 1, f_) != 1) {
            return std::unexpected(StoreError::WriteFailed);
        }
        stats_ = Stats{};
        stats_.bytes = sizeof(h);
        return {};
    }

    /// ALTAIR_HOT in the sense that it runs per tick, but it is BUFFERED I/O
    /// and therefore not on the decision path. The recorder thread owns it.
    [[nodiscard]] std::expected<void, StoreError> write(const Tick& t) noexcept {
        return write_record(RecordTag::Tick, &t, sizeof(t), stats_.ticks);
    }

    [[nodiscard]] std::expected<void, StoreError>
    write(const DepthUpdate& d) noexcept {
        return write_record(RecordTag::Depth, &d, sizeof(d), stats_.depths);
    }

    /// Flush without closing. The recorder calls this at intervals so a crash
    /// costs seconds of tape rather than the session.
    [[nodiscard]] std::expected<void, StoreError> flush() noexcept {
        if (f_ == nullptr) {
            return std::unexpected(StoreError::NotOpen);
        }
        return (std::fflush(f_) == 0) ? std::expected<void, StoreError>{}
                                      : std::unexpected(StoreError::WriteFailed);
    }

    [[nodiscard]] std::expected<void, StoreError> close() noexcept {
        if (f_ == nullptr) {
            return {};
        }
        const int rc = std::fclose(f_);
        f_ = nullptr;
        return (rc == 0) ? std::expected<void, StoreError>{}
                         : std::unexpected(StoreError::WriteFailed);
    }

    [[nodiscard]] bool is_open() const noexcept { return f_ != nullptr; }
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    static std::FILE* fopen_portable(const char* path, const char* mode) noexcept {
        std::FILE* f = nullptr;
#if defined(_MSC_VER)
        if (::fopen_s(&f, path, mode) != 0) {
            f = nullptr;
        }
#else
        f = std::fopen(path, mode);
#endif
        return f;
    }

    [[nodiscard]] std::expected<void, StoreError>
    write_record(RecordTag tag, const void* p, std::size_t n,
                 std::uint64_t& counter) noexcept {
        if (f_ == nullptr) {
            return std::unexpected(StoreError::NotOpen);
        }
        const std::uint8_t t = static_cast<std::uint8_t>(tag);
        if (std::fwrite(&t, 1, 1, f_) != 1) {
            return std::unexpected(StoreError::WriteFailed);
        }
        if (std::fwrite(p, n, 1, f_) != 1) {
            return std::unexpected(StoreError::WriteFailed);
        }
        ++counter;
        stats_.bytes += 1 + n;
        return {};
    }

    std::FILE* f_ = nullptr;
    Stats stats_{};

    friend class TickStoreReader;
};

// ─────────────────────────────────────────────────────────────────────────
// Reader
// ─────────────────────────────────────────────────────────────────────────
class TickStoreReader {
public:
    struct Record {
        RecordTag   tag;
        Tick        tick;    // valid when tag == Tick
        DepthUpdate depth;   // valid when tag == Depth
    };

    TickStoreReader() noexcept = default;
    TickStoreReader(const TickStoreReader&) = delete;
    TickStoreReader& operator=(const TickStoreReader&) = delete;
    ~TickStoreReader() noexcept { (void)close(); }

    /// Open and VALIDATE. D2/D3: magic, file version, and the struct sizes the
    /// file was written with are all checked before a single record is read.
    [[nodiscard]] std::expected<TickStoreHeader, StoreError>
    open(const char* path) noexcept {
        (void)close();
        f_ = TickStoreWriter::fopen_portable(path, "rb");
        if (f_ == nullptr) {
            return std::unexpected(StoreError::OpenFailed);
        }
        TickStoreHeader h{};
        if (std::fread(&h, sizeof(h), 1, f_) != 1) {
            (void)close();
            return std::unexpected(StoreError::Truncated);
        }
        if (h.magic != kTickStoreMagic) {
            (void)close();
            return std::unexpected(StoreError::BadMagic);
        }
        if (h.store_version != kTickStoreVersion) {
            (void)close();
            return std::unexpected(StoreError::BadVersion);
        }
        // D3: the struct shape must match EXACTLY. A file whose Tick was 64
        // bytes cannot be read by a build whose Tick is 72 — reinterpreting it
        // would produce plausible garbage, which is the worst outcome
        // available. Refuse and say why.
        if (h.wire_version != kTickWireVersion
            || h.tick_bytes != sizeof(Tick)
            || h.depth_bytes != sizeof(DepthUpdate)) {
            (void)close();
            return std::unexpected(StoreError::BadWireVersion);
        }
        header_ = h;
        return h;
    }

    /// Read the next record. Returns Truncated at a partial tail (D4) and
    /// `false` in the optional at a clean end of file.
    [[nodiscard]] std::expected<bool, StoreError> next(Record& out) noexcept {
        if (f_ == nullptr) {
            return std::unexpected(StoreError::NotOpen);
        }
        std::uint8_t tag = 0;
        const std::size_t got = std::fread(&tag, 1, 1, f_);
        if (got == 0) {
            return false;                       // clean end of file
        }
        if (tag == static_cast<std::uint8_t>(RecordTag::Tick)) {
            if (std::fread(&out.tick, sizeof(Tick), 1, f_) != 1) {
                return std::unexpected(StoreError::Truncated);
            }
            out.tag = RecordTag::Tick;
            return true;
        }
        if (tag == static_cast<std::uint8_t>(RecordTag::Depth)) {
            if (std::fread(&out.depth, sizeof(DepthUpdate), 1, f_) != 1) {
                return std::unexpected(StoreError::Truncated);
            }
            out.tag = RecordTag::Depth;
            return true;
        }
        // D5: an unknown tag means the framing is lost. Everything after it is
        // unaddressable, so skipping is not an option.
        return std::unexpected(StoreError::BadRecord);
    }

    [[nodiscard]] std::expected<void, StoreError> close() noexcept {
        if (f_ == nullptr) {
            return {};
        }
        const int rc = std::fclose(f_);
        f_ = nullptr;
        return (rc == 0) ? std::expected<void, StoreError>{}
                         : std::unexpected(StoreError::ReadFailed);
    }

    [[nodiscard]] bool is_open() const noexcept { return f_ != nullptr; }
    [[nodiscard]] const TickStoreHeader& header() const noexcept { return header_; }

private:
    std::FILE* f_ = nullptr;
    TickStoreHeader header_{};
};

} // namespace altair
