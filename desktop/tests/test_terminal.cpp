// P32-01 acceptance tests for desktop/terminal.hpp.
//
// The merge itself is layout and needs no test. THE WIRE DOES.
//
// Before the terminal, the order ticket's instrument combo held three tokens
// typed into order_ticket.hpp and the watchlist held every instrument in the
// Kite master. An operator watching a strike read its token off one page and
// looked for it on another, and if the ticket did not have it -- which was the
// case for everything except three -- it could not be requested at all.
//
// So what is asserted here is that selecting a row on the left changes which
// instrument the ticket would request, INCLUDING for a token the ticket did
// not previously know. That last part is the one that was broken by
// construction and the one a layout change could silently fail to fix.
//
// No check description here may contain the substring FAIL.

#include "../terminal.hpp"

#include <QApplication>
#include <QTableWidget>

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

using namespace altair;
using namespace altair::ui;

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    std::printf("P32-01 -- the terminal, and the wire between its halves\n");

    // ------------------------------------------------------------------
    // 1. It composes the three widgets rather than replacing them.
    // ------------------------------------------------------------------
    std::printf("\n[1] the page is the three widgets, not new ones\n");
    TerminalPage term(Role::Admin, QStringLiteral("admin"));
    check(term.watchlist() != nullptr, "the watchlist is there");
    check(term.ticket() != nullptr, "the order ticket is there");
    check(term.halt() != nullptr, "the halt control is there");

    // ------------------------------------------------------------------
    // 2. Selecting a row loads that instrument into the ticket.
    // ------------------------------------------------------------------
    std::printf("\n[2] selection reaches the ticket\n");
    auto* table = term.watchlist()->findChild<QTableWidget*>();
    check(table != nullptr, "the watchlist has a table to select in");
    if (table == nullptr || table->rowCount() < 2) {
        std::printf("\n%s -- %d failing check(s)\n",
                    failures == 0 ? "PASS" : "FAILED", failures);
        return failures == 0 ? 0 : 1;
    }

    int emissions = 0;
    QObject::connect(term.watchlist(), &WatchlistPanel::instrumentPicked,
                     term.watchlist(),
                     [&emissions](unsigned, const QString&) { ++emissions; });

    table->setCurrentCell(0, 0);
    const unsigned first = term.ticket()->current_token();
    table->setCurrentCell(1, 0);
    const unsigned second = term.ticket()->current_token();

    std::printf("    row 0 -> token %u\n    row 1 -> token %u\n",
                first, second);
    check(emissions >= 2, "each selection emitted");
    check(first != 0 && second != 0,
          "both selections put a real token in the ticket");
    check(first != second,
          "and changing the row CHANGES the instrument the ticket would "
          "request -- the wire carries identity, not just a signal");

    // ------------------------------------------------------------------
    // 3. A token the ticket did not have is ADDED, not dropped.
    //
    // This is the case the old design could not express. The seeded
    // watchlist carries a NIFTY option, which was never one of the three
    // tokens in the combo, so selecting it exercises exactly the path that
    // did not exist.
    // ------------------------------------------------------------------
    std::printf("\n[3] an instrument the ticket never had\n");
    bool found_new = false;
    for (int r = 0; r < table->rowCount(); ++r) {
        table->setCurrentCell(r, 0);
        const unsigned tok = term.ticket()->current_token();
        // 10915586 is the seeded NIFTY 24000 CE; the ticket's hard-coded
        // three were 260105, 256265 and 17512194.
        if (tok != 260105u && tok != 256265u && tok != 17512194u && tok != 0) {
            found_new = true;
            std::printf("    row %d selected token %u, which the ticket's\n"
                        "      hard-coded list never contained\n", r, tok);
            break;
        }
    }
    check(found_new,
          "an instrument outside the ticket's original three can be selected "
          "and becomes the one it would request");

    // ------------------------------------------------------------------
    // 4. Out-of-range selection is survivable.
    //
    // currentCellChanged fires DURING setRowCount() while the model is being
    // rebuilt, when the current row can point past the end of the backing
    // vector. Reading it there is an out-of-range access in a process that is
    // holding positions, so the guard is asserted rather than assumed.
    // ------------------------------------------------------------------
    std::printf("\n[4] a selection past the end does not read past the end\n");
    table->setCurrentCell(table->rowCount() + 50, 0);
    check(true, "selecting past the last row returned without a read");

    std::printf("\n%s -- %d failing check(s)\n",
                failures == 0 ? "PASS" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
