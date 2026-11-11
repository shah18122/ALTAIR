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
install(TARGETS altair_credential_helper RUNTIME DESTINATION bin)

if(ALTAIR_BUILD_TESTS)
    add_test(NAME app_selftest COMMAND altair --selftest)
    add_test(NAME app_instruments COMMAND altair --instruments)
    add_executable(altair_paper_session_test tests/test_paper_session.cpp paper_session.hpp)
    target_link_libraries(altair_paper_session_test PRIVATE altair_oms altair_flags)
    target_include_directories(altair_paper_session_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    add_test(NAME app_paper_session COMMAND altair_paper_session_test)
endif()

# ── the broker helpers ────────────────────────────────────────────────────
#
# SEPARATE BINARIES, not modes of `altair`. They reach the public internet
# with a credential and link OpenSSL. Folding them into the engine binary
# would put a TLS stack and a secret-reading code path inside the process
# that places orders, for no benefit.
#
# Built only when the vcpkg `net` feature is present, so `--preset default`
# keeps working with no vcpkg at all.
if(ALTAIR_HAVE_NET)
    add_executable(altair_fyers_webhook fyers_webhook_main.cpp)
    target_link_libraries(altair_fyers_webhook PRIVATE altair_broker_net altair_flags)

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
    # FyersDataAdapter. Read-only; a separate process so the credential stays
    # out of the window and the engine.
    add_executable(altair_fyers_ticker fyers_ticker_main.cpp)
    target_link_libraries(altair_fyers_ticker PRIVATE
        altair_broker_net altair_feed altair_instruments altair_flags)
    target_compile_definitions(altair_fyers_ticker PRIVATE
        ALTAIR_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    # --bypass IFACE: the same feed over AF_XDP and ALTAIR's own TCP/TLS (net/).
    if(ALTAIR_KERNEL_BYPASS)
        target_link_libraries(altair_fyers_ticker PRIVATE altair_bypass_ws)
        target_compile_definitions(altair_fyers_ticker PRIVATE
            ALTAIR_KERNEL_BYPASS ALTAIR_XDP_OBJECT="${ALTAIR_XDP_OBJECT}")
    endif()

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

    # Saves the day's NIFTY / BANKNIFTY option chains from FYERS before they
    # expire (app/fyers_chain.hpp): FYERS history refuses expired contracts, so
    # this record is the only chain history ALTAIR will have.
    add_executable(altair_fyers_chain fyers_chain_main.cpp)
    target_link_libraries(altair_fyers_chain PRIVATE
        altair_broker_net altair_instruments altair_flags)
    target_include_directories(altair_fyers_chain PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_compile_definitions(altair_fyers_chain PRIVATE
        ALTAIR_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

    # P34-01. Brings dataset/ up to date from FYERS in one call, MERGING into
    # the month files rather than replacing them (app/dataset_merge.hpp).
    add_executable(altair_dataset_update dataset_update_main.cpp)
    target_link_libraries(altair_dataset_update PRIVATE
        altair_broker_net altair_flags)

    # data/instruments.csv from the FYERS public symbol masters
    # (live/instrument_master.hpp). Public files, no credential.
    add_executable(altair_instrument_master instrument_master_main.cpp)
    target_link_libraries(altair_instrument_master PRIVATE
        altair_broker_net altair_live altair_flags)
    target_include_directories(altair_instrument_master PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

    # P37-03. THE PRICE SERVICE. One process owns the FYERS connection and
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
            altair_live altair_lockfree altair_credential_store altair_affinity altair_flags Boost::headers)
        if(TARGET altair_bypass_ws_link AND TARGET altair_dpdk_port)   # --dpdk-port: the HSM feed on a DPDK port
            target_link_libraries(altair_price_service PRIVATE altair_bypass_ws_link altair_dpdk_port)
            target_compile_definitions(altair_price_service PRIVATE ALTAIR_DPDK_FEED)
        endif()
        if(ALTAIR_KERNEL_BYPASS)   # --bypass: the HSM feed over AF_XDP (net/bypass_ws)
            target_link_libraries(altair_price_service PRIVATE altair_bypass_ws)
            target_compile_definitions(altair_price_service PRIVATE
                ALTAIR_KERNEL_BYPASS ALTAIR_XDP_OBJECT="${ALTAIR_XDP_OBJECT}")
        endif()
        target_compile_definitions(altair_price_service PRIVATE
            ALTAIR_DATASET_DIR="${CMAKE_SOURCE_DIR}/dataset"
            ALTAIR_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    else()
        message(STATUS "altair: price service skipped (no Boost)")
    endif()

    set(_altair_helper_destination "bin")
    install(TARGETS
        altair_fyers_login altair_fyers_account altair_dataset_update
        altair_instrument_master altair_fyers_history altair_fyers_chain
        altair_fyers_webhook altair_fyers_ticker altair_fyers_quotes
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
        add_test(NAME dataset_update_dry_run COMMAND altair_dataset_update)
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

# CX02-A1a / CX02-A2. What altair_dataset_update writes into
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

# Data audit: dataset/ checked across timeframes, against its other sources
# (_superseded_*) and against broker candles (ops/linux/fetch.py audit). Offline:
# it reads files and writes data/verified/ (data_audit.xlsx + merged CSVs).
# The training store: tapes decoded, dataset/ and FYERS merged into one
# snapshot at one cutoff (app/training_store.hpp). Offline, no credential.
add_executable(altair_store store_main.cpp)
target_link_libraries(altair_store PRIVATE altair_live altair_flags)
target_include_directories(altair_store PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_definitions(altair_store PRIVATE ALTAIR_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
if(ALTAIR_BUILD_TESTS)
    add_executable(altair_training_store_test tests/test_training_store.cpp)
    target_link_libraries(altair_training_store_test PRIVATE altair_live altair_flags)
    target_include_directories(altair_training_store_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    add_test(NAME training_store COMMAND altair_training_store_test)
    add_test(NAME store_help COMMAND altair_store --help)
endif()

add_executable(altair_data_audit data_audit_main.cpp)
target_link_libraries(altair_data_audit PRIVATE altair_flags)
target_include_directories(altair_data_audit PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
if(ALTAIR_BUILD_TESTS)
    add_executable(altair_data_audit_test tests/test_data_audit.cpp)
    target_link_libraries(altair_data_audit_test PRIVATE altair_flags)
    target_include_directories(altair_data_audit_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    add_test(NAME data_audit COMMAND altair_data_audit_test)
    # The whole audit over the real dataset/ (label `dataset`: runs where the
    # data is). Writes its report under the build tree, never into data/.
    add_test(NAME data_audit_dataset COMMAND altair_data_audit
        --dataset ${CMAKE_SOURCE_DIR}/dataset --out ${CMAKE_CURRENT_BINARY_DIR}/data_audit_out --no-merged)
    set_tests_properties(data_audit_dataset PROPERTIES LABELS dataset TIMEOUT 600)
endif()

# The forecast curriculum: every forecaster in models/ learns 3 days, forecasts,
# keeps score, refits on 6, 12, 24 ... days. Offline: reads dataset/, writes
# data/verified/forecast_curriculum.xlsx and per-forecast logs.
add_executable(altair_forecast_curriculum forecast_curriculum_main.cpp)
target_link_libraries(altair_forecast_curriculum PRIVATE altair_instruments altair_models altair_flags)
target_include_directories(altair_forecast_curriculum PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Orrell's eq. 8 smile against NSE option closes (research/models/quantum_oscillator.hpp).
add_executable(altair_quantum_smile quantum_smile_main.cpp)
target_link_libraries(altair_quantum_smile PRIVATE altair_instruments altair_research altair_flags)
target_include_directories(altair_quantum_smile PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
if(ALTAIR_BUILD_TESTS)
    add_test(NAME quantum_smile_help COMMAND altair_quantum_smile --help)
endif()

# Magnitude-aware scoring and the per-call value gate over the curriculum's logs.
# How big the next move is, every timeframe, on FYERS history (models/vol_forecast.hpp).
add_executable(altair_vol_forecast vol_forecast_main.cpp)
target_link_libraries(altair_vol_forecast PRIVATE altair_models altair_analytics altair_flags)
target_include_directories(altair_vol_forecast PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_executable(altair_magnitude magnitude_main.cpp)
target_link_libraries(altair_magnitude PRIVATE altair_models altair_analytics altair_flags)
target_include_directories(altair_magnitude PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Nations' volatility slope: INDIA VIX change per 1% index bucket, and the
# asymmetric spot-vol betas (analytics/option_math.hpp).
add_executable(altair_vol_slope vol_slope_main.cpp)
target_link_libraries(altair_vol_slope PRIVATE altair_analytics altair_flags)
target_include_directories(altair_vol_slope PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
if(ALTAIR_BUILD_TESTS)
    add_test(NAME vol_slope_help COMMAND altair_vol_slope --help)
endif()

# Order-book study over recorded FYERS depth (book/depth_study.hpp).
add_executable(altair_depth_study depth_study_main.cpp)
target_link_libraries(altair_depth_study PRIVATE altair_flags)
target_include_directories(altair_depth_study PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

# The latency probe: FYERS round trip, timer wake-up jitter, memory locking
# and the host's tuning as in effect (core/host_tuning.hpp). Read-only.
add_executable(altair_latency_probe latency_probe_main.cpp)
target_link_libraries(altair_latency_probe PRIVATE altair_affinity altair_flags Threads::Threads)
target_include_directories(altair_latency_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_definitions(altair_latency_probe PRIVATE ALTAIR_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
if(ALTAIR_BUILD_TESTS)
    add_test(NAME latency_probe COMMAND altair_latency_probe --no-network --wakeups 200
             --out ${CMAKE_CURRENT_BINARY_DIR}/latency_probe_out)
endif()

# The band-fade short-option demo: the range band's forecast, one lot, priced
# synthetically, costed through risk/cost.hpp (refused while unverified).
# The live models, paper-trading on altair_price_service's stream: bars from
# ticks, the strangle, direction-gate, pairs and stat-arb models, a paper book
# that fills at the bid and ask and charges every fill. Reads a loopback
# socket and writes data/live/; no broker, no OMS, no orders.
find_package(Boost CONFIG QUIET COMPONENTS headers)
if(TARGET altair_charges_toml AND Boost_FOUND)
    add_executable(altair_live_engine live_engine_main.cpp)
    target_link_libraries(altair_live_engine PRIVATE
        altair_live altair_models altair_strategies altair_analytics altair_charges_toml altair_flags altair_oms altair_affinity
        Boost::headers Threads::Threads)
    target_include_directories(altair_live_engine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_compile_definitions(altair_live_engine PRIVATE ALTAIR_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    # The commit at configure time, written beside each model bundle (not part of its id).
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(COMMAND ${GIT_EXECUTABLE} describe --always --dirty --abbrev=12
                        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} OUTPUT_VARIABLE altair_git_rev
                        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(altair_git_rev)
            target_compile_definitions(altair_live_engine PRIVATE ALTAIR_GIT_REV="${altair_git_rev}")
        endif()
    endif()
    # The event-driven strategies tuned on the recorded session tapes.
    add_executable(altair_tune tune_main.cpp)
    target_link_libraries(altair_tune PRIVATE altair_live altair_charges_toml altair_flags)
    target_include_directories(altair_tune PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_compile_definitions(altair_tune PRIVATE ALTAIR_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    # The OBI trader's stocks for the next session, scored on a recorded one.
    add_executable(altair_obi_select obi_select_main.cpp)
    target_link_libraries(altair_obi_select PRIVATE altair_live altair_flags)
    target_include_directories(altair_obi_select PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_compile_definitions(altair_obi_select PRIVATE ALTAIR_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    # Everything the price service publishes, recorded to tapes all session
    # (a scheduled task runs it whether or not the desktop is open).
    add_executable(altair_feed_recorder feed_recorder_main.cpp)
    target_link_libraries(altair_feed_recorder PRIVATE altair_live altair_flags Boost::headers Threads::Threads)
    target_include_directories(altair_feed_recorder PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_compile_definitions(altair_feed_recorder PRIVATE ALTAIR_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    # Each minute's ATM implied vol from the recorded FYERS chain.
    add_executable(altair_iv_record iv_record_main.cpp)
    target_link_libraries(altair_iv_record PRIVATE altair_live altair_flags)
    target_include_directories(altair_iv_record PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    if(ALTAIR_BUILD_TESTS)
        add_test(NAME live_engine_help COMMAND altair_live_engine --help)
        add_test(NAME tune_help COMMAND altair_tune --help)
        add_test(NAME iv_record_help COMMAND altair_iv_record --help)
        add_test(NAME feed_recorder_dry_run COMMAND altair_feed_recorder --seconds 5)
        add_executable(altair_iv_record_test tests/test_iv_record.cpp)
        target_link_libraries(altair_iv_record_test PRIVATE altair_live altair_flags)
        target_include_directories(altair_iv_record_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
        add_test(NAME iv_record COMMAND altair_iv_record_test)
        add_executable(altair_live_tune_test tests/test_live_tune.cpp)
        target_link_libraries(altair_live_tune_test PRIVATE altair_live altair_flags)
        target_include_directories(altair_live_tune_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
        add_test(NAME live_tune COMMAND altair_live_tune_test)
        add_test(NAME obi_select_help COMMAND altair_obi_select --help)
        add_executable(altair_obi_select_test tests/test_obi_select.cpp)
        target_link_libraries(altair_obi_select_test PRIVATE altair_live altair_flags)
        target_include_directories(altair_obi_select_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
        add_test(NAME obi_select COMMAND altair_obi_select_test)
        # A simulated morning recorded, then replayed: the same journal and
        # decisions byte for byte (needs dataset/ and a shell).
        if(TARGET altair_price_service)
            add_test(NAME live_replay
                     COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/live_replay.sh $<TARGET_FILE:altair_price_service>
                             $<TARGET_FILE:altair_live_engine> ${CMAKE_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/live_replay_work
                             $<TARGET_FILE:altair_exec_study> $<TARGET_FILE:altair_paper_report> $<TARGET_FILE:altair_readiness>)
            set_tests_properties(live_replay PROPERTIES LABELS "dataset;e2e" TIMEOUT 1100 RUN_SERIAL TRUE)
        endif()
    endif()
endif()

# The paper book's daily mark-to-market report (live/report.hpp).
add_executable(altair_paper_report paper_report_main.cpp)
target_link_libraries(altair_paper_report PRIVATE altair_live altair_flags)
target_include_directories(altair_paper_report PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_definitions(altair_paper_report PRIVATE ALTAIR_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
if(ALTAIR_BUILD_TESTS)
    add_test(NAME paper_report_help COMMAND altair_paper_report --help)
endif()

# Execution labels for each paper fill, from the session tapes (live/exec_study.hpp).
add_executable(altair_exec_study exec_study_main.cpp)
target_link_libraries(altair_exec_study PRIVATE altair_live altair_flags)
target_include_directories(altair_exec_study PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_definitions(altair_exec_study PRIVATE ALTAIR_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
if(ALTAIR_BUILD_TESTS)
    add_test(NAME exec_study_help COMMAND altair_exec_study --help)
endif()

# The operational and economic gates before any money (live/readiness.hpp).
add_executable(altair_readiness readiness_main.cpp)
target_link_libraries(altair_readiness PRIVATE altair_live altair_flags)
target_include_directories(altair_readiness PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_definitions(altair_readiness PRIVATE ALTAIR_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
if(ALTAIR_BUILD_TESTS)
    add_test(NAME readiness_help COMMAND altair_readiness --help)
endif()

# The paper engine's expenses against a contract note (app/charges_check.hpp).
if(TARGET altair_charges_toml)
    add_executable(altair_charges_check charges_check_main.cpp)
    target_link_libraries(altair_charges_check PRIVATE altair_types altair_risk altair_charges_toml altair_flags)
    target_include_directories(altair_charges_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_compile_definitions(altair_charges_check PRIVATE ALTAIR_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    if(ALTAIR_BUILD_TESTS)
        add_test(NAME charges_check_help COMMAND altair_charges_check --help)
        add_executable(altair_charges_check_test tests/test_charges_check.cpp)
        target_link_libraries(altair_charges_check_test PRIVATE altair_types altair_risk altair_charges_toml altair_flags)
        target_include_directories(altair_charges_check_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
        target_compile_definitions(altair_charges_check_test PRIVATE ALTAIR_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
        add_test(NAME charges_check COMMAND altair_charges_check_test)
    endif()
endif()

# Avellaneda-Stoikov quoting on FYERS one-minute futures bars, paper only, and
# the HMM stress regime, walk-forward. Neither reads config/charges.toml, so
# both build without toml++.
add_executable(altair_as_mm as_mm_main.cpp)
target_link_libraries(altair_as_mm PRIVATE altair_models altair_strategies altair_flags)
target_include_directories(altair_as_mm PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
add_executable(altair_regimes regimes_main.cpp)
target_link_libraries(altair_regimes PRIVATE altair_models altair_flags)
target_include_directories(altair_regimes PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
if(ALTAIR_BUILD_TESTS)
    add_test(NAME as_mm_help COMMAND altair_as_mm --help)
    add_test(NAME regimes_help COMMAND altair_regimes --help)
endif()

if(TARGET altair_charges_toml)
    add_executable(altair_band_option_demo band_option_demo_main.cpp)
    target_link_libraries(altair_band_option_demo PRIVATE altair_models altair_strategies altair_charges_toml altair_flags)
    target_include_directories(altair_band_option_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

    # altair_trader: one model over every model's out-of-sample output,
    # refitted and traded day by day on the index futures.
    add_executable(altair_trader trader_main.cpp trader_combos.cpp)
    target_link_libraries(altair_trader PRIVATE altair_instruments altair_models altair_strategies altair_charges_toml altair_flags)
    target_include_directories(altair_trader PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

    # The owner's threshold strategies (threshold_strategy/), on paper.
    add_executable(altair_threshold threshold_main.cpp)
    target_link_libraries(altair_threshold PRIVATE altair_strategies altair_charges_toml altair_flags)
    target_include_directories(altair_threshold PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    if(ALTAIR_BUILD_TESTS)
        add_test(NAME threshold_help COMMAND altair_threshold --help)
    endif()

    # Pairs hedged with futures, walked forward over config/pairs.csv.
    add_executable(altair_pairs_futures pairs_futures_main.cpp)
    target_link_libraries(altair_pairs_futures PRIVATE altair_strategies altair_charges_toml altair_flags)
    target_include_directories(altair_pairs_futures PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

    # The variance premium, sold delta hedged: HAR forecast against implied
    # vol, on VIX-priced options or the saved FYERS chain closes.
    add_executable(altair_vol_premium vol_premium_main.cpp)
    target_link_libraries(altair_vol_premium PRIVATE altair_instruments altair_analytics altair_strategies altair_charges_toml altair_flags)
    target_include_directories(altair_vol_premium PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

    # Sell vol-of-vol on NSE closes (strategies/vol_of_vol.hpp): short the
    # 25-delta strangle, long the ATM straddle, vega-neutral, delta hedged.
    add_executable(altair_vol_of_vol vol_of_vol_main.cpp)
    target_link_libraries(altair_vol_of_vol PRIVATE altair_instruments altair_analytics altair_strategies altair_charges_toml altair_flags)
    target_include_directories(altair_vol_of_vol PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    add_test(NAME vol_of_vol_help COMMAND altair_vol_of_vol --help)

    # Forsyth's HJB optimal execution (research/execution/hjb_execution.hpp) on the paper's
    # data, against Table 7.3 and every Almgren-Chriss trajectory.
    add_executable(altair_optimal_exec optimal_exec_main.cpp)
    target_link_libraries(altair_optimal_exec PRIVATE altair_research altair_flags)
    target_include_directories(altair_optimal_exec PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    add_test(NAME optimal_exec_help COMMAND altair_optimal_exec --help)

    # Cross-sectional residual reversion over config/universe_nifty50.csv.
    add_executable(altair_resid_reversion resid_reversion_main.cpp)
    target_link_libraries(altair_resid_reversion PRIVATE altair_strategies altair_charges_toml altair_flags)
    target_include_directories(altair_resid_reversion PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
endif()
if(ALTAIR_BUILD_TESTS)
    add_executable(altair_forecast_tracks_test tests/test_forecast_tracks.cpp)
    target_link_libraries(altair_forecast_tracks_test PRIVATE altair_models altair_flags)
    target_include_directories(altair_forecast_tracks_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    add_test(NAME forecast_tracks COMMAND altair_forecast_tracks_test)
    # No forecast sees its future: every builder and every model gives the
    # same answers up to a cut whatever happens after it; planted leaks are caught.
    add_executable(altair_forecast_leak_test tests/test_forecast_leak.cpp)
    target_link_libraries(altair_forecast_leak_test PRIVATE altair_models altair_flags)
    target_include_directories(altair_forecast_leak_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    add_test(NAME forecast_leak COMMAND altair_forecast_leak_test)

    add_executable(altair_ivrv_feed_test tests/test_ivrv_feed.cpp)
    target_link_libraries(altair_ivrv_feed_test PRIVATE altair_models altair_flags)
    target_include_directories(altair_ivrv_feed_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    add_test(NAME ivrv_feed COMMAND altair_ivrv_feed_test)
    # The live 10:15 direction models: fitted before the session, they call
    # exactly what a fit at 10:15 would; the conditional gate's inputs.
    add_executable(altair_live_direction_test tests/test_live_direction.cpp)
    target_link_libraries(altair_live_direction_test PRIVATE
        altair_live altair_models altair_strategies altair_analytics altair_flags)
    target_include_directories(altair_live_direction_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    add_test(NAME live_direction COMMAND altair_live_direction_test)
    # A switched-on strategy's real orders: requests, sizing to the caps, fills,
    # the P&L record, legging and the halt -- on files, no broker.
    add_executable(altair_live_real_orders_test tests/test_live_real_orders.cpp)
    target_link_libraries(altair_live_real_orders_test PRIVATE altair_live altair_oms altair_flags)
    target_include_directories(altair_live_real_orders_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    add_test(NAME live_real_orders COMMAND altair_live_real_orders_test)
    # docs/DATA_POLICY.md, both locks: the run-time file check, and the source
    # scan that fails on any mention of the banned exchange file.
    add_executable(altair_data_policy_test tests/test_data_policy.cpp)
    target_link_libraries(altair_data_policy_test PRIVATE altair_flags)
    target_include_directories(altair_data_policy_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    add_test(NAME data_policy COMMAND altair_data_policy_test)
    find_program(ALTAIR_BASH bash)
    if(ALTAIR_BASH)
        add_test(NAME data_policy_scan COMMAND ${ALTAIR_BASH} ${PROJECT_SOURCE_DIR}/tools/check_data_policy.sh
                 WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    endif()
    add_executable(altair_fyers_chain_test tests/test_fyers_chain.cpp)
    target_link_libraries(altair_fyers_chain_test PRIVATE altair_instruments altair_flags)
    target_include_directories(altair_fyers_chain_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    add_test(NAME fyers_chain COMMAND altair_fyers_chain_test)
    # End to end on the real dataset/: one daily track (INDIA VIX), every
    # direction and band model, zero look-ahead refusals or the exit code says
    # so. ~30 s optimised, several minutes under the Debug ASan build. Writes
    # under the build tree, never into data/.
    add_test(NAME forecast_curriculum_dataset COMMAND altair_forecast_curriculum
        --dataset ${CMAKE_SOURCE_DIR}/dataset --out ${CMAKE_CURRENT_BINARY_DIR}/forecast_out
        --only "INDIA VIX daily" --no-log --jobs 1)
    set_tests_properties(forecast_curriculum_dataset PROPERTIES LABELS dataset TIMEOUT 3000)
    # Every daily track: a long market measurement (a few minutes optimised,
    # far longer under ASan), so `experiment`, which CI never runs;
    # `ctest -L dataset` does.
    add_test(NAME forecast_curriculum_daily COMMAND altair_forecast_curriculum
        --dataset ${CMAKE_SOURCE_DIR}/dataset --out ${CMAKE_CURRENT_BINARY_DIR}/forecast_daily_out
        --only daily --no-log)
    set_tests_properties(forecast_curriculum_daily PROPERTIES LABELS "dataset;experiment" TIMEOUT 3600)
endif()

# The live terminal's FYERS source: one FYERS socket update into the price
# bus's trade, quote and book frames. Offline; no socket.
if(ALTAIR_BUILD_TESTS)
    add_executable(altair_fyers_price_frames_test tests/test_fyers_price_frames.cpp)
    target_link_libraries(altair_fyers_price_frames_test PRIVATE
        altair_server altair_feed altair_instruments altair_flags)
    target_include_directories(altair_fyers_price_frames_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    add_test(NAME fyers_price_frames COMMAND altair_fyers_price_frames_test)

    # The price service's bus: an owner thread, a ring from the sources, and
    # snapshots for late joiners only. Runs threads: label `concurrency`.
    find_package(Boost CONFIG QUIET COMPONENTS headers)
    if(Boost_FOUND)
        add_executable(altair_shared_bus_test tests/test_shared_bus.cpp)
        target_link_libraries(altair_shared_bus_test PRIVATE
            altair_server altair_live altair_lockfree altair_flags Boost::headers Threads::Threads)
        target_include_directories(altair_shared_bus_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
        add_test(NAME shared_bus COMMAND altair_shared_bus_test)
        set_tests_properties(shared_bus PROPERTIES LABELS concurrency)

        # The engine's socket drained on its own thread: a stall in the
        # engine never makes the bus drop frames for it.
        add_executable(altair_feed_reader_test tests/test_feed_reader.cpp)
        target_link_libraries(altair_feed_reader_test PRIVATE
            altair_server altair_live altair_lockfree altair_flags Boost::headers Threads::Threads)
        target_include_directories(altair_feed_reader_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
        add_test(NAME feed_reader COMMAND altair_feed_reader_test)
        # A throughput check: run alone, never beside the CPU-heavy model
        # tests that `ctest -j` would otherwise schedule next to it.
        set_tests_properties(feed_reader PROPERTIES LABELS concurrency RUN_SERIAL TRUE)
    endif()
endif()
