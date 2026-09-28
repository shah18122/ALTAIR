// P25-04 acceptance tests for desktop/order_ticket.hpp, extended by CX02-B3/B4.
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
// Tests 5-9 are CX02-B3/B4: findings C17-010 (the confirmation hid the price
// and a Rs 24,000 default limit survived every instrument change), C17-011
// (a failed write was reported as PENDING), C13-005's writer half (the next
// append glued itself onto a torn tail), C17-013 and C17-019.
//
// Needs a QApplication: tests 8 and 9 construct the ticket widget.
//
// No check description here may contain the substring FAIL.

#include "../order_ticket.hpp"

#include <QApplication>
#include <QDate>
#include <QDir>
#include <QFile>
#include <QLockFile>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <QTime>
#include <QTimeZone>

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

QByteArray slurp(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

} // namespace

using namespace altair::ui;

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    std::printf("P25-04 / CX02-B3/B4 -- the order ticket writer\n");

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

    // -----------------------------------------------------------------------
    // 5. C17-011 / C13-005 writer half: the append is checked and starts a
    //    new line after a torn tail.
    // -----------------------------------------------------------------------
    std::printf("\n[5] append_after_torn_tail_starts_a_new_line\n");
    const QString dir = QDir::current().absoluteFilePath(
        QStringLiteral("cx02_ticket_%1").arg(QCoreApplication::applicationPid()));
    QDir().mkpath(dir);
    const QString q = dir + QStringLiteral("/order_intents.jsonl");
    {
        QFile f(q);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write("{\"v\":1,\"id\":\"torn");            // a crashed writer
        }
    }
    check(append_intent(q, a) == AppendResult::Ok,
          "an append after a torn tail succeeds");
    const QByteArray after = slurp(q);
    check(after == QByteArray("{\"v\":1,\"id\":\"torn\n") + vec[0].toUtf8()
                       + QByteArray("\n"),
          "and puts the record on its OWN line -- it used to be glued onto the"
          " torn tail, and the pair was rejected together");
    check(append_intent(q, b) == AppendResult::Ok
              && slurp(q).endsWith(QByteArray("\n") + vec[1].toUtf8() + "\n")
              && !slurp(q).contains("\n\n"),
          "a second append adds no blank line: the file already ended in one");

    std::printf("\n[6] append_reports_failure_instead_of_pending\n");
    check(append_intent(dir, a) == AppendResult::OpenFailed,
          "a queue path that cannot be opened reports OpenFailed");
    {
        QLockFile held(q + QStringLiteral(".lock"));
        const bool got = held.tryLock(0);
        check(got && append_intent(q, c) == AppendResult::LockTimeout,
              "a queue whose lock another writer holds reports LockTimeout"
              " after the wait, and writes nothing");
    }
    check(!slurp(q).contains("MKT1"),
          "(the refused record is not in the file)");
    check(AppendResult{} == AppendResult::Unset,
          "a zeroed AppendResult reads as Unset, not as Ok");
    QFile::remove(q);
    QFile::remove(q + QStringLiteral(".lock"));

    // -----------------------------------------------------------------------
    // 7. C17-019: `at` carries its offset.
    // -----------------------------------------------------------------------
    std::printf("\n[7] at_carries_offset\n");
    const QDateTime utc(QDate(2026, 9, 8), QTime(12, 52, 31), QTimeZone::UTC);
    check(intent_timestamp(utc) == QStringLiteral("2026-09-08T12:52:31Z"),
          "a UTC instant is written with Z");
    const QDateTime ist(QDate(2026, 9, 8), QTime(18, 22, 31),
                        QTimeZone::fromSecondsAheadOfUtc(19'800));
    check(intent_timestamp(ist) == QStringLiteral("2026-09-08T12:52:31Z"),
          "and 18:22:31+05:30 is written as the same instant in UTC -- never as"
          " a local time with no offset");

    // -----------------------------------------------------------------------
    // 8. C17-010: the confirmation shows every field that decides the order.
    // -----------------------------------------------------------------------
    std::printf("\n[8] confirmation_names_price_type_product_validity\n");
    const QString conf = confirmation_text(a, QStringLiteral("REQUEST BUY"));
    check(conf.contains(QStringLiteral("LIMIT"))
              && conf.contains(QStringLiteral("57114.90"))
              && conf.contains(QStringLiteral("5711490 paise")),
          "the limit price is on the confirmation, in rupees and in paise --"
          " it used to be the one field the safety step never showed");
    check(conf.contains(QStringLiteral("NRML"))
              && conf.contains(QStringLiteral("DAY"))
              && conf.contains(QStringLiteral("NSE"))
              && conf.contains(QStringLiteral("NIFTY BANK"))
              && conf.contains(QStringLiteral("BUY 1 lot")),
          "and so are product, validity, exchange, symbol, side and lots");
    const QString mconf = confirmation_text(c, QStringLiteral("REQUEST BUY"));
    check(mconf.contains(QStringLiteral("MARKET"))
              && !mconf.contains(QStringLiteral("paise)"))
              && mconf.contains(QStringLiteral("IOC")),
          "a MARKET request says MARKET and shows no limit");
    IntentDraft amp = a;
    amp.symbol = QStringLiteral("M&M<b>");
    check(confirmation_text(amp, QStringLiteral("REQUEST BUY"))
              .contains(QStringLiteral("M&amp;M&lt;b&gt;")),
          "a symbol is HTML-escaped, so the dialog shows the name it will send");

    // -----------------------------------------------------------------------
    // 9. C17-010 / C17-013: the limit does not outlive its instrument.
    // -----------------------------------------------------------------------
    std::printf("\n[9] limit_cleared_on_contract_change\n");
    {
        OrderTicket t(Role::Admin, QStringLiteral("smit"));
        check(t.limit_paise() == 0,
              "a fresh ticket has NO limit -- it used to start at 2,400,000"
              " paise, Rs 24,000");
        t.set_contract(99990001u, QStringLiteral("TESTOPTA"), 65, 5,
                       QStringLiteral("NFO"));
        t.set_price(10'000);
        check(t.limit_paise() == 10'000 && t.last_snap() == TickSnap::None,
              "an on-tick clicked price is taken as is");
        t.set_contract(99990001u, QStringLiteral("TESTOPTA"), 65, 5,
                       QStringLiteral("NFO"));
        check(t.limit_paise() == 10'000,
              "re-selecting the SAME contract keeps its limit");
        t.set_contract(99990002u, QStringLiteral("TESTOPTB"), 65, 5,
                       QStringLiteral("NFO"));
        check(t.limit_paise() == 0,
              "selecting a DIFFERENT contract clears the limit -- the Rs 24,000"
              " default used to survive every change");

        t.set_price(12'347);
        check(t.limit_paise() == 12'345 && t.last_snap() == TickSnap::Down,
              "an off-tick price snaps down, and the snap is remembered");
        check(snap_too_aggressive(false, TickSnap::Down)
                  && !snap_too_aggressive(true, TickSnap::Down)
                  && snap_too_aggressive(true, TickSnap::Up),
              "a DOWN snap is refused for a SELL and allowed for a BUY; an UP"
              " snap the other way round");

        const int before = t.limit_paise();
        t.set_price(5'000'000'000LL);
        check(t.limit_paise() == before,
              "a price above the control's range is REFUSED -- it used to be"
              " narrowed to int and clamped");

        t.set_instrument(99990002u, QStringLiteral("TESTOPTB-RENAMED"));
        check(t.current_symbol() == QStringLiteral("TESTOPTB-RENAMED"),
              "a known token given a new name is renamed, not left stale");

        OrderTicket unnamed(Role::Admin, QStringLiteral("smit"));
        unnamed.set_contract(99990003u, QString(), 75, 5,
                             QStringLiteral("NFO"));
        check(unnamed.symbol_is_invented(99990003u)
                  && unnamed.current_symbol()
                         == QStringLiteral("token 99990003"),
              "an unnamed token gets an explicitly invented display label");
        check(refuse_request(75, 5, QStringLiteral("NFO"), true, 0, true,
                            TickSnap::None, unnamed.current_symbol(),
                            QStringLiteral("smit"),
                            unnamed.symbol_is_invented(99990003u))
                  == TicketRefusal::InventedSymbol,
              "invented_symbol_refused: the ticket label cannot become a tradingsymbol");
    }

    // -----------------------------------------------------------------------
    // 10. CX02-B4c (R-AB-044). Every refusal, as a pure decision. All of these
    //     used to live inside submit(), behind a modal dialog, so deleting any
    //     of them kept the suite green.
    // -----------------------------------------------------------------------
    std::printf("\n[10] every reason the ticket refuses to write a request\n");
    const QString sym = QStringLiteral("NIFTY26SEPFUT");
    const QString who = QStringLiteral("smit");
    const auto refuse = [&](qint64 lot, qint64 tick, const QString& exch,
                            bool market, int limit, bool buy, TickSnap snap,
                            const QString& symbol, const QString& by,
                            bool invented) {
        return refuse_request(lot, tick, exch, market, limit, buy, snap,
                              symbol, by, invented);
    };
    check(refuse(75, 5, QStringLiteral("NFO"), false, 12'345, true,
                 TickSnap::None, sym, who, false) == TicketRefusal::None,
          "a complete, on-tick LIMIT request is accepted");
    check(refuse(0, 0, QString(), false, 12'345, true, TickSnap::None, sym,
                 who, false) == TicketRefusal::NoSpec,
          "no_spec_refused: a contract with no lot, tick or exchange");
    check(refuse(75, 5, QStringLiteral("NFO"), false, 0, true, TickSnap::None,
                 sym, who, false) == TicketRefusal::NoLimit,
          "unset_limit_refused: a LIMIT order with no limit price");
    check(refuse(75, 5, QStringLiteral("NFO"), true, 0, true, TickSnap::None,
                 sym, who, false) == TicketRefusal::None,
          "while a MARKET order needs none");
    check(refuse(75, 5, QStringLiteral("NFO"), false, 12'347, true,
                 TickSnap::None, sym, who, false) == TicketRefusal::OffTick,
          "off_tick_limit_refused: a limit that is not a multiple of the tick");
    check(refuse(75, 5, QStringLiteral("NFO"), false, 12'345, false,
                 TickSnap::Down, sym, who, false)
              == TicketRefusal::AggressiveSnap,
          "aggressive_snap_refused: a SELL priced by a DOWN snap");
    check(refuse(75, 5, QStringLiteral("NFO"), false, 12'345, true,
                 TickSnap::Down, sym, who, false) == TicketRefusal::None,
          "and the same snap is fine for a BUY");
    check(refuse(75, 5, QStringLiteral("nfo"), false, 12'345, true,
                 TickSnap::None, sym, who, false) == TicketRefusal::BadExchange,
          "an exchange outside oms/'s class is refused");
    check(refuse(75, 5, QStringLiteral("NFO"), false, 12'345, true,
                 TickSnap::None, sym, who, true)
              == TicketRefusal::InventedSymbol,
          "invented_symbol_refused: a name this window made up for a token");

    // -----------------------------------------------------------------------
    // 11. CX02-B4c (R-AB-043, R-AB-006). The writer refuses what the reader
    //     refuses, so a request logged PENDING is one oms/ can act on.
    // -----------------------------------------------------------------------
    std::printf("\n[11] a_field_the_reader_would_refuse_is_refused_here\n");
    check(refuse(75, 5, QStringLiteral("NFO"), false, 12'345, true,
                 TickSnap::None, QStringLiteral("NIF\nTY"), who, false)
              == TicketRefusal::BadSymbol,
          "a NEWLINE in the symbol is refused -- it used to be written, and it"
          " splits one record into two lines that are BOTH quarantined");
    check(refuse(75, 5, QStringLiteral("NFO"), false, 12'345, true,
                 TickSnap::None, QStringLiteral("NIF\tTY"), who, false)
              == TicketRefusal::BadSymbol,
          "and so is a tab");
    check(refuse(75, 5, QStringLiteral("NFO"), false, 12'345, true,
                 TickSnap::None, sym, QStringLiteral("smit\nshah"), false)
              == TicketRefusal::BadUser,
          "a NEWLINE in the user field is refused before it can split a record");
    check(refuse(75, 5, QStringLiteral("NFO"), false, 12'345, true,
                 TickSnap::None, sym, QStringLiteral("smit\tshah"), false)
              == TicketRefusal::BadUser,
          "a tab in the user field is refused by the same B1 grammar");
    check(refuse(75, 5, QStringLiteral("NFO"), false, 12'345, true,
                 TickSnap::None, QString(65, QLatin1Char('A')), who, false)
              == TicketRefusal::BadSymbol,
          "a symbol longer than oms/'s 64 is refused here, not there");
    check(refuse(75, 5, QStringLiteral("NFO"), false, 12'345, true,
                 TickSnap::None, sym, QStringLiteral("Smit Shah"), false)
              == TicketRefusal::BadUser,
          "a user name with a space is refused -- every request from that"
          " login would otherwise be quarantined on arrival");
    check(intent_symbol_ok(QStringLiteral("M&M"))
              && intent_symbol_ok(QStringLiteral("BAJAJ-AUTO"))
              && intent_symbol_ok(QStringLiteral("NIFTY BANK")),
          "while the tradingsymbols the master really carries pass");
    check(intent_symbol_ok(QString(64, QLatin1Char('A')))
              && intent_user_ok(QString(64, QLatin1Char('a'))),
          "the reader's 64-character symbol and user limits are inclusive");
    check(!intent_symbol_ok(QStringLiteral(" NIFTY"))
              && !intent_symbol_ok(QString())
              && !intent_symbol_ok(QStringLiteral("NIFTY "))
              && !intent_symbol_ok(QStringLiteral("NIF\"TY"))
              && !intent_symbol_ok(QStringLiteral("NIF\\TY"))
              && !intent_symbol_ok(QString(65, QLatin1Char('A'))),
          "leading/trailing spaces, empty, escaped bytes and 65 chars are refused");
    check(intent_user_ok(QStringLiteral("smit.shah@altair"))
              && intent_user_ok(QStringLiteral("smit_1-2@x.y"))
              && !intent_user_ok(QStringLiteral("smit shah"))
              && !intent_user_ok(QString(65, QLatin1Char('a')))
              && !intent_user_ok(QString::fromUtf8("smité")),
          "the user class matches oms/'s: dots, underscores, hyphens and @");

    // -----------------------------------------------------------------------
    // 12. CX02-B4c (R-AB-042). The instant on the record is the instant it
    //     was written, not the instant the dialog opened.
    // -----------------------------------------------------------------------
    std::printf("\n[12] at_is_stamped_at_the_write_not_at_the_dialog\n");
    IntentDraft st;
    st.token = 260105u;
    const QDateTime opened(QDate(2026, 9, 16), QTime(10, 0, 0), QTimeZone::UTC);
    const QDateTime written = opened.addSecs(45);
    const IntentDraft s1 = stamp_draft(st, opened);
    const IntentDraft s2 = stamp_draft(st, written);
    check(s1.at == QStringLiteral("2026-09-16T10:00:00Z")
              && s2.at == QStringLiteral("2026-09-16T10:00:45Z"),
          "the stamp is a function of the instant it is given");
    check(s1.id != s2.id && s2.id.endsWith(QStringLiteral("-260105")),
          "and the id carries that instant too, with the token");
    std::printf("        submit() calls stamp_draft AFTER the confirmation is"
                " accepted;\n        45 s in a dialog used to make a 30 s TTL"
                " quarantine the request.\n");

    QDir(dir).removeRecursively();
    std::printf("\n%s -- %d failing check(s)\n",
                failures == 0 ? "PASS" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
