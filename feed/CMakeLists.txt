# feed/ — replay skeleton (P0-09b). Decoders, normaliser and failover land in
# Phase 2; this is the cursor that lets the Phase 0 exit criterion run.
# Header-only: the INTERFACE library carries the include root so consumers
# write `#include <feed/replay.hpp>`.

add_library(altair_feed INTERFACE)
add_library(altair::feed ALIAS altair_feed)

target_include_directories(altair_feed INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(altair_feed INTERFACE
    altair_types
    altair_time
    altair_invariant
    altair_instruments   # P2-01: a normalised Tick carries an InstrumentId
    altair_flags
)

if(ALTAIR_BUILD_TESTS)
    add_executable(altair_replay_test tests/test_replay.cpp)
    target_link_libraries(altair_replay_test PRIVATE altair_feed altair_flags)
    add_test(NAME replay COMMAND altair_replay_test)

    add_executable(altair_tick_test tests/test_tick.cpp)
    target_link_libraries(altair_tick_test PRIVATE altair_feed altair_flags)
    add_test(NAME tick COMMAND altair_tick_test)

    # The FYERS 50-level book: vectors encoded by the official SDK.
    add_executable(altair_fyers_tbt_test tests/test_fyers_tbt.cpp)
    target_link_libraries(altair_fyers_tbt_test PRIVATE altair_feed altair_flags)
    target_compile_definitions(altair_fyers_tbt_test PRIVATE
        ALTAIR_TBT_VECTORS="${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors/fyers_tbt.txt")
    add_test(NAME fyers_tbt COMMAND altair_fyers_tbt_test)

    add_executable(altair_kite_decoder_test tests/test_kite_decoder.cpp)
    target_link_libraries(altair_kite_decoder_test PRIVATE altair_feed altair_flags)
    add_test(NAME kite_decoder COMMAND altair_kite_decoder_test)

    add_executable(altair_failover_test tests/test_failover.cpp)
    target_link_libraries(altair_failover_test PRIVATE altair_feed altair_flags)
    add_test(NAME failover COMMAND altair_failover_test)

    add_executable(altair_tick_store_test tests/test_tick_store.cpp)
    target_link_libraries(altair_tick_store_test PRIVATE altair_feed altair_flags)
    add_test(NAME tick_store COMMAND altair_tick_store_test)

    add_executable(altair_normaliser_test tests/test_normaliser.cpp)
    target_link_libraries(altair_normaliser_test PRIVATE altair_feed altair_flags)
    add_test(NAME normaliser COMMAND altair_normaliser_test)

    add_executable(altair_fyers_decoder_test tests/test_fyers_decoder.cpp)
    target_link_libraries(altair_fyers_decoder_test PRIVATE altair_feed altair_flags)
    add_test(NAME fyers_decoder COMMAND altair_fyers_decoder_test)

    add_executable(altair_fyers_adapter_test tests/test_fyers_adapter.cpp)
    target_link_libraries(altair_fyers_adapter_test PRIVATE altair_feed altair_flags)
    add_test(NAME fyers_adapter COMMAND altair_fyers_adapter_test)

    # FYERS data-socket (HSM) protocol, checked against the official SDK's
    # own frames and on_message output (tests/fyers_hsm_golden.hpp).
    add_executable(altair_fyers_hsm_test tests/test_fyers_hsm.cpp)
    target_link_libraries(altair_fyers_hsm_test PRIVATE altair_feed altair_flags)
    add_test(NAME fyers_hsm COMMAND altair_fyers_hsm_test)
endif()
