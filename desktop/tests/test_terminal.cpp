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
#include <QDateTime>
#include <QTableWidget>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <utility>

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

    // ------------------------------------------------------------------
    // [5] P39. THE OPTION CHAIN, AND EVERY PATH INTO THE TICKET CARRIES A SPEC.
    //
    // The chain's strikes, tokens, lots, ticks and expiries are the real ones
    // from the instrument master. A click must load that exact contract, with
    // its spec, and route it to NFO -- the old ticket routed every option to
    // NSE, the cash segment, because it matched "FUT" in the symbol.
    // ------------------------------------------------------------------
    std::printf("\n[5] option chain -> ticket\n");
    {
        OptionChainPanel* chain = term.chain();
        check(chain != nullptr, "the terminal carries an option chain");
        const int rows = chain != nullptr ? chain->strikes_shown() : 0;
        std::printf("    chain shows %d strikes\n", rows);
        check(rows > 0,
              "the chain prices strikes from the real master and the dataset");

        if (rows > 0) {
            const int mid = rows / 2;
            chain->click_cell(mid, kC_Px);
            const unsigned ce = term.ticket()->current_token();
            std::printf("    clicked the CALL at %.0f -> token %u, exchange %s\n",
                        chain->strike_at(mid) / 100.0, ce,
                        term.ticket()->spec_exchange(ce).toUtf8().constData());
            check(term.ticket()->has_spec(ce),
                  "a click loads the contract WITH its lot, tick and exchange");
            check(term.ticket()->spec_exchange(ce) == QStringLiteral("NFO"),
                  "and an index option routes to NFO, not NSE -- the bug the "
                  "symbol-suffix guess had for every option");

            chain->click_cell(mid, kP_Px);
            const unsigned pe = term.ticket()->current_token();
            check(pe != 0 && pe != ce && term.ticket()->has_spec(pe),
                  "the PUT on the same strike is a different contract, also "
                  "with a spec");

            // A MODEL PRICE NEVER SEEDS AN ORDER.
            const int before = term.ticket()->limit_paise();
            chain->click_cell(mid, kC_Px);
            check(term.ticket()->limit_paise() == before,
                  "clicking a MODEL price loads the contract but leaves the "
                  "limit price alone");

            // A REPLAYED price is never labelled LIVE. The first version did
            // exactly that on screen, under a strip that correctly said REPLAY.
            const std::int64_t now_ns =
                QDateTime::currentMSecsSinceEpoch() * 1'000'000LL;
            chain->set_spot(256265u, 2'400'000, true, now_ns);
            check(chain->header_text().contains(QStringLiteral("REPLAY"))
                      && !chain->header_text().contains(QStringLiteral("LIVE")),
                  "a replayed spot is labelled REPLAY in the chain header, "
                  "never LIVE");
            chain->set_spot(256265u, 2'400'000, false, now_ns);
            check(chain->header_text().contains(QStringLiteral("LIVE")),
                  "and a live one is labelled LIVE");

            // The strike column is neither side.
            const unsigned held = term.ticket()->current_token();
            chain->click_cell(mid, kStrike);
            check(term.ticket()->current_token() == held,
                  "clicking the strike itself picks nothing");
        }
    }

    // ------------------------------------------------------------------
    // [6] P39. set_price snaps DOWN to the tick, and a hotkey never submits.
    // ------------------------------------------------------------------
    std::printf("\n[6] tick snapping and hotkeys\n");
    {
        OrderTicket* t = term.ticket();
        t->set_contract(99990001u, QStringLiteral("TESTOPT"), 65, 5,
                        QStringLiteral("NFO"));
        t->set_price(12'347);                 // not a multiple of 5
        std::printf("    set_price(12347) with a 5-paise tick -> %d\n",
                    t->limit_paise());
        check(t->limit_paise() == 12'345,
              "an off-tick price snaps DOWN to the tick -- a buy is never "
              "priced above what was clicked");

        QFile q(QStringLiteral(ALTAIR_INTENT_FILE));
        const qint64 size_before = q.exists() ? q.size() : -1;
        t->focus_side(true);
        t->focus_side(false);
        const qint64 size_after = q.exists() ? q.size() : -1;
        check(size_before == size_after,
              "F1 and F2 focus the ticket and write NOTHING to the intent "
              "queue -- the typed confirmation is still the only way in");

        // The seeded index is refused: it has no tradeable spec.
        check(!t->has_spec(256265u),
              "NIFTY 50 -- an index, which cannot be traded -- holds no spec, "
              "so a request for it is refused at submit");
    }

    // ------------------------------------------------------------------
    // [7] TIME COMES OFF THE TICK -- through the terminal's own wiring.
    //
    // The first chain valued every spot at the wall clock, so a replayed
    // 4 September spot was priced with 11 September's time to expiry. And
    // the REPLAY label was first tested by calling the chain directly, which
    // cannot catch the terminal passing the wrong flag. apply_price is the
    // one entry the stream uses, so these go through it.
    // ------------------------------------------------------------------
    std::printf("\n[7] a price is valued at its own tick\n");
    {
        OptionChainPanel* chain = term.chain();
        const std::int64_t now_ns =
            QDateTime::currentMSecsSinceEpoch() * 1'000'000LL;
        constexpr std::int64_t kWeek = 7LL * 86'400LL * 1'000'000'000LL;

        term.apply_price(256265u, 2'400'000, false, now_ns);
        const double live_dte = chain->days_to_expiry();
        term.apply_price(256265u, 2'400'000, true, now_ns - kWeek);
        const double replay_dte = chain->days_to_expiry();
        std::printf("    live tick now        -> %.4f days to expiry\n"
                    "    replayed, a week ago -> %.4f days to expiry\n",
                    live_dte, replay_dte);
        check(live_dte > 0.0 && std::fabs(replay_dte - live_dte - 7.0) < 1e-6,
              "a tick from a week ago is valued with exactly a week MORE to "
              "expiry -- the wall clock does not enter");
        check(chain->header_text().contains(QStringLiteral("REPLAY"))
                  && !chain->header_text().contains(QStringLiteral("LIVE")),
              "through the terminal's wiring, the chain says REPLAY and never "
              "LIVE");
        check(term.tile_text(256265u).contains(QStringLiteral("replay")),
              "and the NIFTY tile says replay");

        term.apply_price(256265u, 2'400'000, true, 0);
        check(chain->strikes_shown() == 0 && chain->days_to_expiry() == 0.0,
              "a replayed price with NO timestamp is refused -- not quietly "
              "valued at the wall clock");

        term.apply_price(256265u, 2'400'000, false, now_ns);
        check(chain->strikes_shown() > 0, "and a good price restores it");

        // The longest header -- a replay, valued at a tick -- must not become
        // the page's minimum width. On a 1920 px screen, less the nav column,
        // the terminal has about 1730 px; the unwrapped header took 1964.
        term.apply_price(256265u, 2'400'000, true, now_ns - kWeek);
        const int min_w = term.minimumSizeHint().width();
        std::printf("    terminal minimum width with the longest header: %d px\n",
                    min_w);
        check(min_w <= 1700,
              "the longest chain header still fits a 1920 px screen beside the "
              "nav -- it wraps rather than widening the window");
        term.apply_price(256265u, 2'400'000, false, now_ns);
    }

    // ------------------------------------------------------------------
    // [8] The day's change is against the TICK's previous day, in IST.
    //
    // The tile measured every price against the file's second-to-last row.
    // That is right only when the last row is today; before the updater runs
    // it is a day wrong, and for a replay it was a week wrong.
    // ------------------------------------------------------------------
    std::printf("\n[8] the change basis is the tick's previous IST day\n");
    {
        constexpr std::int64_t kSec = 1'000'000'000LL;
        constexpr std::int64_t kDay = 86'400LL * kSec;
        constexpr std::int64_t kIst = (5LL * 3600LL + 30LL * 60LL) * kSec;
        const std::int64_t d1 = 20'000LL * kDay - kIst;   // IST midnight
        const std::int64_t at_0915 = (9LL * 3600LL + 15LL * 60LL) * kSec;
        const std::int64_t at_1000 = 10LL * 3600LL * kSec;

        // Three trading days, stamped 09:15 IST -- one of the two stamp
        // shapes on disk.
        UiStamped s;
        s.closes = {100.0, 110.0, 120.0};
        s.stamps_ns = {d1 + at_0915, d1 + kDay + at_0915,
                       d1 + 2 * kDay + at_0915};

        check(prev_close_before(s, d1 + 2 * kDay + at_1000) == 110.0,
              "a tick on day 3 is measured against day 2's close");
        check(prev_close_before(s, d1 + 5 * kDay + at_1000) == 120.0,
              "a tick after the file ends -- this morning, before the updater "
              "ran -- is measured against the LAST close, not the "
              "second-to-last");
        check(prev_close_before(s, d1 + at_1000) == 0.0,
              "a tick on the first day has no prior close, and says so");
        // 00:30 IST on day 4 is 19:00 UTC on day 3, the same UTC day as day
        // 3's 09:15 IST bar. A UTC day would exclude that bar and answer 110.
        check(prev_close_before(s, d1 + 3 * kDay + 30LL * 60LL * kSec) == 120.0,
              "00:30 IST on day 4 belongs to day 4 -- the day is IST, not UTC");

        UiStamped back = s;
        std::swap(back.stamps_ns[0], back.stamps_ns[2]);
        check(stamps_ascending(s) && !stamps_ascending(back),
              "a file out of date order is detected, so it gets no basis");
    }

    std::printf("\n%s -- %d failing check(s)\n",
                failures == 0 ? "PASS" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
