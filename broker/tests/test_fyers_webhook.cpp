#include <broker/fyers_webhook.hpp>

#include <cstdio>
#include <string>

int main() {
    using namespace altair::fyers_webhook;
    int failures = 0;
    const auto check = [&](bool ok, const char* text) {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", text);
        if (!ok) ++failures;
    };
    const std::string body = R"({"status":"Traded","order_id":"OID-7"})";
    const auto event = validate("strong-secret", "strong-secret", body);
    check(event && event->status == Status::Traded && event->order_id == "OID-7",
          "selected traded event validates");
    check(!validate("strong-secret", "wrong", body)
              && validate("strong-secret", "wrong", body).error() == WebhookError::SecretMismatch,
          "wrong secret refuses");
    check(!validate("strong-secret", "strong-secret", R"({"status":"New"})"),
          "unselected status refuses");
    check(!validate("strong-secret", "strong-secret", R"({"order_id":"OID-7"})")
              && validate("strong-secret", "strong-secret", R"({"order_id":"OID-7"})").error()
                     == WebhookError::MissingStatus,
          "missing status refuses");
    check(!validate("strong-secret", "strong-secret", R"({"status":"Pending"})",
                    Policy{false, true, true, true}),
          "disabled pending status refuses");
    std::string oversized(kMaxBodyBytes + 1, 'x');
    check(!validate("strong-secret", "strong-secret", oversized)
              && validate("strong-secret", "strong-secret", oversized).error()
                     == WebhookError::BodyTooLarge,
          "oversized body refuses");
    const auto t = split_target("/fyers/callback?x=1&key=abc123");
    check(t.path == "/fyers/callback" && t.key == "abc123", "the URL's key is found");
    check(split_target("/fyers/callback").key.empty(), "no query: no key");
    check(authorised("abc123", "abc123", "", "{}") && authorised("abc123", "", "abc123", "{}")
              && authorised("abc123", "", "", R"({"secret":"abc123"})") && !authorised("abc123", "abc12", "x", "{}")
              && !authorised("", "", "", "{}"),
          "the secret in the URL, the header or the body; anything else refuses");
    check(order_id_of(R"({"id":"2511120\"x"})") == "2511120" && order_id_of(R"({"order_id":"A-1_b"})") == "A-1_b",
          "the order id keeps only safe characters");
    check(failures == 0 ? (std::puts("FYERS webhook: PASS"), true) : false,
          "all webhook checks pass");
    return failures == 0 ? 0 : 1;
}