# app/ — the `altair` binary (P0-10).
# This is the Phase 0 exit criterion: it wires nine core modules together and
# proves they compose, which no unit test can.

add_executable(altair main.cpp instruments_demo.cpp)

target_include_directories(altair PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(altair PRIVATE
    altair_core
    altair_feed
    altair_instruments
    altair_flags
)

if(ALTAIR_BUILD_TESTS)
    add_test(NAME app_selftest COMMAND altair --selftest)
    add_test(NAME app_instruments COMMAND altair --instruments)
endif()

# ── the Kite login, P2-10c ────────────────────────────────────────────────
#
# A SEPARATE BINARY, not a mode of `altair`. It is run by hand a few times a
# day, it is the only thing in the tree that reaches the public internet with
# a credential, and it links OpenSSL. Folding it into the engine binary would
# put a TLS stack and a secret-reading code path inside the process that
# places orders, for no benefit.
#
# Built only when the vcpkg `net` feature is present, so `--preset default`
# keeps working with no vcpkg at all.
if(ALTAIR_HAVE_NET)
    add_executable(altair_kite_login kite_login_main.cpp)
    target_link_libraries(altair_kite_login PRIVATE
        altair_broker_net altair_flags)

    # P2-12c. Fills dataset/ from the historical candle API. Separate binary
    # from `altair` for the same reason altair_kite_login is: a tool that reads
    # a live trading credential should not be inside the process that places
    # orders, and it runs once by hand rather than in the engine's lifetime.
    add_executable(altair_kite_fetch kite_fetch_main.cpp)
    target_link_libraries(altair_kite_fetch PRIVATE
        altair_broker_net altair_flags)

    # P20-01. Snapshots the READ-ONLY account state for the UI to render.
    # Separate binary for the same reason as the other two: desktop/ may not
    # link broker/, so the network half lives out here and the window reads a
    # file. Every endpoint it calls is read-only; place/modify/cancel are
    # deliberately absent and belong to oms/.
    add_executable(altair_kite_account kite_account_main.cpp)
    target_link_libraries(altair_kite_account PRIVATE
        altair_broker_net altair_flags)

    # P34-01. Brings dataset/ up to date in one call, MERGING into the month
    # files rather than replacing them -- altair_kite_fetch refuses to
    # overwrite an existing month, which is right for a bulk backfill and
    # exactly wrong for a daily top-up, where the month being appended to is
    # always the one already on disk.
    add_executable(altair_kite_update kite_update_main.cpp)
    target_link_libraries(altair_kite_update PRIVATE
        altair_broker_net altair_flags)

    # P32-07. THE LIVE TICK FEED. feed/kite_decoder.hpp has turned Kite's
    # binary frames into Ticks since P2-02 and nothing had ever handed it a
    # frame off a socket -- every "live" number in this tree has come from a
    # replay file. Separate binary for the same reason as the other four: a
    # program that reads a live trading credential does not belong inside the
    # process holding positions, and desktop/ may not link broker/ at all.
    add_executable(altair_kite_ticker kite_ticker_main.cpp)
    target_link_libraries(altair_kite_ticker PRIVATE
        altair_broker_net altair_feed altair_instruments altair_flags)

    # P29-01. Snapshots /quote for the watchlist. broker/kite_quote.hpp has
    # PARSED a quote since P20-03 with twenty-two green checks and nothing had
    # ever FETCHED one, so the watchlist showed a replay price and no bid, no
    # ask, no depth and no spread. The parser was written first deliberately --
    # fetching is a socket and a header, parsing is where the bugs are -- and
    # this is the other half. /quote is a GET; it has no order vocabulary.
    add_executable(altair_kite_quote kite_quote_main.cpp)
    target_link_libraries(altair_kite_quote PRIVATE
        altair_broker_net altair_flags)
endif()

# P12-02. Warm restart from state. Header-only and testable without a broker:
# the four ways a restart resumes into a wrong state all produce a file that
# PARSES, which is why the test exists.
if(ALTAIR_BUILD_TESTS)
    add_executable(altair_warm_restart_test tests/test_warm_restart.cpp)
    target_link_libraries(altair_warm_restart_test PRIVATE altair_types altair_flags)
    target_include_directories(altair_warm_restart_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    add_test(NAME warm_restart COMMAND altair_warm_restart_test)
endif()

# P12-03. Monitoring and alerting. Header-only; the test is what carries the
# card, because the failure it prevents (a dead feed passing every threshold)
# is invisible in the code and obvious in the assertions.
if(ALTAIR_BUILD_TESTS)
    add_executable(altair_monitor_test tests/test_monitor.cpp)
    target_link_libraries(altair_monitor_test PRIVATE altair_types altair_flags)
    target_include_directories(altair_monitor_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    add_test(NAME monitor COMMAND altair_monitor_test)
endif()

# P12-04. Daily reconciliation against the contract note. Links the REAL cost
# model: a reconciler tested only against hand-typed charge figures proves that
# subtraction works.
if(ALTAIR_BUILD_TESTS)
    add_executable(altair_note_reconcile_test tests/test_note_reconcile.cpp)
    target_link_libraries(altair_note_reconcile_test PRIVATE
        altair_types altair_risk altair_flags)
    target_include_directories(altair_note_reconcile_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    # Named `note_reconcile`: instruments/ already owns `reconcile`, which is the
    # three-way instrument-master check. Different question, different data.
    add_test(NAME note_reconcile COMMAND altair_note_reconcile_test)
endif()
