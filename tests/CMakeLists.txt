# tests/ — unit tests live next to the module they test, in <module>/tests/.
# This file only wires up shared test infrastructure and the smoke test.

# Shared helpers for all test targets (fixtures, golden data paths, matchers).
add_library(altair_testkit INTERFACE)
target_include_directories(altair_testkit INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/testkit)
target_compile_definitions(altair_testkit INTERFACE
    ALTAIR_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
)

# Smoke test — proves the toolchain, Catch2, and CTest wiring are alive.
# Replaced by real coverage as the Phase 0 cards land.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/smoke.cpp")
    add_executable(altair_smoke_test smoke.cpp)
    target_link_libraries(altair_smoke_test PRIVATE altair_flags altair_testkit)
    # target_link_libraries(altair_smoke_test PRIVATE Catch2::Catch2WithMain)
    add_test(NAME smoke COMMAND altair_smoke_test)
endif()
