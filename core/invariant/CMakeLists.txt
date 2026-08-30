# core/invariant — conservation checks (P0-09a).
# Header-only: the INTERFACE library carries the include root so consumers
# write `#include <invariant/conservation.hpp>`.

add_library(altair_invariant INTERFACE)
add_library(altair::invariant ALIAS altair_invariant)

target_include_directories(altair_invariant INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(altair_invariant INTERFACE altair_types altair_flags)

if(ALTAIR_BUILD_TESTS)
    add_executable(altair_conservation_test tests/test_conservation.cpp)
    target_link_libraries(altair_conservation_test
        PRIVATE altair_invariant altair_flags)
    add_test(NAME conservation COMMAND altair_conservation_test)
endif()
