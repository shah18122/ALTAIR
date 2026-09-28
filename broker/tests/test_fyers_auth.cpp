// Acceptance checks for the paper-safe FYERS authentication vocabulary.

#include <broker/fyers_api.hpp>

#include <cstdio>
#include <cstring>
#include <string>

namespace {
int failures = 0;

void check(bool ok, const char* what) {
    if (ok) {
        std::printf("PASS %s\n", what);
    } else {
        ++failures;
        std::printf("FAIL %s\n", what);
    }
}
}  // namespace

int main() {
    using namespace altair::fyers;

    char url[kLoginUrlMax]{};
    const auto login = login_url("APP123", "https%3A%2F%2Fexample.test%2Fcb",
                                 "csrf-1", url, sizeof(url));
    check(login.has_value(), "FYERS login URL builds");
    check(std::strstr(url, "generate-authcode?client_id=APP123") != nullptr,
          "FYERS login URL names the client id");
    check(std::strstr(url, "response_type=code&state=csrf-1") != nullptr,
          "FYERS login URL carries code flow and state");

    char hash[altair::kSha256HexChars + 1]{};
    const auto h = app_id_hash("APP", "SECRET", hash, sizeof(hash));
    check(h.has_value(), "FYERS app id hash builds");
    check(std::strcmp(hash,
                      "472590b110a43b63d56c95ec6a403d432ba8a844a180505142da37f040bd4793") == 0,
          "FYERS app id hash is SHA-256(client_id:secret)");

    char header[kHeaderMax]{};
    const auto auth = authorization_header("APP123", "ACCESS", header,
                                           sizeof(header));
    check(auth.has_value() && std::strcmp(header, "APP123:ACCESS") == 0,
          "FYERS Authorization value uses client_id:access_token");

    const std::string long_token(2'048, 't');
    const auto long_auth = authorization_header("APP123", long_token.c_str());
    check(long_auth.has_value()
              && long_auth->size() == std::strlen("APP123:") + long_token.size()
              && long_auth->starts_with("APP123:")
              && long_auth->ends_with(long_token),
          "FYERS multi-kilobyte access token uses dynamic authorization storage");

    const std::string oversized_token(kAccessTokenMax + 1, 't');
    check(authorization_header("APP123", oversized_token.c_str()).error()
              == AuthError::MissingAccessToken,
          "FYERS access token remains defensively bounded");

    check(login_url("", "https%3A%2F%2Fexample.test", "state", url,
                    sizeof(url)).error() == AuthError::MissingClientId,
          "missing FYERS client id is refused");
    check(app_id_hash("APP", "SECRET", hash, 4).error()
              == AuthError::BufferTooSmall,
          "undersized FYERS hash buffer is refused");
    check(authorization_header("APP", "", header, sizeof(header)).error()
              == AuthError::MissingAccessToken,
          "missing FYERS access token is refused");

    check(kAccountEndpoints.size() == 5,
          "FYERS read-only account surface has five explicit endpoints");
    check(std::strcmp(kAccountEndpoints[0].path, "/api/v3/profile") == 0
              && std::strcmp(kAccountEndpoints[4].path, "/api/v3/orders") == 0,
          "FYERS account endpoint paths match the v3 contract");
    bool all_get_only = true;
    for (const auto& endpoint : kAccountEndpoints) {
        all_get_only = all_get_only && endpoint.name != nullptr
            && endpoint.path != nullptr
            && std::strncmp(endpoint.path, "/api/v3/", 8) == 0
            && std::strstr(endpoint.path, "/sync") == nullptr
            && std::strstr(endpoint.path, "multi-order") == nullptr;
    }
    check(all_get_only,
          "FYERS account helper vocabulary contains no order mutation path");

    std::printf("FYERS auth: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
