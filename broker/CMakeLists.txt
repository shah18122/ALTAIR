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
if(WIN32)
    target_link_libraries(altair_credential_store PRIVATE Advapi32)
elseif(APPLE)
    find_library(ALTAIR_SECURITY_FRAMEWORK Security REQUIRED)
    target_link_libraries(altair_credential_store PRIVATE
        "${ALTAIR_SECURITY_FRAMEWORK}")
endif()

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
    if(WIN32)
        # CertOpenSystemStore / CertEnumCertificatesInStore. Needed because
        # OpenSSL's default verify paths are empty on Windows, so the platform
        # trust store has to be loaded by hand -- see broker/https_client.hpp.
        target_link_libraries(altair_broker_net INTERFACE crypt32)
    endif()
    set(ALTAIR_HAVE_NET ON CACHE INTERNAL "broker net half is available")
    message(STATUS "altair: broker net half ENABLED (OpenSSL + Boost ${Boost_VERSION})")
else()
    message(STATUS "altair: broker net half disabled (needs OpenSSL and "
                   "Boost) -- configure with --preset net for P2-10c")
endif()

if(ALTAIR_BUILD_TESTS)
    add_executable(altair_fyers_webhook_test tests/test_fyers_webhook.cpp)
    target_link_libraries(altair_fyers_webhook_test PRIVATE altair_broker altair_flags)
    add_test(NAME fyers_webhook COMMAND altair_fyers_webhook_test)
    add_executable(altair_kite_auth_test tests/test_kite_auth.cpp)
    target_link_libraries(altair_kite_auth_test PRIVATE altair_broker altair_flags)
    add_test(NAME kite_auth COMMAND altair_kite_auth_test)

    # Links altair_instruments as well: the round-trip test proves this
    # header's paise->rupees formatter is the exact inverse of P1-04's parser.
    add_executable(altair_kite_api_test tests/test_kite_api.cpp)
    target_link_libraries(altair_kite_api_test
        PRIVATE altair_broker altair_instruments altair_flags)
    add_test(NAME kite_api COMMAND altair_kite_api_test)

    # P2-12. Historical candles: URI, chunking and the response parser. Links
    # altair_broker (the non-net half) only -- there is no transport and no
    # credential in it, so it builds and runs in EVERY preset rather than only
    # where OpenSSL is present.
    # P32-07. The ticker's WIRE: the URL, the two control messages and the
    # redaction. Needs altair_broker_net because kite_ticker.hpp includes
    # https_client.hpp for the platform-root loader, so it exists only where
    # OpenSSL does -- guarded below rather than here.
    if(ALTAIR_HAVE_NET)
        add_executable(altair_kite_ticker_test tests/test_kite_ticker.cpp)
        target_link_libraries(altair_kite_ticker_test
            PRIVATE altair_broker_net altair_flags)
        add_test(NAME kite_ticker COMMAND altair_kite_ticker_test)
    endif()

    # The ticker's read loop, against a local WebSocket server over plain TCP:
    # idle, heartbeat-only, stop and close all end it promptly. Header-only
    # Beast; no TLS, no credential, no network beyond loopback.
    find_package(Boost CONFIG QUIET COMPONENTS headers)
    if(Boost_FOUND)
        add_executable(altair_ws_pump_test tests/test_ws_pump.cpp)
        target_link_libraries(altair_ws_pump_test PRIVATE altair_flags Boost::headers Threads::Threads)
        target_include_directories(altair_ws_pump_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
        if(WIN32)
            target_link_libraries(altair_ws_pump_test PRIVATE ws2_32 mswsock)
        endif()
        add_test(NAME ws_pump COMMAND altair_ws_pump_test)
        set_tests_properties(ws_pump PROPERTIES LABELS concurrency)
    endif()

    add_executable(altair_kite_historical_test tests/test_kite_historical.cpp)
    target_link_libraries(altair_kite_historical_test
        PRIVATE altair_broker altair_flags)
    add_test(NAME kite_historical COMMAND altair_kite_historical_test)

    add_executable(altair_fyers_historical_test tests/test_fyers_historical.cpp)
    target_link_libraries(altair_fyers_historical_test PRIVATE
        altair_broker altair_flags)
    add_test(NAME fyers_historical COMMAND altair_fyers_historical_test)

    # P20-03. The /quote parser. Links altair_broker (the non-net half) only,
    # for the same reason kite_historical does: it is a PARSER, with no
    # transport and no credential in it, so it builds and runs in every preset
    # rather than only where OpenSSL is present. Fetching a quote needs a
    # session; parsing one does not, and the parser is where the bugs are.
    add_executable(altair_kite_quote_test tests/test_kite_quote.cpp)
    target_link_libraries(altair_kite_quote_test
        PRIVATE altair_broker altair_flags)
    add_test(NAME kite_quote COMMAND altair_kite_quote_test)

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
