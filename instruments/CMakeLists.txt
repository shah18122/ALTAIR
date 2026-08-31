# instruments/ — ContractSpec and the point-in-time spec store (P1-01).
# Parsers for the exchange and broker masters land in P1-02..P1-05.
# Header-only: the INTERFACE library carries the include root so consumers
# write `#include <instruments/contract_spec.hpp>`.

add_library(altair_instruments INTERFACE)
add_library(altair::instruments ALIAS altair_instruments)

target_include_directories(altair_instruments INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(altair_instruments INTERFACE
    altair_types
    altair_time
    altair_flags
)

if(ALTAIR_BUILD_TESTS)
    add_executable(altair_spec_store_test tests/test_spec_store.cpp)
    target_link_libraries(altair_spec_store_test
        PRIVATE altair_instruments altair_flags)
    add_test(NAME spec_store COMMAND altair_spec_store_test)

    add_executable(altair_kite_dump_test tests/test_kite_dump.cpp)
    target_link_libraries(altair_kite_dump_test
        PRIVATE altair_instruments altair_flags)
    add_test(NAME kite_dump COMMAND altair_kite_dump_test)

    add_executable(altair_reconcile_test tests/test_reconcile.cpp)
    target_link_libraries(altair_reconcile_test
        PRIVATE altair_instruments altair_flags)
    add_test(NAME reconcile COMMAND altair_reconcile_test)

    add_executable(altair_snapshot_test tests/test_snapshot.cpp)
    target_link_libraries(altair_snapshot_test
        PRIVATE altair_instruments altair_flags)
    add_test(NAME snapshot COMMAND altair_snapshot_test)

    add_executable(altair_nse_masters_test tests/test_nse_masters.cpp)
    target_link_libraries(altair_nse_masters_test
        PRIVATE altair_instruments altair_flags)
    add_test(NAME nse_masters COMMAND altair_nse_masters_test)

    add_executable(altair_udiff_master_test tests/test_udiff_master.cpp)
    target_link_libraries(altair_udiff_master_test
        PRIVATE altair_instruments altair_flags)
    add_test(NAME udiff_master COMMAND altair_udiff_master_test)
endif()
