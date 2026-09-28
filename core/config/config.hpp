#pragma once

// P0-08a — versioned, hashed, allocation-free configuration snapshot.
//
// Two hard requirements shape this, both from CLAUDE.md.
//
// Rule 10: every live decision must be reproducible from
// {model_hash, feature_version, config_hash, spec_version, tick_seqno}. So a
// snapshot carries a VERSION and a CONTENT HASH, and the hash is
// order-independent — if reading the same file twice produced two different
// hashes, the reproducibility claim would be worthless.
//
// Rule 4: no allocation on the hot path, and config IS read on the hot path.
// So nothing here allocates, the key is a fixed array (so a snapshot can be
// copied through the preallocated SnapshotSlots store), and lookup resolves
// to a handle once.
//
// Nothing in this file knows what a file is. Parsing is P0-08b.

#include <types/units.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>

namespace altair {

// ─────────────────────────────────────────────────────────────────────────
// Why a config operation failed. There is deliberately no "return a default"
// path: a missing or mistyped key is a configuration bug, and CLAUDE.md rule 9
// says fail loud rather than guess.
// ─────────────────────────────────────────────────────────────────────────
enum class ConfigError : std::uint8_t {
    NotFound,     // no such key
    WrongType,    // the key exists but holds another type
    Full,         // the snapshot is at kMaxConfigEntries
    KeyTooLong,   // key exceeds kMaxConfigKeyLen
    EmptyKey      // a null or zero-length key
};

/// What an entry holds.
enum class ConfigType : std::uint8_t { None = 0, Int = 1, Real = 2, Bool = 3 };

/// Entries in one snapshot. UNIT: count.
inline constexpr std::size_t kMaxConfigEntries = 128;

/// Longest key, excluding the NUL. UNIT: bytes.
inline constexpr std::size_t kMaxConfigKeyLen = 47;

// ─────────────────────────────────────────────────────────────────────────
// One key/value pair. Fixed-size and trivially copyable for snapshot copies.
//
// The key text is stored ALONGSIDE its hash, and lookup compares both. The
// hash alone would be a silent-collision bug: two distinct keys mapping to one
// entry would make a strategy read someone else's limit, and nothing would say so.
// ─────────────────────────────────────────────────────────────────────────
struct ConfigEntry {
    /// FNV-1a 64 of the key. UNIT: none. Used only as a fast reject.
    std::uint64_t key_hash;
    /// Raw payload: int64, double, or bool, bit-cast per `type`. UNIT: none.
    std::uint64_t bits;
    /// What `bits` means.
    ConfigType type;
    std::uint8_t reserved[7];
    /// NUL-terminated key text. UNIT: none.
    char key[kMaxConfigKeyLen + 1];
};

// ─────────────────────────────────────────────────────────────────────────
// An opaque, resolved position in a snapshot. Resolve once at startup, read
// on every tick: lookup by key is a linear scan, by handle it is an index.
// A handle is only valid for the snapshot that produced it.
// ─────────────────────────────────────────────────────────────────────────
class ConfigHandle {
public:
    static constexpr std::uint32_t kInvalid = 0xFFFF'FFFFu;

    constexpr ConfigHandle() noexcept = default;
    constexpr explicit ConfigHandle(std::uint32_t idx) noexcept : idx_(idx) {}

    /// UNIT: none. False for a default-constructed handle.
    [[nodiscard]] constexpr bool valid() const noexcept { return idx_ != kInvalid; }

    /// UNIT: none. kInvalid when !valid().
    [[nodiscard]] constexpr std::uint32_t index() const noexcept { return idx_; }

    friend constexpr bool operator==(ConfigHandle, ConfigHandle) noexcept = default;

private:
    std::uint32_t idx_ = kInvalid;
};

/// FNV-1a 64 of a NUL-terminated string. UNIT: none. PRECONDITION: s non-null.
[[nodiscard]] constexpr std::uint64_t fnv1a64(const char* s) noexcept {
    std::uint64_t h = 14695981039346656037ull;
    for (; *s != '\0'; ++s) {
        h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(*s));
        h *= 1099511628211ull;
    }
    return h;
}

namespace detail {

/// splitmix64 finaliser. Spreads a combined word so that a commutative sum of
/// entry hashes still moves on any single-bit content change.
/// UNIT: none. PRECONDITION: none.
[[nodiscard]] constexpr std::uint64_t config_mix64(std::uint64_t x) noexcept {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

/// Length of a NUL-terminated string, capped so a runaway pointer cannot spin.
/// UNIT: bytes. PRECONDITION: s non-null.
[[nodiscard]] constexpr std::size_t config_key_len(const char* s,
                                                   std::size_t cap) noexcept {
    std::size_t n = 0;
    while (n <= cap && s[n] != '\0') {
        ++n;
    }
    return n;
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────
// ConfigSnapshot — an immutable-by-convention set of values, plus the identity
// that makes a decision reproducible. Trivially copyable and allocation-free.
// ─────────────────────────────────────────────────────────────────────────
class ConfigSnapshot {
public:
    constexpr ConfigSnapshot() noexcept = default;

    // ── Build side. Not hot; called by a loader, not a strategy. ──

    /// Insert or overwrite. UNIT: none.
    /// Returns EmptyKey, KeyTooLong, or Full. Overwriting an existing key
    /// REPLACES its type as well as its value.
    [[nodiscard]] std::expected<void, ConfigError>
    set_int(const char* key, std::int64_t v) noexcept {
        return set_bits(key, std::bit_cast<std::uint64_t>(v), ConfigType::Int);
    }

    [[nodiscard]] std::expected<void, ConfigError>
    set_real(const char* key, double v) noexcept {
        return set_bits(key, std::bit_cast<std::uint64_t>(v), ConfigType::Real);
    }

    [[nodiscard]] std::expected<void, ConfigError>
    set_bool(const char* key, bool v) noexcept {
        return set_bits(key, v ? 1ull : 0ull, ConfigType::Bool);
    }

    // ── Read side. ──

    /// Resolve a key to a handle. UNIT: none. Linear scan — do this once at
    /// startup and keep the handle. Returns NotFound.
    /// PRECONDITION: key non-null.
    [[nodiscard]] ALTAIR_HOT std::expected<ConfigHandle, ConfigError>
    find(const char* key) const noexcept {
        if (key == nullptr) {
            return std::unexpected(ConfigError::NotFound);
        }
        const std::uint64_t h = fnv1a64(key);
        for (std::uint32_t i = 0; i < count_; ++i) {
            // Hash first as a fast reject, then the TEXT. Matching on the hash
            // alone would silently hand a strategy someone else's value.
            if (entries_[i].key_hash == h &&
                std::strcmp(entries_[i].key, key) == 0) {
                return ConfigHandle{i};
            }
        }
        return std::unexpected(ConfigError::NotFound);
    }

    /// UNIT: none. Returns NotFound for an invalid or out-of-range handle,
    /// WrongType when the entry is not an Int.
    [[nodiscard]] ALTAIR_HOT std::expected<std::int64_t, ConfigError>
    get_int(ConfigHandle h) const noexcept {
        const ConfigEntry* e = at(h);
        if (e == nullptr) {
            return std::unexpected(ConfigError::NotFound);
        }
        if (e->type != ConfigType::Int) {
            return std::unexpected(ConfigError::WrongType);
        }
        return std::bit_cast<std::int64_t>(e->bits);
    }

    [[nodiscard]] ALTAIR_HOT std::expected<double, ConfigError>
    get_real(ConfigHandle h) const noexcept {
        const ConfigEntry* e = at(h);
        if (e == nullptr) {
            return std::unexpected(ConfigError::NotFound);
        }
        if (e->type != ConfigType::Real) {
            return std::unexpected(ConfigError::WrongType);
        }
        return std::bit_cast<double>(e->bits);
    }

    [[nodiscard]] ALTAIR_HOT std::expected<bool, ConfigError>
    get_bool(ConfigHandle h) const noexcept {
        const ConfigEntry* e = at(h);
        if (e == nullptr) {
            return std::unexpected(ConfigError::NotFound);
        }
        if (e->type != ConfigType::Bool) {
            return std::unexpected(ConfigError::WrongType);
        }
        return e->bits != 0;
    }

    /// Type at a handle. UNIT: none. ConfigType::None for an invalid handle.
    [[nodiscard]] ConfigType type_of(ConfigHandle h) const noexcept {
        const ConfigEntry* e = at(h);
        return e == nullptr ? ConfigType::None : e->type;
    }

    /// Entries stored. UNIT: count.
    [[nodiscard]] std::size_t size() const noexcept {
        return static_cast<std::size_t>(count_);
    }

    // ── Identity. CLAUDE.md rule 10. ──

    /// Hash over every key and value. UNIT: none.
    /// ORDER-INDEPENDENT: two snapshots with identical content hash the same
    /// regardless of the order the keys were set. Computed on demand, O(size()).
    ///
    /// The commutative sum is deliberate. An order-dependent hash would mean
    /// the same config file read twice could produce two different hashes, and
    /// "every decision reproducible from config_hash" would be false.
    [[nodiscard]] std::uint64_t content_hash() const noexcept {
        std::uint64_t acc = 0;
        for (std::uint32_t i = 0; i < count_; ++i) {
            const ConfigEntry& e = entries_[i];
            // Type participates: the same key with bits == 1 as an Int and as
            // a Bool are different content.
            std::uint64_t w = e.key_hash;
            w = detail::config_mix64(w ^ e.bits);
            w = detail::config_mix64(w + static_cast<std::uint64_t>(e.type));
            acc += w;   // wrapping, commutative
        }
        return acc;
    }

    /// Monotonic generation, assigned by whoever built this snapshot.
    /// UNIT: none. Zero for a default-constructed snapshot.
    [[nodiscard]] std::uint64_t version() const noexcept { return version_; }

    void set_version(std::uint64_t v) noexcept { version_ = v; }

    /// Drop every entry. UNIT: none. Does NOT reset the version.
    void clear() noexcept { count_ = 0; }

private:
    [[nodiscard]] const ConfigEntry* at(ConfigHandle h) const noexcept {
        if (!h.valid() || h.index() >= count_) {
            return nullptr;
        }
        return &entries_[h.index()];
    }

    [[nodiscard]] std::expected<void, ConfigError>
    set_bits(const char* key, std::uint64_t bits, ConfigType type) noexcept {
        if (key == nullptr || key[0] == '\0') {
            return std::unexpected(ConfigError::EmptyKey);
        }
        const std::size_t len = detail::config_key_len(key, kMaxConfigKeyLen);
        if (len > kMaxConfigKeyLen) {
            return std::unexpected(ConfigError::KeyTooLong);
        }

        const std::uint64_t h = fnv1a64(key);

        // Overwrite in place if the key exists — value AND type.
        for (std::uint32_t i = 0; i < count_; ++i) {
            if (entries_[i].key_hash == h && std::strcmp(entries_[i].key, key) == 0) {
                entries_[i].bits = bits;
                entries_[i].type = type;
                return {};
            }
        }

        if (count_ >= kMaxConfigEntries) {
            return std::unexpected(ConfigError::Full);
        }

        ConfigEntry& e = entries_[count_];
        e.key_hash = h;
        e.bits = bits;
        e.type = type;
        for (std::size_t i = 0; i < sizeof(e.reserved); ++i) {
            e.reserved[i] = 0;
        }
        std::memcpy(e.key, key, len);
        for (std::size_t i = len; i <= kMaxConfigKeyLen; ++i) {
            e.key[i] = '\0';   // zero the tail so the hash of the raw bytes is stable
        }
        ++count_;
        return {};
    }

    ConfigEntry entries_[kMaxConfigEntries]{};
    std::uint32_t count_ = 0;
    std::uint32_t reserved_ = 0;
    std::uint64_t version_ = 0;
};

} // namespace altair
