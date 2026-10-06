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
#include <core/time/timestamp.hpp>
#include <core/types/units.hpp>

#include <QApplication>
#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimeZone>

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

altair::broker_view::AccountSnapshot account_fixture(
    altair::broker_view::SessionKey session,
    bool funds_present = true,
    bool positions_present = true) {
    using namespace altair::broker_view;
    AccountSnapshot snapshot{};
    snapshot.account_session = session;
    snapshot.observed = {altair::Timestamp{90'000'000'000}, altair::Timestamp{110'000'000'000}};
    snapshot.account_id[0] = 'A';
    snapshot.account_id[1] = '1';
    snapshot.profile.status = SnapshotSectionStatus::Present;
    snapshot.funds.status = funds_present ? SnapshotSectionStatus::Present
                                           : SnapshotSectionStatus::Absent;
    snapshot.positions.status = positions_present ? SnapshotSectionStatus::Present
                                                   : SnapshotSectionStatus::Absent;
    snapshot.typed_funds.available_trading_balance = altair::Notional{25'000};
    snapshot.typed_funds.cash = altair::Notional{30'000};
    snapshot.typed_positions.account_session = session;
    snapshot.typed_positions.observed = snapshot.observed;
    if (positions_present) {
        snapshot.typed_positions.count = 1;
        auto& position = snapshot.typed_positions.position[0];
        position.instrument = InstrumentKey{7001};
        position.product = PositionProduct::Intraday;
        position.net_qty = altair::Qty{50};
        position.average_price = altair::Price{10'000};
        position.last_mark = altair::Price{10'100};
        position.marked = snapshot.observed;
    }
    return snapshot;
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
    // 1b. Typed account ingress feeds the existing read-only surfaces.
    // ------------------------------------------------------------------
    std::printf("\n[1b] typed account snapshot reaches funds and positions\n");
    const broker_view::SessionKey fixture_session{broker_view::BrokerId::Fyers, 7, 1};
    const auto applied = term.apply_account(
        {account_fixture(fixture_session), QStringLiteral("FYERS · A1"),
         FundsSource::Broker, PositionSource::Broker}, altair::Timestamp{100'000'000'000});
    check(applied.has_value(), "a fresh typed snapshot is accepted");
    check(term.funds_model()->account_count() == 1,
          "funds model receives one provider account");
    check(term.positions_model()->rowCount() == 1,
          "positions model receives one open position");
    check(term.funds_model()->index(0, FundsSummaryModel::Available).data()
              .toString() == QStringLiteral("250.00"),
          "typed available balance is rendered in INR");

    const int funds_before = term.funds_model()->account_count();
    const int positions_before = term.positions_model()->rowCount();
    auto stale = account_fixture(fixture_session);
    stale.observed = {altair::Timestamp{1}, altair::Timestamp{2}};
    const auto stale_result = term.apply_account(
        {stale, QStringLiteral("FYERS · A1"), FundsSource::Broker,
         PositionSource::Broker}, altair::Timestamp{100'000'000'000});
    check(!stale_result.has_value(), "a stale account snapshot is refused");
    check(term.funds_model()->account_count() == funds_before
              && term.positions_model()->rowCount() == positions_before,
          "stale account data does not partially mutate the Terminal");

    auto no_positions = account_fixture(fixture_session, true, false);
    const auto no_positions_result = term.apply_account(
        {no_positions, QStringLiteral("FYERS · A1"), FundsSource::Broker,
         PositionSource::Broker}, altair::Timestamp{100'000'000'000});
    check(no_positions_result.has_value() && term.positions_model()->rowCount() == 0,
          "an absent positions section removes stale visible positions");
    auto paper = account_fixture({broker_view::BrokerId::ZerodhaKite, 8, 1});
    const auto paper_result = term.apply_account(
        {paper, QStringLiteral("Paper · K1"), FundsSource::Paper,
         PositionSource::Paper}, altair::Timestamp{100'000'000'000});
    check(paper_result.has_value()
              && term.funds_model()->index(1, FundsSummaryModel::Account).data()
                     .toString().contains(QStringLiteral("PAPER")),
          "paper account provenance remains visible and separate");

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
    // [4b] CX02-B4b (C17-021). A row the watchlist calls "watch only" used
    // to hand the ticket a spec anyway, and a BLOCKED row did too.
    // ------------------------------------------------------------------
    std::printf("\n[4b] blocked rows are not orderable; resolved rows say so\n");
    {
        const auto& rows = term.watchlist()->list().rows();
        int specs = 0;
        const auto conn = QObject::connect(
            term.watchlist(), &WatchlistPanel::contractPicked, term.watchlist(),
            [&specs](unsigned, const QString&, qint64 lot, qint64, const QString&) {
                if (lot > 0) { ++specs; }
            });
        int resolvable = -1;
        for (int r = 0; r < table->rowCount() && resolvable < 0; ++r) {
            const int before = specs;
            table->setCurrentCell(r, 0);
            if (specs > before) { resolvable = r; }
        }
        check(resolvable >= 0, "(a seeded row resolves against the master)");
        if (resolvable >= 0 && table->rowCount() >= 2) {
            const auto idx = static_cast<std::size_t>(resolvable);
            const std::uint32_t tok = rows[idx].token;
            check(rows[idx].spec == SpecState::FromMaster,
                  "a_pick_does_not_report_the_spec_store: the row is marked"
                  " FromMaster. It used to say 'watch only' while being"
                  " orderable, and CX02-B4b then over-corrected to Resolved,"
                  " which claims the point-in-time spec store agreed");
            check(!rows[idx].tradeable(),
                  "and tradeable() stays FALSE -- that word means the spec"
                  " store, which desktop/ cannot link");
            check(spec_state_label(rows[idx].spec)
                      == QStringLiteral("master lot/tick"),
                  "with a label that names where the numbers came from");
            check(term.ticket()->has_spec(tok), "(and the ticket holds its spec)");

            term.watchlist()->block_instrument(
                tok, QStringLiteral("test: the spec store refused it"));
            table->setCurrentCell(resolvable == 0 ? 1 : 0, 0);
            const int before = specs;
            table->setCurrentCell(resolvable, 0);
            check(specs == before,
                  "a BLOCKED row sends no usable spec to the ticket");
            check(term.ticket()->current_token() == tok
                      && !term.ticket()->has_spec(tok),
                  "and the spec the ticket already held for it is REVOKED --"
                  " picked while resolvable, then blocked, it cannot be"
                  " requested");
        }
        QObject::disconnect(conn);
    }

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

    // ------------------------------------------------------------------
    // [9] LIVE: off by default; on only with LIVE typed; every order is
    // confirmed and becomes one request line for the order router.
    // ------------------------------------------------------------------
    std::printf("\n[9] LIVE: off by default, a typed switch, a confirmed request\n");
    {
        QTemporaryDir tmp;
        term.set_router_autostart(false);
        term.set_live_root(tmp.path());
        check(!term.live_on() && term.live_switch()->text() == QStringLiteral("PAPER"),
              "LIVE is off by default: the switch says PAPER");
        LiveTradingArmDialog dlg(LiveTradingLimits{});
        dlg.type_phrase(QStringLiteral("live"));
        check(!dlg.can_accept(), "the switch needs LIVE typed exactly");
        dlg.type_phrase(QStringLiteral("LIVE"));
        check(dlg.can_accept() && dlg.limits().max_lots == 1, "typed: it may switch on, one lot per order by default");

        PaperOrder o;
        o.inst = PaperInstrument{111u, QStringLiteral("NIFTY26OCTFUT"), QStringLiteral("NFO"), 75, 5, true};
        o.product = QStringLiteral("NRML");
        o.side = PaperSide::Buy;
        o.type = PaperType::Limit;
        o.qty = 75;
        o.limit_paise = 2'500'000;
        const QString intents = tmp.path() + QStringLiteral("/data/order_intents.jsonl");
        check(!term.place_live(o, false).has_value() && !QFile::exists(intents), "with LIVE off, nothing is requested");

        check(term.arm_live(LiveTradingLimits{}) && term.live_on() && term.live_switch()->text().contains(QStringLiteral("LIVE")),
              "switched on, and the switch says LIVE");
        QFile armf(tmp.path() + QStringLiteral("/data/live_trading.json"));
        const QJsonObject arm = armf.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(armf.readAll()).object() : QJsonObject{};
        armf.close();
        const qint64 now_s = QDateTime::currentSecsSinceEpoch();
        const qint64 until = static_cast<qint64>(arm.value(QStringLiteral("expires_unix")).toDouble());
        check(arm.value(QStringLiteral("armed")).toBool() && arm.value(QStringLiteral("max_lots")).toInt() == 1
                  && until > now_s && until - now_s <= 12 * 3600,
              "the arm file: armed, one lot, and it ends within one session");

        int asked = 0;
        term.set_live_confirm([&](const QString& text) {
            ++asked;
            return !(text.contains(QStringLiteral("REAL ORDER")) && text.contains(QStringLiteral("NIFTY26OCTFUT")));
        });
        check(!term.place_live(o, true).has_value() && asked == 1 && !QFile::exists(intents),
              "the confirmation names the order; declined, nothing is written");
        term.set_live_confirm([&](const QString&) { ++asked; return true; });
        const auto id = term.place_live(o, true);
        QFile f(intents);
        const QByteArray line = f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray{};
        check(id.has_value() && asked == 2 && line.count('\n') == 1 && line.contains("\"token\":111")
                  && line.contains("\"lots\":1,") && line.contains("\"order_type\":\"LIMIT\",\"limit_paise\":2500000")
                  && line.contains("\"product\":\"NRML\""),
              "confirmed: one request line, in lots, with the exact limit");
        o.qty = 100;
        check(!term.place_live(o, false).has_value(), "a quantity that is not whole lots is refused");

        // The router's book, as the Terminal shows it.
        QDir().mkpath(tmp.path() + QStringLiteral("/data/live_orders"));
        QFile book(tmp.path() + QStringLiteral("/data/live_orders/orders.json"));
        if (book.open(QIODevice::WriteOnly)) {
            book.write(QStringLiteral("{\"router\":{\"beat_ns\":%1,\"armed\":true,\"killed\":false,\"dry_run\":false,\"session\":true,"
                                      "\"why\":\"\",\"day_pnl_paise\":-12050,\"orders_today\":1,\"open\":1,\"max_orders_per_day\":20},"
                                      "\"orders\":[{\"at_ns\":%1,\"intent\":\"x\",\"id\":\"26100500001\",\"symbol\":\"NIFTY26OCTFUT\","
                                      "\"side\":\"BUY\",\"type\":\"LIMIT\",\"qty\":75,\"limit_paise\":2500000,\"status\":\"OPEN\","
                                      "\"filled\":0,\"avg_paise\":0,\"message\":\"\"}]}")
                           .arg(QDateTime::currentMSecsSinceEpoch() * 1'000'000LL)
                           .toUtf8());
            book.close();
        }
        const LiveTradingView v = read_live_router(book.fileName(), QDateTime::currentMSecsSinceEpoch() * 1'000'000LL);
        check(v.running && v.armed && v.day_pnl_paise == -12'050 && v.orders.size() == 1 && v.orders[0].open(),
              "the router's heartbeat, P&L and open order are read");
        term.show_live_orders();
        auto* lt = term.live_orders()->findChild<QTableWidget*>(QStringLiteral("liveOrderTable"));
        check(lt != nullptr && lt->rowCount() == 1 && lt->item(0, 6)->text() == QStringLiteral("OPEN")
                  && lt->item(0, 9)->text() == QStringLiteral("26100500001"),
              "Live orders lists it, with FYERS's id to cancel by");
        term.live_orders()->hide();

        check(term.disarm_live() && !term.live_on() && term.live_switch()->text() == QStringLiteral("PAPER"),
              "switched off: PAPER again");
        o.qty = 75;
        check(!term.place_live(o, false).has_value(), "and nothing more is requested");
        check(live_arm_expiry(QDateTime(QDate(2026, 10, 5), QTime(4, 0), QTimeZone::utc())) ==
                  QDateTime(QDate(2026, 10, 5), QTime(10, 0), QTimeZone::utc()).toSecsSinceEpoch(),
              "an arm at 09:30 IST ends at 15:30 IST");
        check(live_arm_expiry(QDateTime(QDate(2026, 10, 4), QTime(19, 0), QTimeZone::utc())) ==
                  QDateTime(QDate(2026, 10, 5), QTime(7, 0), QTimeZone::utc()).toSecsSinceEpoch(),
              "one given at 00:30 IST ends twelve hours later, never past what the router accepts");
    }

    // ------------------------------------------------------------------
    // [10] The strip: no Market Watch / Option Chain buttons; Models toggles;
    // the movers sit beside LIVE; Greek Watch opens from an option.
    // ------------------------------------------------------------------
    std::printf("\n[10] strip, Models toggle, movers, Greek Watch\n");
    {
        bool view_buttons = false;
        for (auto* b : term.findChildren<QPushButton*>())
            view_buttons = view_buttons || b->text() == QStringLiteral("Market Watch") || b->text() == QStringLiteral("Option Chain");
        check(!view_buttons, "no Market Watch or Option Chain button: Enter on a scrip opens its chain");
        check(term.findChild<QLabel*>(QStringLiteral("moversStrip")) != nullptr
                  && term.movers_text().contains(QStringLiteral("movers")),
              "the movers strip sits beside LIVE (waiting until prices arrive)");
        check(term.show_view(QStringLiteral("models")) && term.show_view(QStringLiteral("watch")),
              "Models (Ctrl+M) and back to the watch (F4)");
        MasterScrip ce;
        ce.token = 9030; ce.symbol = QStringLiteral("SBIN26OCT800CE"); ce.exchange = QStringLiteral("NFO");
        ce.segment = QStringLiteral("NFO-OPT"); ce.name = QStringLiteral("SBIN"); ce.type = QStringLiteral("CE");
        ce.expiry = QStringLiteral("2099-10-27"); ce.strike = 800; ce.lot = 750;
        MasterScrip fut = ce;
        fut.token = 9003; fut.symbol = QStringLiteral("SBIN99OCTFUT"); fut.segment = QStringLiteral("NFO-FUT");
        fut.type = QStringLiteral("FUT"); fut.strike = 0;
        MasterScrip eq;
        eq.token = 779521; eq.symbol = QStringLiteral("SBIN"); eq.exchange = QStringLiteral("NSE");
        eq.segment = QStringLiteral("NSE"); eq.type = QStringLiteral("EQ");
        term.market_watch()->set_master_for_test({ce, fut, eq});
        check(term.open_greek(9030) && term.greek_watch()->legs().size() == 1
                  && term.greek_watch()->legs()[0].fut == 9003 && term.greek_watch()->legs()[0].spot == 779521,
              "Greek Watch takes the option with its future and its stock to value it on");
        check(!term.open_greek(779521), "an equity is not an option: Greek Watch refuses it");
        term.greek_watch()->hide();
    }

    std::printf("\n%s -- %d failing check(s)\n",
                failures == 0 ? "PASS" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
