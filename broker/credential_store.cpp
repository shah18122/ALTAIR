#include <broker/credential_store.hpp>

#include <array>
#include <limits>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <wincred.h>
#elif defined(__APPLE__)
#  include <CoreFoundation/CoreFoundation.h>
#  include <Security/Security.h>
#endif

namespace altair::broker {
namespace {

[[nodiscard]] bool valid(const CredentialKey& key) noexcept {
    return valid_credential_key(key.service) && valid_credential_key(key.account);
}

#if defined(_WIN32)
[[nodiscard]] std::expected<std::wstring, CredentialError>
wide(std::string_view value) noexcept {
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return std::unexpected(CredentialError::TooLong);
    const int count = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return std::unexpected(CredentialError::InvalidKey);
    std::wstring out(static_cast<std::size_t>(count), L'\0');
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), out.data(), count) != count)
        return std::unexpected(CredentialError::PlatformFailure);
    return out;
}

[[nodiscard]] std::expected<std::wstring, CredentialError>
target(const CredentialKey& key) noexcept {
    return wide("Altair/" + key.service + "/" + key.account);
}
#elif defined(__APPLE__)
class CfValue {
public:
    explicit CfValue(CFTypeRef value = nullptr) noexcept : value_(value) {}
    ~CfValue() { if (value_) CFRelease(value_); }
    CfValue(const CfValue&) = delete;
    CfValue& operator=(const CfValue&) = delete;
    [[nodiscard]] CFTypeRef get() const noexcept { return value_; }
private:
    CFTypeRef value_;
};

[[nodiscard]] CFStringRef cf_string(std::string_view value) noexcept {
    return CFStringCreateWithBytes(kCFAllocatorDefault,
        reinterpret_cast<const UInt8*>(value.data()),
        static_cast<CFIndex>(value.size()), kCFStringEncodingUTF8, false);
}

[[nodiscard]] CFMutableDictionaryRef query(const CredentialKey& key) noexcept {
    CFMutableDictionaryRef result = CFDictionaryCreateMutable(kCFAllocatorDefault,
        0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!result) return nullptr;
    CfValue service{cf_string(key.service)};
    CfValue account{cf_string(key.account)};
    if (!service.get() || !account.get()) { CFRelease(result); return nullptr; }
    CFDictionarySetValue(result, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(result, kSecAttrService, service.get());
    CFDictionarySetValue(result, kSecAttrAccount, account.get());
    return result;
}
#endif

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
#if defined(_WIN32)
    const auto name = target(key);
    if (!name) return std::unexpected(name.error());
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<wchar_t*>(name->c_str());
    credential.CredentialBlobSize = static_cast<DWORD>(secret.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(
        const_cast<char*>(secret.data()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.UserName = const_cast<wchar_t*>(L"Altair");
    if (!::CredWriteW(&credential, 0))
        return std::unexpected(CredentialError::PlatformFailure);
    return {};
#elif defined(__APPLE__)
    CfValue q{query(key)};
    if (!q.get()) return std::unexpected(CredentialError::PlatformFailure);
    (void)::SecItemDelete(static_cast<CFDictionaryRef>(q.get()));
    auto* mutable_q = static_cast<CFMutableDictionaryRef>(const_cast<void*>(q.get()));
    CfValue data{CFDataCreate(kCFAllocatorDefault,
        reinterpret_cast<const UInt8*>(secret.data()),
        static_cast<CFIndex>(secret.size()))};
    if (!data.get()) return std::unexpected(CredentialError::PlatformFailure);
    CFDictionarySetValue(mutable_q, kSecValueData, data.get());
    return ::SecItemAdd(mutable_q, nullptr) == errSecSuccess
        ? std::expected<void, CredentialError>{}
        : std::unexpected(CredentialError::PlatformFailure);
#else
    return std::unexpected(CredentialError::Unsupported);
#endif
}

std::expected<std::string, CredentialError>
load_credential(const CredentialKey& key) noexcept {
    if (!valid(key)) return std::unexpected(CredentialError::InvalidKey);
#if defined(_WIN32)
    const auto name = target(key);
    if (!name) return std::unexpected(name.error());
    PCREDENTIALW raw = nullptr;
    if (!::CredReadW(name->c_str(), CRED_TYPE_GENERIC, 0, &raw)) {
        return std::unexpected(::GetLastError() == ERROR_NOT_FOUND
            ? CredentialError::NotFound : CredentialError::PlatformFailure);
    }
    std::string secret(reinterpret_cast<const char*>(raw->CredentialBlob),
                       raw->CredentialBlobSize);
    ::SecureZeroMemory(raw->CredentialBlob, raw->CredentialBlobSize);
    ::CredFree(raw);
    return secret;
#elif defined(__APPLE__)
    CfValue q{query(key)};
    if (!q.get()) return std::unexpected(CredentialError::PlatformFailure);
    auto* mutable_q = static_cast<CFMutableDictionaryRef>(const_cast<void*>(q.get()));
    CFDictionarySetValue(mutable_q, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(mutable_q, kSecMatchLimit, kSecMatchLimitOne);
    CFTypeRef raw = nullptr;
    const OSStatus status = ::SecItemCopyMatching(mutable_q, &raw);
    if (status == errSecItemNotFound) return std::unexpected(CredentialError::NotFound);
    CfValue data{raw};
    if (status != errSecSuccess || !raw || CFGetTypeID(raw) != CFDataGetTypeID())
        return std::unexpected(CredentialError::PlatformFailure);
    const auto* bytes = CFDataGetBytePtr(static_cast<CFDataRef>(raw));
    const CFIndex size = CFDataGetLength(static_cast<CFDataRef>(raw));
    return std::string(reinterpret_cast<const char*>(bytes),
                       static_cast<std::size_t>(size));
#else
    return std::unexpected(CredentialError::Unsupported);
#endif
}

std::expected<void, CredentialError>
erase_credential(const CredentialKey& key) noexcept {
    if (!valid(key)) return std::unexpected(CredentialError::InvalidKey);
#if defined(_WIN32)
    const auto name = target(key);
    if (!name) return std::unexpected(name.error());
    if (!::CredDeleteW(name->c_str(), CRED_TYPE_GENERIC, 0))
        return std::unexpected(::GetLastError() == ERROR_NOT_FOUND
            ? CredentialError::NotFound : CredentialError::PlatformFailure);
    return {};
#elif defined(__APPLE__)
    CfValue q{query(key)};
    if (!q.get()) return std::unexpected(CredentialError::PlatformFailure);
    const OSStatus status = ::SecItemDelete(static_cast<CFDictionaryRef>(q.get()));
    if (status == errSecItemNotFound) return std::unexpected(CredentialError::NotFound);
    return status == errSecSuccess ? std::expected<void, CredentialError>{}
        : std::unexpected(CredentialError::PlatformFailure);
#else
    return std::unexpected(CredentialError::Unsupported);
#endif
}

} // namespace altair::broker
