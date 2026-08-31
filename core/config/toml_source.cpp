// core/config/toml_source.cpp — the TOML half of P0-08b.
//
// The only file in core/ that links a third-party parser. Kept in a .cpp so
// config/config.hpp stays dependency-free: a strategy that includes the config
// header must not drag tomlplusplus into the hot path.

#include <config/toml_source.hpp>

#include <toml++/toml.hpp>

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace altair {
namespace {

/// Append `.piece` to `prefix`, refusing rather than truncating.
///
/// A truncated key is worse than a rejected one: it silently collides with
/// another key and a strategy reads someone else's limit. ConfigSnapshot's own
/// lookup compares the full text for exactly this reason.
bool join_key(const char* prefix, std::string_view piece, char* out) noexcept
{
    const std::size_t plen = std::strlen(prefix);
    const std::size_t need = plen + (plen > 0 ? 1u : 0u) + piece.size();
    if (need > kMaxConfigKeyLen) {
        return false;
    }
    std::size_t o = 0;
    std::memcpy(out, prefix, plen);
    o = plen;
    if (plen > 0) {
        out[o++] = '.';
    }
    std::memcpy(out + o, piece.data(), piece.size());
    o += piece.size();
    out[o] = '\0';
    return true;
}

void note_skipped(TomlLoadReport& rep, const char* key) noexcept
{
    if (rep.first_skipped[0] == '\0') {
        std::snprintf(rep.first_skipped, sizeof(rep.first_skipped), "%s", key);
    }
}

/// Store one scalar. Returns false only on a hard snapshot failure.
bool put_scalar(const toml::node& n, const char* key, ConfigSnapshot& out,
                TomlLoadReport& rep, TomlError& err) noexcept
{
    if (const auto* v = n.as_integer()) {
        // TOML integers are int64 in toml++, which is exactly what the
        // snapshot holds. No narrowing, no double.
        if (!out.set_int(key, v->get())) {
            err = TomlError::SnapshotFull;
            return false;
        }
        ++rep.loaded;
        return true;
    }
    if (const auto* v = n.as_boolean()) {
        if (!out.set_bool(key, v->get())) {
            err = TomlError::SnapshotFull;
            return false;
        }
        ++rep.loaded;
        return true;
    }
    if (const auto* v = n.as_floating_point()) {
        // Rule 3: doubles are analytics only. A config value that is MONEY
        // must be written as an integer of paise in the TOML itself — this
        // loader cannot tell the difference and does not pretend to.
        if (!out.set_real(key, v->get())) {
            err = TomlError::SnapshotFull;
            return false;
        }
        ++rep.loaded;
        return true;
    }
    if (n.is_string()) {
        // D1: a string cannot ride a seqlock. Counted, not failed.
        ++rep.skipped_string;
        note_skipped(rep, key);
        return true;
    }
    // Dates, times, and anything else. Representing a TOML date in the
    // snapshot would need a units decision this card is not making.
    ++rep.skipped_other;
    note_skipped(rep, key);
    return true;
}

bool walk(const toml::node& node, const char* prefix, ConfigSnapshot& out,
          TomlLoadReport& rep, TomlError& err) noexcept
{
    if (const auto* tbl = node.as_table()) {
        ++rep.tables;
        for (const auto& [k, v] : *tbl) {
            char key[kMaxConfigKeyLen + 1];
            if (!join_key(prefix, std::string_view{k.str()}, key)) {
                err = TomlError::KeyTooLong;
                return false;
            }
            if (!walk(v, key, out, rep, err)) {
                return false;
            }
        }
        return true;
    }

    if (const auto* arr = node.as_array()) {
        // Scalar arrays flatten with a numeric suffix. An array of TABLES is
        // a different shape and is counted rather than invented into keys.
        std::size_t i = 0;
        for (const auto& e : *arr) {
            if (e.is_table() || e.is_array()) {
                ++rep.skipped_other;
                note_skipped(rep, prefix);
                ++i;
                continue;
            }
            char idx[16];
            std::snprintf(idx, sizeof(idx), "%zu", i);
            char key[kMaxConfigKeyLen + 1];
            if (!join_key(prefix, std::string_view{idx}, key)) {
                err = TomlError::KeyTooLong;
                return false;
            }
            if (!put_scalar(e, key, out, rep, err)) {
                return false;
            }
            ++i;
        }
        return true;
    }

    return put_scalar(node, prefix, out, rep, err);
}

} // namespace

std::expected<TomlLoadReport, TomlError>
load_toml(const char* text, std::size_t len, ConfigSnapshot& out) noexcept
{
    if (text == nullptr) {
        return std::unexpected(TomlError::ParseFailed);
    }
    if (len > kMaxTomlBytes) {
        return std::unexpected(TomlError::FileTooLarge);
    }

    toml::table tbl;
    try {
        tbl = toml::parse(std::string_view{text, len});
    } catch (const toml::parse_error&) {
        return std::unexpected(TomlError::ParseFailed);
    } catch (...) {
        return std::unexpected(TomlError::ParseFailed);
    }

    TomlLoadReport rep{};
    TomlError err = TomlError::ParseFailed;
    if (!walk(tbl, "", out, rep, err)) {
        return std::unexpected(err);
    }
    return rep;
}

std::expected<TomlLoadReport, TomlError>
load_toml_file(const char* path, ConfigSnapshot& out) noexcept
{
    if (path == nullptr) {
        return std::unexpected(TomlError::FileNotFound);
    }
    std::FILE* f = nullptr;
#if defined(_MSC_VER)
    if (::fopen_s(&f, path, "rb") != 0) {
        f = nullptr;
    }
#else
    f = std::fopen(path, "rb");
#endif
    if (f == nullptr) {
        return std::unexpected(TomlError::FileNotFound);
    }
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n < 0 || static_cast<std::size_t>(n) > kMaxTomlBytes) {
        std::fclose(f);
        return std::unexpected(TomlError::FileTooLarge);
    }

    std::string buf;
    try {
        buf.resize(static_cast<std::size_t>(n));
    } catch (...) {
        std::fclose(f);
        return std::unexpected(TomlError::FileTooLarge);
    }
    const std::size_t got = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    return load_toml(buf.data(), got, out);
}

} // namespace altair
