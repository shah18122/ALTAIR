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

    add_executable(altair_nse_mktlots_test tests/test_nse_mktlots.cpp)
    target_link_libraries(altair_nse_mktlots_test
        PRIVATE altair_instruments altair_flags)
    add_test(NAME nse_mktlots COMMAND altair_nse_mktlots_test)

    add_executable(altair_nse_equity_test tests/test_nse_equity_master.cpp)
    target_link_libraries(altair_nse_equity_test
        PRIVATE altair_instruments altair_flags)
    add_test(NAME nse_equity COMMAND altair_nse_equity_test)

    add_executable(altair_udiff_master_test tests/test_udiff_master.cpp)
    target_link_libraries(altair_udiff_master_test
        PRIVATE altair_instruments altair_flags)
    add_test(NAME udiff_master COMMAND altair_udiff_master_test)

    add_executable(altair_universe_test tests/test_universe.cpp)
    target_link_libraries(altair_universe_test
        PRIVATE altair_instruments altair_flags)
    add_test(NAME universe COMMAND altair_universe_test)

    add_executable(altair_fyers_identity_test tests/test_fyers_identity.cpp)
    target_link_libraries(altair_fyers_identity_test
        PRIVATE altair_instruments altair_flags)
    add_test(NAME fyers_identity COMMAND altair_fyers_identity_test)

    # P3-01 parser. Reads the FYERS public symbol master; fixtures are real
    # rows fetched from public.fyers.in, so the column map is checked against
    # live data.
    add_executable(altair_fyers_master_test tests/test_fyers_master.cpp)
    target_link_libraries(altair_fyers_master_test
        PRIVATE altair_instruments altair_flags)
    add_test(NAME fyers_master COMMAND altair_fyers_master_test)

    # GETS screens: FYERS tickers -> underlying, expiry, strike, right.
    add_executable(altair_fyers_symbol_test tests/test_fyers_symbol.cpp)
    target_link_libraries(altair_fyers_symbol_test
        PRIVATE altair_instruments altair_flags)
    add_test(NAME fyers_symbol COMMAND altair_fyers_symbol_test)

    # P7-01. The eligible cross-venue universe: NSE<->BSE by ISIN.
    add_executable(altair_cross_venue_test tests/test_cross_venue.cpp)
    target_link_libraries(altair_cross_venue_test
        PRIVATE altair_instruments altair_flags)
    add_test(NAME cross_venue COMMAND altair_cross_venue_test)
endif()
