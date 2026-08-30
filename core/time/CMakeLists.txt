# core/time — affine time algebra and IST (P0-02), TSC clock (P0-03).
#
# Was an INTERFACE library while core/time was header-only. P0-03 adds
# tsc_clock.cpp (platform intrinsics + calibration), so it is now STATIC.
# The include root is exported so consumers write `#include <time/...>`.

add_library(altair_time STATIC
    tsc_clock.cpp
)
add_library(altair::time ALIAS altair_time)

target_include_directories(altair_time PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(altair_time PUBLIC altair_types altair_flags)

if(ALTAIR_BUILD_TESTS)
    add_executable(altair_time_test tests/test_timestamp.cpp)
    target_link_libraries(altair_time_test PRIVATE altair_time altair_flags)
    add_test(NAME time COMMAND altair_time_test)

    add_executable(altair_tsc_clock_test tests/test_tsc_clock.cpp)
    target_link_libraries(altair_tsc_clock_test PRIVATE altair_time altair_flags Threads::Threads)
    add_test(NAME tsc_clock COMMAND altair_tsc_clock_test)

    add_executable(altair_exchange_ts_test tests/test_exchange_ts.cpp)
    target_link_libraries(altair_exchange_ts_test PRIVATE altair_time altair_flags)
    add_test(NAME exchange_ts COMMAND altair_exchange_ts_test)
endif()
