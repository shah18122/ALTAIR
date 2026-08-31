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
endif()
