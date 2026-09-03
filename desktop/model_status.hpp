// desktop/model_status.hpp -- what every model has, what it is missing, and
// where the data comes from.
//
// P11Q-05c.
//
// "TRAINED" IS NOT A STATE A PANEL MAY INFER.
//
// Smit asked for a page showing every model, what data it has processed and
// what is left. The dangerous version of that page is the one that renders a
// row per model with a progress bar, because a progress bar at 0% and a
// progress bar that has never been connected to anything look identical.
//
// So `ModelState` has `NeverTrained` as its ordinal-zero default and there is
// no path that promotes a model to `Trained` without an artefact. Eleven of
// the twelve models in this tree are `NeverTrained`, and the panel says so in
// eleven rows rather than in a footnote.
//
// THE BINDING CONSTRAINT IS DATA, NOT COMPUTE, AND THE PANEL NAMES IT.
//
// The reflex reading of "no models are trained" is that LibTorch is missing.
// It is missing, and that is not why. `dataset/` holds 8,756 daily NIFTY bars,
// 3,153 sixty-minute, 1,207 one-minute -- and no tick data at all. The
// ten-minute forecast model cannot be trained on four partial days of
// one-minute bars whatever hardware is attached.
//
// Each row therefore carries `needs` and `has` as strings a person can compare
// on sight. The one model that IS trainable on what exists -- the Markov
// regime chain, P8-13 -- shows exactly why, and it is the only row that is
// green.
//
// AND THE FLOW DIAGRAM IS DRAWN FROM THE SAME TABLE.
//
// A hand-drawn architecture picture and a status table drift apart within a
// month, and the picture is the one people trust. `pipeline()` returns the
// stages and `ModelCatalogue` returns the models; the diagram widget renders
// both and colours each stage by whether anything downstream of it can
// actually run. One source, two views.

#pragma once

#include "feed_status.hpp"

#include <QString>

#include <cstdint>
#include <vector>

namespace altair::ui {

enum class ModelState : std::uint8_t {
    /// Ordinal zero, and the honest default. No artefact exists.
    NeverTrained = 0,
    /// Trainable on data that is present, and demonstrated by a test.
    TrainedOnRealData,
    /// Implemented and tested, but only against synthetic data.
    ValidatedOnSyntheticOnly,
    /// Cannot be trained here at all: the data it needs does not exist.
    BlockedOnData
};

[[nodiscard]] inline QString model_state_label(ModelState s) {
    switch (s) {
    case ModelState::TrainedOnRealData:
        return QStringLiteral("trained on real data");
    case ModelState::ValidatedOnSyntheticOnly:
        return QStringLiteral("synthetic only");
    case ModelState::BlockedOnData:
        return QStringLiteral("blocked on data");
    case ModelState::NeverTrained:
    default:
        return QStringLiteral("never trained");
    }
}

[[nodiscard]] inline QColor model_state_colour(ModelState s) {
    switch (s) {
    case ModelState::TrainedOnRealData:
        return QColor(0x1B, 0x8A, 0x4B);
    case ModelState::ValidatedOnSyntheticOnly:
        return QColor(0xB9, 0x77, 0x0B);
    case ModelState::BlockedOnData:
        return QColor(0xC0, 0x39, 0x2B);
    case ModelState::NeverTrained:
    default:
        return QColor(0x7F, 0x8C, 0x8D);
    }
}

struct ModelRow {
    QString name;
    QString card;
    QString header;
    ModelState state = ModelState::NeverTrained;
    /// What the model needs to be trainable. Plain words on purpose: this is
    /// read by a person deciding what data to go and get.
    QString needs;
    /// What exists today.
    QString has;
    /// The instrument this model is aimed at, in Smit's stated order.
    QString instrument;
};

/// Every model in `models/`, with the honest state of each.
///
/// Hand-maintained, and every row names its card. The alternative -- probing
/// for a `.pt` on disk -- would answer "is there a file" rather than "is this
/// model worth anything", and would report a model trained on four days of
/// data as trained.
[[nodiscard]] inline std::vector<ModelRow> model_catalogue() {
    return {
        {QStringLiteral("Markov regime chain"), QStringLiteral("P8-13"),
         QStringLiteral("models/markov.hpp"),
         ModelState::TrainedOnRealData,
         QStringLiteral("a few thousand daily bars"),
         QStringLiteral("8,755 daily NIFTY returns — chi2 298 vs 19 shuffled"),
         QStringLiteral("NIFTY spot")},

        {QStringLiteral("DCF (FCFF / FCFE)"), QStringLiteral("P10-02"),
         QStringLiteral("models/dcf.hpp"),
         ModelState::BlockedOnData,
         QStringLiteral("point-in-time fundamentals with FILING dates"),
         QStringLiteral("none — no filings ingested"),
         QStringLiteral("equities")},

        {QStringLiteral("MLP"), QStringLiteral("P8-08"),
         QStringLiteral("models/mlp.hpp"),
         ModelState::ValidatedOnSyntheticOnly,
         QStringLiteral("labelled intraday features"),
         QStringLiteral("synthetic only; 4 partial days of 1-minute bars"),
         QStringLiteral("NIFTY spot")},

        {QStringLiteral("GRU / LSTM"), QStringLiteral("P8-06"),
         QStringLiteral("models/recurrent.hpp"),
         ModelState::ValidatedOnSyntheticOnly,
         QStringLiteral("long intraday sequences"),
         QStringLiteral("synthetic only"),
         QStringLiteral("NIFTY future")},

        {QStringLiteral("Attention encoder"), QStringLiteral("P8-07"),
         QStringLiteral("models/attention.hpp"),
         ModelState::ValidatedOnSyntheticOnly,
         QStringLiteral("long intraday sequences + causal masking"),
         QStringLiteral("synthetic only"),
         QStringLiteral("NIFTY future")},

        {QStringLiteral("Training harness"), QStringLiteral("P8-04"),
         QStringLiteral("models/training.hpp"),
         ModelState::ValidatedOnSyntheticOnly,
         QStringLiteral("a dataset and a model to train"),
         QStringLiteral("walk-forward + purged CV implemented and tested"),
         QStringLiteral("—")},

        {QStringLiteral("Model registry"), QStringLiteral("P8-10"),
         QStringLiteral("models/registry.hpp"),
         ModelState::NeverTrained,
         QStringLiteral("a trained artefact to register"),
         QStringLiteral("no .pt or .onnx anywhere in the tree"),
         QStringLiteral("—")},

        {QStringLiteral("ONNX serving"), QStringLiteral("P8-11"),
         QStringLiteral("models/serving.hpp"),
         ModelState::NeverTrained,
         QStringLiteral("an exported ONNX graph + onnxruntime"),
         QStringLiteral("neither present"),
         QStringLiteral("—")},

        {QStringLiteral("Signal aggregator"), QStringLiteral("P8-12"),
         QStringLiteral("models/aggregator.hpp"),
         ModelState::NeverTrained,
         QStringLiteral("two or more member models producing signals"),
         QStringLiteral("one member exists (Markov) — n_eff needs more"),
         QStringLiteral("—")},

        {QStringLiteral("India VIX AR(1)"), QStringLiteral("P10-07 / P11Q-07"),
         QStringLiteral("strategies/vix_forecast.hpp"),
         ModelState::TrainedOnRealData,
         QStringLiteral("a VIX history; a realised-vol series for the premium"),
         QStringLiteral("527 daily bars — level residual sd 2.97x higher when "
                        "stressed; level band covers 88.1% there vs nominal "
                        "95.4%"),
         QStringLiteral("India VIX")},

        {QStringLiteral("Cointegration / pairs"), QStringLiteral("P10-04"),
         QStringLiteral("strategies/cointegration.hpp"),
         ModelState::BlockedOnData,
         QStringLiteral("two or more correlated daily price series"),
         QStringLiteral("only NIFTY and India VIX — no equity universe"),
         QStringLiteral("equities")},

        {QStringLiteral("Regime detector"), QStringLiteral("P6-03"),
         QStringLiteral("strategies/regime.hpp"),
         ModelState::ValidatedOnSyntheticOnly,
         QStringLiteral("a return series with a dwell requirement"),
         QStringLiteral("daily NIFTY is sufficient; not yet fitted"),
         QStringLiteral("NIFTY spot")},
    };
}

/// The instrument order Smit asked models to be built against.
[[nodiscard]] inline std::vector<QString> instrument_priority() {
    return {QStringLiteral("NIFTY spot"), QStringLiteral("NIFTY future"),
            QStringLiteral("India VIX"), QStringLiteral("BANKNIFTY spot"),
            QStringLiteral("BANKNIFTY future")};
}

// ---------------------------------------------------------------------------
// The pipeline
// ---------------------------------------------------------------------------

struct Stage {
    QString name;
    QString directory;
    WiringState state = WiringState::Unspecified;
    QString detail;
};

/// The data path, end to end, in the order it runs, FOR A GIVEN SOURCE.
///
/// THE SOURCE IS AN ARGUMENT BECAUSE "FEED" IS NOT ONE THING.
///
/// The first version of this function had a single `Feed` stage marked
/// blocked, which put the wall at stage 0 and drew every box hollow -- while
/// the window was at that moment being driven by a replay through that exact
/// stage. The picture said nothing works; the application was running.
///
/// Both halves were true of different paths. A replayed tick passes through
/// feed, normaliser, spec store, book, analytics and features and stops at
/// models. A live tick never starts, because there is no transport and no
/// session. Collapsing those into one box makes the diagram wrong for
/// whichever path you are actually on.
[[nodiscard]] inline std::vector<Stage> pipeline(FeedSource source) {
    const bool replaying = source == FeedSource::Replay;
    return {
        {replaying ? QStringLiteral("Feed (replay)")
                   : QStringLiteral("Feed (live)"),
         QStringLiteral("feed/"),
         replaying ? WiringState::Built : WiringState::BlockedOnInput,
         replaying
             ? QStringLiteral("forward-only replayer + real bar loader")
             : QStringLiteral("decoders are built; no transport, no session")},
        {QStringLiteral("Normaliser"), QStringLiteral("feed/normaliser.hpp"),
         WiringState::Built,
         QStringLiteral("broker token -> canonical InstrumentId")},
        {QStringLiteral("Spec store"), QStringLiteral("instruments/"),
         WiringState::Built,
         QStringLiteral("lot, tick, expiry — point-in-time")},
        {QStringLiteral("Book"), QStringLiteral("book/"),
         WiringState::Built,
         QStringLiteral("L2, imbalance, microprice — no live depth to feed it")},
        {QStringLiteral("Analytics"), QStringLiteral("analytics/"),
         WiringState::Built,
         QStringLiteral("greeks, IV, rolling stats, India VIX")},
        {QStringLiteral("Features"), QStringLiteral("features/"),
         WiringState::Built,
         QStringLiteral("versioned, horizon-banded registry")},
        {QStringLiteral("Models"), QStringLiteral("models/"),
         WiringState::BlockedOnInput,
         QStringLiteral("1 of 12 trained on real data (Markov, P8-13); the "
                        "rest need data that does not exist")},
        {QStringLiteral("Aggregator"), QStringLiteral("models/aggregator.hpp"),
         WiringState::BlockedOnInput,
         QStringLiteral("needs two or more members to combine")},
        {QStringLiteral("Risk"), QStringLiteral("risk/"),
         WiringState::Built,
         QStringLiteral("sizing, limits, portfolio greeks, cost calculator")},
        {QStringLiteral("OMS"), QStringLiteral("oms/"),
         WiringState::BlockedOnInput,
         QStringLiteral("state machine and translation built; no transport, "
                        "no session")},
        {QStringLiteral("Broker"), QStringLiteral("broker/"),
         WiringState::NotBuilt,
         QStringLiteral("HTTPS half needs the vcpkg `net` feature")},
    };
}

/// How far down the pipeline a tick can actually get on `source`.
///
/// Returns the index of the first stage that is NOT built -- so the diagram
/// draws the wall where it really is, and a caller cannot turn this into a
/// boolean that implies the whole path runs.
[[nodiscard]] inline std::size_t furthest_working_stage(FeedSource source) {
    const auto stages = pipeline(source);
    for (std::size_t i = 0; i < stages.size(); ++i) {
        if (stages[i].state != WiringState::Built) {
            return i;
        }
    }
    return stages.size();
}

} // namespace altair::ui
