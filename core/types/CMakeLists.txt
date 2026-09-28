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

    add_executable(altair_broker_state_test tests/test_broker_state.cpp)
    target_link_libraries(altair_broker_state_test PRIVATE altair_types altair_flags)
    add_test(NAME broker_state COMMAND altair_broker_state_test)

    add_executable(altair_broker_positions_test tests/test_broker_positions.cpp)
    target_link_libraries(altair_broker_positions_test PRIVATE altair_types altair_flags)
    add_test(NAME broker_positions COMMAND altair_broker_positions_test)

    add_executable(altair_broker_log_test tests/test_broker_log.cpp)
    target_link_libraries(altair_broker_log_test PRIVATE altair_types altair_flags)
    add_test(NAME broker_log COMMAND altair_broker_log_test)
endif()
