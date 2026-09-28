#include <broker/oauth_attempt.hpp>

#include <cstdlib>
#include <iostream>

namespace {
bool check(bool condition, const char* message) {
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}
}

int main() {
    using namespace altair::broker;
    const OAuthAttempt attempt{"https://example.test/callback", "state-123",
                               1'000, 600};
    bool ok = true;
    const auto valid = validate_oauth_redirect(
        "https://example.test/callback?auth_code=a%2Bb&state=state-123",
        attempt, 1'300, "auth_code");
    ok &= check(valid && *valid == "a+b", "valid callback/code was refused");
    ok &= check(validate_oauth_redirect(
                    "https://evil.test/callback?auth_code=x&state=state-123",
                    attempt, 1'300, "auth_code").error()
                    == OAuthRedirectError::CallbackMismatch,
                "callback mismatch was not distinct");
    ok &= check(validate_oauth_redirect(
                    "https://example.test/callback?status=cancel&state=state-123",
                    attempt, 1'300, "auth_code").error()
                    == OAuthRedirectError::Cancelled,
                "cancelled redirect was not distinct");
    ok &= check(validate_oauth_redirect(
                    "https://example.test/callback?auth_code=x&state=state-123",
                    attempt, 1'601, "auth_code").error()
                    == OAuthRedirectError::Expired,
                "expired attempt was not distinct");
    ok &= check(validate_oauth_redirect(
                    "https://example.test/callback?auth_code=x&state=wrong",
                    attempt, 1'300, "auth_code").error()
                    == OAuthRedirectError::StateMismatch,
                "state mismatch was not distinct");
    ok &= check(validate_oauth_redirect(
                    "https://example.test/callback?state=state-123",
                    attempt, 1'300, "auth_code").error()
                    == OAuthRedirectError::MissingCode,
                "missing code was not distinct");
    ok &= check(validate_oauth_redirect(
                    "https://example.test/callback?auth_code=%Q0&state=state-123",
                    attempt, 1'300, "auth_code").error()
                    == OAuthRedirectError::MalformedEncoding,
                "malformed encoding was not refused");
    std::cout << (ok ? "oauth attempt: ok\n" : "oauth attempt: FAILED\n");
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
