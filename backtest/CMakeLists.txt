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

    add_executable(altair_agent_market_test tests/test_agent_market.cpp)
    target_link_libraries(altair_agent_market_test PRIVATE altair_backtest altair_flags)
    add_test(NAME backtest_agent_market COMMAND altair_agent_market_test)

    # P17-01. Monte Carlo OPTION pricing. Links altair_analytics because the
    # European control is priced by the real black76 -- an MC pricer validated
    # against its own reimplementation of Black-76 would be checking a copy of
    # itself.
    add_executable(altair_mc_pricing_test tests/test_mc_pricing.cpp)
    target_link_libraries(altair_mc_pricing_test
        PRIVATE altair_backtest altair_analytics altair_flags)
    add_test(NAME backtest_mc_pricing COMMAND altair_mc_pricing_test)
endif()
