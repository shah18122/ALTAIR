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
    altair_invariant
    altair_types
    altair_time
    altair_flags
)

if(ALTAIR_BUILD_TESTS)
    # P25-03. The intent queue the UI writes and oms/ drains. The vectors
    # path is compiled in so the test runs from any working directory, and
    # BOTH sides point at the same file -- desktop/ has its own emitter and
    # never includes oms/, so the vectors are the only thing keeping the two
    # ends of the queue agreeing.
    add_executable(altair_order_intent_test tests/test_order_intent.cpp)
    target_link_libraries(altair_order_intent_test PRIVATE altair_oms altair_flags)
    target_compile_definitions(altair_order_intent_test PRIVATE
        ALTAIR_INTENT_VECTORS="${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors/intents.jsonl")
    add_test(NAME order_intent COMMAND altair_order_intent_test)

    # CX02-B2. The drain, rebuilt: a missing, truncated or replaced file is an
    # error; intents expire; ids are deduplicated across restarts; every bound
    # refuses or reports. Writes its scratch files in the working directory.
    add_executable(altair_intent_queue_test tests/test_intent_queue.cpp)
    target_link_libraries(altair_intent_queue_test PRIVATE altair_oms altair_flags)
    add_test(NAME intent_queue COMMAND altair_intent_queue_test)

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

    add_executable(altair_throttle_test tests/test_throttle.cpp)
    target_link_libraries(altair_throttle_test PRIVATE altair_oms altair_flags)
    add_test(NAME throttle COMMAND altair_throttle_test)

    add_executable(altair_broker_dispatch_gate_test tests/test_broker_dispatch_gate.cpp)
    target_link_libraries(altair_broker_dispatch_gate_test PRIVATE altair_oms altair_flags)
    add_test(NAME broker_dispatch_gate COMMAND altair_broker_dispatch_gate_test)

    add_executable(altair_fyers_order_adapter_test tests/test_fyers_adapter.cpp)
    target_link_libraries(altair_fyers_order_adapter_test PRIVATE altair_oms altair_flags)
    add_test(NAME fyers_order_adapter COMMAND altair_fyers_order_adapter_test)

    # P13-01..06. Execution algorithms and implementation shortfall. They live
    # in oms/ because they decide the quantity and timing of orders, which is
    # placing an order in every sense that matters -- a scheduler in
    # strategies/ would be exactly the violation CLAUDE.md calls unconditional.
    add_executable(altair_execution_test tests/test_execution.cpp)
    target_link_libraries(altair_execution_test PRIVATE altair_oms altair_flags)
    add_test(NAME oms_execution COMMAND altair_execution_test)

    # P13-07. Market impact and the capacity ceiling. QUANTLAB Q7: a linear,
    # size-blind cost model cannot express a bound at all.
    add_executable(altair_capacity_test tests/test_capacity.cpp)
    target_link_libraries(altair_capacity_test PRIVATE altair_oms altair_flags)
    add_test(NAME oms_capacity COMMAND altair_capacity_test)

    # P7-07. The paper execution venue: a deterministic fill simulator that
    # reuses the state machine and the conservation ledger; no live route.
    add_executable(altair_paper_venue_test tests/test_paper_venue.cpp)
    target_link_libraries(altair_paper_venue_test PRIVATE altair_oms altair_flags)
    add_test(NAME paper_venue COMMAND altair_paper_venue_test)
endif()
