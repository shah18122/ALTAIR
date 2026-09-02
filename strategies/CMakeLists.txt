# strategies/ - signal generation and the arbitrage scanners.
#
# THIS DIRECTORY DETECTS. IT DOES NOT TRADE.
#
# A scanner produces a priced, executability-tagged observation. Deciding what
# to do with one -- whether to take it, at what size, against what limits --
# belongs to the trade handler in oms/, which is the only thing in the tree
# that can place an order. Nothing here links oms/, and the missing dependency
# below is the enforcement, not an oversight.

add_library(altair_strategies INTERFACE)
add_library(altair::strategies ALIAS altair_strategies)

target_include_directories(altair_strategies INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(altair_strategies INTERFACE
    altair_types
    altair_time
    altair_analytics
    altair_instruments
    altair_risk
    altair_features
)

if(ALTAIR_BUILD_TESTS)
    add_executable(altair_basis_test tests/test_basis.cpp)
    target_link_libraries(altair_basis_test PRIVATE altair_strategies altair_flags)
    add_test(NAME strategies_basis COMMAND altair_basis_test)

    add_executable(altair_parity_test tests/test_parity.cpp)
    target_link_libraries(altair_parity_test PRIVATE altair_strategies altair_flags)
    add_test(NAME strategies_parity COMMAND altair_parity_test)

    add_executable(altair_calendar_test tests/test_calendar.cpp)
    target_link_libraries(altair_calendar_test PRIVATE altair_strategies altair_flags)
    add_test(NAME strategies_calendar COMMAND altair_calendar_test)

    add_executable(altair_oppty_log_test tests/test_oppty_log.cpp)
    target_link_libraries(altair_oppty_log_test PRIVATE altair_strategies altair_flags)
    add_test(NAME strategies_oppty_log COMMAND altair_oppty_log_test)

    add_executable(altair_regime_test tests/test_regime.cpp)
    target_link_libraries(altair_regime_test PRIVATE altair_strategies altair_flags)
    add_test(NAME strategies_regime COMMAND altair_regime_test)

    add_executable(altair_score_test tests/test_score.cpp)
    target_link_libraries(altair_score_test PRIVATE altair_strategies altair_flags)
    add_test(NAME strategies_score COMMAND altair_score_test)

    add_executable(altair_fundamentals_test tests/test_fundamentals.cpp)
    target_link_libraries(altair_fundamentals_test PRIVATE altair_strategies altair_models altair_flags)
    add_test(NAME strategies_fundamentals COMMAND altair_fundamentals_test)

    add_executable(altair_cointegration_test tests/test_cointegration.cpp)
    target_link_libraries(altair_cointegration_test PRIVATE altair_strategies altair_flags)
    add_test(NAME strategies_cointegration COMMAND altair_cointegration_test)
endif()
