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

    add_executable(altair_band_option_fade_test tests/test_band_option_fade.cpp)
    target_link_libraries(altair_band_option_fade_test PRIVATE altair_strategies altair_flags)
    add_test(NAME strategies_band_option_fade COMMAND altair_band_option_fade_test)

    add_executable(altair_pairs_futures_test tests/test_pairs_futures.cpp)
    target_link_libraries(altair_pairs_futures_test PRIVATE altair_strategies altair_flags)
    add_test(NAME strategies_pairs_futures COMMAND altair_pairs_futures_test)

    add_executable(altair_stat_arb_test tests/test_stat_arb.cpp)
    target_link_libraries(altair_stat_arb_test PRIVATE altair_strategies altair_flags)
    add_test(NAME strategies_stat_arb COMMAND altair_stat_arb_test)

    add_executable(altair_pairs_trade_test tests/test_pairs_trade.cpp)
    target_link_libraries(altair_pairs_trade_test PRIVATE altair_strategies altair_flags)
    add_test(NAME strategies_pairs_trade COMMAND altair_pairs_trade_test)

    # P21-01/02. Momentum and mean reversion -- the Stage 3 hole. One test
    # binary for both because the whole reason to build them together is that
    # they are comparable, and a shared control (the random walk both must lose
    # money on) belongs in one place rather than copied into two.
    add_executable(altair_directional_test tests/test_directional.cpp)
    target_link_libraries(altair_directional_test PRIVATE altair_strategies altair_flags)
    add_test(NAME strategies_directional COMMAND altair_directional_test)

    # P21-03. The same two strategies on REAL NIFTY at four sampling rates.
    # Separate binary, matching the regime_real precedent above: the synthetic
    # acceptance test proves the machinery, this one asks whether anything is
    # actually there.
    add_executable(altair_directional_real_test tests/test_directional_real.cpp)
    target_link_libraries(altair_directional_real_test PRIVATE altair_strategies altair_flags)
    target_compile_definitions(altair_directional_real_test PRIVATE
        ALTAIR_DATASET_DIR="${CMAKE_SOURCE_DIR}/dataset")
    add_test(NAME strategies_directional_real COMMAND altair_directional_real_test)
    # Reads dataset/ (not in git): CI runs `ctest -LE dataset`.
    set_tests_properties(strategies_directional_real PROPERTIES LABELS dataset)

    # P22-01/02. The overnight gap. Two binaries for the same reason as
    # P21: the synthetic one proves the arithmetic on data whose answer is
    # known by construction, the real one asks whether the gap is tradeable.
    add_executable(altair_overnight_test tests/test_overnight.cpp)
    target_link_libraries(altair_overnight_test PRIVATE altair_strategies altair_flags)
    add_test(NAME strategies_overnight COMMAND altair_overnight_test)

    add_executable(altair_overnight_real_test tests/test_overnight_real.cpp)
    target_link_libraries(altair_overnight_real_test PRIVATE altair_strategies altair_flags)
    target_compile_definitions(altair_overnight_real_test PRIVATE
        ALTAIR_DATASET_DIR="${CMAKE_SOURCE_DIR}/dataset")
    add_test(NAME strategies_overnight_real COMMAND altair_overnight_real_test)
    # Reads dataset/ (not in git): CI runs `ctest -LE dataset`.
    set_tests_properties(strategies_overnight_real PROPERTIES LABELS dataset)

    # P6-03b. The regime detector on 36 years of REAL daily NIFTY rather than
    # on a synthetic series with a regime built into it. The Models page has
    # said "daily NIFTY is sufficient; not yet fitted" since P6-03 shipped.
    add_executable(altair_regime_real_test tests/test_regime_real.cpp)
    target_link_libraries(altair_regime_real_test PRIVATE altair_strategies altair_flags)
    target_compile_definitions(altair_regime_real_test PRIVATE
        ALTAIR_DATASET_DIR="${CMAKE_SOURCE_DIR}/dataset")
    add_test(NAME strategies_regime_real COMMAND altair_regime_real_test)

    # P7-02. The event-driven cross-venue scan scheduler.
    add_executable(altair_scan_scheduler_test tests/test_scan_scheduler.cpp)
    target_link_libraries(altair_scan_scheduler_test PRIVATE altair_strategies altair_flags)
    add_test(NAME strategies_scan_scheduler COMMAND altair_scan_scheduler_test)
endif()
