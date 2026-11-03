#include <broker/credential_store.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

namespace {
using namespace altair::broker;
int failures{};
void check(bool ok, const char* text) {
    if (!ok) { ++failures; std::printf("FAIL: %s\n", text); }
}
}

int main() {
    check(valid_credential_key("fyers") && valid_credential_key("primary.data-1"),
          "bounded portable keys accepted");
    for (const auto bad : {"", "has space", "slash/name", "line\nbreak", "colon:name"})
        check(!valid_credential_key(bad), "unsafe key refused");
    check(!valid_credential_key(std::string(kMaxCredentialKeyBytes + 1, 'x')),
          "oversized key refused");
    const CredentialKey bad{"slash/name", "account"};
    check(!store_credential(bad, "never stored")
              && store_credential(bad, "never stored").error()
                     == CredentialError::InvalidKey,
          "store validates before touching platform vault");
    const CredentialKey good{"altair-test", "validation-only"};
    check(!store_credential(good, "")
              && store_credential(good, "").error() == CredentialError::EmptySecret,
          "empty secret refused before platform vault");
    check(!store_credential(good, std::string(kMaxCredentialSecretBytes + 1, 'x'))
              && store_credential(good, std::string(kMaxCredentialSecretBytes + 1, 'x')).error()
                     == CredentialError::TooLong,
          "oversized secret refused before platform vault");
    {
        // The Linux file vault, in a private temp directory.
        char tmpl[] = "/tmp/altair-vault-XXXXXX";
        const char* dir = ::mkdtemp(tmpl);
        check(dir != nullptr, "temp vault dir");
        ::setenv("ALTAIR_CREDENTIAL_DIR", (std::string{dir} + "/v").c_str(), 1);
        const CredentialKey k{"altair.fyers", "client_secret"};
        check(!load_credential(k) && load_credential(k).error() == CredentialError::NotFound,
              "an absent credential is NotFound, not a failure");
        check(store_credential(k, "s3cr3t").has_value(), "store into the file vault");
        check(load_credential(k).value_or("") == "s3cr3t", "load returns what was stored");
        check(store_credential(k, "rotated").has_value() && load_credential(k).value_or("") == "rotated",
              "a second store replaces the first");
        struct stat st {};
        const std::string file = std::string{dir} + "/v/altair.fyers__client_secret";
        check(::stat(file.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600, "the file is 0600");
        check(::stat((std::string{dir} + "/v").c_str(), &st) == 0 && (st.st_mode & 0777) == 0700, "the vault is 0700");
        ::chmod(file.c_str(), 0644);
        check(!load_credential(k) && load_credential(k).error() == CredentialError::PlatformFailure,
              "a file others can read is refused, not read");
        ::chmod(file.c_str(), 0600);
        check(erase_credential(k).has_value(), "erase");
        check(!erase_credential(k) && erase_credential(k).error() == CredentialError::NotFound, "erase twice: NotFound");
        ::rmdir((std::string{dir} + "/v").c_str());
        ::rmdir(dir);
    }
    std::printf("Credential store validation: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
