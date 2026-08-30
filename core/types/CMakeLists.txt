# core/types — dimensional units and exact money (P0-01).
# Header-only: the INTERFACE library carries the include root so consumers
# write `#include <types/units.hpp>`.

add_library(altair_types INTERFACE)
add_library(altair::types ALIAS altair_types)

target_include_directories(altair_types INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(altair_types INTERFACE altair_flags)

if(ALTAIR_BUILD_TESTS)
    add_executable(altair_types_test tests/test_units.cpp)
    target_link_libraries(altair_types_test PRIVATE altair_types altair_flags)
    add_test(NAME types COMMAND altair_types_test)
endif()
