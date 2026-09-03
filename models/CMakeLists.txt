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
endif()
