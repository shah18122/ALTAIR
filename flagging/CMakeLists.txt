# flagging/ - per-model scorecards, drift detection, auto-correction.
#
# The machinery behind CLAUDE.md's reality check: "Quarterly retraining
# degrades as easily as it improves. Shadow -> canary -> auto-rollback is what
# stops it becoming a slow-motion self-inflicted loss."
#
# Links no oms/ header. This directory decides whether a model should be
# trusted; the trade handler decides what to do with a model that is.
add_library(altair_flagging INTERFACE)
add_library(altair::flagging ALIAS altair_flagging)

target_include_directories(altair_flagging INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(altair_flagging INTERFACE
    altair_types
    altair_time
    altair_analytics
    altair_features
    altair_strategies
    altair_models
)

if(ALTAIR_BUILD_TESTS)
    add_executable(altair_flagging_test tests/test_flagging.cpp)
    target_link_libraries(altair_flagging_test PRIVATE altair_flagging altair_flags)
    add_test(NAME flagging COMMAND altair_flagging_test)
endif()
