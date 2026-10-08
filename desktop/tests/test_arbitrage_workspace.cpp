#include "../arbitrage_workspace.hpp"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

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
    page.set_manual_pair(QStringLiteral("CIPLA"), 0, QStringLiteral("SUNPHARMA"), 1);
    check(!page.manual_pair_valid(), "two different stocks are a pair trade, not an arbitrage: refused");

    // The live model's reading and positions, from the engine's state file.
    QTemporaryDir tmp;
    QDir().mkpath(tmp.path() + QStringLiteral("/data/live"));
    QFile st(tmp.path() + QStringLiteral("/data/live/engine_state.json"));
    if (st.open(QIODevice::WriteOnly)) {
        st.write(R"JSON({"engine_ns": 1, "source": "SIM", "models": [
          {"name": "Cross-exchange arbitrage", "family": "arbitrage", "state": "in position",
           "signal": "RELIANCE NSE 1000.00/1000.10 · BSE 998.50/998.60 · edge 14.0 bp, needs 10.0",
           "reason": "1 stock(s) held both ways until their prices meet",
           "fields": [["pairs", "50 (50 quoted both sides)"], ["pair", "RELIANCE: NSE 1000.00/1000.10 · BSE 998.50/998.60 · edge 14.0 bp, needs 10.0"],
                      ["pair", "INFY: NSE 1500.00/1500.10 · BSE 1500.00/1500.15 · edge -0.7 bp, needs 10.2"]]}],
          "positions": [
          {"model": "Cross-exchange arbitrage", "symbol": "RELIANCE", "token": 2, "side": 1, "qty": 100, "entry": 998.6, "why_in": "RELIANCE: sell NSE 1000.00, buy BSE 998.60 x100"},
          {"model": "Cross-exchange arbitrage", "symbol": "RELIANCE", "token": 1, "side": -1, "qty": 100, "entry": 1000.0, "why_in": "RELIANCE: sell NSE 1000.00, buy BSE 998.60 x100"},
          {"model": "Pairs", "symbol": "NIFTY26OCTFUT", "token": 9, "side": 1, "qty": 75, "entry": 25000.0, "why_in": "z"}]})JSON");
        st.close();
    }
    page.set_root(tmp.path());
    check(page.demo_state().contains(QStringLiteral("DEMO TRADING ON")) && page.demo_state().contains(QStringLiteral("in position")),
          "demo trading is shown on, with the model's state");
    check(page.pairs_table()->rowCount() == 2 && page.pairs_table()->item(0, 0)->text() == QStringLiteral("RELIANCE"),
          "every pair it watches, best edge first");
    check(page.held_table()->rowCount() == 2 && page.held_table()->item(0, 1)->text() == QStringLiteral("B")
              && page.held_table()->item(1, 1)->text() == QStringLiteral("S"),
          "and both legs it holds, nothing of another model's");
    QTemporaryDir empty;
    page.set_root(empty.path());
    check(page.demo_state().contains(QStringLiteral("waiting")) && page.pairs_table()->rowCount() == 0,
          "no engine yet: it says demo trading is waiting for the feed");
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
