// P25-04 acceptance tests for desktop/order_ticket.hpp.
//
// THE UI SIDE OF A CONTRACT IT DOES NOT SHARE A HEADER WITH.
//
// `desktop/` writes the intent queue and `oms/` reads it, and neither includes
// the other. The only thing keeping them agreeing is
// `oms/tests/vectors/intents.jsonl`, which the oms test parses and this test
// REPRODUCES BYTE FOR BYTE. A field reordered, renamed, or newly escaped on
// either side fails here rather than becoming two different orders at the two
// ends of one file.
//
// Test 2 is the role gate, and test 3 is the property the whole design exists
// for: the emitted line carries no quantity and no order id, so nothing this
// window writes can reach a broker without oms/ having consulted the spec
// store and assigned an identity.
//
// No check description here may contain the substring FAIL.

#include "../order_ticket.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QString>
#include <QStringList>
#include <QTextStream>

#include <cstdio>

#ifndef ALTAIR_INTENT_VECTORS
#  define ALTAIR_INTENT_VECTORS "oms/tests/vectors/intents.jsonl"
#endif

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

QStringList read_vectors()
{
    QStringList out;
    QFile f(QStringLiteral(ALTAIR_INTENT_VECTORS));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) { return out; }
    QTextStream ts(&f);
    while (!ts.atEnd()) {
        const QString line = ts.readLine().trimmed();
        if (!line.isEmpty()) { out << line; }
    }
    return out;
}

} // namespace

using namespace altair::ui;

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    std::printf("P25-04 -- the order ticket writer\n");

    const QStringList vec = read_vectors();
    std::printf("  vectors: %s (%d lines)\n", ALTAIR_INTENT_VECTORS,
                static_cast<int>(vec.size()));
    check(vec.size() >= 3, "the shared vector file was found");
    if (vec.size() < 3) { return 1; }

    // -----------------------------------------------------------------------
    // 1. The emitter reproduces each vector byte for byte.
    // -----------------------------------------------------------------------
    std::printf("\n[1] byte-for-byte against the vectors oms/ parses\n");

    IntentDraft a;
    a.id = QStringLiteral("01J8XA0000000000000000BUY1");
    a.at = QStringLiteral("2026-09-08T18:22:31+05:30");
    a.by = QStringLiteral("smit");
    a.token = 260105u;
    a.symbol = QStringLiteral("NIFTY BANK");
    a.exchange = QStringLiteral("NSE");
    a.buy = true;
    a.lots = 1;
    a.market = false;
    a.limit_paise = 5711490;
    a.product = QStringLiteral("NRML");
    a.validity = QStringLiteral("DAY");

    IntentDraft b;
    b.id = QStringLiteral("01J8XA0000000000000000SEL1");
    b.at = QStringLiteral("2026-09-08T18:23:02+05:30");
    b.by = QStringLiteral("smit");
    b.token = 256265u;
    b.symbol = QStringLiteral("NIFTY 50");
    b.exchange = QStringLiteral("NSE");
    b.buy = false;
    b.lots = 3;
    b.market = false;
    b.limit_paise = 2415580;
    b.product = QStringLiteral("MIS");
    b.validity = QStringLiteral("DAY");

    IntentDraft c;
    c.id = QStringLiteral("01J8XA0000000000000000MKT1");
    c.at = QStringLiteral("2026-09-08T18:24:10+05:30");
    c.by = QStringLiteral("smit");
    c.token = 17512194u;
    c.symbol = QStringLiteral("NIFTY26SEPFUT");
    c.exchange = QStringLiteral("NFO");
    c.buy = true;
    c.lots = 2;
    c.market = true;
    // Deliberately non-zero: a MARKET draft must EMIT zero regardless of what
    // the price control happens to hold, or a disabled spin box would put a
    // limit into a market order.
    c.limit_paise = 999999;
    c.product = QStringLiteral("NRML");
    c.validity = QStringLiteral("IOC");

    const IntentDraft* drafts[] = {&a, &b, &c};
    for (int i = 0; i < 3; ++i) {
        const QString got = intent_line(*drafts[i]);
        const bool same = (got == vec[i]);
        if (!same) {
            std::printf("        vector %d differs\n          want %s\n"
                        "          got  %s\n", i + 1,
                        vec[i].toUtf8().constData(), got.toUtf8().constData());
        }
        check(same, i == 0 ? "vector 1 reproduced exactly"
                  : (i == 1 ? "vector 2 reproduced exactly"
                            : "vector 3 reproduced exactly"));
    }
    check(intent_line(c).contains(QStringLiteral("\"limit_paise\":0")),
          "a MARKET draft emits price 0 even with a price control set");

    // -----------------------------------------------------------------------
    // 2. The role gate.
    // -----------------------------------------------------------------------
    std::printf("\n[2] who may request\n");
    check(may(Role::Admin, Capability::RequestOrder),
          "admin may put an intent on the queue");
    check(!may(Role::Staff, Capability::RequestOrder),
          "staff may not, though it reads every number in the window");
    check(!may(Role::None, Capability::RequestOrder),
          "and an unauthenticated session certainly may not");

    // -----------------------------------------------------------------------
    // 3. What the line does NOT carry.
    // -----------------------------------------------------------------------
    std::printf("\n[3] the line carries a request, not an order\n");
    const QString line = intent_line(a);
    check(!line.contains(QStringLiteral("quantity"))
              && !line.contains(QStringLiteral("\"qty\"")),
          "no quantity: oms/ multiplies lots by the spec store's lot size");
    check(line.contains(QStringLiteral("\"lots\":1")),
          "lots is what it carries");
    check(!line.contains(QStringLiteral("order_id"))
              && !line.contains(QStringLiteral("broker")),
          "no order or broker id: it has no identity oms/ did not give it");
    check(line.contains(QStringLiteral("\"limit_paise\":5711490")),
          "price is integer PAISE, so no double crosses the boundary");

    // -----------------------------------------------------------------------
    // 4. Escaping matches what the reader will undo.
    // -----------------------------------------------------------------------
    std::printf("\n[4] escaping\n");
    check(intent_escape(QStringLiteral("A\"B")) == QStringLiteral("A\\\"B"),
          "a quote is escaped");
    check(intent_escape(QStringLiteral("A\\B")) == QStringLiteral("A\\\\B"),
          "a backslash is escaped");
    check(intent_escape(QStringLiteral("NIFTY BANK"))
              == QStringLiteral("NIFTY BANK"),
          "and an ordinary symbol passes through untouched");

    std::printf("\n%s -- %d failing check(s)\n",
                failures == 0 ? "PASS" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
