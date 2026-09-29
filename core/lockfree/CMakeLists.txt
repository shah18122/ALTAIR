# core/lockfree — SPSC ring (P0-06a), MPSC ring (P0-06b).
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
    # Runs threads: the TSan CI job runs exactly the `concurrency` label.
    set_tests_properties(spsc_ring PROPERTIES LABELS concurrency)

    add_executable(altair_mpsc_ring_test tests/test_mpsc_seqlock.cpp)
    target_link_libraries(altair_mpsc_ring_test
        PRIVATE altair_lockfree altair_flags Threads::Threads)
    add_test(NAME mpsc_ring COMMAND altair_mpsc_ring_test)
    # Runs threads: the TSan CI job runs exactly the `concurrency` label.
    set_tests_properties(mpsc_ring PROPERTIES LABELS concurrency)

    # CX02-C1. Slot publication: the writer never writes a slot a reader can
    # be copying (C02-002 / C21-003), and a contended read is refused rather
    # than spun (C02-003). Interleavings are forced through a test hook.
    add_executable(altair_snapshot_slots_test tests/test_snapshot_slots.cpp)
    target_link_libraries(altair_snapshot_slots_test
        PRIVATE altair_lockfree altair_flags Threads::Threads)
    add_test(NAME snapshot_slots COMMAND altair_snapshot_slots_test)
    # Runs threads: the TSan CI job runs exactly the `concurrency` label.
    set_tests_properties(snapshot_slots PROPERTIES LABELS concurrency)
endif()
