// live/tape.hpp -- everything the engine was told, in order, so a session
// can be run again and decide exactly as it did.
//
// The engine decides on the FEED's clock, so the bytes off the price bus
// almost determine it. Almost: a few inputs come from the machine and the
// environment, and a replay must be handed those too, at the same points in
// the stream --
//   * whether the feed was stale (30 s of wall clock without a frame);
//   * the kill request file and the ledger halt (a write that failed);
//   * the watchdog, which submits overdue exits on the machine's clock;
//   * connects and disconnects, which bound one stream from the next.
// So the tape holds the bus chunks AND those control events, each with the
// wall time it happened at. Replayed in order into the same engine with the
// same bundle (live/bundle.hpp), the journal and the decisions come out
// byte for byte the same (app/tests/live_replay.sh proves it end to end).
//
// Format: the magic line, then records of
//     u8 kind | i64 wall_ns | u32 length | length bytes
// little-endian. A tape cut short (a crash mid-write) reads up to its last
// whole record and says how many bytes it dropped.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace altair::live {

inline constexpr const char* kTapeMagic = "ALTAIR-TAPE 1\n";

enum class TapeKind : std::uint8_t {
    Data = 1,          ///< bytes off the bus, as received
    Connected = 2,     ///< a new stream begins
    Disconnected = 3,  ///< the stream ended (bytes: why)
    Stale = 4,         ///< bytes: "1" stale from here, "0" fresh again
    Kill = 5,          ///< bytes: "1" a kill request in force, "0" cleared
    Halt = 6,          ///< bytes: the halt reason; empty clears it
    Watchdog = 7,      ///< the watchdog acted at wall_ns
    Start = 8,         ///< the first record: what the session started from (tape_sections)
};

struct TapeRecord {
    TapeKind kind = TapeKind::Data;
    std::int64_t wall_ns = 0;
    std::vector<std::uint8_t> bytes;
    [[nodiscard]] std::string text() const { return std::string(bytes.begin(), bytes.end()); }
};

/// The Start record's payload: named sections ("name\nlength\n" then the
/// bytes), in order -- the small files and options a session started from.
using TapeSections = std::vector<std::pair<std::string, std::string>>;

[[nodiscard]] inline std::string tape_pack(const TapeSections& sections) {
    std::string o;
    for (const auto& [name, body] : sections) o += name + "\n" + std::to_string(body.size()) + "\n" + body;
    return o;
}

/// False when the payload is not a well-formed list of sections.
[[nodiscard]] inline bool tape_unpack(const std::string& packed, TapeSections& out) {
    out.clear();
    std::size_t at = 0;
    while (at < packed.size()) {
        const std::size_t nl = packed.find('\n', at);
        if (nl == std::string::npos) return false;
        const std::size_t nl2 = packed.find('\n', nl + 1);
        if (nl2 == std::string::npos) return false;
        const std::string name = packed.substr(at, nl - at);
        char* end = nullptr;
        const std::string len_text = packed.substr(nl + 1, nl2 - nl - 1);
        const unsigned long long len = std::strtoull(len_text.c_str(), &end, 10);
        if (len_text.empty() || end == nullptr || *end != '\0' || nl2 + 1 + len > packed.size()) return false;
        out.emplace_back(name, packed.substr(nl2 + 1, static_cast<std::size_t>(len)));
        at = nl2 + 1 + static_cast<std::size_t>(len);
    }
    return true;
}

[[nodiscard]] inline const std::string* tape_section(const TapeSections& s, const std::string& name) {
    for (const auto& [n, body] : s) if (n == name) return &body;
    return nullptr;
}

class TapeWriter {
public:
    /// Opens (truncating) `path`. ok() says whether it could.
    explicit TapeWriter(const std::string& path) : f_(path, std::ios::binary | std::ios::trunc) {
        if (f_) f_.write(kTapeMagic, static_cast<std::streamsize>(std::strlen(kTapeMagic)));
        ok_ = static_cast<bool>(f_);
    }
    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] std::uint64_t bytes_written() const noexcept { return written_; }

    void write(TapeKind kind, std::int64_t wall_ns, const std::uint8_t* data, std::size_t n) {
        if (!ok_) return;
        std::uint8_t head[13];
        head[0] = static_cast<std::uint8_t>(kind);
        for (int k = 0; k < 8; ++k) head[1 + k] = static_cast<std::uint8_t>(static_cast<std::uint64_t>(wall_ns) >> (8 * k));
        const auto len = static_cast<std::uint32_t>(n);
        for (int k = 0; k < 4; ++k) head[9 + k] = static_cast<std::uint8_t>(len >> (8 * k));
        f_.write(reinterpret_cast<const char*>(head), sizeof head);
        if (n > 0) f_.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(n));
        written_ += sizeof head + n;
        ok_ = static_cast<bool>(f_);
    }
    void write(TapeKind kind, std::int64_t wall_ns, const std::string& text = {}) {
        write(kind, wall_ns, reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
    }
    /// Push what is buffered to the file (the CLI does this every second).
    bool flush() {
        f_.flush();
        ok_ = ok_ && static_cast<bool>(f_);
        return ok_;
    }

private:
    std::ofstream f_;
    bool ok_ = false;
    std::uint64_t written_ = 0;
};

class TapeReader {
public:
    explicit TapeReader(const std::string& path) : f_(path, std::ios::binary) {
        std::string magic(std::strlen(kTapeMagic), '\0');
        if (f_) f_.read(magic.data(), static_cast<std::streamsize>(magic.size()));
        ok_ = static_cast<bool>(f_) && magic == kTapeMagic;
    }
    /// The file opened and is a tape.
    [[nodiscard]] bool ok() const noexcept { return ok_; }

    /// The next whole record; false at the end (or at a record cut short).
    bool next(TapeRecord& r) {
        if (!ok_) return false;
        std::uint8_t head[13];
        f_.read(reinterpret_cast<char*>(head), sizeof head);
        if (f_.gcount() == 0) return false;
        if (f_.gcount() != static_cast<std::streamsize>(sizeof head)) { truncated_ += static_cast<std::uint64_t>(f_.gcount()); return false; }
        std::uint64_t w = 0;
        for (int k = 0; k < 8; ++k) w |= static_cast<std::uint64_t>(head[1 + k]) << (8 * k);
        std::uint32_t len = 0;
        for (int k = 0; k < 4; ++k) len |= static_cast<std::uint32_t>(head[9 + k]) << (8 * k);
        if (head[0] < 1 || head[0] > 8) { ok_ = false; return false; }   // not a record: stop rather than guess
        r.kind = static_cast<TapeKind>(head[0]);
        r.wall_ns = static_cast<std::int64_t>(w);
        r.bytes.resize(len);
        if (len > 0) f_.read(reinterpret_cast<char*>(r.bytes.data()), len);
        if (len > 0 && f_.gcount() != static_cast<std::streamsize>(len)) {
            truncated_ += sizeof head + static_cast<std::uint64_t>(f_.gcount());
            return false;
        }
        ++records_;
        return true;
    }
    [[nodiscard]] std::uint64_t records() const noexcept { return records_; }
    /// Bytes of a last record that was cut short (0 for a whole tape).
    [[nodiscard]] std::uint64_t truncated() const noexcept { return truncated_; }

private:
    std::ifstream f_;
    bool ok_ = false;
    std::uint64_t records_ = 0, truncated_ = 0;
};

} // namespace altair::live
