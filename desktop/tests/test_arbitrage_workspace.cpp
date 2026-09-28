#include "../arbitrage_workspace.hpp"

#include <QApplication>

#include <cstdio>

namespace { int failures = 0; void check(bool ok, const char* s) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", s); if (!ok) ++failures;
}}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    altair::ui::ArbitrageWorkspace page;
    check(page.manual_pair_valid()
              && page.manual_pair_status().contains(QStringLiteral("two-sided depth")),
          "manual NSE/BSE pair is accepted but remains depth and cost gated");
    page.set_manual_pair(QStringLiteral("CIPLA"), 0, QStringLiteral("CIPLA"), 0);
    check(!page.manual_pair_valid()
              && page.manual_pair_status().contains(QStringLiteral("REFUSED")),
          "an identical instrument and venue cannot manufacture arbitrage");
    page.set_manual_pair(QStringLiteral("CIPLA"), 0, QStringLiteral("CIPLA"), 1);
    check(page.manual_pair_valid(), "the same instrument may be paired across NSE and BSE");
    altair::ui::ArbitrageViewRow row;
    row.sequence = 1;
    row.instrument = QStringLiteral("CIPLA");
    row.venues = QStringLiteral("NSE → BSE");
    row.source = QStringLiteral("FYERS · broker websocket");
    row.quote_a = QStringLiteral("ask 1,482.10");
    row.quote_b = QStringLiteral("bid 1,482.25");
    row.size_a = 100;
    row.size_b = 75;
    row.quote_age_ns = 4'000'000;
    row.premium_discount = QStringLiteral("cash cross-venue");
    row.gross_paise = 1'500;
    row.cost_paise = 2'100;
    row.net_paise = -600;
    row.verdict = QStringLiteral("REFUSED · costs exceed edge");
    row.correlation_id = QStringLiteral("scan-1");
    check(page.inbox().publish(row), "scanner event enters bounded handoff");
    check(page.model()->rowCount() == 0, "producer does not mutate UI model directly");
    page.drain();
    check(page.model()->rowCount() == 1, "throttled UI drain publishes one row");
    check(page.model()->index(0, altair::ui::ArbitrageTableModel::Net).data().toString()
              == QStringLiteral("-6.00"), "post-cost edge renders exactly in INR");
    check(page.model()->index(0, altair::ui::ArbitrageTableModel::Verdict).data().toString()
              .contains(QStringLiteral("REFUSED")), "refusal reason remains visible");
    check(page.model()->index(0, altair::ui::ArbitrageTableModel::QuoteAge).data().toString()
              == QStringLiteral("4 ms"), "quote age is explicit");
    bool refused = false;
    for (std::size_t i = 0; i < altair::ui::kArbitrageUiCapacity + 1; ++i)
        refused = !page.inbox().publish(row) || refused;
    check(refused, "bounded handoff refuses overflow instead of overwriting silently");
    page.drain();
    return failures == 0 ? 0 : 1;
}
