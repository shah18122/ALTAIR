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
    add_executable(altair_labels_test tests/test_labels.cpp)
    target_link_libraries(altair_labels_test PRIVATE altair_models altair_flags)
    add_test(NAME models_labels COMMAND altair_labels_test)

    add_executable(altair_dataset_test tests/test_dataset.cpp)
    target_link_libraries(altair_dataset_test PRIVATE altair_models altair_flags)
    add_test(NAME models_dataset COMMAND altair_dataset_test)

    add_executable(altair_training_test tests/test_training.cpp)
    target_link_libraries(altair_training_test PRIVATE altair_models altair_flags)
    add_test(NAME models_training COMMAND altair_training_test)

    add_executable(altair_recurrent_test tests/test_recurrent.cpp)
    target_link_libraries(altair_recurrent_test PRIVATE altair_models altair_flags)
    add_test(NAME models_recurrent COMMAND altair_recurrent_test)

    add_executable(altair_attention_test tests/test_attention.cpp)
    target_link_libraries(altair_attention_test PRIVATE altair_models altair_flags)
    add_test(NAME models_attention COMMAND altair_attention_test)

    add_executable(altair_serving_test tests/test_serving.cpp)
    target_link_libraries(altair_serving_test PRIVATE altair_models altair_strategies altair_flags)
    add_test(NAME models_serving COMMAND altair_serving_test)

    # P8-13. The ONLY model test that runs against real market data, because a
    # Markov chain over daily return states is the only model in this tree the
    # available data can actually train. dataset/ is gitignored; the test says
    # so and returns non-zero rather than passing vacuously if it is absent.
    add_executable(altair_markov_test tests/test_markov.cpp)
    target_link_libraries(altair_markov_test PRIVATE altair_models altair_flags)
    target_compile_definitions(altair_markov_test PRIVATE
        ALTAIR_DATASET_DIR="${CMAKE_SOURCE_DIR}/dataset")
    add_test(NAME models_markov COMMAND altair_markov_test)

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

    # P8-15. Horizon forecast net of cost, on the real 60-minute series.
    add_executable(altair_horizon_test tests/test_horizon_eval.cpp)
    target_link_libraries(altair_horizon_test
        PRIVATE altair_models altair_backtest altair_flags)
    target_compile_definitions(altair_horizon_test PRIVATE
        ALTAIR_DATASET_DIR="${CMAKE_SOURCE_DIR}/dataset")
    add_test(NAME models_horizon COMMAND altair_horizon_test)

    # P8-16. Trains the neural tier on the real 5-minute series. Links
    # altair_backtest for the walk-forward split, like P8-14 and P8-15.
    add_executable(altair_neural_5m_test tests/test_neural_5m.cpp)
    target_link_libraries(altair_neural_5m_test
        PRIVATE altair_models altair_backtest altair_flags)
    target_compile_definitions(altair_neural_5m_test PRIVATE
        ALTAIR_DATASET_DIR="${CMAKE_SOURCE_DIR}/dataset")
    add_test(NAME models_neural_5m COMMAND altair_neural_5m_test)
endif()
