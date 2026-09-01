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
endif()
