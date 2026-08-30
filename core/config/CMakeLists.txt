# core/config — versioned, hashed, hot-swappable config store (P0-08a).
# The TOML source (P0-08b) is BLOCKED on vcpkg and is not part of this target.
# Header-only: the INTERFACE library carries the include root so consumers
# write `#include <config/config.hpp>`.

add_library(altair_config INTERFACE)
add_library(altair::config ALIAS altair_config)

target_include_directories(altair_config INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(altair_config INTERFACE
    altair_types
    altair_lockfree
    altair_flags
)

if(ALTAIR_BUILD_TESTS)
    add_executable(altair_config_test tests/test_config.cpp)
    target_link_libraries(altair_config_test
        PRIVATE altair_config altair_flags Threads::Threads)
    add_test(NAME config COMMAND altair_config_test)
endif()
