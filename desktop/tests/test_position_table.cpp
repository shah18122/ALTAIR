// P4-05 acceptance tests: identity-addressed, read-only open positions.

#include "../position_table.hpp"

#include <QCoreApplication>

#include <cstdio>
#include <initializer_list>
#include <cstdint>
#include <optional>

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

using namespace altair;
using namespace altair::ui;
using namespace altair::broker_view;

// UTC ns. now = 100 s after epoch.
constexpr Timestamp kNow{100'000'000'000};
// A snapshot/mark window that is fresh at kNow.
constexpr EvidenceWindow kFresh{Timestamp{97'000'000'000},
                                Timestamp{110'000'000'000}};

constexpr SessionKey fyers(std::uint64_t slot = 1,
                           std::uint64_t generation = 1)
{
    return {BrokerId::Fyers, slot, generation};
}

constexpr SessionKey kite(std::uint64_t slot = 1,
                          std::uint64_t generation = 1)
{
    return {BrokerId::ZerodhaKite, slot, generation};
}

Position make_position(std::uint32_t instrument, PositionProduct product,
                       std::int64_t qty, std::int64_t average = 10'000,
                       std::optional<std::int64_t> mark = 10'100,
                       EvidenceWindow marked = kFresh)
{
    Position p;
    p.instrument = InstrumentKey{instrument};
    p.product = product;
    p.net_qty = Qty{qty};
    p.average_price = Price{average};
    if (mark) p.last_mark = Price{*mark};
    p.marked = marked;
    return p;
}

PositionSnapshot snapshot(SessionKey session,
                          std::initializer_list<Position> positions,
                          EvidenceWindow observed = kFresh)
{
    PositionSnapshot s;
    s.account_session = session;
    s.observed = observed;
    s.count = static_cast<std::uint16_t>(positions.size());
    std::size_t i = 0;
    for (const Position& p : positions) s.position[i++] = p;
    return s;
}

AccountView account(SessionKey session, const char* label = "Account",
                    PositionSource source = PositionSource::Broker)
{
    return {session, source, QString::fromUtf8(label)};
}

InstrumentResolver resolver()
{
    return [](InstrumentKey key) -> std::optional<InstrumentDisplay> {
        InstrumentDisplay d;
        d.exchange = QStringLiteral("NSE");
        d.symbol = QStringLiteral("SYNTH-%1")
                       .arg(static_cast<std::uint32_t>(key));
        d.segment = QStringLiteral("OPT");
        d.expiry = QDate{2026, 9, 24};
        d.strike = Price{2'500'000};
        d.option_type = QStringLiteral("CE");
        d.lot_size = LotSize{50};
        return d;
    };
}

int row_for_instrument(const PositionTableModel& model, std::uint32_t instrument)
{
    for (int row = 0; row < model.rowCount(); ++row) {
        const RowKey key = model.index(row, 0)
                               .data(PositionTableModel::KeyRole).value<RowKey>();
        if (key.instrument == InstrumentKey{instrument}) return row;
    }
    return -1;
}

static void test_apply_refuses_mismatch_and_unusable()
{
    PositionTableModel model(resolver());
    const AccountView a = account(fyers());
    const auto mismatch = snapshot(kite(), {
        make_position(7, PositionProduct::Intraday, 50)});
    const auto bad = model.apply(a, mismatch, kNow);
    check(!bad && bad.error() == ApplyError::SessionMismatch,
          "mismatched account and snapshot are refused");
    check(model.rowCount() == 0, "mismatch changes no rows");

    const auto good = snapshot(fyers(), {
        make_position(7, PositionProduct::Intraday, 50),
        make_position(8, PositionProduct::Delivery, -25)});
    check(model.apply(a, good, kNow).value() == 2, "good snapshot adds two rows");

    auto unusable = good;
    unusable.observed.expires_at = kNow;
    const auto refused = model.apply(a, unusable, kNow);
    check(!refused && refused.error() == ApplyError::SnapshotUnusable,
          "expired snapshot is refused");
    check(model.rowCount() == 2, "refused snapshot leaves open risk visible");

    // Two rows, one key, opposite signs: net +20 would render as -30 if the
    // model kept the last row. Ambiguous input is refused, not guessed.
    auto dup = snapshot(fyers(), {
        make_position(7, PositionProduct::Intraday, 50),
        make_position(7, PositionProduct::Intraday, -30)});
    const auto r = model.apply(a, dup, kNow);
    check(!r.has_value() && r.error() == ApplyError::DuplicatePosition,
          "a repeated RowKey in one snapshot is refused");
    check(model.rowCount() == 2,
          "a refused duplicate leaves the account's existing rows intact");

    // A duplicate where one copy is flat is still ambiguous.
    dup.position[1].net_qty = Qty{0};
    const auto f = model.apply(a, dup, kNow);
    check(!f.has_value() && f.error() == ApplyError::DuplicatePosition,
          "an open row and a flat row sharing a key are refused");
}

static void test_no_netting_across_accounts_or_products()
{
    PositionTableModel accounts(resolver());
    (void)accounts.apply(account(fyers(), "FYERS"), snapshot(fyers(), {
        make_position(7, PositionProduct::Intraday, 50)}), kNow);
    (void)accounts.apply(account(kite(), "Kite"), snapshot(kite(), {
        make_position(7, PositionProduct::Intraday, 50)}), kNow);
    check(accounts.rowCount() == 2, "same instrument in two accounts is two rows");
    for (int row = 0; row < accounts.rowCount(); ++row) {
        check(accounts.index(row, PositionTableModel::NetQty).data().toString()
                  != QStringLiteral("+100"),
              "account quantities are never netted");
    }

    PositionTableModel products(resolver());
    (void)products.apply(account(fyers()), snapshot(fyers(), {
        make_position(7, PositionProduct::Intraday, 50),
        make_position(7, PositionProduct::CarryForward, 50)}), kNow);
    check(products.rowCount() == 2, "same instrument under two products is two rows");
    for (int row = 0; row < products.rowCount(); ++row) {
        check(products.index(row, PositionTableModel::NetQty).data().toString()
                  != QStringLiteral("+100"),
              "product quantities are never netted");
    }
}

static void test_relogin_removes_previous_generation()
{
    PositionTableModel model(resolver());
    (void)model.apply(account(fyers(1, 1)), snapshot(fyers(1, 1), {
        make_position(7, PositionProduct::Intraday, 50),
        make_position(8, PositionProduct::Delivery, 20)}), kNow);
    (void)model.apply(account(fyers(1, 2)), snapshot(fyers(1, 2), {
        make_position(9, PositionProduct::Margin, -10)}), kNow);
    check(model.rowCount() == 1, "new generation replaces previous login rows");
    bool old_generation = false;
    for (int row = 0; row < model.rowCount(); ++row) {
        const RowKey key = model.index(row, 0)
                               .data(PositionTableModel::KeyRole).value<RowKey>();
        old_generation = old_generation || key.generation == 1;
    }
    check(!old_generation, "no previous generation key remains");
}

static void test_update_diffs_instead_of_reset()
{
    PositionTableModel model(resolver());
    int resets = 0;
    int inserted = 0;
    int removed = 0;
    int changed = 0;
    QObject::connect(&model, &QAbstractItemModel::modelReset,
                     [&] { ++resets; });
    QObject::connect(&model, &QAbstractItemModel::rowsInserted,
                     [&](const QModelIndex&, int, int) { ++inserted; });
    QObject::connect(&model, &QAbstractItemModel::rowsRemoved,
                     [&](const QModelIndex&, int, int) { ++removed; });
    QObject::connect(&model, &QAbstractItemModel::dataChanged,
                     [&](const QModelIndex&, const QModelIndex&, const QList<int>&) {
                         ++changed;
                     });

    const AccountView a = account(fyers());
    (void)model.apply(a, snapshot(fyers(), {
        make_position(7, PositionProduct::Intraday, 50, 10'000, 10'100),
        make_position(8, PositionProduct::Intraday, 50),
        make_position(9, PositionProduct::Intraday, 50)}), kNow);
    inserted = 0;
    removed = 0;
    changed = 0;
    (void)model.apply(a, snapshot(fyers(), {
        make_position(7, PositionProduct::Intraday, 50, 10'000, 10'200),
        make_position(9, PositionProduct::Intraday, 50),
        make_position(10, PositionProduct::Intraday, 50)}), kNow);

    check(resets == 0, "snapshot diffs never reset the model");
    check(removed == 1, "vanished key emits one removal");
    check(inserted == 1, "new key emits one insertion");
    check(changed >= 1, "surviving keys emit dataChanged");
    const int row7 = row_for_instrument(model, 7);
    check(row7 >= 0
          && model.index(row7, PositionTableModel::Unrealised).data().toString()
                 == format_paise(10'000, true),
          "updated key shows P&L from its new mark");
}

static void test_flat_rows_are_not_shown()
{
    PositionTableModel model(resolver());
    int removed = 0;
    int resets = 0;
    QObject::connect(&model, &QAbstractItemModel::rowsRemoved,
                     [&](const QModelIndex&, int, int) { ++removed; });
    QObject::connect(&model, &QAbstractItemModel::modelReset,
                     [&] { ++resets; });
    const AccountView a = account(fyers());
    (void)model.apply(a, snapshot(fyers(), {
        make_position(7, PositionProduct::Intraday, 50),
        make_position(8, PositionProduct::Intraday, 0),
        make_position(9, PositionProduct::Intraday, -20)}), kNow);
    check(model.rowCount() == 2, "flat row is excluded from initial snapshot");
    (void)model.apply(a, snapshot(fyers(), {
        make_position(7, PositionProduct::Intraday, 0),
        make_position(9, PositionProduct::Intraday, -20)}), kNow);
    check(model.rowCount() == 1 && removed == 1,
          "position becoming flat is removed structurally");
    check(resets == 0, "flat transition does not reset the model");
}

static void test_absent_failed_and_zero_render_differently()
{
    PositionTableModel model(resolver());
    const Position absent = make_position(7, PositionProduct::Intraday, 50,
                                          10'000, std::nullopt);
    const Position zero = make_position(8, PositionProduct::Intraday, 50,
                                        10'000, 10'000);
    const Position overflow = make_position(
        9, PositionProduct::Intraday, 9'223'372'036'854'775'807LL,
        0, 1'000'000'000);
    (void)model.apply(account(fyers()),
                      snapshot(fyers(), {absent, zero, overflow}), kNow);

    const QModelIndex a = model.index(row_for_instrument(model, 7),
                                      PositionTableModel::Unrealised);
    const QModelIndex z = model.index(row_for_instrument(model, 8),
                                      PositionTableModel::Unrealised);
    const QModelIndex e = model.index(row_for_instrument(model, 9),
                                      PositionTableModel::Unrealised);
    check(a.data().toString() == QStringLiteral("—")
          && a.data(PositionTableModel::BlankRole).toBool()
          && !a.data(PositionTableModel::SortRole).isValid(),
          "absent P&L is blank and unsortable");
    check(z.data().toString() == format_paise(0, true)
          && !z.data(PositionTableModel::BlankRole).toBool()
          && z.data(PositionTableModel::SortRole).toLongLong() == 0,
          "known zero P&L remains a real numeric value");
    check(e.data().toString() == QStringLiteral("ERR")
          && e.data(PositionTableModel::BlankRole).toBool()
          && !e.data(PositionTableModel::SortRole).isValid(),
          "failed P&L is an explicit error and unsortable");
    check(a.data().toString() != z.data().toString()
          && a.data().toString() != e.data().toString()
          && z.data().toString() != e.data().toString(),
          "absent, zero and failed render differently");
}

static void test_unresolved_instrument_is_shown_not_hidden()
{
    PositionTableModel model([](InstrumentKey) {
        return std::optional<InstrumentDisplay>{};
    });
    (void)model.apply(account(fyers()), snapshot(fyers(), {
        make_position(7, PositionProduct::Intraday, 50)}), kNow);
    check(model.rowCount() == 1, "unresolved position remains visible");
    check(model.index(0, PositionTableModel::Instrument).data().toString()
              == QStringLiteral("unresolved #7"),
          "unresolved position exposes its numeric key");
    check(model.index(0, PositionTableModel::Exchange).data().toString()
              == QStringLiteral("—"),
          "unresolved exchange is absent, not invented");
    check(model.index(0, PositionTableModel::NetQty).data().toString()
              == QStringLiteral("+50"),
          "unresolved position still exposes its risk quantity");
}

static void test_freshness_future_stale_and_refresh()
{
    PositionTableModel model(resolver());
    const EvidenceWindow stale{Timestamp{97'000'000'000},
                               Timestamp{99'000'000'000}};
    const EvidenceWindow future{Timestamp{101'000'000'000},
                                Timestamp{110'000'000'000}};
    const EvidenceWindow never{Timestamp::epoch(), Timestamp{110'000'000'000}};
    (void)model.apply(account(fyers()), snapshot(fyers(), {
        make_position(7, PositionProduct::Intraday, 50, 10'000, 10'100, kFresh),
        make_position(8, PositionProduct::Intraday, 50, 10'000, 10'100, stale),
        make_position(9, PositionProduct::Intraday, 50, 10'000, 10'100, future),
        make_position(10, PositionProduct::Intraday, 50, 10'000, 10'100, never)}),
        kNow);
    check(model.index(row_for_instrument(model, 7), PositionTableModel::Freshness)
              .data().toString() == QStringLiteral("3s"),
          "fresh mark renders whole-second age");
    check(model.index(row_for_instrument(model, 8), PositionTableModel::Freshness)
              .data().toString() == QStringLiteral("3s stale"),
          "expired mark appends stale");
    check(model.index(row_for_instrument(model, 9), PositionTableModel::Freshness)
              .data().toString() == QStringLiteral("FUTURE"),
          "future mark fails loud");
    check(model.index(row_for_instrument(model, 10), PositionTableModel::Freshness)
              .data().toString() == QStringLiteral("—"),
          "never-observed mark renders absent");

    bool wrong_column = false;
    int emissions = 0;
    QObject::connect(&model, &QAbstractItemModel::dataChanged,
                     [&](const QModelIndex& top, const QModelIndex& bottom,
                         const QList<int>&) {
                         ++emissions;
                         wrong_column = wrong_column
                             || top.column() != PositionTableModel::Freshness
                             || bottom.column() != PositionTableModel::Freshness;
                     });
    model.refresh_freshness(Timestamp{105'000'000'000});
    check(model.index(row_for_instrument(model, 7), PositionTableModel::Freshness)
              .data().toString() == QStringLiteral("8s"),
          "freshness refresh uses its supplied timestamp");
    check(emissions > 0 && !wrong_column,
          "freshness refresh emits only the Freshness column");
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    std::printf("P4-05 -- open-position table model\n");
    test_apply_refuses_mismatch_and_unusable();
    test_no_netting_across_accounts_or_products();
    test_relogin_removes_previous_generation();
    test_update_diffs_instead_of_reset();
    test_flat_rows_are_not_shown();
    test_absent_failed_and_zero_render_differently();
    test_unresolved_instrument_is_shown_not_hidden();
    test_freshness_future_stale_and_refresh();
    std::printf("\n%s\n", failures == 0 ? "all checks passed"
                                        : "checks did not pass");
    return failures == 0 ? 0 : 1;
}