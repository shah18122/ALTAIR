// P26-01 acceptance tests for desktop/kite_link.hpp.
//
// The two pure functions here are the ones that can leak a credential or send
// a person to the wrong place, so they are the ones with tests. The QProcess
// half is not tested: it launches a binary that only the `net` preset builds,
// and a test that skipped itself in the default preset would be a test that
// passes by not running.
//
// Test 1 is redaction, and it matters most. The redirect URL a person pastes
// carries a live request_token, and the log pane is the thing most likely to
// end up in a screenshot.
//
// Test 2 is URL extraction from the subprocess's usage text -- the mechanism
// that keeps the api key out of this process entirely.
//
// No check description here may contain the substring FAIL.

#include "../kite_link.hpp"

#include <QCoreApplication>
#include <QString>

#include <cstdio>

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    if (ok) {
        std::printf("  ok  : %s\n", what);
    } else {
        ++failures;
        std::printf("  FAIL: %s\n", what);
    }
}

} // namespace

using namespace altair::ui;

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    std::printf("P26-01 -- the Kite link panel\n");

    // -----------------------------------------------------------------------
    // 1. Redaction.
    // -----------------------------------------------------------------------
    std::printf("\n[1] a pasted redirect URL must not survive into the log\n");
    const QString real = QStringLiteral(
        "https://altair.thesmitshah.com/zerodha/callback"
        "?action=login&type=login&status=success"
        "&request_token=IIJDsuv8Gs1ey7vSX7Krux21a8WUtj9p");
    const QString red = redact_request_token(real);
    std::printf("        %s\n", red.toUtf8().constData());
    check(!red.contains(QStringLiteral("IIJDsuv8Gs1ey7vSX7Krux21a8WUtj9p")),
          "the token value is gone");
    check(red.contains(QStringLiteral("<32 chars, redacted>")),
          "and its LENGTH survives, because a 31-character token is a clipped "
          "paste and that is diagnostic");
    check(red.contains(QStringLiteral("status=success")),
          "the rest of the URL is untouched");

    // A bare token with no URL around it is the other thing people paste.
    const QString bare = QStringLiteral("request_token=abc123XYZ");
    check(!redact_request_token(bare).contains(QStringLiteral("abc123XYZ")),
          "a bare request_token= form is redacted too");

    // Two on one line, which a paste of two attempts produces.
    const QString twice = QStringLiteral(
        "request_token=AAAAAAAA and request_token=BBBBBBBB");
    const QString rt = redact_request_token(twice);
    check(!rt.contains(QStringLiteral("AAAAAAAA"))
              && !rt.contains(QStringLiteral("BBBBBBBB")),
          "every occurrence is redacted, not just the first");

    // Text with no token must come through byte for byte, or the log becomes
    // untrustworthy in the ordinary case.
    const QString plain = QStringLiteral("exchange did not succeed: Refused");
    check(redact_request_token(plain) == plain,
          "text with no token is unchanged");

    // -----------------------------------------------------------------------
    // 2. Pulling the login URL out of the subprocess's usage text.
    // -----------------------------------------------------------------------
    std::printf("\n[2] the login URL comes from the subprocess, not from us\n");
    const QString usage = QStringLiteral(
        "\n  Kite login -- step 2 of 2\n"
        "  ----------------------------------------------------------\n"
        "  1. Open this and log in:\n"
        "       https://kite.zerodha.com/connect/login?api_key=abcd1234&v=3\n"
        "  2. Zerodha redirects to your registered callback with"
        " ?request_token=...\n");
    const QString url = login_url_from(usage);
    std::printf("        %s\n", url.toUtf8().constData());
    check(url.startsWith(
              QStringLiteral("https://kite.zerodha.com/connect/login")),
          "the login URL is found in the usage text");
    check(url.contains(QStringLiteral("api_key=abcd1234")),
          "with the api key the subprocess put there — this process never "
          "read the environment variable that holds it");
    check(!url.contains(QLatin1Char('\n')) && !url.contains(QLatin1Char(' ')),
          "and it stops at whitespace rather than swallowing the next line");

    check(login_url_from(QStringLiteral("no url here")).isEmpty(),
          "text with no URL yields an empty string, not a partial one");

    // -----------------------------------------------------------------------
    // 3. Locating the binary.
    // -----------------------------------------------------------------------
    std::printf("\n[3] locating altair_kite_login\n");
    const QString exe = find_kite_login();
    std::printf("        %s\n",
                exe.isEmpty() ? "not found in this build"
                              : exe.toUtf8().constData());
    // Either answer is correct: the default preset does not build it. What
    // must not happen is a path being returned for a file that is not there.
    check(exe.isEmpty() || QFileInfo(exe).exists(),
          "the returned path, if any, is a file that exists");

    std::printf("\n%s -- %d failing check(s)\n",
                failures == 0 ? "PASS" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
