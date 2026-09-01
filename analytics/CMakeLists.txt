# analytics/ - greeks, IV, rolling stats, India VIX (Phase 3).
#
# Header-only. Doubles are permitted here and nowhere near the ledger:
# CLAUDE.md rule 3 allows them in analytics and bans them in money.

add_library(altair_analytics INTERFACE)
add_library(altair::analytics ALIAS altair_analytics)

target_include_directories(altair_analytics INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(altair_analytics INTERFACE
    altair_types
    altair_time
    altair_flags
)

if(ALTAIR_BUILD_TESTS)
    add_executable(altair_greeks_test tests/test_greeks.cpp)
    target_link_libraries(altair_greeks_test PRIVATE altair_analytics altair_flags)
    add_test(NAME greeks COMMAND altair_greeks_test)
endif()
