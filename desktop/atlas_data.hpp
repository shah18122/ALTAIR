// desktop/atlas_data.hpp -- what Altair actually has, family by family.
//
// P36-01. Smit gave me a roadmap listing ten families of quant model and said
// "i want gui for bettewr understanding i dont understand this". The document
// is a taxonomy of the field; this table is the same taxonomy with one extra
// column that no textbook can supply -- whether THIS engine has it, and where.
//
// THE ABSENCES ARE THE POINT, AND THEY ARE STATED AS LOUDLY AS THE PRESENCES.
//
// A catalogue that lists only what exists teaches you that everything exists.
// The rows marked Absent below are the honest shape of the project: no factor
// model, no PCA, no mean-variance optimiser, no ARIMA, no random forest, no
// alternative data of any kind. Each was verified by search before being
// written down, because "I did not find it" and "it is not there" are
// different claims and only one of them belongs in a reference.
//
// AND THE PARTIALS ARE THE ROWS THAT MISLEAD.
//
// "LSTM" in a feature list sounds like a trained recurrent network. What is
// here is an exact LSTM forward pass whose recurrent weights are FROZEN at
// initialisation, with only a linear readout solved by ridge -- an echo-state
// reservoir, not backpropagation. "Transformer" is a single-head causal
// attention primitive with no multi-head, no positional encoding, no layer
// norm and no stacked block. Somebody reading a one-word status would deploy
// those believing something false, so Partial exists and says exactly what is
// missing.
//
// EVERY PATH IS CHECKED AT CONFIGURE TIME. cmake/AtlasAudit.cmake fails the
// build if a row names a file that does not exist, so this table cannot rot
// into a description of a tree that has moved on without it. That check was
// itself verified by planting a bogus path -- the standing lesson from P33,
// where three checks shipped reporting clean while broken.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace altair::ui {

enum class AtlasStatus : std::uint8_t {
    /// Built, tested, and does what its name says.
    Implemented,
    /// The primitive is here; the named model is not. The row says what is
    /// missing, because this is the status that misleads.
    Partial,
    /// Not in this tree. Verified by search, not assumed.
    Absent
};

[[nodiscard]] inline const char* atlas_status_text(AtlasStatus s) noexcept {
    switch (s) {
    case AtlasStatus::Implemented: return "BUILT";
    case AtlasStatus::Partial:     return "PARTIAL";
    case AtlasStatus::Absent:      return "ABSENT";
    }
    return "?";
}

struct AtlasRow {
    /// One of the ten families from the roadmap.
    const char* family;
    /// The model, named as the roadmap names it so the two can be read side
    /// by side.
    const char* model;
    AtlasStatus status;
    /// Header it lives in, repo-relative. Empty when Absent.
    const char* file;
    /// Nav page that RUNS it, or -1. This is the column that turns a
    /// catalogue into a way of learning: press the button and watch the model
    /// answer on real NIFTY data.
    int page;
    /// One sentence, in plain language, on what the thing is FOR. Not what it
    /// is called and not how it works -- what question it answers.
    const char* what;
};

// ---------------------------------------------------------------------------
// Nav indices referenced below are the post-P35-03b layout (32 pages, 0..31).
// If the nav moves, these move with it; the audit checks paths, not indices,
// so a stale index shows up as a button that opens the wrong page rather than
// as a build failure. Keep them honest by hand.
// ---------------------------------------------------------------------------

inline constexpr std::array kAtlasRows{
    // -- 1. Statistical alpha -----------------------------------------------
    AtlasRow{"1. Statistical alpha", "Mean reversion", AtlasStatus::Implemented,
             "strategies/meanrev.hpp", 19,
             "Bets that a price stretched far from its own recent average "
             "snaps back. Fades the extreme."},
    AtlasRow{"1. Statistical alpha", "Momentum / trend following",
             AtlasStatus::Implemented, "strategies/momentum.hpp", 19,
             "The opposite bet: what has been rising keeps rising. Rides the "
             "move instead of fading it."},
    AtlasRow{"1. Statistical alpha", "Cointegration", AtlasStatus::Implemented,
             "strategies/cointegration.hpp", 26,
             "Tests whether two drifting series are tied together, so their "
             "GAP is tradeable even though neither price is."},
    AtlasRow{"1. Statistical alpha", "Pairs trading", AtlasStatus::Partial,
             "strategies/cointegration.hpp", 26,
             "The diagnostics exist -- hedge ratio, half-life, pair selection "
             "-- but there is no entry/exit rule and no pair P&L."},
    AtlasRow{"1. Statistical alpha", "Statistical arbitrage",
             AtlasStatus::Partial, "strategies/score.hpp", 19,
             "The parts are here (cointegration, signal blending, "
             "neutralisation); nothing assembles them into a cross-sectional "
             "book."},
    AtlasRow{"1. Statistical alpha", "Factor models (Fama-French, PCA)",
             AtlasStatus::Absent, "", -1,
             "Decomposing returns into common drivers -- value, size, "
             "momentum. Nothing here extracts factors, and there is no PCA."},
    AtlasRow{"1. Statistical alpha", "Relative value",
             AtlasStatus::Implemented, "strategies/fundamentals.hpp", -1,
             "Is this company cheap against its own sector, on multiples "
             "known at the time rather than restated later?"},
    AtlasRow{"1. Statistical alpha", "Spread / calendar trading",
             AtlasStatus::Implemented, "strategies/calendar.hpp", 31,
             "Prices the same underlying at two expiries against each other "
             "and flags when the pair is mutually impossible."},
    AtlasRow{"1. Statistical alpha", "Regime detection",
             AtlasStatus::Implemented, "strategies/regime.hpp", 17,
             "Classifies the market as trending, choppy or stressed BEFORE a "
             "signal is applied, because the same signal is not always right."},

    // -- 2. Machine learning ------------------------------------------------
    AtlasRow{"2. Machine learning", "Linear / ridge regression",
             AtlasStatus::Implemented, "strategies/cointegration.hpp", 26,
             "The straight-line fit everything else is measured against. "
             "Ridge adds a penalty so it does not chase noise."},
    AtlasRow{"2. Machine learning", "Gradient boosting (GBDT / LightGBM)",
             AtlasStatus::Implemented, "models/gbdt.hpp", 16,
             "Hundreds of small decision trees, each correcting the last. The "
             "workhorse for tabular financial data, and Altair's main model."},
    AtlasRow{"2. Machine learning", "Decision tree", AtlasStatus::Implemented,
             "models/gbdt.hpp", 16,
             "A single tree of yes/no splits. Here it is a component of the "
             "booster rather than a standalone model."},
    AtlasRow{"2. Machine learning", "MLP / feedforward network",
             AtlasStatus::Partial, "models/mlp.hpp", 29,
             "Forward pass is exact, but the hidden layer is FROZEN at random "
             "initialisation and only a linear readout is trained. Not "
             "backpropagation."},
    AtlasRow{"2. Machine learning", "LSTM / GRU", AtlasStatus::Partial,
             "models/recurrent.hpp", 29,
             "Exact gate equations, but the recurrent weights never learn -- "
             "an echo-state reservoir with a ridge readout, not a trained "
             "recurrent network."},
    AtlasRow{"2. Machine learning", "Transformer / attention",
             AtlasStatus::Partial, "models/attention.hpp", 29,
             "Single-head causal attention with a mandatory no-peeking mask. "
             "No multi-head, no positional encoding, no layer norm, no stack."},
    AtlasRow{"2. Machine learning", "CNN", AtlasStatus::Partial,
             "models/attention.hpp", 29,
             "A one-dimensional dilated causal convolution (a TCN block). No "
             "two-dimensional convolution and no pooling."},
    AtlasRow{"2. Machine learning", "Random forest", AtlasStatus::Absent,
             "", -1,
             "Many trees trained on random subsets and averaged. A strong "
             "baseline for regime classification; not present."},
    AtlasRow{"2. Machine learning", "Logistic regression",
             AtlasStatus::Absent, "", -1,
             "Predicts a probability rather than a number -- 'will the next "
             "bar be up'. Only the logistic FUNCTION exists, not a classifier."},
    AtlasRow{"2. Machine learning", "SVM / KNN / autoencoder",
             AtlasStatus::Absent, "", -1,
             "None of the three. Autoencoders in particular would be the tool "
             "for compressing a feature set or flagging an odd day."},

    // -- 3. Time series and volatility --------------------------------------
    AtlasRow{"3. Time series & volatility", "AR (autoregression)",
             AtlasStatus::Implemented, "models/horizon_eval.hpp", 18,
             "Predicts the next value from the last few, in a straight line. "
             "The simplest forecast that is not just 'no change'."},
    AtlasRow{"3. Time series & volatility", "ARMA / ARIMA / SARIMA",
             AtlasStatus::Absent, "", -1,
             "AR plus a moving average of past ERRORS, plus differencing and "
             "seasonality. The classical forecasting family; not here."},
    AtlasRow{"3. Time series & volatility", "VAR (vector autoregression)",
             AtlasStatus::Absent, "", -1,
             "Several series predicting each other at once -- NIFTY, "
             "BANKNIFTY and VIX jointly. Only a bivariate VECM step exists."},
    AtlasRow{"3. Time series & volatility", "Kalman filter",
             AtlasStatus::Implemented, "analytics/kalman.hpp", 7,
             "Tracks a quantity you cannot observe directly from noisy "
             "measurements, updating as each one arrives. Scalar only here."},
    AtlasRow{"3. Time series & volatility", "Hidden Markov model",
             AtlasStatus::Implemented, "analytics/hmm.hpp", 17,
             "Assumes the market is in one of a few hidden states and infers "
             "which, and how long it usually stays."},
    AtlasRow{"3. Time series & volatility", "Markov regime chain",
             AtlasStatus::Implemented, "models/markov.hpp", 17,
             "Counts how often the market moves from one return bucket to "
             "another, and tests whether that is more than chance."},
    AtlasRow{"3. Time series & volatility", "Ornstein-Uhlenbeck",
             AtlasStatus::Partial, "strategies/cointegration.hpp", 26,
             "The mean-reversion speed of a spread (its half-life) is fitted; "
             "there is no OU simulator or full parameter estimation."},
    AtlasRow{"3. Time series & volatility", "Brownian motion / GBM",
             AtlasStatus::Implemented, "backtest/montecarlo.hpp", -1,
             "The random walk that underlies option pricing: prices drift and "
             "jitter, and the jitter compounds."},
    AtlasRow{"3. Time series & volatility", "Historical volatility & EWMA",
             AtlasStatus::Implemented, "analytics/ewma.hpp", 13,
             "How much the price has been moving, with recent days weighted "
             "more heavily. Decayed in TIME, not in ticks."},
    AtlasRow{"3. Time series & volatility", "GARCH / GJR-GARCH",
             AtlasStatus::Implemented, "analytics/garch.hpp", 13,
             "Volatility clusters: calm follows calm and violence follows "
             "violence. GJR adds that falls raise it more than rises do."},
    AtlasRow{"3. Time series & volatility", "EGARCH", AtlasStatus::Absent,
             "", -1,
             "A GARCH variant modelling log-variance, so it cannot go "
             "negative. GJR covers the same asymmetry here."},
    AtlasRow{"3. Time series & volatility", "Heston stochastic volatility",
             AtlasStatus::Partial, "backtest/montecarlo.hpp", -1,
             "Paths can be SIMULATED with volatility that is itself random. "
             "There is no Heston option pricer."},
    AtlasRow{"3. Time series & volatility", "Hurst exponent",
             AtlasStatus::Implemented, "analytics/hurst.hpp", 17,
             "Measures whether a series trends, mean-reverts, or is a coin "
             "flip -- and says how sure it is."},

    // -- 4. Microstructure and execution ------------------------------------
    AtlasRow{"4. Microstructure & execution", "Order-book imbalance",
             AtlasStatus::Implemented, "book/microstructure.hpp", 24,
             "More size resting on the bid than the ask hints at the next "
             "tick. Decays in milliseconds."},
    AtlasRow{"4. Microstructure & execution", "Microprice",
             AtlasStatus::Implemented, "book/microstructure.hpp", 24,
             "A fairer 'current price' than the mid, weighted by which side "
             "has more size behind it."},
    AtlasRow{"4. Microstructure & execution", "VPIN",
             AtlasStatus::Implemented, "book/flow.hpp", 24,
             "Estimates how much of today's flow is informed rather than "
             "noise -- a toxicity gauge that rises before dislocations."},
    AtlasRow{"4. Microstructure & execution", "Kyle's lambda",
             AtlasStatus::Implemented, "book/flow.hpp", 24,
             "How far the price moves per unit of net buying. The market's "
             "price of impatience."},
    AtlasRow{"4. Microstructure & execution", "Slippage & market impact",
             AtlasStatus::Implemented, "risk/slippage.hpp", 12,
             "What your own order costs you by moving the price against "
             "itself, fitted from your actual fills."},
    AtlasRow{"4. Microstructure & execution", "TWAP / VWAP / POV",
             AtlasStatus::Implemented, "oms/execution.hpp", 12,
             "Three ways to slice a large order over time so it does not "
             "announce itself: by clock, by volume, or as a share of it."},
    AtlasRow{"4. Microstructure & execution", "Almgren-Chriss",
             AtlasStatus::Implemented, "oms/shortfall.hpp", 12,
             "The trade-off between trading fast (costly impact) and slow "
             "(costly risk), solved rather than guessed."},
    AtlasRow{"4. Microstructure & execution", "Implementation shortfall",
             AtlasStatus::Implemented, "oms/shortfall.hpp", 12,
             "The gap between the price you decided at and the price you got, "
             "split into the reasons for it."},
    AtlasRow{"4. Microstructure & execution", "Queue position",
             AtlasStatus::Absent, "", -1,
             "Where your order sits in the line at a price level, which "
             "decides whether it fills at all. Deferred since P2-09c."},
    AtlasRow{"4. Microstructure & execution", "Fill probability",
             AtlasStatus::Absent, "", -1,
             "The chance a resting limit order gets hit. The backtester uses "
             "deterministic fill rules instead."},
    AtlasRow{"4. Microstructure & execution", "Trade arrival (Hawkes)",
             AtlasStatus::Absent, "", -1,
             "Trades cluster: one arrival makes the next more likely. Nothing "
             "here models arrival intensity."},

    // -- 5. Risk ------------------------------------------------------------
    AtlasRow{"5. Risk", "VaR -- historical, parametric, Monte Carlo",
             AtlasStatus::Implemented, "risk/var.hpp", 14,
             "'On a bad day, how much?' Three ways, which disagree -- and the "
             "disagreement is the useful part."},
    AtlasRow{"5. Risk", "Expected shortfall (CVaR)",
             AtlasStatus::Implemented, "risk/var.hpp", 14,
             "Not the threshold but the average loss BEYOND it. What replaced "
             "VaR at institutional desks after 2008."},
    AtlasRow{"5. Risk", "Stress testing", AtlasStatus::Implemented,
             "risk/stress.hpp", 14,
             "Replays a real crisis window against today's book instead of "
             "asking a model to imagine one."},
    AtlasRow{"5. Risk", "Correlation & covariance (Ledoit-Wolf)",
             AtlasStatus::Implemented, "risk/covariance.hpp", 15,
             "How positions move together -- and a shrunk estimate, because "
             "raw sample covariance is unreliable and correlations spike in "
             "crises."},
    AtlasRow{"5. Risk", "Drawdown", AtlasStatus::Implemented,
             "backtest/montecarlo.hpp", -1,
             "The worst peak-to-trough fall. The number that decides whether "
             "a strategy is survivable, not whether it is profitable."},
    AtlasRow{"5. Risk", "Beta / sector neutralisation",
             AtlasStatus::Implemented, "risk/neutralise.hpp", 15,
             "Strips out market and sector exposure so what is left is the "
             "bet you actually meant to make."},
    AtlasRow{"5. Risk", "Factor risk model", AtlasStatus::Absent, "", -1,
             "Splitting portfolio risk into named common factors. Only sector "
             "and beta buckets exist; there is no factor covariance."},
    AtlasRow{"5. Risk", "Conformal risk control", AtlasStatus::Implemented,
             "models/conformal.hpp", 18,
             "Makes a forecast's error bar HONEST by calibrating it against "
             "how wrong the model has actually been. From the 2026 Schmitt "
             "paper."},

    // -- 6. Options and derivatives -----------------------------------------
    AtlasRow{"6. Options & derivatives", "Black-Scholes / Black-76",
             AtlasStatus::Implemented, "analytics/greeks.hpp", 21,
             "The closed-form fair value of a European option. Black-76 "
             "prices off the FUTURE, which is what NIFTY options settle on."},
    AtlasRow{"6. Options & derivatives",
             "Greeks -- delta, gamma, vega, theta, rho",
             AtlasStatus::Implemented, "analytics/greeks.hpp", 21,
             "How the option's value moves when the spot moves, when that "
             "movement accelerates, when volatility shifts, and as time runs "
             "out."},
    AtlasRow{"6. Options & derivatives",
             "Higher greeks -- vanna, volga, charm", AtlasStatus::Implemented,
             "analytics/greeks2.hpp", 21,
             "Second-order effects: how delta itself drifts with volatility "
             "and with time. What a hedged book actually bleeds on."},
    AtlasRow{"6. Options & derivatives", "Implied volatility",
             AtlasStatus::Implemented, "analytics/iv.hpp", 21,
             "Runs the pricer BACKWARDS: given the traded price, what "
             "volatility does the market believe? Carries its own error bar."},
    AtlasRow{"6. Options & derivatives", "SVI volatility smile",
             AtlasStatus::Implemented, "analytics/svi_fit.hpp", 21,
             "Fits one smooth curve through a whole expiry's implied "
             "volatilities, and checks the fit is not internally arbitrageable."},
    AtlasRow{"6. Options & derivatives", "SABR", AtlasStatus::Implemented,
             "analytics/sabr.hpp", 21,
             "A smile model where volatility is itself random. Industry "
             "standard, with a check that its implied density stays positive."},
    AtlasRow{"6. Options & derivatives", "Local volatility (Dupire)",
             AtlasStatus::Implemented, "analytics/sabr.hpp", 21,
             "Backs out the volatility at each price and time that would "
             "reproduce every option quote at once."},
    AtlasRow{"6. Options & derivatives", "American approximation",
             AtlasStatus::Implemented, "analytics/american.hpp", 21,
             "Values an option that can be exercised early. NIFTY options "
             "cannot, so this is for equity single stocks."},
    AtlasRow{"6. Options & derivatives", "Monte Carlo option pricing",
             AtlasStatus::Implemented, "backtest/mc_pricing.hpp", -1,
             "Prices by simulating thousands of futures and averaging. Every "
             "price here carries its standard error."},
    AtlasRow{"6. Options & derivatives", "India VIX replication",
             AtlasStatus::Implemented, "analytics/vix.hpp", 13,
             "Rebuilds the volatility index from the option chain, which is "
             "how you learn what it is actually measuring."},
    AtlasRow{"6. Options & derivatives", "Binomial / trinomial tree",
             AtlasStatus::Absent, "", -1,
             "Prices by stepping through a lattice of up/down moves. The most "
             "intuitive method; referenced only as an external benchmark."},
    AtlasRow{"6. Options & derivatives", "Finite-difference PDE pricer",
             AtlasStatus::Absent, "", -1,
             "Solves the pricing equation on a grid. Finite differences are "
             "used for local vol, but there is no PDE pricer."},

    // -- 7. Portfolio construction ------------------------------------------
    AtlasRow{"7. Portfolio construction", "Position sizing",
             AtlasStatus::Implemented, "risk/sizing.hpp", 25,
             "Turns an edge into a number of lots. Three methods run and the "
             "SMALLEST binds, and the page says which one did."},
    AtlasRow{"7. Portfolio construction", "Kelly criterion",
             AtlasStatus::Implemented, "risk/sizing.hpp", 25,
             "The mathematically optimal fraction to bet given an edge -- "
             "sized here on the edge's LOWER bound, never the point estimate."},
    AtlasRow{"7. Portfolio construction", "Volatility targeting",
             AtlasStatus::Implemented, "risk/sizing.hpp", 25,
             "If the market gets twice as wild, halve the position. Keeps "
             "risk steady instead of exposure steady."},
    AtlasRow{"7. Portfolio construction", "Risk parity",
             AtlasStatus::Implemented, "risk/optimise.hpp", 15,
             "Allocate so every holding contributes the same RISK, not the "
             "same money."},
    AtlasRow{"7. Portfolio construction", "Minimum variance",
             AtlasStatus::Implemented, "risk/optimise.hpp", 15,
             "The mix that wobbles least, ignoring expected return entirely."},
    AtlasRow{"7. Portfolio construction", "Black-Litterman",
             AtlasStatus::Implemented, "risk/stress.hpp", 15,
             "Blends the market's implied view with your own, weighted by how "
             "confident you are. Absolute views only."},
    AtlasRow{"7. Portfolio construction", "Mean-variance optimisation",
             AtlasStatus::Absent, "", -1,
             "The Markowitz efficient frontier -- the textbook optimiser. Its "
             "absence is why Black-Litterman's output has nothing to feed."},

    // -- 8. Alternative data ------------------------------------------------
    AtlasRow{"8. Alternative data", "News / NLP / sentiment",
             AtlasStatus::Absent, "", -1,
             "Reading text as a signal. Nothing in this tree ingests text; "
             "there is no corpus and no model."},
    AtlasRow{"8. Alternative data", "Event models", AtlasStatus::Absent,
             "", -1,
             "Trading around earnings, policy and expiry. Expiry TIMING is a "
             "feature, but nothing models the event's effect."},

    // -- 9. Simulation ------------------------------------------------------
    AtlasRow{"9. Simulation", "Monte Carlo", AtlasStatus::Implemented,
             "backtest/montecarlo.hpp", -1,
             "Runs the world thousands of times to get a DISTRIBUTION of "
             "outcomes instead of one guess."},
    AtlasRow{"9. Simulation", "Bootstrapping", AtlasStatus::Implemented,
             "backtest/montecarlo.hpp", -1,
             "Reshuffles real history rather than assuming a bell curve. The "
             "block variant keeps volatility clustering intact."},
    AtlasRow{"9. Simulation", "Jump diffusion", AtlasStatus::Implemented,
             "backtest/montecarlo.hpp", -1,
             "Adds sudden gaps to the smooth random walk, because real prices "
             "leap and Brownian motion does not."},
    AtlasRow{"9. Simulation", "Walk-forward & purged CV",
             AtlasStatus::Implemented, "backtest/validation.hpp", 16,
             "Tests only on data the model has never seen, with a gap so a "
             "label cannot leak backwards. Random K-fold is banned."},
    AtlasRow{"9. Simulation", "Agent-based simulation", AtlasStatus::Absent,
             "", -1,
             "Simulating a market as interacting traders rather than as an "
             "equation. Not present."},

    // -- 10. Reinforcement learning -----------------------------------------
    AtlasRow{"10. Reinforcement learning", "Q-learning",
             AtlasStatus::Implemented, "models/regime_rl.hpp", 17,
             "Learns by trial and error which action pays in which market "
             "state. Shipped against a ten-line heuristic it must beat."},
    AtlasRow{"10. Reinforcement learning", "DQN / PPO / actor-critic",
             AtlasStatus::Absent, "", -1,
             "Neural-network reinforcement learning. QUANTLAB trained a PPO "
             "agent and REJECTED it: it lost to a simple heuristic."},

    // -- cross-cutting, and not in the roadmap's ten ------------------------
    AtlasRow{"Cross-cutting", "Transaction cost calculator",
             AtlasStatus::Implemented, "risk/cost.hpp", 6,
             "Every charge itemised in exact integer paise -- STT, exchange, "
             "GST, stamp. No signal is allowed to exist before its cost."},
    AtlasRow{"Cross-cutting", "Model scorecards & drift",
             AtlasStatus::Implemented, "flagging/drift.hpp", 23,
             "Watches each model's own record per regime and raises a flag "
             "when the world stops matching what it was trained on."},
    AtlasRow{"Cross-cutting", "Forecast scorecard",
             AtlasStatus::Implemented, "models/forecast_scorecard.hpp", 18,
             "Judges a price forecast against a random walk with a paired "
             "test, and says BETTER, WORSE, NO BETTER, or NOT ENOUGH "
             "EVIDENCE."},
    AtlasRow{"Cross-cutting", "Ensemble aggregator",
             AtlasStatus::Implemented, "models/aggregator.hpp", 10,
             "Combines several models' forecasts, correcting for the fact "
             "that models which agree are not independent evidence."},
    AtlasRow{"Cross-cutting", "Feature registry",
             AtlasStatus::Implemented, "features/registry.hpp", 30,
             "Every feature declared, versioned and hashed, and refused for a "
             "horizon it cannot in principle predict."},
    AtlasRow{"Cross-cutting", "DCF valuation", AtlasStatus::Implemented,
             "models/dcf.hpp", 9,
             "What a company is worth from the cash it will produce. FCFF and "
             "FCFE kept type-separate so the wrong discount rate cannot be "
             "used."},
};

inline constexpr std::size_t kAtlasCount = kAtlasRows.size();

}  // namespace altair::ui
