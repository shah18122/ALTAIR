#include "desktop/fyers_link.hpp"

#include <QCoreApplication>

#include <cstdlib>
#include <iostream>

namespace {

bool check(bool condition, const char* message) {
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    using altair::ui::redact_fyers_redirect;

    bool ok = true;
    const QString redacted = redact_fyers_redirect(QStringLiteral(
        "https://example.test/callback?s=ok&code=xy&auth_code=abcdef&state=csrf"));
    ok &= check(redacted == QStringLiteral(
                    "https://example.test/callback?s=ok&code=<2 chars, redacted>"
                    "&auth_code=<6 chars, redacted>&state=csrf"),
                "FYERS redirect redaction corrupted the callback text");
    ok &= check(!redacted.contains(QStringLiteral("%2")),
                "FYERS redirect redaction left a format placeholder");
    ok &= check(!redacted.contains(QStringLiteral("abcdef"))
                    && !redacted.contains(QStringLiteral("code=xy")),
                "FYERS redirect redaction leaked an authorization code");
    ok &= check(redact_fyers_redirect(redacted) == redacted,
                "FYERS redirect redaction must be idempotent in the GUI log pipeline");

    std::cout << (ok ? "fyers link: ok\n" : "fyers link: FAILED\n");
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}