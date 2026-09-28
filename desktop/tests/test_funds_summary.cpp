// P4-06 acceptance tests: distinct broker balances and honest totals.
#include "../funds_summary.hpp"

#include <QCoreApplication>

#include <cstdio>

namespace {
int failures = 0;
void check(bool ok, const char* text) {
    std::printf("  %s: %s\n", ok ? "ok  " : "FAIL", text);
    if (!ok) ++failures;
}
}

using namespace altair;
using namespace altair::ui;
using namespace altair::broker_view;

static FundsAccountView account(BrokerId broker, std::uint64_t generation,
                                const char* label, bool connected,
                                std::optional<std::int64_t> cash,
                                std::optional<std::int64_t> available,
                                FundsSource source = FundsSource::Broker) {
    FundsAccountView row;
    row.session = {broker, 1, generation};
    row.source = source;
    row.label = QString::fromUtf8(label);
    row.connected = connected;
    if (cash) row.funds.cash = Notional{*cash};
    if (available) row.funds.available_trading_balance = Notional{*available};
    return row;
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    FundsSummaryModel model;
    check(model.rowCount() == 1
              && model.index(0, FundsSummaryModel::State).data().toString()
                     == QStringLiteral("no account snapshots"),
          "empty terminal states that no account snapshot exists");

    check(model.apply(account(BrokerId::Fyers, 1, "FYERS · A1", true, 0, 0)),
          "known-zero FYERS snapshot is accepted");
    check(model.index(0, FundsSummaryModel::Cash).data().toString()
              == QStringLiteral("0.00"),
          "known zero renders as zero, never as missing");

    check(model.apply(account(BrokerId::ZerodhaKite, 1, "Kite · B1", false,
                              std::nullopt, 25'000,
                              FundsSource::Paper)),
          "paper Kite snapshot is accepted as a separate account");
    check(model.account_count() == 2, "two broker accounts stay two rows");
    const int total = model.total_row();
    check(model.index(total, FundsSummaryModel::Cash).data().toString()
              .contains(QStringLiteral("INCOMPLETE")),
          "a partial cash total is explicitly incomplete");
    check(model.index(total, FundsSummaryModel::Available).data().toString()
              == QStringLiteral("250.00"),
          "complete comparable available balance sums exactly");
    check(model.index(total, FundsSummaryModel::Account).data().toString()
              .contains(QStringLiteral("NOT FUNGIBLE")),
          "cross-broker arithmetic never claims funds are fungible");
    check(model.index(1, FundsSummaryModel::Account).data().toString()
              .contains(QStringLiteral("PAPER")),
          "paper and broker sources are visibly distinct");

    check(model.apply(account(BrokerId::Fyers, 2, "FYERS · A1", true,
                              10'000, 20'000)),
          "new login generation replaces the same broker account");
    check(model.account_count() == 2, "provider segment/relogin cannot duplicate a balance");
    check(model.index(model.total_row(), FundsSummaryModel::Cash).data().toString()
              .contains(QStringLiteral("INCOMPLETE")),
          "unknown Kite cash remains incomplete after FYERS refresh");

    std::printf("Funds summary: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
