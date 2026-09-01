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
endif()
