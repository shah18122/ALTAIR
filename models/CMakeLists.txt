# models/ - labels, dataset assembly, training, the model zoo, serving,
# aggregation and confidence intervals.
#
# NOTE ON LibTorch. The ROADMAP names LibTorch for training and ONNX for
# serving. Neither is vendored yet (LibTorch is ~2.5 GB and the `net` preset
# already caps at 4 jobs for compiler-heap reasons), so the architectures here
# are implemented in plain C++23 behind `models/tensor.hpp` and a stable
# `Model` interface. A LibTorch backend drops in behind the same interface
# without touching a caller.
#
# Links no oms/ header. A model produces a forecast with an error bar; the
# trade handler decides what to do with it.

add_library(altair_models INTERFACE)
add_library(altair::models ALIAS altair_models)

target_include_directories(altair_models INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(altair_models INTERFACE
    altair_types
    altair_time
    altair_analytics
    altair_features
    altair_backtest
    altair_risk
    altair_strategies
)

if(ALTAIR_BUILD_TESTS)
    # Classical, deterministic research primitives added for the Atlas:
    # logistic classification, shallow random forests, PCA factors and VAR(1).
    add_executable(altair_classical_models_test tests/test_classical.cpp)
    target_link_libraries(altair_classical_models_test PRIVATE altair_models altair_flags)
    add_test(NAME models_classical COMMAND altair_classical_models_test)

    add_executable(altair_time_series_test tests/test_time_series.cpp)
    target_link_libraries(altair_time_series_test PRIVATE altair_models altair_flags)
    add_test(NAME models_time_series COMMAND altair_time_series_test)

    add_executable(altair_fill_probability_test tests/test_fill_probability.cpp)
    target_link_libraries(altair_fill_probability_test PRIVATE altair_models altair_flags)
    add_test(NAME models_fill_probability COMMAND altair_fill_probability_test)

    add_executable(altair_hawkes_test tests/test_hawkes.cpp)
    target_link_libraries(altair_hawkes_test PRIVATE altair_models altair_flags)
    add_test(NAME models_hawkes COMMAND altair_hawkes_test)

    add_executable(altair_event_study_test tests/test_event_study.cpp)
    target_link_libraries(altair_event_study_test PRIVATE altair_models altair_flags)
    add_test(NAME models_event_study COMMAND altair_event_study_test)

    add_executable(altair_sentiment_test tests/test_sentiment.cpp)
    target_link_libraries(altair_sentiment_test PRIVATE altair_models altair_flags)
    add_test(NAME models_sentiment COMMAND altair_sentiment_test)

    add_executable(altair_deep_rl_test tests/test_deep_rl.cpp)
    target_link_libraries(altair_deep_rl_test PRIVATE altair_models altair_flags)
    add_test(NAME models_deep_rl COMMAND altair_deep_rl_test)

    add_executable(altair_named_factors_test tests/test_named_factors.cpp)
    target_link_libraries(altair_named_factors_test PRIVATE altair_models altair_flags)
    add_test(NAME models_named_factors COMMAND altair_named_factors_test)

    add_executable(altair_stream_forecast_test tests/test_stream_forecast.cpp)
    target_link_libraries(altair_stream_forecast_test PRIVATE altair_models altair_flags)
    add_test(NAME models_stream_forecast COMMAND altair_stream_forecast_test)
    add_executable(altair_labels_test tests/test_labels.cpp)
    target_link_libraries(altair_labels_test PRIVATE altair_models altair_flags)
    add_test(NAME models_labels COMMAND altair_labels_test)

    add_executable(altair_dataset_test tests/test_dataset.cpp)
    target_link_libraries(altair_dataset_test PRIVATE altair_models altair_flags)
    add_test(NAME models_dataset COMMAND altair_dataset_test)

    # M12. The trainable MLP: hidden-layer backpropagation, checked against a
    # central finite difference of the objective it descends. Links
    # altair_backtest for `Block`, the same way the neural tier below does.
    add_executable(altair_mlp_test tests/test_mlp.cpp)
    target_link_libraries(altair_mlp_test PRIVATE altair_models altair_backtest altair_flags)
    add_test(NAME models_mlp COMMAND altair_mlp_test)

    add_executable(altair_training_test tests/test_training.cpp)
    target_link_libraries(altair_training_test PRIVATE altair_models altair_flags)
    add_test(NAME models_training COMMAND altair_training_test)

    add_executable(altair_recurrent_test tests/test_recurrent.cpp)
    target_link_libraries(altair_recurrent_test PRIVATE altair_models altair_flags)
    add_test(NAME models_recurrent COMMAND altair_recurrent_test)

    add_executable(altair_trainable_recurrent_test tests/test_trainable_recurrent.cpp)
    target_link_libraries(altair_trainable_recurrent_test PRIVATE altair_models altair_flags)
    add_test(NAME models_trainable_recurrent COMMAND altair_trainable_recurrent_test)

    add_executable(altair_attention_test tests/test_attention.cpp)
    target_link_libraries(altair_attention_test PRIVATE altair_models altair_flags)
    add_test(NAME models_attention COMMAND altair_attention_test)

    add_executable(altair_transformer_test tests/test_transformer.cpp)
    target_link_libraries(altair_transformer_test PRIVATE altair_models altair_flags)
    add_test(NAME models_transformer COMMAND altair_transformer_test)

    add_executable(altair_serving_test tests/test_serving.cpp)
    target_link_libraries(altair_serving_test PRIVATE altair_models altair_strategies altair_flags)
    add_test(NAME models_serving COMMAND altair_serving_test)
    # Runs threads: the TSan CI job runs exactly the `concurrency` label.
    set_tests_properties(models_serving PROPERTIES LABELS concurrency)

    # P8-13. The ONLY model test that runs against real market data, because a
    # Markov chain over daily return states is the only model in this tree the
    # available data can actually train. dataset/ is gitignored; the test says
    # so and returns non-zero rather than passing vacuously if it is absent.
    add_executable(altair_markov_test tests/test_markov.cpp)
    target_link_libraries(altair_markov_test PRIVATE altair_models altair_flags)
    target_compile_definitions(altair_markov_test PRIVATE
        ALTAIR_DATASET_DIR="${CMAKE_SOURCE_DIR}/dataset")
    add_test(NAME models_markov COMMAND altair_markov_test)
    # Reads dataset/ (not in git): CI runs `ctest -LE dataset`.
    set_tests_properties(models_markov PROPERTIES LABELS dataset)

    # P8-14. Walk-forward evaluation of the same chain. Links altair_backtest
    # for WalkForwardSpec -- the split discipline is defined once, in the
    # directory that owns it, and is not reimplemented here.
    add_executable(altair_markov_eval_test tests/test_markov_eval.cpp)
    target_link_libraries(altair_markov_eval_test
        PRIVATE altair_models altair_backtest altair_flags)
    target_compile_definitions(altair_markov_eval_test PRIVATE
        ALTAIR_DATASET_DIR="${CMAKE_SOURCE_DIR}/dataset")
    add_test(NAME models_markov_eval COMMAND altair_markov_eval_test)

    # P10-02b. DCF sensitivity grid + the reverse DCF. Finishes the part of
    # the DCF that never needed filings: the forward direction is blocked on
    # point-in-time fundamentals, the inversion only needs a price.
    add_executable(altair_dcf_sensitivity_test tests/test_dcf_sensitivity.cpp)
    target_link_libraries(altair_dcf_sensitivity_test PRIVATE altair_models altair_flags)
    add_test(NAME models_dcf_sensitivity COMMAND altair_dcf_sensitivity_test)

    # P16-01..05. Gradient-boosted trees -- the one model family models/ did
    # not have and the one that actually wins on tabular financial data. Aimed
    # at META-LABELING per QUANTLAB Q6, not direction: P8-15 and P8-16 already
    # established there is nothing to predict at these horizons.
    add_executable(altair_gbdt_test tests/test_gbdt.cpp)
    target_link_libraries(altair_gbdt_test PRIVATE altair_models altair_flags)
    target_compile_definitions(altair_gbdt_test PRIVATE
        ALTAIR_DATASET_DIR="${CMAKE_SOURCE_DIR}/dataset")
    add_test(NAME models_gbdt COMMAND altair_gbdt_test)

    # P18-01/02/03. Regime clustering ranked return-blind, a tabular
    # Q-learner on execution timing only, and the ten-line heuristic control
    # QUANTLAB rejected PPO against. The control IS the card.
    add_executable(altair_regime_rl_test tests/test_regime_rl.cpp)
    target_link_libraries(altair_regime_rl_test PRIVATE altair_models altair_flags)
    target_compile_definitions(altair_regime_rl_test PRIVATE
        ALTAIR_DATASET_DIR="${CMAKE_SOURCE_DIR}/dataset")
    add_test(NAME models_regime_rl COMMAND altair_regime_rl_test)

    # P16-06. Gradient-boosted SPOT FORECAST across four horizons, net of
    # cost, with leaf-wise (LightGBM) growth run against level-wise on
    # identical folds. The cost hurdle and the perfect-oracle bound are
    # printed BEFORE any model number, per rule 5.
    add_executable(altair_spot_forecast_test tests/test_spot_forecast.cpp)
    target_link_libraries(altair_spot_forecast_test PRIVATE altair_models altair_flags)
    target_compile_definitions(altair_spot_forecast_test PRIVATE
        ALTAIR_DATASET_DIR="${CMAKE_SOURCE_DIR}/dataset")
    add_test(NAME models_spot_forecast COMMAND altair_spot_forecast_test)
    # Reads dataset/ (not in git): CI runs `ctest -LE dataset`.
    set_tests_properties(models_spot_forecast PROPERTIES LABELS dataset)

    # P33-01. The forecast SCORECARD and its decision maker. Needs no dataset:
    # it is checked against two synthetic series whose answer is known before
    # the test -- one a forecaster can beat and one it cannot -- because a
    # judge that always says "no better" is right about almost every financial
    # series and is worthless.
    add_executable(altair_forecast_scorecard_test
        tests/test_forecast_scorecard.cpp)
    target_link_libraries(altair_forecast_scorecard_test
        PRIVATE altair_models altair_flags)
    add_test(NAME models_forecast_scorecard
             COMMAND altair_forecast_scorecard_test)

    # P35-01. Conformal calibration -- the band that covers what it claims.
    #
    # The test is the paper's own experiment on synthetic data: a band at half
    # the width it needs, whose exceedance the wrapper must pull back to
    # target. The control is the check that matters -- an ALREADY calibrated
    # band run through the same wrapper must come out still near target,
    # because a calibrator that only ever widens would pass the first test and
    # be useless.
    add_executable(altair_conformal_test tests/test_conformal.cpp)
    target_link_libraries(altair_conformal_test PRIVATE altair_models altair_flags)
    add_test(NAME models_conformal COMMAND altair_conformal_test)

    # P35-02. The calibrated band on a price forecast, and the by-regime
    # breakout that stops an average coverage number hiding a band which is
    # wrong exactly when volatility is high.
    #
    # Its first check is hard rule 7 made executable: detonate one bar and
    # require every volatility value at or before it to be bit-identical.
    add_executable(altair_calibration_test tests/test_calibration.cpp)
    target_link_libraries(altair_calibration_test PRIVATE altair_models altair_flags)
    add_test(NAME models_calibration COMMAND altair_calibration_test)

    # P41. Richer features, and whether the paired t moves.
    #
    # Its first check is hard rule 7 with a planted violation: detonate r[i],
    # the first element of the LABEL, and require the feature row to come back
    # bit-identical. The first draft of append_row() read r[i] and would have
    # scored magnificently on its own answer.
    #
    # The experiment is a WEAK gate and a LOUD report: asserting that rich
    # features must beat plain ones on NIFTY would be asserting a market fact.
    add_executable(altair_rich_features_test tests/test_rich_features.cpp)
    target_link_libraries(altair_rich_features_test
        PRIVATE altair_models altair_flags)
    target_compile_definitions(altair_rich_features_test PRIVATE
        ALTAIR_DATASET_DIR="${CMAKE_SOURCE_DIR}/dataset")
    add_test(NAME models_rich_features COMMAND altair_rich_features_test)
    # The market experiment, separately. Eight walk-forwards on real data is a
    # MEASUREMENT, not a unit check: labelled so the sanitizer presets can skip
    # it, and given its own timeout because under a plain Debug build it is
    # already the slowest test in the tree.
    add_test(NAME models_rich_features_experiment
             COMMAND altair_rich_features_test --experiment)
    set_tests_properties(models_rich_features_experiment PROPERTIES
        LABELS "experiment;dataset" TIMEOUT 1800)

    # P35-02b. The same calibration on the REAL NIFTY daily series.
    #
    # A weak gate and a loud report, on purpose. Asserting "conformal must beat
    # the RMSE band on NIFTY" would be asserting a market fact, and the day it
    # stopped being true the right response would be to read the number rather
    # than to edit the threshold until the build went green. So it asserts only
    # what must hold for any sane run and PRINTS the comparison.
    add_executable(altair_calibration_real_test tests/test_calibration_real.cpp)
    target_link_libraries(altair_calibration_real_test
        PRIVATE altair_models altair_flags)
    target_compile_definitions(altair_calibration_real_test PRIVATE
        ALTAIR_DATASET_DIR="${CMAKE_SOURCE_DIR}/dataset")
    add_test(NAME models_calibration_real COMMAND altair_calibration_real_test)
    # Reads dataset/ (not in git): CI runs `ctest -LE dataset`.
    set_tests_properties(models_calibration_real PROPERTIES LABELS dataset)

    # P8-15. Horizon forecast net of cost, on the real 60-minute series.
    add_executable(altair_horizon_test tests/test_horizon_eval.cpp)
    target_link_libraries(altair_horizon_test
        PRIVATE altair_models altair_backtest altair_flags)
    target_compile_definitions(altair_horizon_test PRIVATE
        ALTAIR_DATASET_DIR="${CMAKE_SOURCE_DIR}/dataset")
    add_test(NAME models_horizon COMMAND altair_horizon_test)
    # Reads dataset/ (not in git): CI runs `ctest -LE dataset`.
    set_tests_properties(models_horizon PROPERTIES LABELS dataset)

    # P8-16. Trains the neural tier on the real 5-minute series. Links
    # altair_backtest for the walk-forward split, like P8-14 and P8-15.
    add_executable(altair_neural_5m_test tests/test_neural_5m.cpp)
    target_link_libraries(altair_neural_5m_test
        PRIVATE altair_models altair_backtest altair_flags)
    target_compile_definitions(altair_neural_5m_test PRIVATE
        ALTAIR_DATASET_DIR="${CMAKE_SOURCE_DIR}/dataset")
    add_test(NAME models_neural_5m COMMAND altair_neural_5m_test)
endif()
