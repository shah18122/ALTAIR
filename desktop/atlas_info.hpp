// desktop/atlas_info.hpp -- the ⓘ behind every Model Atlas row.
//
// What a model is FOR is the row's own `what`. This adds the three questions a
// trader asks next: how was it trained, on what data, and what does it do in
// the real market today. The answers are written from the code, not hoped
// for: a model that is research only says so, and "not trained" is an answer
// (a pricer or a rule has parameters, not a fit).

#pragma once

#include "atlas_data.hpp"

#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QString>
#include <QVBoxLayout>
#include <cstring>

namespace altair::ui {

struct AtlasInfo {
    const char* trained;
    const char* data;
    const char* market;
};

namespace atlas_info_detail {

inline constexpr const char* kBars =
    "dataset/: NIFTY, BANKNIFTY and NIFTY 50 stock bars (1-minute to daily) from FYERS/Kite history, with INDIA VIX.";
inline constexpr const char* kResearch =
    "Research only: run from its page on history. The live models engine does not trade it.";
inline constexpr const char* kWalkForward =
    "Fitted fold by fold in a purged, embargoed walk-forward: each fold trains on earlier days only and is "
    "scored on the days after; scaling is fitted inside the fold.";
inline constexpr const char* kLiveBook =
    "The live order book from the price bus: 5 levels from the FYERS HSM socket, 50 from TBT.";
inline constexpr const char* kNotTrained = "Not trained: a closed-form or numerical method with no fitted parameters.";

struct Entry { const char* key; AtlasInfo info; };

// Specific models first (several rows share a header).
inline constexpr Entry kByModel[] = {
    {"Logistic regression", {kWalkForward, kBars,
        "Live: one of the five direction models in the engine. It decides during the session and holds to the 15:20 "
        "square-off, one lot of the near NIFTY future, taken only when the magnitude gate says the expected gain "
        "beats costs. Demo by default."}},
    {"Linear / ridge regression", {kWalkForward, kBars,
        "Live: ridge is one of the five direction models (same gate and square-off as the others). Demo by default."}},
    {"AR (autoregression)", {"Least squares on every finished session; refitted before each session, never inside the market-data path.",
        kBars, "Live: AR(2) is a direction model in the engine (demo by default)."}},
    {"ARMA / ARIMA / SARIMA", {"Conditional maximum likelihood on every finished session; ARMA(1,1) is refitted before each session.",
        kBars, "Live: ARMA(1,1) is a direction model in the engine (demo by default)."}},
    {"Ornstein-Uhlenbeck", {"AR(1) regression on the spread gives the mean, speed and half-life.",
        "Daily closes of the pair (BANKNIFTY/NIFTY and stock pairs) from dataset/.",
        "Live: sets the half-life the 'Pairs BANKNIFTY/NIFTY' model uses for its exits (demo)."}},
    {"Gradient boosting (GBDT / LightGBM)", {kWalkForward, kBars,
        "Live: GBDT is one of the five direction models (demo by default)."}},
    {"Decision tree", {kWalkForward, kBars, "Inside the GBDT direction model (its weak learner); not traded on its own."}},
    {"Cointegration", {"Engle-Granger: OLS hedge ratio on a rolling window, ADF test on the residual.",
        "Daily closes of index and stock futures from dataset/.",
        "Live: the pairs model trades the BANKNIFTY/NIFTY ratio when its z-score is stretched (demo)."}},
    {"Pairs trading", {"Hedge ratio and z-score from a rolling window; entry, stop and time exits are fixed rules.",
        "Daily and minute closes of the two futures.",
        "Live: 'Pairs BANKNIFTY/NIFTY' in the models engine, futures legs, demo by default."}},
    {"Statistical arbitrage", {"Avellaneda-Lee: factors estimated on a rolling window, an OU process fitted to each residual.",
        "NIFTY 50 stocks' bars from dataset/ and the live stream.",
        "Live: 'Stat-arb NIFTY 50' in the models engine, dollar-neutral, demo by default."}},
    {"Spread / calendar trading", {"Not trained: a mispricing rule — the gap must beat every leg's expenses, spreads and a margin.",
        "Live quotes: NSE and BSE listings of the same stock; option strikes and the future for parity.",
        "Live: the cross-exchange arbitrage and the option arbitrage run on every quote. Demo by default; real "
        "orders only while LIVE is on AND the strategy's own switch is on, within its caps."}},
    {"Order-book imbalance", {kNotTrained, kLiveBook,
        "Live: the option arbitrage reads it to decide which leg goes first and to skip a mispricing the book is "
        "about to remove."}},
    {"Microprice", {kNotTrained, kLiveBook,
        "Live: the option arbitrage prices its legs off the microprice rather than the mid."}},
    {"Black-Scholes / Black-76", {kNotTrained, "Live option and future prices from the bus.",
        "Live: Greek Watch and the option chain on every tick; the strangles price their legs with it."}},
    {"Greeks -- delta, gamma, vega, theta, rho", {kNotTrained, "Live option prices, the future and days to expiry.",
        "Live: every Greek Watch column and the chain's Δ."}},
    {"Implied volatility", {"Solved per option (Newton with a bracketing fallback) from the market mid.",
        "Live option quotes.", "Live: the IV column of the chain and Greek Watch."}},
    {"Transaction cost calculator", {"Not trained: brokerage, STT, exchange, SEBI, stamp duty and GST from config/charges.toml.",
        "config/charges.toml (NSE and BSE schedules).",
        "Live: every demo fill and every real round trip records its expenses with it; P&L is shown gross and net."}},
    {"Ensemble aggregator", {"Weights from each member's out-of-sample score.", kBars,
        "Live: the 'Vote' direction model is the majority of the five."}},
    {"Feature registry", {"Not trained: sealed feature definitions.", kBars,
        "Live: the same features feed backtests and the live engine, fingerprinted so the two cannot differ."}},
    {"Walk-forward & purged CV", {"It IS the training procedure the other models use.", kBars,
        "Live: every live direction model was trained and calibrated this way."}},
    {"Model scorecards & drift", {"Scores built from each model's own calls.", "Backtest calls and the engine's paper trades.",
        "Flags a model whose live results drift from its backtest."}},
};

// Then by header.
inline constexpr Entry kByFile[] = {
    {"strategies/meanrev.hpp", {"A rule: lookback and entry band are parameters, run walk-forward with costs on the Strategies page.", kBars, kResearch}},
    {"strategies/momentum.hpp", {"A rule: lookback and holding period are parameters, run walk-forward with costs on the Strategies page.", kBars, kResearch}},
    {"strategies/fundamentals.hpp", {"Not trained: ratios from typed fundamentals.", "Typed company fundamentals; no filings feed is ingested.", kResearch}},
    {"strategies/regime.hpp", {"Thresholds on trend and volatility measured on history.", kBars, kResearch}},
    {"models/classical.hpp", {kWalkForward, kBars, kResearch}},
    {"models/gbdt.hpp", {kWalkForward, kBars, kResearch}},
    {"models/mlp.hpp", {"Back-propagation (Adam) on walk-forward folds with fixed seeds.", kBars,
        "Research (Neural page): not run live — out of sample it did not beat costs."}},
    {"models/trainable_recurrent.hpp", {"Back-propagation through time (analytic gradients) on walk-forward folds.", kBars,
        "Research (Neural page): not run live — out of sample it did not beat costs."}},
    {"models/transformer.hpp", {"Causal multi-head attention trained by back-propagation on walk-forward folds.", kBars,
        "Research (Neural page): not run live — out of sample it did not beat costs."}},
    {"models/attention.hpp", {"Convolution kernels trained by back-propagation on walk-forward folds.", kBars,
        "Research (Neural page): not run live — out of sample it did not beat costs."}},
    {"models/horizon_eval.hpp", {"Least squares on every finished session.", kBars, kResearch}},
    {"models/time_series.hpp", {"Conditional maximum likelihood on history.", kBars, kResearch}},
    {"analytics/kalman.hpp", {"Filter recursion; noise variances by maximum likelihood.", kBars, kResearch}},
    {"analytics/hmm.hpp", {"Baum-Welch (EM) on daily and 5-minute returns.", kBars, "Shows the regime on the Regimes page; not an order source."}},
    {"models/markov.hpp", {"Transition counts between regimes on history.", kBars, "Shows the regime on the Regimes page; not an order source."}},
    {"analytics/hurst.hpp", {"Rescaled-range estimate on a rolling window.", kBars, "Shows trend vs mean-reversion on the Regimes page; not an order source."}},
    {"backtest/montecarlo.hpp", {"Simulation: drift, volatility and jumps estimated from returns.", kBars,
        "Used by the VaR and Monte Carlo pages (10,000 paths); not an order source."}},
    {"analytics/ewma.hpp", {"Exponentially weighted variance; the decay is a parameter.", kBars,
        "Volatility page. The live strangles sell the HAR volatility band, not this one."}},
    {"analytics/garch.hpp", {"Maximum likelihood on daily returns.", kBars,
        "Volatility page. The live strangles sell the HAR volatility band, not this one."}},
    {"analytics/advanced_pricers.hpp", {"Numerical pricer; Heston and EGARCH parameters by maximum likelihood / calibration to the chain.",
        "Option chain and daily returns.", kResearch}},
    {"book/microstructure.hpp", {kNotTrained, kLiveBook, kResearch}},
    {"book/flow.hpp", {"Estimated on volume buckets and a regression of price change on signed volume.", "Trades and quotes recorded from the live feed.", kResearch}},
    {"risk/slippage.hpp", {"Impact coefficients fitted to recorded fills.", "Recorded live quotes and paper fills.",
        "Execution page; demo fills are priced at the touch with size and latency, not with this model."}},
    {"oms/execution.hpp", {"Not trained: scheduling rules.", "Volume profile from dataset/.", "Execution page; the router sends single limit/IOC orders."}},
    {"oms/shortfall.hpp", {"Not trained: the Almgren-Chriss solution for given risk aversion and impact.", "Volatility and impact estimates.", kResearch}},
    {"book/queue_position.hpp", {kNotTrained, kLiveBook, "The execution study and tuner use it to price passive (join-the-queue) fills."}},
    {"models/fill_probability.hpp", {"Fitted to recorded quotes and fills.", "Tapes recorded by the models engine.", "The tuner uses it to compare passive and aggressive execution."}},
    {"models/hawkes.hpp", {"Maximum likelihood on trade arrival times.", "Trades recorded from the live feed.", kResearch}},
    {"risk/var.hpp", {"Historical, parametric and Monte Carlo (10,000 paths) from return history.", kBars, "VaR page; not an order source."}},
    {"risk/stress.hpp", {"Scenario shocks; Black-Litterman blends a prior with views.", kBars, "Risk pages; not an order source."}},
    {"risk/covariance.hpp", {"Ledoit-Wolf shrinkage of the sample covariance.", kBars, "Portfolio page; not an order source."}},
    {"risk/neutralise.hpp", {"Betas by regression on the index.", kBars, "Portfolio page; the stat-arb uses the same neutralisation."}},
    {"risk/factor_risk.hpp", {"Factor exposures by regression.", kBars, "Portfolio page; not an order source."}},
    {"models/conformal.hpp", {"Calibration residuals from walk-forward calls.", kBars, "Bounds the forecast bands; not an order source."}},
    {"analytics/greeks.hpp", {kNotTrained, "Live option prices.", "Live: Greek Watch and the option chain."}},
    {"analytics/greeks2.hpp", {kNotTrained, "Live option prices.", "Options page; not an order source."}},
    {"analytics/iv.hpp", {kNotTrained, "Live option quotes.", "Live: IV in the chain and Greek Watch."}},
    {"analytics/svi_fit.hpp", {"Least-squares fit of the SVI smile to the chain's IVs.", "Option chain.", "Options page; not an order source."}},
    {"analytics/sabr.hpp", {"Calibrated to the chain's IVs.", "Option chain.", "Options page; not an order source."}},
    {"analytics/american.hpp", {kNotTrained, "Option chain.", "Options page; index options are European."}},
    {"backtest/mc_pricing.hpp", {kNotTrained, "Volatility estimate.", kResearch}},
    {"analytics/vix.hpp", {kNotTrained, "The NIFTY option chain (CBOE formula as NSE applies it).", "Volatility page; the live models read INDIA VIX from the feed."}},
    {"risk/sizing.hpp", {"Not trained: sizing rules from volatility and edge estimates.", kBars, "Sizing page. Live models trade fixed lots within the limits you set."}},
    {"risk/optimise.hpp", {"Numerical optimisation on the estimated covariance.", kBars, kResearch}},
    {"models/sentiment.hpp", {"Lexicon scoring.", "Text supplied by hand; no live news feed.", kResearch}},
    {"models/event_study.hpp", {"Abnormal returns around event dates.", kBars, kResearch}},
    {"backtest/validation.hpp", {"It IS the training procedure.", kBars, "Every live direction model was trained this way."}},
    {"backtest/agent_market.hpp", {"Simulated agents with fixed rules.", "Synthetic.", kResearch}},
    {"models/regime_rl.hpp", {"Q-learning on episodes replayed from history.", kBars, kResearch}},
    {"models/deep_rl.hpp", {"DQN / PPO / actor-critic on episodes replayed from history.", kBars, kResearch}},
    {"flagging/drift.hpp", {"Scores built from each model's calls.", "Backtest calls and paper trades.", "Flags drift; not an order source."}},
    {"models/forecast_scorecard.hpp", {"Scores built from each model's calls.", kBars, "Forecast page; not an order source."}},
    {"models/dcf.hpp", {"Not trained: typed cash-flow assumptions.", "Typed assumptions.", kResearch}},
};

} // namespace atlas_info_detail

/// The ⓘ for a row: by model when several rows share a header, else by header.
[[nodiscard]] inline AtlasInfo atlas_info(const AtlasRow& row) noexcept {
    using namespace atlas_info_detail;
    for (const auto& e : kByModel) if (std::strcmp(e.key, row.model) == 0) return e.info;
    for (const auto& e : kByFile) if (std::strcmp(e.key, row.file) == 0) return e.info;
    if (row.status == AtlasStatus::Absent) return {"Not in this tree.", "None.", "Not available."};
    return {"See the model's page.", kBars, kResearch};
}

/// The ⓘ as one block of rich text.
[[nodiscard]] inline QString atlas_info_html(const AtlasRow& row) {
    const AtlasInfo i = atlas_info(row);
    return QStringLiteral(
        "<div style='color:#E6EDF3'><b style='font-size:15px;color:#F0B765'>%1</b>"
        "<span style='color:#8FA3AE'> &nbsp;·&nbsp; %2 &nbsp;·&nbsp; %3</span><br><br>"
        "<b>What it is for.</b> %4<br><br>"
        "<b>How it is trained.</b> %5<br><br>"
        "<b>On what data.</b> %6<br><br>"
        "<b>In the real market.</b> %7<br><br>"
        "<span style='color:#8FA3AE'>Code: <code>%8</code></span></div>")
        .arg(QString::fromUtf8(row.model).toHtmlEscaped(), QString::fromUtf8(row.family).toHtmlEscaped(),
             QString::fromUtf8(atlas_status_text(row.status)), QString::fromUtf8(row.what).toHtmlEscaped(),
             QString::fromUtf8(i.trained).toHtmlEscaped(), QString::fromUtf8(i.data).toHtmlEscaped(),
             QString::fromUtf8(i.market).toHtmlEscaped(),
             QString::fromUtf8(row.file[0] != 0 ? row.file : "(none)"));
}

/// The ⓘ window for a row.
inline void show_atlas_info(QWidget* parent, const AtlasRow& row) {
    QDialog d(parent);
    d.setObjectName(QStringLiteral("atlasInfoDialog"));
    d.setWindowTitle(QStringLiteral("About this model · %1").arg(QString::fromUtf8(row.model)));
    auto* v = new QVBoxLayout(&d);
    auto* text = new QLabel(atlas_info_html(row), &d);
    text->setTextFormat(Qt::RichText);
    text->setWordWrap(true);
    text->setTextInteractionFlags(Qt::TextSelectableByMouse);
    v->addWidget(text);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &d);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    v->addWidget(buttons);
    d.resize(640, 420);
    d.exec();
}

} // namespace altair::ui
