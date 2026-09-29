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

# C22-003: `altair --config` reads config/altair.toml through the TOML loader
# when tomlplusplus is available, and a test keeps the shipped file in schema.
if(TARGET altair_config_toml)
    target_link_libraries(altair PRIVATE altair_config_toml)
    target_compile_definitions(altair PRIVATE ALTAIR_HAVE_TOML=1)
    if(ALTAIR_BUILD_TESTS)
        add_test(NAME engine_config_toml
                 COMMAND altair --config ${CMAKE_SOURCE_DIR}/config/altair.toml)
    endif()
endif()

install(TARGETS altair RUNTIME DESTINATION bin)

# P2-03. The desktop talks to this process over a private stdin pipe. It is the
# only setup path that writes broker app credentials to the OS vault; desktop/
# continues to link neither broker/ nor an order-capable library.
add_executable(altair_credential_helper credential_helper_main.cpp)
target_link_libraries(altair_credential_helper PRIVATE
    altair_credential_store altair_flags)
if(APPLE)
    install(TARGETS altair_credential_helper
        RUNTIME DESTINATION "Altair.app/Contents/Helpers")
else()
    install(TARGETS altair_credential_helper RUNTIME DESTINATION bin)
endif()

if(ALTAIR_BUILD_TESTS)
    add_test(NAME app_selftest COMMAND altair --selftest)
    add_test(NAME app_instruments COMMAND altair --instruments)
    add_executable(altair_paper_session_test tests/test_paper_session.cpp paper_session.hpp)
    target_link_libraries(altair_paper_session_test PRIVATE altair_oms altair_flags)
    target_include_directories(altair_paper_session_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    add_test(NAME app_paper_session COMMAND altair_paper_session_test)
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
    add_executable(altair_fyers_webhook fyers_webhook_main.cpp)
    target_link_libraries(altair_fyers_webhook PRIVATE altair_broker_net altair_flags)
    add_executable(altair_kite_login kite_login_main.cpp)
    target_link_libraries(altair_kite_login PRIVATE
        altair_broker_net altair_credential_store altair_flags)

    # FYERS v3 OAuth helper. Kept as a separate credential-boundary process:
    # desktop/ opens the browser and never loads the app secret or token.
    add_executable(altair_fyers_login fyers_login_main.cpp)
    target_link_libraries(altair_fyers_login PRIVATE
        altair_broker_net altair_credential_store altair_flags)
    target_compile_definitions(altair_fyers_login PRIVATE
        ALTAIR_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

    # P2-07. Read-only FYERS account snapshot helper. It uses only the five
    # explicit GET endpoints in broker/fyers_api.hpp and validates the shared
    # typed snapshot before replacing the UI-facing file.
    add_executable(altair_fyers_account fyers_account_main.cpp)
    target_link_libraries(altair_fyers_account PRIVATE
        altair_broker_net altair_flags)
    target_compile_definitions(altair_fyers_account PRIVATE
        ALTAIR_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

    # The live FYERS data feed: the official HSM socket protocol
    # (feed/fyers_hsm.hpp) over broker/fyers_data_socket.hpp into the existing
    # FyersDataAdapter. Read-only; a separate process for the same reason as
    # altair_kite_ticker.
    add_executable(altair_fyers_ticker fyers_ticker_main.cpp)
    target_link_libraries(altair_fyers_ticker PRIVATE
        altair_broker_net altair_feed altair_instruments altair_flags)
    target_compile_definitions(altair_fyers_ticker PRIVATE
        ALTAIR_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

    # GETS screens: READ-ONLY /data/quotes snapshot (spot, option LTPs,
    # change and OHLC) for the Terminal's Greek, movers and index tabs.
    add_executable(altair_fyers_quotes fyers_quotes_main.cpp)
    target_link_libraries(altair_fyers_quotes PRIVATE
        altair_broker_net altair_flags)
    target_compile_definitions(altair_fyers_quotes PRIVATE
        ALTAIR_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

    add_executable(altair_fyers_history fyers_history_main.cpp)
    target_link_libraries(altair_fyers_history PRIVATE
        altair_broker_net altair_flags)
    target_compile_definitions(altair_fyers_history PRIVATE
        ALTAIR_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

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

    # P37-03. THE PRICE SERVICE. One process owns the Kite connection and
    # republishes every tick on a loopback port as protocol frames, so the
    # window, the chain and the trainers stop each spawning their own fetcher.
    #
    # Needs Boost BY NAME: it uses asio for the socket and no TLS at all.
    # broker/ compiles Beast without ever naming Boost -- it links OpenSSL,
    # whose imported target adds the vcpkg include directory, and Boost's
    # headers happen to live there too. That works for a reason unrelated to
    # Boost and would break the day the two were packaged apart.
    find_package(Boost CONFIG QUIET COMPONENTS headers)
    if(Boost_FOUND)
        add_executable(altair_price_service price_service_main.cpp)
        target_link_libraries(altair_price_service PRIVATE
            altair_server altair_broker_net altair_feed altair_instruments
            altair_flags Boost::headers)
        target_compile_definitions(altair_price_service PRIVATE
            ALTAIR_DATASET_DIR="${CMAKE_SOURCE_DIR}/dataset")
        if(WIN32)
            target_link_libraries(altair_price_service PRIVATE ws2_32 mswsock)
        endif()
    else()
        message(STATUS "altair: price service skipped (no Boost)")
    endif()

    if(APPLE)
        set(_altair_helper_destination "Altair.app/Contents/Helpers")
    else()
        set(_altair_helper_destination "bin")
    endif()
    install(TARGETS
        altair_kite_login altair_fyers_login altair_kite_fetch
        altair_kite_account altair_fyers_account altair_kite_update altair_kite_ticker
        altair_kite_quote altair_fyers_history altair_fyers_webhook altair_fyers_ticker
        altair_fyers_quotes
        RUNTIME DESTINATION "${_altair_helper_destination}")
    if(TARGET altair_price_service)
        install(TARGETS altair_price_service
            RUNTIME DESTINATION "${_altair_helper_destination}")
    endif()
    unset(_altair_helper_destination)

    if(ALTAIR_BUILD_TESTS)
        add_test(NAME fyers_account_dry_run COMMAND altair_fyers_account)
        add_test(NAME fyers_ticker_dry_run COMMAND altair_fyers_ticker
            --symbols NSE:SBIN-EQ,NSE:NIFTY50-INDEX --depth --seconds 5)
        add_test(NAME fyers_quotes_dry_run COMMAND altair_fyers_quotes
            --symbols-file ${CMAKE_SOURCE_DIR}/config/market_watch.txt
            --symbols NSE:NIFTY26SEP25000CE)
        add_test(NAME kite_account_dry_run COMMAND altair_kite_account)
        add_test(NAME fyers_history_dry_run COMMAND altair_fyers_history
            --symbol NSE:SBIN-EQ --from 2026-09-01 --to 2026-09-02
            --out-1m fyers-1m.csv --out-5m fyers-5m.csv)
    endif()
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

    # P2-09. The broker-service poll loop: drives broker/service_runtime against
    # a transport seam. The test uses a scripted fake transport, so the poll
    # cadence, helper-death timestamp retention and revocation are deterministic.
    add_executable(altair_broker_service_test tests/test_broker_service.cpp)
    target_link_libraries(altair_broker_service_test
        PRIVATE altair_broker altair_types altair_flags)
    target_include_directories(altair_broker_service_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    add_test(NAME app_broker_service COMMAND altair_broker_service_test)
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

# CX02-A1a / CX02-A2. What altair_kite_update and altair_kite_fetch write into
# dataset/: exact price text (C14-001), and a merge and file replacement that
# cannot lose a stored bar (C14-005/006/007). Header-only and offline -- the
# fetch half needs a credential and is not exercised here. Links altair_broker
# (no net) for RawCandle and verify_coverage.
if(ALTAIR_BUILD_TESTS)
    add_executable(altair_price_text_test tests/test_price_text.cpp)
    target_link_libraries(altair_price_text_test PRIVATE altair_flags)
    target_include_directories(altair_price_text_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    add_test(NAME price_text COMMAND altair_price_text_test)

    add_executable(altair_dataset_merge_test tests/test_dataset_merge.cpp)
    target_link_libraries(altair_dataset_merge_test PRIVATE
        altair_broker altair_flags)
    target_include_directories(altair_dataset_merge_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    add_test(NAME dataset_merge COMMAND altair_dataset_merge_test)
    # Runs threads: the TSan CI job runs exactly the `concurrency` label.
    set_tests_properties(dataset_merge PROPERTIES LABELS concurrency)
endif()
