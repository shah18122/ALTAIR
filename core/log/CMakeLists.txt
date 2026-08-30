# core/log — binary async logger + off-thread decoder (P0-07).
# Header-only: the INTERFACE library carries the include root so consumers
# write `#include <log/binlog.hpp>`.

add_library(altair_log INTERFACE)
add_library(altair::log ALIAS altair_log)

target_include_directories(altair_log INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(altair_log INTERFACE
    altair_types
    altair_time
    altair_lockfree
    altair_flags
)

if(ALTAIR_BUILD_TESTS)
    add_executable(altair_binlog_test tests/test_binlog.cpp)
    target_link_libraries(altair_binlog_test
        PRIVATE altair_log altair_flags Threads::Threads)
    add_test(NAME binlog COMMAND altair_binlog_test)
endif()
