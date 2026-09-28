// Cold-path OS credential vault adapter. Never include this from desktop/.
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace altair::broker {

inline constexpr std::size_t kMaxCredentialKeyBytes = 96;
inline constexpr std::size_t kMaxCredentialSecretBytes = 4096;

enum class CredentialError : std::uint8_t {
    InvalidKey,
    EmptySecret,
    TooLong,
    NotFound,
    Unsupported,
    PlatformFailure
};

struct CredentialKey {
    std::string service;
    std::string account;
};

/// Printable identifier validation; no path/control/separator characters.
[[nodiscard]] bool valid_credential_key(std::string_view value) noexcept;

/// Store in the current user's OS vault. The plaintext is never persisted by
/// Altair and must not be logged by the caller.
[[nodiscard]] std::expected<void, CredentialError>
store_credential(const CredentialKey& key, std::string_view secret) noexcept;

/// Load from the current user's OS vault. NotFound is not PlatformFailure.
[[nodiscard]] std::expected<std::string, CredentialError>
load_credential(const CredentialKey& key) noexcept;

/// Delete one vault item. Deleting an absent item returns NotFound.
[[nodiscard]] std::expected<void, CredentialError>
erase_credential(const CredentialKey& key) noexcept;

} // namespace altair::broker
