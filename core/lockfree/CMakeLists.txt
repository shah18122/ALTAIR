# core/lockfree — SPSC ring (P0-06a), MPSC ring + seqlock (P0-06b).
# Header-only: the INTERFACE library carries the include root so consumers
# write `#include <lockfree/spsc_ring.hpp>`.

add_library(altair_lockfree INTERFACE)
add_library(altair::lockfree ALIAS altair_lockfree)

target_include_directories(altair_lockfree INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(altair_lockfree INTERFACE altair_types altair_flags)

if(ALTAIR_BUILD_TESTS)
    add_executable(altair_spsc_ring_test tests/test_spsc_ring.cpp)
    target_link_libraries(altair_spsc_ring_test
        PRIVATE altair_lockfree altair_flags Threads::Threads)
    add_test(NAME spsc_ring COMMAND altair_spsc_ring_test)

    add_executable(altair_mpsc_seqlock_test tests/test_mpsc_seqlock.cpp)
    target_link_libraries(altair_mpsc_seqlock_test
        PRIVATE altair_lockfree altair_flags Threads::Threads)
    add_test(NAME mpsc_seqlock COMMAND altair_mpsc_seqlock_test)
endif()
