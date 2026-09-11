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

    # The option chain as structure-of-arrays, cache-line aligned, one expiry
    # in L1D. Held to black76 on every strike and to put-call parity on every
    # row, because it is a second implementation of the same model and second
    # implementations drift.
    add_executable(altair_chain_soa_test tests/test_chain_soa.cpp)
    target_link_libraries(altair_chain_soa_test PRIVATE altair_analytics altair_flags)
    add_test(NAME chain_soa COMMAND altair_chain_soa_test)

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



    # P14-01/02/03. GARCH, GJR, and the volatility horse race against the EWMA
    # already in this directory. QUANTLAB found EWMA wins on QLIKE; whether
    # that holds on 36 years of NIFTY is the experiment.
    add_executable(altair_garch_test tests/test_garch.cpp)
    target_link_libraries(altair_garch_test PRIVATE altair_analytics altair_flags)
    target_compile_definitions(altair_garch_test PRIVATE
        ALTAIR_DATASET_DIR="${CMAKE_SOURCE_DIR}/dataset")
    add_test(NAME analytics_garch COMMAND altair_garch_test)

    # P14-04/05. Kalman filter, Joseph form, validated on the NIFTY
    # spot-versus-futures pair where the hedge ratio is known to be 1.
    add_executable(altair_kalman_test tests/test_kalman.cpp)
    target_link_libraries(altair_kalman_test PRIVATE altair_analytics altair_flags)
    target_compile_definitions(altair_kalman_test PRIVATE
        ALTAIR_DATASET_DIR="${CMAKE_SOURCE_DIR}/dataset")
    add_test(NAME analytics_kalman COMMAND altair_kalman_test)

    # P17-02/03 + P14-06. SABR with its arbitrage check, Dupire local vol, and
    # a hidden Markov model read against the separation the same fitter finds
    # in pure noise.
    add_executable(altair_surface_hmm_test tests/test_surface_hmm.cpp)
    target_link_libraries(altair_surface_hmm_test PRIVATE altair_analytics altair_flags)
    target_compile_definitions(altair_surface_hmm_test PRIVATE
        ALTAIR_DATASET_DIR="${CMAKE_SOURCE_DIR}/dataset")
    add_test(NAME analytics_surface_hmm COMMAND altair_surface_hmm_test)
endif()

# SoA versus AoS at L1, L2, L3 and RAM working sets. An EXECUTABLE, not a test:
# timing under a parallel ctest run is noise, and a latency number that fails
# the build on a busy afternoon teaches everybody to ignore latency numbers.
if(ALTAIR_BUILD_BENCH)
    add_executable(altair_bench_chain bench/bench_chain.cpp)
    target_link_libraries(altair_bench_chain PRIVATE altair_analytics altair_flags)
endif()
