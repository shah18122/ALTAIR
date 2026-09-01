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
    altair_risk
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

    # Links altair_risk: reconciliation trips the P4-02 kill switch, which is
    # the whole point of running it -- an orphan must stop trading.
    add_executable(altair_oms_reconcile_test tests/test_reconcile.cpp)
    target_link_libraries(altair_oms_reconcile_test
        PRIVATE altair_oms altair_risk altair_flags)
    add_test(NAME oms_reconcile COMMAND altair_oms_reconcile_test)

    # Links altair_broker for the P2-11 endpoint and parameter constants, and
    # altair_risk for Side. The adapter TRANSLATES only -- it never sends, so
    # this test needs no network and no credential.
    add_executable(altair_kite_adapter_test tests/test_kite_adapter.cpp)
    target_link_libraries(altair_kite_adapter_test
        PRIVATE altair_oms altair_broker altair_risk altair_flags)
    add_test(NAME kite_adapter COMMAND altair_kite_adapter_test)
endif()
