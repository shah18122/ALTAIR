# risk/ - sizing, limits, portfolio greeks, and the COST CALCULATOR.
#
# P3-09 is the cost calculator: CLAUDE.md rule 5 says every signal is priced
# net of full cost before it exists, and this is the file that makes that
# enforceable.

add_library(altair_risk INTERFACE)
add_library(altair::risk ALIAS altair_risk)

target_include_directories(altair_risk INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(altair_risk INTERFACE
    altair_analytics
    altair_types
    altair_time
    altair_instruments
    altair_flags
)

if(ALTAIR_BUILD_TESTS)
    add_executable(altair_cost_test tests/test_cost.cpp)
    target_link_libraries(altair_cost_test PRIVATE altair_risk altair_flags)
    add_test(NAME cost COMMAND altair_cost_test)

    add_executable(altair_slippage_test tests/test_slippage.cpp)
    target_link_libraries(altair_slippage_test PRIVATE altair_risk altair_flags)
    add_test(NAME slippage COMMAND altair_slippage_test)

    add_executable(altair_sizing_test tests/test_sizing.cpp)
    target_link_libraries(altair_sizing_test PRIVATE altair_risk altair_flags)
    add_test(NAME sizing COMMAND altair_sizing_test)

    add_executable(altair_limits_test tests/test_limits.cpp)
    target_link_libraries(altair_limits_test PRIVATE altair_risk altair_flags)
    add_test(NAME limits COMMAND altair_limits_test)

    add_executable(altair_portfolio_test tests/test_portfolio.cpp)
    target_link_libraries(altair_portfolio_test PRIVATE altair_risk altair_flags)
    add_test(NAME portfolio COMMAND altair_portfolio_test)
endif()
