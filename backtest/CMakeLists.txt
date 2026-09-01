# backtest/ - the tick replayer, the walk-forward harness, and Monte Carlo.
#
# Consumes `Tick`, which is what the live normaliser emits (rule 6): there is
# no backtest-only tick type and no branch on whether this is a replay. If the
# two diverged, the backtest would be measuring something that will never
# trade.
#
# Links no oms/ header. A backtest applies a fill MODEL; it does not place an
# order, and the missing dependency is the enforcement.

add_library(altair_backtest INTERFACE)
add_library(altair::backtest ALIAS altair_backtest)

target_include_directories(altair_backtest INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(altair_backtest INTERFACE
    altair_types
    altair_time
    altair_feed
    altair_risk
    altair_analytics
    altair_strategies
)

if(ALTAIR_BUILD_TESTS)
    add_executable(altair_backtest_engine_test tests/test_engine.cpp)
    target_link_libraries(altair_backtest_engine_test PRIVATE altair_backtest altair_flags)
    add_test(NAME backtest_engine COMMAND altair_backtest_engine_test)

    add_executable(altair_validation_test tests/test_validation.cpp)
    target_link_libraries(altair_validation_test PRIVATE altair_backtest altair_flags)
    add_test(NAME backtest_validation COMMAND altair_validation_test)

    add_executable(altair_montecarlo_test tests/test_montecarlo.cpp)
    target_link_libraries(altair_montecarlo_test PRIVATE altair_backtest altair_flags)
    add_test(NAME backtest_montecarlo COMMAND altair_montecarlo_test)
endif()
