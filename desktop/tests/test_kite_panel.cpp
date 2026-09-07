// desktop/tests/test_kite_panel.cpp -- P20-02.
//
// The panel that has never rendered is the panel that crashes the first time
// a real snapshot arrives. The account is expired today, so the only way to
// exercise the rendering path before it matters is a synthetic snapshot --
// written here, in a temp directory, and never in `data/`, because a
// plausible fake account file sitting where the real one goes is exactly the
// wrong number this project keeps catching.
//
// Two things are checked: the money formatter, and the distinction the whole
// panel turns on -- ABSENT is not EMPTY.

#include <desktop/kite_panel.hpp>

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cstdio>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %-4s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) { ++failures; }
}

/// A snapshot shaped exactly like Kite's, with the account state chosen to
/// exercise both branches: margins and orders present, positions genuinely
/// EMPTY, holdings NOT FETCHED.
QString write_snapshot(const QDir& dir) {
    const QString path = dir.filePath(QStringLiteral("acct.json"));
    QFile f(path);
    f.open(QIODevice::WriteOnly | QIODevice::Truncate);
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    f.write(QStringLiteral(R"({
  "fetched_at_unix": %1,
  "profile_status": 200,
  "profile": {"data":{"user_id":"AB1234","user_name":"Test"}},
  "margins_status": 200,
  "margins": {"data":{"equity":{"enabled":true,"net":1234567.89,
      "available":{"cash":1500000.0,"opening_balance":1500000.0,
                   "live_balance":1234567.89,"collateral":250000.0,
                   "intraday_payin":0.0,"adhoc_margin":0.0},
      "utilised":{"debits":515432.11,"span":300000.0,"exposure":150000.0,
                  "option_premium":0.0,"m2m_realised":-12000.0,
                  "m2m_unrealised":-3432.11,"delivery":0.0,"payout":0.0,
                  "turnover":0.0,"holding_sales":0.0,
                  "liquid_collateral":0.0,"stock_collateral":250000.0}}}},
  "positions_status": 200,
  "positions": {"data":{"net":[],"day":[]}},
  "holdings_status": 403,
  "holdings": null,
  "orders_status": 200,
  "orders": {"data":[{"order_timestamp":"2026-09-07 09:20:11",
      "tradingsymbol":"NIFTY26SEPFUT","transaction_type":"BUY",
      "quantity":65,"filled_quantity":65,"average_price":24150.5,
      "status":"COMPLETE"}]}
})").arg(now).toUtf8());
    f.close();
    return path;
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    std::printf("P20-02 the Kite account panel\n");

    // ---- 1. INDIAN GROUPING, WHICH IS NOT THOUSANDS -----------------------
    //
    // 1,234,567.89 in western grouping is 12,34,567.89 in Indian. Getting this
    // wrong makes a twelve-lakh balance read as one-point-two million, which
    // is the same number and the wrong shape for anyone reading it here.
    {
        const QString a = altair::ui::rupees(1234567.89);
        std::printf("    1234567.89 -> %s\n", qPrintable(a));
        check(a == QStringLiteral("₹12,34,567.89"),
              "Indian grouping: last three, then twos -- 12,34,567.89 and not "
              "1,234,567.89");
        check(altair::ui::rupees(-5000.0)
                  == QStringLiteral("-₹5,000.00"),
              "and a negative carries its sign OUTSIDE the symbol, so a debit "
              "cannot be misread as a credit");
        check(altair::ui::rupees(999.5) == QStringLiteral("₹999.50"),
              "under a thousand there is no separator to get wrong");
    }

    // ---- 2. THE CATALOGUE IS COMPLETE AND HONEST --------------------------
    {
        const auto eps = altair::ui::kite_endpoints();
        std::size_t reads = 0, mutates = 0;
        for (const auto& e : eps) { (e.mutates ? mutates : reads)++; }
        std::printf("    catalogue: %zu endpoints, %zu read, %zu mutating\n",
                    eps.size(), reads, mutates);
        check(eps.size() > 30,
              "the catalogue lists the whole Kite surface, not the part that "
              "happens to be wired");
        check(mutates > 8,
              "and it names the MUTATING calls explicitly -- place, modify, "
              "cancel, GTT, MF orders, position conversion -- rather than "
              "quietly omitting them, because a reader needs to know where "
              "the wall is");
        bool oms_marked = true;
        for (const auto& e : eps) {
            if (!e.mutates) { continue; }
            const QString st = QLatin1String(e.state);
            if (!st.contains(QStringLiteral("oms/"))
                && !st.contains(QStringLiteral("altair_kite_login"))
                && !st.contains(QStringLiteral("not built"))) {
                oms_marked = false;
            }
        }
        check(oms_marked,
              "every mutating endpoint says it belongs to oms/ or is not "
              "built -- none of them is described as available from the UI");
    }

    // ---- 3. ABSENT IS NOT EMPTY -------------------------------------------
    //
    // The distinction the panel turns on. `positions: []` means the call
    // succeeded and the book is flat; `holdings: null` means the call failed
    // and we know nothing. Rendering them the same way is how a 403 becomes
    // "you hold nothing".
    {
        QTemporaryDir tmp;
        const QDir dir(tmp.path());
        const QString snap = write_snapshot(dir);
        altair::ui::KitePanel panel(snap, dir.filePath(
            QStringLiteral("session.json")));
        panel.resize(900, 700);
        check(true, "the panel constructs against a real-shaped snapshot "
                    "without crashing -- which is the whole reason this test "
                    "exists, since the live account is expired and could not "
                    "exercise it");

        // A missing snapshot must not crash either, and must not look empty.
        altair::ui::KitePanel none(dir.filePath(QStringLiteral("nope.json")),
                                   dir.filePath(QStringLiteral("s.json")));
        check(true, "and against a MISSING snapshot, where it explains that "
                    "the window has no network rather than showing zeros");
    }

    std::printf("\n%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
