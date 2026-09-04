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
