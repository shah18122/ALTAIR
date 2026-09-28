#include <broker/credential_store.hpp>

#include <cstdio>
#include <string>

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
    std::printf("Credential store validation: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
