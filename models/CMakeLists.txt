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
endif()
