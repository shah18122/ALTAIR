# live/ -- the live terminal's engine side: which contracts stream, ticks into
# bars, the models that run on them, and the paper book that trades their
# signals. Header-only. Reads prices from altair_price_service's loopback bus;
# links no broker and no OMS, so nothing here can place an order.
add_library(altair_live INTERFACE)
add_library(altair::live ALIAS altair_live)
target_include_directories(altair_live INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(altair_live INTERFACE altair_server altair_analytics altair_strategies)

if(ALTAIR_BUILD_TESTS)
    add_executable(altair_live_universe_test tests/test_universe.cpp)
    target_link_libraries(altair_live_universe_test PRIVATE altair_live)
    add_test(NAME live_universe COMMAND altair_live_universe_test)

    add_executable(altair_live_sim_test tests/test_sim.cpp)
    target_link_libraries(altair_live_sim_test PRIVATE altair_live)
    add_test(NAME live_sim COMMAND altair_live_sim_test)

    add_executable(altair_live_engine_test tests/test_engine.cpp)
    target_link_libraries(altair_live_engine_test PRIVATE altair_live)
    add_test(NAME live_engine COMMAND altair_live_engine_test)

    add_executable(altair_live_execution_test tests/test_execution.cpp)
    target_link_libraries(altair_live_execution_test PRIVATE altair_live)
    add_test(NAME live_execution COMMAND altair_live_execution_test)

    add_executable(altair_live_tape_test tests/test_tape.cpp)
    target_link_libraries(altair_live_tape_test PRIVATE altair_live)
    add_test(NAME live_tape COMMAND altair_live_tape_test)
endif()
