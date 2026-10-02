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
#include "../combined_account.hpp"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QString>
#include <QTemporaryDir>

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
    QApplication app(argc, argv);
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

    // -----------------------------------------------------------------------
    // 4. The button says what happened, where it was clicked.
    // -----------------------------------------------------------------------
    std::printf("\n[4] a refused login is visible next to the button\n");
    {
        KiteLinkPanel panel(Role::Admin, [] { return QString{}; });
        panel.set_helper_for_test(QString{});
        panel.open_login();
        auto* out = panel.outcome();
        check(out != nullptr && !out->isHidden()
                  && out->kind() == LoginOutcomeKind::Problem
                  && out->text().contains(QStringLiteral("build.bat net")),
              "with no helper built, the click explains how to build it");
    }
#ifndef _WIN32
    {
        QTemporaryDir dir;
        const QString script = dir.filePath(QStringLiteral("fake_kite_login.sh"));
        QFile f(script);
        f.open(QIODevice::WriteOnly);
        f.write("#!/bin/sh\necho 'Kite app credentials are incomplete in the OS vault or environment.'\nexit 2\n");
        f.close();
        f.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        KiteLinkPanel panel(Role::Admin, [] { return QString{}; });
        panel.set_helper_for_test(script);
        panel.open_login();
        auto* out = panel.outcome();
        check(out->kind() == LoginOutcomeKind::Working && !out->isHidden(),
              "the click shows it is working at once");
        QElapsedTimer t;
        t.start();
        while (out->kind() == LoginOutcomeKind::Working && t.elapsed() < 10000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        check(out->kind() == LoginOutcomeKind::Problem
                  && out->text().contains(QStringLiteral("credentials are not saved")),
              "credentials missing: the message names the form that fixes it");
        check(out->url().isEmpty(), "no login link is offered when there is no URL");
    }
#endif

    // -----------------------------------------------------------------------
    // 5. Both brokers' snapshots read into one book.
    // -----------------------------------------------------------------------
    std::printf("\n[5] FYERS and Zerodha snapshots combine\n");
    {
        QTemporaryDir dir;
        const auto write = [&dir](const char* name, const char* body) {
            QFile f(dir.filePath(QString::fromLatin1(name)));
            f.open(QIODevice::WriteOnly);
            f.write(body);
            return f.fileName();
        };
        const QString fy = write("fyers_account.json", R"({"schema_version":1,"broker":"FYERS",
            "fetched_at_unix":1790000000,"account_id":"XS1234",
            "funds_status":200,"funds_state":"present",
            "funds":{"fund_limit":[{"id":1,"title":"Total Balance","equityAmount":150000},
                                   {"id":2,"title":"Utilized Amount","equityAmount":50000},
                                   {"id":10,"title":"Available Balance","equityAmount":100000}]},
            "positions_status":200,"positions_state":"present",
            "positions":{"netPositions":[{"symbol":"NSE:NIFTY26OCTFUT","netQty":75,"netAvg":25000,
                                          "ltp":25100,"pl":7500,"productType":"MARGIN"}]}})");
        const QString kt = write("kite_account.json", R"({"schema_version":1,"broker":"ZERODHA_KITE",
            "fetched_at_unix":1790000000,"account_id":"AB1234",
            "margins_status":200,
            "margins":{"data":{"equity":{"net":40000.5,"available":{"live_balance":40000.5},
                                         "utilised":{"debits":9999.5}}}},
            "positions_status":200,
            "positions":{"data":{"net":[{"tradingsymbol":"BANKNIFTY26OCTFUT","quantity":-30,
                                         "average_price":56000,"last_price":56100,"pnl":-3000,
                                         "product":"NRML"}]}}})");
        const auto now = QDateTime::fromSecsSinceEpoch(1790000060, QTimeZone::UTC);
        const CombinedAccount a = read_combined_account(fy, kt, now);
        check(a.funds.size() == 2 && a.funds[0].present && a.funds[1].present,
              "both brokers' funds are read");
        check(a.funds[0].available == 100000.0 && a.funds[0].used == 50000.0
                  && a.funds[0].age_s == 60,
              "FYERS funds map by row title, with the snapshot age");
        check(a.funds[1].available == 40000.5 && a.funds[1].account == QStringLiteral("AB1234"),
              "Zerodha equity margin maps to available");
        check(a.positions.size() == 2 && a.total_pnl() == 4500.0,
              "positions from both brokers, one P&L");
        check(a.positions[1].qty == -30 && a.positions[1].broker == QStringLiteral("Zerodha"),
              "each row keeps its broker and sign");
        const CombinedAccount none = read_combined_account(dir.filePath(QStringLiteral("nope.json")),
                                                           kt, now);
        check(!none.funds[0].present && none.notes.size() == 1
                  && none.notes[0].startsWith(QStringLiteral("FYERS")),
              "a missing snapshot is said, not shown as zero");
    }

    std::printf("\n%s -- %d failing check(s)\n",
                failures == 0 ? "PASS" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
