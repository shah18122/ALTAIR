# oms/ - THE TRADE HANDLER.
#
# Nothing outside this directory places or amends an order. That is a
# deployment and blast-radius boundary, not a filing convention: if
# order-placing code appears anywhere else, that is a review failure rather
# than a refactor opportunity (CLAUDE.md, "One component, one directory").

add_library(altair_oms INTERFACE)
add_library(altair::oms ALIAS altair_oms)

target_include_directories(altair_oms INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(altair_oms INTERFACE
    altair_types
    altair_time
    altair_flags
)

if(ALTAIR_BUILD_TESTS)
    add_executable(altair_order_state_test tests/test_order_state.cpp)
    target_link_libraries(altair_order_state_test PRIVATE altair_oms altair_flags)
    add_test(NAME order_state COMMAND altair_order_state_test)

    add_executable(altair_exit_ladder_test tests/test_exit_ladder.cpp)
    target_link_libraries(altair_exit_ladder_test PRIVATE altair_oms altair_flags)
    add_test(NAME exit_ladder COMMAND altair_exit_ladder_test)
endif()
