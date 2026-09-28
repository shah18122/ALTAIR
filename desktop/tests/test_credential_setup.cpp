#include "desktop/credential_setup.hpp"

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
    using altair::ui::CredentialBroker;
    using altair::ui::credential_control_frame;
    using altair::ui::credential_frame;

    bool ok = true;
    const QByteArray frame = credential_frame(
        CredentialBroker::Fyers, QByteArrayLiteral("app-id"),
        QByteArrayLiteral("top-secret"),
        QByteArrayLiteral("https://example.test/callback"));
    ok &= check(frame.startsWith("ALTAIR-CREDENTIAL/1\nSAVE\nFYERS\n"),
                "save frame version or broker is wrong");
    ok &= check(!frame.contains("app-id") && !frame.contains("top-secret")
                    && !frame.contains("example.test"),
                "plaintext value leaked into the framed pipe payload");
    ok &= check(frame.contains(QByteArrayLiteral("dG9wLXNlY3JldA==")),
                "secret field is not base64 framed");

    ok &= check(credential_control_frame(CredentialBroker::Kite, "STATUS")
                    == QByteArrayLiteral("ALTAIR-CREDENTIAL/1\nSTATUS\nKITE\n"),
                "status control frame is wrong");
    ok &= check(credential_control_frame(CredentialBroker::Fyers, "DELETE")
                    == QByteArrayLiteral("ALTAIR-CREDENTIAL/1\nDELETE\nFYERS\n"),
                "delete control frame is wrong");
    std::cout << (ok ? "credential setup: ok\n" : "credential setup: FAILED\n");
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
