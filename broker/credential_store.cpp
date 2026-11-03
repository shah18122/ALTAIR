#include <broker/credential_store.hpp>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>


namespace altair::broker {
namespace {

[[nodiscard]] bool valid(const CredentialKey& key) noexcept {
    return valid_credential_key(key.service) && valid_credential_key(key.account);
}

// THE LINUX VAULT. One file a credential, in a directory only this user can
// open: $ALTAIR_CREDENTIAL_DIR, else $XDG_CONFIG_HOME/altair/credentials,
// else ~/.config/altair/credentials. The directory is 0700 and each file 0600.
// A file another user owns, or one group or others can read, is refused rather
// than read: that is a vault someone has tampered with. Writes go to a temp
// file that is fsync'd and renamed, so a crash never leaves half a secret.
// Keys are validated first (letters, digits, - _ .), so a name can never climb
// out of the directory.
[[nodiscard]] std::string vault_dir() {
    if (const char* d = std::getenv("ALTAIR_CREDENTIAL_DIR"); d != nullptr && *d != '\0') return d;
    if (const char* x = std::getenv("XDG_CONFIG_HOME"); x != nullptr && *x == '/') return std::string{x} + "/altair/credentials";
    if (const char* h = std::getenv("HOME"); h != nullptr && *h == '/') return std::string{h} + "/.config/altair/credentials";
    return {};
}

[[nodiscard]] std::string vault_file(const std::string& dir, const CredentialKey& key) {
    return dir + "/" + key.service + "__" + key.account;
}

[[nodiscard]] bool make_private_dir(const std::string& dir) noexcept {
    // Create each missing component; only the last one has to be private.
    for (std::size_t at = 1; at <= dir.size(); ++at) {
        if (at != dir.size() && dir[at] != '/') continue;
        const std::string part = dir.substr(0, at);
        if (::mkdir(part.c_str(), at == dir.size() ? 0700 : 0755) != 0 && errno != EEXIST) return false;
    }
    struct stat st {};
    if (::lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != ::geteuid()) return false;
    if ((st.st_mode & 077) != 0 && ::chmod(dir.c_str(), 0700) != 0) return false;
    return true;
}

[[nodiscard]] bool write_all(int fd, std::string_view data) noexcept {
    while (!data.empty()) {
        const ssize_t n = ::write(fd, data.data(), data.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        data.remove_prefix(static_cast<std::size_t>(n));
    }
    return true;
}


} // namespace

bool valid_credential_key(std::string_view value) noexcept {
    if (value.empty() || value.size() > kMaxCredentialKeyBytes) return false;
    for (const unsigned char c : value) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        if (!ok) return false;
    }
    return true;
}

std::expected<void, CredentialError>
store_credential(const CredentialKey& key, std::string_view secret) noexcept {
    if (!valid(key)) return std::unexpected(CredentialError::InvalidKey);
    if (secret.empty()) return std::unexpected(CredentialError::EmptySecret);
    if (secret.size() > kMaxCredentialSecretBytes)
        return std::unexpected(CredentialError::TooLong);
    try {
        const std::string dir = vault_dir();
        if (dir.empty() || !make_private_dir(dir)) return std::unexpected(CredentialError::PlatformFailure);
        const std::string file = vault_file(dir, key);
        const std::string tmp = file + ".tmp." + std::to_string(::getpid());
        const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0) return std::unexpected(CredentialError::PlatformFailure);
        const bool ok = write_all(fd, secret) && ::fsync(fd) == 0;
        ::close(fd);
        if (!ok || ::rename(tmp.c_str(), file.c_str()) != 0) {
            ::unlink(tmp.c_str());
            return std::unexpected(CredentialError::PlatformFailure);
        }
        return {};
    } catch (...) {
        return std::unexpected(CredentialError::PlatformFailure);
    }
}

std::expected<std::string, CredentialError>
load_credential(const CredentialKey& key) noexcept {
    if (!valid(key)) return std::unexpected(CredentialError::InvalidKey);
    try {
        const std::string dir = vault_dir();
        if (dir.empty()) return std::unexpected(CredentialError::PlatformFailure);
        const int fd = ::open(vault_file(dir, key).c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) {
            return std::unexpected(errno == ENOENT ? CredentialError::NotFound : CredentialError::PlatformFailure);
        }
        struct stat st {};
        if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != ::geteuid() || (st.st_mode & 077) != 0
            || st.st_size <= 0 || st.st_size > static_cast<off_t>(kMaxCredentialSecretBytes)) {
            ::close(fd);
            return std::unexpected(CredentialError::PlatformFailure);
        }
        std::string out(static_cast<std::size_t>(st.st_size), '\0');
        std::size_t got = 0;
        while (got < out.size()) {
            const ssize_t n = ::read(fd, out.data() + got, out.size() - got);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            got += static_cast<std::size_t>(n);
        }
        ::close(fd);
        if (got != out.size()) return std::unexpected(CredentialError::PlatformFailure);
        return out;
    } catch (...) {
        return std::unexpected(CredentialError::PlatformFailure);
    }
}

std::expected<void, CredentialError>
erase_credential(const CredentialKey& key) noexcept {
    if (!valid(key)) return std::unexpected(CredentialError::InvalidKey);
    try {
        const std::string dir = vault_dir();
        if (dir.empty()) return std::unexpected(CredentialError::PlatformFailure);
        if (::unlink(vault_file(dir, key).c_str()) != 0) {
            return std::unexpected(errno == ENOENT ? CredentialError::NotFound : CredentialError::PlatformFailure);
        }
        return {};
    } catch (...) {
        return std::unexpected(CredentialError::PlatformFailure);
    }
}

} // namespace altair::broker
