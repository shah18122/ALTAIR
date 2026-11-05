# broker/ -- broker session and authentication.
#
# The ONLY directory that touches a credential. feed/ needs an access token to
# open a socket and oms/ needs one to place an order, so the session cannot
# live in either without the other including it -- and a client that can
# include an OMS header is a client that can be made to trade.
#
# Header-only, except that the HTTP half (P2-10c) needs OpenSSL and Beast.
# Those are behind the vcpkg `net` feature, so altair_broker_net is a SEPARATE
# target: the rest of the tree must keep building with no vcpkg at all, which
# the `default` preset does and which every card before this one relied on.

add_library(altair_broker INTERFACE)
add_library(altair::broker ALIAS altair_broker)

add_library(altair_credential_store STATIC credential_store.cpp)
add_library(altair::credential_store ALIAS altair_credential_store)
target_include_directories(altair_credential_store PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(altair_credential_store PUBLIC altair_flags)

target_include_directories(altair_broker INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(altair_broker INTERFACE
    altair_types
    altair_time
    altair_lockfree
    altair_flags
)

# The networked half. Only defined when the net feature actually built.
#
# It needs BOTH OpenSSL and Boost (Asio/Beast). Keying it on OpenSSL alone
# broke any machine that has OpenSSL but no Boost -- GitHub's Windows image
# is one -- because every helper under app/ then failed to find Beast.
find_package(OpenSSL QUIET)
find_package(Boost CONFIG QUIET COMPONENTS headers)
if(OpenSSL_FOUND AND Boost_FOUND)
    add_library(altair_broker_net INTERFACE)
    add_library(altair::broker_net ALIAS altair_broker_net)
    target_include_directories(altair_broker_net
        INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_link_libraries(altair_broker_net INTERFACE
        altair_broker
        OpenSSL::SSL
        OpenSSL::Crypto
        Boost::headers)
    set(ALTAIR_HAVE_NET ON CACHE INTERNAL "broker net half is available")
    message(STATUS "altair: broker net half ENABLED (OpenSSL + Boost ${Boost_VERSION})")
else()
    message(STATUS "altair: broker net half disabled (needs OpenSSL and "
                   "Boost) -- configure with --preset net for P2-10c")
endif()

if(ALTAIR_BUILD_TESTS)
    # The FYERS TOTP auto-login (broker/tools/fyers_auto_login.py) against a
    # simulated FYERS: no network, no credential.
    find_package(Python3 3.9 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME fyers_auto_login_py
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_fyers_auto_login.py)
        add_test(NAME fyers_callback_server_py
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_fyers_callback_server.py)
    endif()

    add_executable(altair_fyers_webhook_test tests/test_fyers_webhook.cpp)
    target_link_libraries(altair_fyers_webhook_test PRIVATE altair_broker altair_flags)
    add_test(NAME fyers_webhook COMMAND altair_fyers_webhook_test)
    # The ticker's read loop, against a local WebSocket server over plain TCP:
    # idle, heartbeat-only, stop and close all end it promptly. Header-only
    # Beast; no TLS, no credential, no network beyond loopback.
    find_package(Boost CONFIG QUIET COMPONENTS headers)
    if(Boost_FOUND)
        add_executable(altair_ws_pump_test tests/test_ws_pump.cpp)
        target_link_libraries(altair_ws_pump_test PRIVATE altair_flags Boost::headers Threads::Threads)
        target_include_directories(altair_ws_pump_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
        add_test(NAME ws_pump COMMAND altair_ws_pump_test)
        set_tests_properties(ws_pump PROPERTIES LABELS concurrency)
    endif()

    # broker/candles.hpp: IST dates and the coverage check every history fetch runs.
    add_executable(altair_candles_test tests/test_candles.cpp)
    target_link_libraries(altair_candles_test PRIVATE altair_broker altair_flags)
    add_test(NAME candles COMMAND altair_candles_test)

    add_executable(altair_fyers_historical_test tests/test_fyers_historical.cpp)
    target_link_libraries(altair_fyers_historical_test PRIVATE
        altair_broker altair_flags)
    add_test(NAME fyers_historical COMMAND altair_fyers_historical_test)

    add_executable(altair_fyers_auth_test tests/test_fyers_auth.cpp)
    target_link_libraries(altair_fyers_auth_test PRIVATE altair_broker altair_flags)
    add_test(NAME fyers_auth COMMAND altair_fyers_auth_test)

    add_executable(altair_oauth_attempt_test tests/test_oauth_attempt.cpp)
    target_link_libraries(altair_oauth_attempt_test PRIVATE
        altair_broker altair_flags)
    add_test(NAME oauth_attempt COMMAND altair_oauth_attempt_test)

    add_executable(altair_broker_state_store_test tests/test_state_store.cpp)
    target_link_libraries(altair_broker_state_store_test
        PRIVATE altair_broker altair_flags)
    add_test(NAME broker_state_store COMMAND altair_broker_state_store_test)

    add_executable(altair_account_snapshot_test tests/test_account_snapshot.cpp)
    target_link_libraries(altair_account_snapshot_test PRIVATE
        altair_broker altair_flags)
    add_test(NAME account_snapshot COMMAND altair_account_snapshot_test)

    add_executable(altair_account_snapshot_parser_test
        tests/test_account_snapshot_parser.cpp)
    target_link_libraries(altair_account_snapshot_parser_test PRIVATE
        altair_broker altair_flags)
    add_test(NAME account_snapshot_parser COMMAND altair_account_snapshot_parser_test)

    add_executable(altair_snapshot_lifecycle_test tests/test_snapshot_lifecycle.cpp)
    target_link_libraries(altair_snapshot_lifecycle_test PRIVATE
        altair_broker altair_flags)
    add_test(NAME snapshot_lifecycle COMMAND altair_snapshot_lifecycle_test)

    add_executable(altair_snapshot_publisher_test tests/test_snapshot_publisher.cpp)
    target_link_libraries(altair_snapshot_publisher_test PRIVATE
        altair_broker altair_flags)
    add_test(NAME snapshot_publisher COMMAND altair_snapshot_publisher_test)

    add_executable(altair_broker_service_runtime_test tests/test_service_runtime.cpp)
    target_link_libraries(altair_broker_service_runtime_test PRIVATE
        altair_broker altair_flags)
    add_test(NAME broker_service_runtime COMMAND altair_broker_service_runtime_test)

    add_executable(altair_credential_store_test tests/test_credential_store.cpp)
    target_link_libraries(altair_credential_store_test
        PRIVATE altair_credential_store altair_flags)
    add_test(NAME credential_store COMMAND altair_credential_store_test)
endif()
