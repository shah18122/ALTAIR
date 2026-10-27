// desktop/trade_log_tabs.hpp -- a strategy page with its trades beside it.
//
// The Strategies, Overnight Gap, Pair Trading and Basis pages report a study.
// This puts the trades on the same page, one tab each, so a figure and the
// trades behind it are never two pages apart:
//
//   Study                the page as it was;
//   Study trades         every round trip of the study's rule on dataset/
//                        (desktop/study_trades.hpp), built on request;
//   Paper & real trades  what the models engine paper-traded for the matching
//                        strategy, and what real orders filled
//                        (desktop/strategy_record.hpp);
//   Saved runs           the backtest trade logs on disk for this strategy
//                        (the Backtests page, filtered).
//
// A tab whose source a page does not have is left out. Every tab reads; none
// places an order.
#pragma once

#include "backtests_page.hpp"
#include "strategy_record.hpp"
#include "study_trades.hpp"
#include "theme.hpp"

#include <QComboBox>
#include <QLabel>
#include <QTabWidget>
#include <QVBoxLayout>

#include <vector>

namespace altair::ui {

struct TradeLogSpec {
    std::vector<Study> studies;   ///< rules whose trades can be rebuilt here
    QStringList models;           ///< live engine strategy names (model column of trades.csv)
    QStringList saved;            ///< words a saved run's path contains
    QString note;                 ///< one line for the Paper & real tab
};

/// The live engine's strategies, by the names their trades carry.
[[nodiscard]] inline QStringList engine_strategy_names() {
    return {QStringLiteral("OHL"), QStringLiteral("Strangle 80%"), QStringLiteral("Vol band (HAR)"),
            QStringLiteral("Pairs BANKNIFTY/NIFTY"), QStringLiteral("Stat-arb NIFTY 50"), QStringLiteral("Desk trader"),
            QStringLiteral("Cross-exchange arbitrage"), QStringLiteral("Option arbitrage")};
}

/// `study` in a tab widget with its trades. `study` is reparented.
[[nodiscard]] inline QTabWidget* with_trade_logs(QWidget* study, const TradeLogSpec& spec, const QString& dataset_root,
                                                 const QString& root) {
    auto* tabs = new QTabWidget;
    tabs->setObjectName(QStringLiteral("tradeLogTabs"));
    tabs->setDocumentMode(true);
    tabs->addTab(study, QStringLiteral("Study"));
    if (!spec.studies.empty())
        tabs->addTab(new StudyTradesPanel(dataset_root, root, spec.studies), QStringLiteral("Study trades"));
    if (!spec.models.isEmpty()) {
        auto* w = new QWidget;
        w->setObjectName(QStringLiteral("paperRealTrades"));
        auto* v = new QVBoxLayout(w);
        v->setContentsMargins(0, 8, 0, 0);
        auto* row = new QHBoxLayout;
        auto* note = new QLabel(spec.note.isEmpty()
                                    ? QStringLiteral("Every round trip the models engine paper-traded, and every real fill, "
                                                     "with and without expenses. Double-click one for why it was taken.")
                                    : spec.note,
                                w);
        note->setWordWrap(true);
        set_role(note, QStringLiteral("note"));
        row->addWidget(note, 1);
        QComboBox* pick = nullptr;
        if (spec.models.size() > 1) {
            pick = new QComboBox(w);
            pick->setObjectName(QStringLiteral("tradeModelPick"));
            pick->addItems(spec.models);
            row->addWidget(new QLabel(QStringLiteral("Strategy"), w));
            row->addWidget(pick);
        }
        v->addLayout(row);
        auto* record = new StrategyRecordPanel(spec.models.front(), root, w);
        v->addWidget(record, 1);
        if (pick != nullptr)
            QObject::connect(pick, &QComboBox::currentTextChanged, record, [record](const QString& m) { record->set_strategy(m); });
        tabs->addTab(w, QStringLiteral("Paper && real trades"));
    }
    if (!spec.saved.isEmpty()) tabs->addTab(new BacktestsPage(root, nullptr, spec.saved), QStringLiteral("Saved runs"));
    return tabs;
}

} // namespace altair::ui
