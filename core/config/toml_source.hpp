// core/config/toml_source.hpp — TOML file → ConfigSnapshot.
//
// P0-08b. Blocked on vcpkg since Phase 0; unblocked 2026-08-31 when vcpkg was
// installed natively on Windows and `tomlplusplus` built. This is the last
// Phase 0 card.
//
// NOT header-only, and not hot. It links tomlplusplus, runs once at startup,
// and is deliberately kept out of every translation unit that only wants to
// READ config — `config/config.hpp` stays dependency-free so a strategy
// including it does not drag a TOML parser into the hot path.
//
// THE CONSTRAINT THAT SHAPES THIS FILE: ConfigSnapshot stores int64, double
// and bool, and nothing else. It rides a seqlock, so every entry must be
// trivially copyable and fixed-size. A string cannot go in one. Strings in
// altair.toml -- env-var NAMES, URLs, paths -- are therefore skipped and
// counted here, and read separately by cold startup code that can afford a
// parser. That is a real boundary, not an omission: see D1.

#pragma once

#include <config/config.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

enum class TomlError : std::uint8_t {
    ParseFailed,     // the document is not valid TOML
    FileNotFound,
    FileTooLarge,    // beyond kMaxTomlBytes
    SnapshotFull,    // more scalars than the snapshot holds
    KeyTooLong       // a flattened dotted key exceeds kMaxConfigKeyLen
};

/// Largest config file accepted. UNIT: bytes. A config that does not fit is a
/// config nobody is reading carefully.
inline constexpr std::size_t kMaxTomlBytes = 1u << 20;   // 1 MiB

struct TomlLoadReport {
    std::size_t loaded = 0;          // scalars placed in the snapshot
    std::size_t skipped_string = 0;  // D1 — a real category, not a failure
    std::size_t skipped_other = 0;   // dates, times, and arrays of tables
    std::size_t tables = 0;          // tables walked
    /// The first key that could not be stored, for a message that names it.
    char first_skipped[kMaxConfigKeyLen + 1] = {};
};

/// Flatten a TOML document into a ConfigSnapshot.
///
/// Nested tables flatten with '.', so `[feed.kite] ws_mode = 1` becomes
/// `feed.kite.ws_mode`. Arrays of scalars flatten with a numeric suffix, so
/// `events = [1501, 1502]` becomes `feed.xts.events.0` and `.1` — the array is
/// genuinely needed by the XTS subscriber, and dropping it silently would be
/// worse than a slightly odd key.
///
/// The snapshot is NOT cleared first: this is a merge, so a later file can
/// override an earlier one. Call `ConfigSnapshot{}` yourself for a clean load.
[[nodiscard]] std::expected<TomlLoadReport, TomlError>
load_toml(const char* text, std::size_t len, ConfigSnapshot& out) noexcept;

/// As above, reading from a path. Returns FileNotFound or FileTooLarge.
[[nodiscard]] std::expected<TomlLoadReport, TomlError>
load_toml_file(const char* path, ConfigSnapshot& out) noexcept;

} // namespace altair
