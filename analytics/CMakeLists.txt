# analytics/ - greeks, IV, rolling stats, India VIX (Phase 3).
#
# Header-only. Doubles are permitted here and nowhere near the ledger:
# CLAUDE.md rule 3 allows them in analytics and bans them in money.

add_library(altair_analytics INTERFACE)
add_library(altair::analytics ALIAS altair_analytics)

target_include_directories(altair_analytics INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(altair_analytics INTERFACE
    altair_types
    altair_time
    altair_flags
)

if(ALTAIR_BUILD_TESTS)
    add_executable(altair_greeks_test tests/test_greeks.cpp)
    target_link_libraries(altair_greeks_test PRIVATE altair_analytics altair_flags)
    add_test(NAME greeks COMMAND altair_greeks_test)

    add_executable(altair_iv_test tests/test_iv.cpp)
    target_link_libraries(altair_iv_test PRIVATE altair_analytics altair_flags)
    add_test(NAME iv COMMAND altair_iv_test)

    add_executable(altair_rolling_test tests/test_rolling.cpp)
    target_link_libraries(altair_rolling_test PRIVATE altair_analytics altair_flags)
    add_test(NAME rolling COMMAND altair_rolling_test)

    add_executable(altair_ewma_test tests/test_ewma.cpp)
    target_link_libraries(altair_ewma_test PRIVATE altair_analytics altair_flags)
    add_test(NAME ewma COMMAND altair_ewma_test)

    add_executable(altair_hurst_test tests/test_hurst.cpp)
    target_link_libraries(altair_hurst_test PRIVATE altair_analytics altair_flags)
    add_test(NAME hurst COMMAND altair_hurst_test)

    add_executable(altair_svi_test tests/test_svi.cpp)
    target_link_libraries(altair_svi_test PRIVATE altair_analytics altair_flags)
    add_test(NAME svi COMMAND altair_svi_test)

    add_executable(altair_svi_fit_test tests/test_svi_fit.cpp)
    target_link_libraries(altair_svi_fit_test PRIVATE altair_analytics altair_flags)
    add_test(NAME svi_fit COMMAND altair_svi_fit_test)

    add_executable(altair_greeks2_test tests/test_greeks2.cpp)
    target_link_libraries(altair_greeks2_test PRIVATE altair_analytics altair_flags)
    add_test(NAME greeks2 COMMAND altair_greeks2_test)

    add_executable(altair_vix_test tests/test_vix.cpp)
    target_link_libraries(altair_vix_test PRIVATE altair_analytics altair_flags)
    add_test(NAME vix COMMAND altair_vix_test)

    add_executable(altair_derivatives_test tests/test_derivatives.cpp)
    target_link_libraries(altair_derivatives_test PRIVATE altair_analytics altair_flags)
    add_test(NAME derivatives COMMAND altair_derivatives_test)

    add_executable(altair_american_test tests/test_american.cpp)
    target_link_libraries(altair_american_test PRIVATE altair_analytics altair_flags)
    add_test(NAME american COMMAND altair_american_test)

    add_executable(altair_indicators_test tests/test_indicators.cpp)
    target_link_libraries(altair_indicators_test PRIVATE altair_analytics altair_flags)
    add_test(NAME indicators COMMAND altair_indicators_test)

    add_executable(altair_structure_test tests/test_structure.cpp)
    target_link_libraries(altair_structure_test PRIVATE altair_analytics altair_flags)
    add_test(NAME structure COMMAND altair_structure_test)
endif()
