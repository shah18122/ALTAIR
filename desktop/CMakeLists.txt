# desktop/ — THE UI. Qt 6 Widgets, C++23, in the same process as the engine.
#
# P11Q-01.
#
# GATE 3 IS ENFORCED HERE, IN THE LINK LINE.
#
# CLAUDE.md's "The in-process decision" gives up blast radius and keeps one
# thing in exchange: the UI cannot trade. Out of process that was guaranteed by
# the process boundary. In process it is guaranteed by THIS FILE and nothing
# else — so the link libraries below are a review surface, not a convenience.
#
# altair_oms and altair_broker are ABSENT and must stay absent. A UI target
# that links either of them fails gate 3, regardless of what the code does with
# them. `altair_desktop_link_audit` below makes that a build error rather than
# a thing a reviewer has to notice.

find_package(Qt6 6.8 QUIET COMPONENTS Core Gui Network Svg Widgets)

# P32-03. QT DEFINES `signals`, `slots` AND `emit` AS MACROS, AND THE ENGINE
# IS NOT ALLOWED TO CARE.
#
# `strategies/score.hpp` names a parameter `signals` -- which is the right name
# for the array of signals a score is combined from. Qt's qobjectdefs.h has
# `#define signals public`, so the moment a Qt translation unit included that
# header the declaration became
#
#     combine(const Signal* public, std::size_t n, ...)
#
# and the error MSVC prints is "missing ')' before 'public'" in a file that has
# no Qt in it and no `public` on that line. Nothing had included both until the
# Aggregator page pulled models/aggregator.hpp, and it in turn score.hpp, into
# a window.
#
# The fix belongs on THIS side of the boundary. CLAUDE.md's in-process decision
# lets desktop/ include read-side engine headers; it does not licence Qt to
# reserve three ordinary English words across the whole engine. QT_NO_KEYWORDS
# turns the lowercase macros off and leaves Q_SIGNALS / Q_SLOTS / Q_EMIT, which
# is what every file in desktop/ already uses.
#
# It also fixes the ones nobody has hit yet: `emit` and `slots` appear as
# ordinary words in feature and book headers, and would have failed the same
# way on the day some page happened to include them together.
add_compile_definitions(QT_NO_KEYWORDS)

if(NOT Qt6_FOUND)
    message(STATUS "desktop/: Qt6 not found — UI target skipped. "
                   "Set ALTAIR_QT_ROOT or CMAKE_PREFIX_PATH to a Qt 6.8+ "
                   "msvc2022_64 install.")
    return()
endif()

message(STATUS "desktop/: Qt ${Qt6_VERSION} at ${Qt6_DIR}")

qt_standard_project_setup()

qt_add_executable(altair_desktop
    main.cpp
    helper_process.hpp
    credential_setup.hpp
    main_window.hpp
    navigation_registry.hpp
    navigation_state.hpp
    navigation_search.hpp
    workspace_navigation.hpp
    workspace_pages.hpp
    workspace_layout.hpp
    tick_model.hpp
    filter.hpp
    market_clock.hpp
    format.hpp
    funds_summary.hpp
    position_table.hpp
    auth.hpp
    feed_status.hpp
    broker_activity.hpp
    fyers_link.hpp
    broker_page.hpp
    demo_trading_page.hpp
    arbitrage_workspace.hpp
    model_status.hpp
    watchlist.hpp
    panels.hpp
    chart/chart_widget.hpp
    chart/candles.hpp
    data/bar_csv.hpp
    data/fits.hpp
    data/real_tape.hpp
    data/master_lookup.hpp
    cost_panel.hpp
    audit_panel.hpp
    quant_pages.hpp
    model_job.hpp
    kite_panel.hpp
    account_widgets.hpp
    depth_ladder.hpp
    kill_switch.hpp
    order_ticket.hpp
    # P32-01. AUTOMOC only processes files in the target's own source list,
    # and terminal.hpp declares a Q_OBJECT. Leaving it out links against a moc
    # generated for a different target and fails with unresolved vtable
    # symbols rather than anything that names the cause.
    terminal.hpp
    gets_data.hpp
    gets_tables.hpp
    gets_workspace.hpp
    # P37-04. Q_OBJECT: it is a QObject with signals, so AUTOMOC must see it
    # in THIS target's list or the link fails on an unresolved vtable.
    price_client.hpp
    # P39. The option chain (Q_OBJECT) and the light series loader.
    option_chain.hpp
    data/series_io.hpp
    live_feed.hpp
    live_forecast.hpp
    stream_forecast_page.hpp
    kite_link.hpp
    login_outcome.hpp
    combined_account.hpp
    paper_oms.hpp
    paper_windows.hpp
    chain_panel.hpp
    analytics_panel.hpp
    # P36-01. The Model Atlas. atlas.hpp has no Q_OBJECT, but listing both
    # keeps the target's source list a true statement of what it builds.
    atlas_data.hpp
    atlas.hpp
    atlas_info.hpp
    # The full-screen shell: About, the live data-flow page, F11+F12 halt.
    shell_pages.hpp
    strategy_record.hpp
)

# P34-02. THE ICON RESOURCE WAS NEVER COMPILED, AND NOTHING SAID SO.
#
# assets/altair.qrc was listed as a source of altair_desktop. AUTOMOC and
# AUTOUIC are switched on by qt_standard_project_setup(); AUTORCC IS NOT. So
# the .qrc sat in the source list and no build rule ever mentioned it -- no
# qrc_*.cpp was generated, the resource did not exist, and every lookup of
# ":/altair_eagle.svg" returned nothing.
#
# QIcon and QPixmap both answer a missing resource with a null object rather
# than an error, so `app.setWindowIcon(QIcon(":/altair_eagle.svg"))` in
# main.cpp has been a silent no-op since it was written: the window and the
# taskbar have been showing Qt's default icon the whole time. It surfaced only
# when the login dialog asked for the same file and its label collapsed to
# zero height.
#
# qt_add_resources is explicit and generates a rule that appears in the build
# graph, which is the property the .qrc-in-a-source-list form lacked.
qt_add_resources(altair_desktop "altair_assets"
    PREFIX "/"
    BASE assets
    FILES assets/altair_eagle.svg)

# P11Q-05a. The cost panel reads config/charges.toml through P3-09b's loader,
# which exists only where vcpkg has provided toml++. Where it does not, the
# panel names the blocker instead of showing a plausible cost -- an invented
# charge is worse than no charge, because rule 5 is what everything downstream
# trusts.
if(TARGET altair_charges_toml)
    target_link_libraries(altair_desktop PRIVATE altair_charges_toml)
    target_compile_definitions(altair_desktop PRIVATE
        ALTAIR_HAVE_CHARGES_TOML=1
        ALTAIR_CHARGES_TOML="${CMAKE_SOURCE_DIR}/config/charges.toml")
    message(STATUS "desktop/: cost panel ENABLED (charges.toml loader present)")
else()
    message(STATUS "desktop/: cost panel will name its blocker "
                   "(no charges.toml loader in this preset)")
endif()

# READ-SIDE ONLY. See the header comment; this list is the safety property.
# Qt6::Svg is here for ONE reason: the window/taskbar icon is an SVG, so it
# stays sharp at every size the OS asks for instead of shipping five PNGs that
# drift apart. It is a read-side rendering module and does not widen the UI
# allow-list below in any way that matters -- but it IS a new link, so it is
# named here rather than appearing silently in a diff.
target_link_libraries(altair_desktop PRIVATE
    altair_types
    altair_time
    # P37-04. The price codec (protocol + payload). altair_server depends on
    # altair_types and altair_time and NOTHING else -- it is a byte layout, not
    # a transport. Gate 3 is unaffected: still no altair_oms, still no
    # altair_broker, and the client vocabulary has no word for an order.
    altair_server
    altair_feed
    altair_analytics
    altair_models
    altair_risk
    altair_backtest
    Qt6::Core
    Qt6::Network
    Qt6::Gui
    Qt6::Svg
    Qt6::Widgets
)

target_include_directories(altair_desktop PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

# dataset/ is gitignored and regenerable, so its location is a build-time
# constant rather than something the UI hunts for at runtime.
target_compile_definitions(altair_desktop PRIVATE
    ALTAIR_DATASET_DIR="${CMAKE_SOURCE_DIR}/dataset")

# The session file, by ABSOLUTE path. The Broker Wiring panel reports whether
# it exists, and a relative path would make that answer depend on the working
# directory the window happened to be launched from -- so the panel would say
# "no session" from one shortcut and "session" from another, with nothing on
# screen explaining the difference.
target_compile_definitions(altair_desktop PRIVATE
    ALTAIR_QUOTE_FILE="${CMAKE_SOURCE_DIR}/data/kite_quotes.json"
    ALTAIR_TICKS_FILE="${CMAKE_SOURCE_DIR}/data/kite_ticks.json"
    ALTAIR_INTENT_FILE="${CMAKE_SOURCE_DIR}/data/order_intents.jsonl"
    ALTAIR_SESSION_FILE="${CMAKE_SOURCE_DIR}/data/kite_session.json"
    ALTAIR_FYERS_SESSION_FILE="${CMAKE_SOURCE_DIR}/data/fyers_session.json"
    ALTAIR_FYERS_ACCOUNT_FILE="${CMAKE_SOURCE_DIR}/data/fyers_account.json"
    ALTAIR_ACCOUNT_FILE="${CMAKE_SOURCE_DIR}/data/kite_account.json"
    ALTAIR_BROKER_ACTIVITY_FILE="${CMAKE_SOURCE_DIR}/data/broker_activity.json"
    ALTAIR_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    ALTAIR_KILL_REQUEST_FILE="${CMAKE_SOURCE_DIR}/data/kill_request.json")

# Whether the networked half exists is a property of the BUILD, and the panel
# reads it rather than being told. broker/CMakeLists.txt sets this when
# OpenSSL is found.
if(ALTAIR_HAVE_NET)
    target_compile_definitions(altair_desktop PRIVATE ALTAIR_HAVE_NET=1)
    message(STATUS "desktop/: broker net half present — wiring panel will say so")
endif()

# THE DEFAULT ACCOUNTS EXIST ONLY HERE, AND ONLY OUTSIDE A RELEASE BUILD.
#
# admin/admin and staff/staff are compiled in when this is defined. The `prod`
# preset builds Release, so a shipped binary has no default account to forget
# to remove -- seed_default_users() compiles to an empty function and the login
# screen reports that nothing is provisioned.
if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_definitions(altair_desktop PRIVATE ALTAIR_DEV_CREDENTIALS)
    message(STATUS "desktop/: DEV CREDENTIALS COMPILED IN (admin/admin, staff/staff)")
else()
    message(STATUS "desktop/: release build -- no default accounts")
endif()

# ── The gate 3 audit ─────────────────────────────────────────────────────────
#
# A human reading a diff can miss a line added to target_link_libraries. This
# cannot: it fails configure if the UI links anything not on this list.
#
# IT IS AN ALLOW-LIST, AND IT USED TO BE A DENY-LIST.
#
# The first version failed on {altair_oms, altair_broker}. That catches the
# mistake somebody makes today and nothing else: a library that can place an
# order under a name nobody thought of -- a future altair_execution, a
# convenience façade, a broker SDK pulled in transitively -- passes a deny-list
# silently. A deny-list encodes the failures you have already imagined, which
# is the wrong half of the space.
#
# The safety property is "the UI cannot trade", and it is the ONE thing kept by
# construction after the in-process decision gave up blast radius. It deserves
# the form where adding a dependency is what requires the argument. Adding a
# line here is cheap and deliberate; that is the point.
set(_altair_ui_permitted
    altair_types      # strong units, read-side
    altair_time       # timestamps
    altair_feed       # replay + normalised ticks, read-side
    # P11Q-05a. Parses config/charges.toml into ChargeSchedule and does nothing
    # else -- no venue, no session, no order vocabulary anywhere in it. The
    # cost panel needs the real rates and rule 1 says they come from the file,
    # not from a literal, so the alternative to this line is a hard-coded
    # schedule in the UI, which is worse. Added deliberately, per the error
    # message below.
    altair_charges_toml
    Qt6::Core Qt6::Gui Qt6::Widgets
    # The application icon is an SVG (Aquila, the eagle Altair sits in), so
    # the renderer has to be linked. Qt6::Svg rasterises vector graphics and
    # has no network, no process, and no vocabulary of any kind that could
    # reach an order path. Added deliberately, per the error below, rather
    # than by widening the check.
    Qt6::Svg
    # P19. The quant pages read analytics/, models/, risk/ and backtest/ --
    # ALL READ-SIDE. oms/ and broker/ remain absent and the check below still
    # refuses them, which is the property that matters: the UI can now show
    # every model in the tree and still cannot place an order.
    altair_analytics
    altair_models
    altair_risk
    altair_backtest
    # P37-04. The price codec: protocol.hpp's frame header and
    # price_payload.hpp's byte layout. altair_server links altair_types and
    # altair_time and NOTHING else -- it is a wire format, not a transport,
    # and the client vocabulary protocol.hpp defines is Subscribe,
    # Unsubscribe, SnapshotRequest and KillSwitch. There is no word in it for
    # placing an order and there must never be one.
    altair_server
    # Qt6::Network, AND THIS ONE IS A REAL WIDENING. SAY SO.
    #
    # Until now the UI process had no socket at all. It could not open a
    # connection even in principle, because nothing that could was linked into
    # it. That was a genuine property and this line spends it: the window can
    # now open TCP connections, and a bad cast or a bad index in a chart is
    # running in a process that has that capability.
    #
    # What is bought: the price service. Smit asked for one process to own the
    # feed on a port with everything else reading from it, which necessarily
    # means the reader opens a socket. The alternative is the subprocess-plus-
    # file pattern, which he explicitly rejected, and which is why the depth
    # ladder has been fed a null book since the day it was written.
    #
    # What is NOT bought, and is still enforced here: the UI holds no
    # credential, links no broker/ and no oms/, has no TLS client and no API
    # key, and connects to 127.0.0.1 only. It cannot reach Kite. The thing
    # gate 3 exists to prevent -- the UI placing an order -- is unchanged.
    #
    # This paragraph exists for the same reason the in-process decision's does
    # in CLAUDE.md: so nobody later reads the line and assumes it was an
    # oversight.
    Qt6::Network
)
get_target_property(_altair_ui_links altair_desktop LINK_LIBRARIES)
foreach(_lib IN LISTS _altair_ui_links)
    if(NOT _lib IN_LIST _altair_ui_permitted)
        message(FATAL_ERROR
            "desktop/ links '${_lib}', which is not on the UI's permitted "
            "list. The UI must not be able to place an order — see CLAUDE.md "
            "'The in-process decision'. This is gate 3.\n"
            "If '${_lib}' is genuinely read-side and holds no order-placing "
            "vocabulary, add it to _altair_ui_permitted in this file WITH A "
            "COMMENT SAYING WHY. Do not widen this list to make a build pass.")
    endif()
endforeach()

# Header-only engine code the UI includes without linking -- models/markov.hpp,
# models/markov_eval.hpp, backtest/validation.hpp, as of P21-03
# strategies/momentum.hpp and strategies/meanrev.hpp, and as of P31-01
# strategies/parity.hpp and strategies/calendar.hpp -- is reached through the
# include path above and so never appears in LINK_LIBRARIES. It is pure
# computation with no order-placing vocabulary, which is the same test the list
# applies; noted here because the audit cannot see it and a reader will wonder.

# ── Tests ───────────────────────────────────────────────────────────────────
# The UI's logic is testable without a window: a QCoreApplication is enough for
# a model and a proxy, so gate 4 applies here exactly as it does to the engine.
# CX06 cold-path, read-only data audit. No broker, OMS or existing writer dependency.
qt_add_executable(altair_bar_consistency data/bar_consistency.cpp)
target_link_libraries(altair_bar_consistency PRIVATE altair_flags Qt6::Core)
set_target_properties(altair_bar_consistency PROPERTIES WIN32_EXECUTABLE OFF)

if(ALTAIR_BUILD_TESTS)
    qt_add_executable(altair_credential_setup_test
        tests/test_credential_setup.cpp
        credential_setup.hpp
        helper_process.hpp)
    target_link_libraries(altair_credential_setup_test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Widgets)
    target_include_directories(altair_credential_setup_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    set_target_properties(altair_credential_setup_test
        PROPERTIES WIN32_EXECUTABLE OFF)
    add_test(NAME desktop_credential_setup COMMAND altair_credential_setup_test)

    qt_add_executable(altair_fyers_link_test
        tests/test_fyers_link.cpp
        fyers_link.hpp
        credential_setup.hpp
        helper_process.hpp)
    target_link_libraries(altair_fyers_link_test PRIVATE
        altair_types Qt6::Core Qt6::Gui Qt6::Widgets)
    target_include_directories(altair_fyers_link_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    set_target_properties(altair_fyers_link_test
        PROPERTIES WIN32_EXECUTABLE OFF)
    add_test(NAME desktop_fyers_link COMMAND altair_fyers_link_test)

    qt_add_executable(altair_fyers_panel_test
        tests/test_fyers_panel.cpp
        fyers_panel.hpp
        helper_process.hpp)
    target_link_libraries(altair_fyers_panel_test PRIVATE
        altair_types Qt6::Core Qt6::Gui Qt6::Widgets)
    target_include_directories(altair_fyers_panel_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_compile_definitions(altair_fyers_panel_test PRIVATE
        ALTAIR_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    set_target_properties(altair_fyers_panel_test
        PROPERTIES WIN32_EXECUTABLE OFF)
    add_test(NAME desktop_fyers_panel COMMAND altair_fyers_panel_test)
    set_tests_properties(desktop_fyers_panel PROPERTIES TIMEOUT 60 ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORM_PLUGIN_PATH=${Qt6_DIR}/../../../plugins/platforms")
    if(WIN32)
        set_tests_properties(desktop_fyers_panel PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()

    # P2-02a. Broker helpers must never wait on the UI thread.  The test
    # launches a tiny no-Qt child so it covers real start, output, exit, busy
    # and timeout behaviour without a broker binary or credential.
    add_executable(altair_helper_process_child
        tests/helper_process_child.cpp)
    target_link_libraries(altair_helper_process_child PRIVATE altair_flags)
    qt_add_executable(altair_helper_process_test
        tests/test_helper_process.cpp
        helper_process.hpp)
    target_link_libraries(altair_helper_process_test PRIVATE Qt6::Core)
    target_include_directories(altair_helper_process_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_compile_definitions(altair_helper_process_test PRIVATE
        ALTAIR_HELPER_PROCESS_CHILD="$<TARGET_FILE:altair_helper_process_child>")
    add_dependencies(altair_helper_process_test altair_helper_process_child)
    set_target_properties(altair_helper_process_test
        PROPERTIES WIN32_EXECUTABLE OFF
                   OUTPUT_NAME altair_helper_process_ipc_test)
    add_test(NAME desktop_helper_process COMMAND altair_helper_process_test)

    # Exercise the real shell, but never inherit its real account/dataset paths.
    get_target_property(_cx06_shell_headers altair_desktop SOURCES)
    list(FILTER _cx06_shell_headers INCLUDE REGEX "\\.hpp$")
    get_target_property(_cx06_shell_links altair_desktop LINK_LIBRARIES)
    qt_add_executable(altair_navigation_shell_test
        tests/test_navigation_shell.cpp ${_cx06_shell_headers})
    target_link_libraries(altair_navigation_shell_test PRIVATE ${_cx06_shell_links})
    target_include_directories(altair_navigation_shell_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_compile_definitions(altair_navigation_shell_test PRIVATE
        ALTAIR_SOURCE_DIR="."
        ALTAIR_DATASET_DIR="dataset"
        ALTAIR_QUOTE_FILE="data/kite_quotes.json"
        ALTAIR_TICKS_FILE="data/kite_ticks.json"
        ALTAIR_ACCOUNT_FILE="data/kite_account.json"
        ALTAIR_SESSION_FILE="data/kite_session.json"
        ALTAIR_INTENT_FILE="data/order_intents.jsonl"
        ALTAIR_KILL_REQUEST_FILE="data/kill_request.json")
    set_target_properties(altair_navigation_shell_test PROPERTIES WIN32_EXECUTABLE OFF)
    add_test(NAME desktop_navigation_shell COMMAND altair_navigation_shell_test)
    add_test(NAME desktop_navigation_shell_125 COMMAND altair_navigation_shell_test)
    add_test(NAME desktop_navigation_shell_200 COMMAND altair_navigation_shell_test)
    set_tests_properties(desktop_navigation_shell_125 desktop_navigation_shell_200 PROPERTIES
        TIMEOUT 60 ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORM_PLUGIN_PATH=${Qt6_DIR}/../../../plugins/platforms")
    set_property(TEST desktop_navigation_shell_125 APPEND PROPERTY ENVIRONMENT "QT_SCALE_FACTOR=1.25")
    set_property(TEST desktop_navigation_shell_200 APPEND PROPERTY ENVIRONMENT "QT_SCALE_FACTOR=2")
    set_tests_properties(desktop_navigation_shell PROPERTIES TIMEOUT 60 ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORM_PLUGIN_PATH=${Qt6_DIR}/../../../plugins/platforms;QT_SCALE_FACTOR=1")
    if(WIN32)
        set_property(TEST desktop_navigation_shell desktop_navigation_shell_125 desktop_navigation_shell_200
            APPEND PROPERTY ENVIRONMENT
            "QT_QPA_FONTDIR=$ENV{WINDIR}/Fonts")
        set_tests_properties(desktop_navigation_shell desktop_navigation_shell_125 desktop_navigation_shell_200
            PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()

    qt_add_executable(test_bar_consistency tests/test_bar_consistency.cpp)
    target_link_libraries(test_bar_consistency PRIVATE altair_flags Qt6::Core)
    add_dependencies(test_bar_consistency altair_bar_consistency)
    set_target_properties(test_bar_consistency PROPERTIES WIN32_EXECUTABLE OFF)
    add_test(NAME desktop_bar_consistency COMMAND test_bar_consistency)
    if(WIN32)
        set_tests_properties(desktop_bar_consistency PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()

    # CX06: presentation-only registry, menus, persistence and keyboard tests.
    qt_add_executable(altair_navigation_test tests/test_navigation.cpp)
    target_link_libraries(altair_navigation_test PRIVATE
        altair_flags Qt6::Core Qt6::Gui Qt6::Widgets)
    set_target_properties(altair_navigation_test PROPERTIES WIN32_EXECUTABLE OFF)
    add_test(NAME desktop_navigation COMMAND altair_navigation_test)
    set_tests_properties(desktop_navigation PROPERTIES TIMEOUT 60 ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORM_PLUGIN_PATH=${Qt6_DIR}/../../../plugins/platforms")
    if(WIN32)
        set_property(TEST desktop_navigation APPEND PROPERTY ENVIRONMENT
            "QT_QPA_FONTDIR=$ENV{WINDIR}/Fonts")
        set_tests_properties(desktop_navigation PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()

    # The Q_OBJECT headers must be listed HERE too. AUTOMOC only processes
    # files in the target's own source list, so leaving them out links against
    # a moc that was generated for a different target and fails with seven
    # unresolved vtable symbols rather than anything that names the cause.
    qt_add_executable(altair_desktop_filter_test
        tests/test_filter.cpp
        tick_model.hpp
        filter.hpp)
    target_link_libraries(altair_desktop_filter_test PRIVATE
        altair_types altair_time altair_feed Qt6::Core Qt6::Gui Qt6::Widgets)
    target_include_directories(altair_desktop_filter_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    set_target_properties(altair_desktop_filter_test PROPERTIES WIN32_EXECUTABLE OFF)
    add_test(NAME desktop_filter COMMAND altair_desktop_filter_test)

    # candles.hpp has no Q_OBJECT, so this one needs no moc and no Qt at all
    # beyond the engine types it already uses.
    # P32-05. Now builds the WIDGET as well as candles.hpp, because zoom is a
    # property of the widget: qt_add_executable and the Q_OBJECT header, and
    # Gui/Widgets rather than Core alone.
    qt_add_executable(altair_desktop_chart_test
        tests/test_chart.cpp
        chart/chart_widget.hpp)
    target_link_libraries(altair_desktop_chart_test PRIVATE
        altair_types altair_time altair_feed Qt6::Core Qt6::Gui Qt6::Widgets)
    set_target_properties(altair_desktop_chart_test
        PROPERTIES WIN32_EXECUTABLE OFF)
    target_include_directories(altair_desktop_chart_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    add_test(NAME desktop_chart COMMAND altair_desktop_chart_test)

    # P11Q-03. ladder_view() is pure, so the four ways a ladder renders a
    # plausible WRONG book are testable without a window: zeroed slots drawn as
    # liquidity, a crossed book refused instead of marked, never-updated shown
    # as empty, and an imbalance of +/-1 conjured from an absent side.
    qt_add_executable(altair_depth_ladder_test
        tests/test_depth_ladder.cpp
        depth_ladder.hpp)
    target_link_libraries(altair_depth_ladder_test PRIVATE
        altair_types altair_time altair_feed Qt6::Core Qt6::Gui Qt6::Widgets)
    target_include_directories(altair_depth_ladder_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    set_target_properties(altair_depth_ladder_test PROPERTIES WIN32_EXECUTABLE OFF)
    add_test(NAME desktop_depth_ladder COMMAND altair_depth_ladder_test)

    # Runs against the REAL series in dataset/, not a fixture. dataset/ is
    # gitignored and regenerable, so the test skips loudly rather than failing
    # if it is absent -- see the guard in the test's own output.
    # P11Q-12. The Kite connection pill. The card is that presence is not
    # validity: the session file on this machine parses, reports success, and
    # is a day old, and Kite access tokens are daily.
    qt_add_executable(altair_broker_status_test
        tests/test_broker_status.cpp
        broker_status.hpp)
    target_link_libraries(altair_broker_status_test PRIVATE
        altair_types altair_time Qt6::Core Qt6::Gui)
    target_include_directories(altair_broker_status_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    set_target_properties(altair_broker_status_test PROPERTIES WIN32_EXECUTABLE OFF)
    # THE TEST NEEDS THE SAME DEFINE AS THE UI, and it has to be set HERE.
    #
    # The first attempt put it beside the altair_desktop define higher up in
    # this file, guarded by if(TARGET ...). That guard is evaluated before this
    # block runs, so the target did not exist yet and the define was silently
    # skipped -- the test then asserted the NO-TRANSPORT branch inside the net
    # preset and passed, checking the opposite of what ships. Found by reading
    # the test output rather than its exit code, which was green either way.
    if(ALTAIR_HAVE_NET)
        target_compile_definitions(altair_broker_status_test PRIVATE ALTAIR_HAVE_NET=1)
    endif()
    add_test(NAME desktop_broker_status COMMAND altair_broker_status_test)

    # P2-10. The unified Brokers page consumes only the bounded, credential-free
    # service projection. This test covers expiry/schema/secret rejection,
    # provider attribution, overflow evidence, filtering and export.
    qt_add_executable(altair_broker_activity_test
        tests/test_broker_activity.cpp
        broker_activity.hpp)
    target_link_libraries(altair_broker_activity_test PRIVATE
        altair_types altair_time Qt6::Core Qt6::Gui Qt6::Widgets)
    target_include_directories(altair_broker_activity_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    set_target_properties(altair_broker_activity_test PROPERTIES WIN32_EXECUTABLE OFF)
    add_test(NAME desktop_broker_activity COMMAND altair_broker_activity_test)
    set_tests_properties(desktop_broker_activity PROPERTIES TIMEOUT 60 ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORM_PLUGIN_PATH=${Qt6_DIR}/../../../plugins/platforms")
    if(WIN32)
        set_tests_properties(desktop_broker_activity PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()

    # P20-02. The Kite account panel. Exercised against a SYNTHETIC snapshot
    # because the live account is expired -- a renderer that has never
    # rendered is the one that crashes when a real snapshot finally arrives.
    qt_add_executable(altair_kite_panel_test
        tests/test_kite_panel.cpp
        kite_panel.hpp)
    target_link_libraries(altair_kite_panel_test PRIVATE
        altair_types altair_time altair_feed Qt6::Core Qt6::Gui Qt6::Widgets)
    target_include_directories(altair_kite_panel_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    set_target_properties(altair_kite_panel_test PROPERTIES WIN32_EXECUTABLE OFF)
    add_test(NAME desktop_kite_panel COMMAND altair_kite_panel_test)

    # P22-04. The Overnight Gap page has its OWN reader, and a reader is where
    # a page can start answering a different question without saying so. This
    # checks it against an independent parse of the same files, column by
    # column -- specifically that `open` is the auction print and not a copy of
    # the first bar's close, which are five minutes and 38% of the strategy
    # apart. Links altair_strategies, which is a TEST target and therefore
    # outside the altair_desktop allow-list audit below; the UI target itself
    # still only INCLUDES those headers and links nothing new.
    qt_add_executable(altair_overnight_page_test
        tests/test_overnight_page.cpp
        quant_pages.hpp)
    target_link_libraries(altair_overnight_page_test PRIVATE
        altair_types altair_time altair_feed altair_analytics altair_models
        altair_risk altair_backtest altair_strategies
        Qt6::Core Qt6::Gui Qt6::Widgets)
    target_include_directories(altair_overnight_page_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_compile_definitions(altair_overnight_page_test PRIVATE
        ALTAIR_DATASET_DIR="${CMAKE_SOURCE_DIR}/dataset")
    set_target_properties(altair_overnight_page_test
        PROPERTIES WIN32_EXECUTABLE OFF)
    add_test(NAME desktop_overnight_page COMMAND altair_overnight_page_test)
    # Reads dataset/ (not in git): CI runs `ctest -LE dataset`.
    set_tests_properties(desktop_overnight_page PROPERTIES LABELS dataset)

    # P23-02. The Options page. Checks the pricer and the IV solver are
    # inverses to within the TICK -- the tolerance is derived from vega, not
    # chosen -- that the wings stop resolving, and that the butterfly scan
    # finds a boundary when pushed past one. Prints the page so its columns
    # are read by somebody.
    qt_add_executable(altair_options_page_test
        tests/test_options_page.cpp
        quant_pages.hpp)
    target_link_libraries(altair_options_page_test PRIVATE
        altair_types altair_time altair_feed altair_analytics altair_models
        altair_risk altair_backtest altair_strategies
        Qt6::Core Qt6::Gui Qt6::Widgets)
    target_include_directories(altair_options_page_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_compile_definitions(altair_options_page_test PRIVATE
        ALTAIR_DATASET_DIR="${CMAKE_SOURCE_DIR}/dataset")
    set_target_properties(altair_options_page_test
        PROPERTIES WIN32_EXECUTABLE OFF)
    add_test(NAME desktop_options_page COMMAND altair_options_page_test)

    # P23-05. Renders EVERY compute page and asserts the dataset-backed ones
    # are not showing their no-data fallback. That fallback is correct
    # behaviour and invisible in a screenshot nobody takes, so a renamed
    # directory would otherwise leave a page permanently apologising with
    # every other test green.
    qt_add_executable(altair_pages_test
        tests/test_pages.cpp
        quant_pages.hpp)
    target_link_libraries(altair_pages_test PRIVATE
        altair_types altair_time altair_feed altair_analytics altair_models
        altair_risk altair_backtest altair_strategies
        Qt6::Core Qt6::Gui Qt6::Widgets)
    target_include_directories(altair_pages_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_compile_definitions(altair_pages_test PRIVATE
        ALTAIR_DATASET_DIR="${CMAKE_SOURCE_DIR}/dataset")
    # P31-01. THE COSTED HALF OF THE PARITY PAGE HAD NEVER RUN IN A TEST.
    #
    # altair_desktop links the charges loader and this test did not, so under
    # the `net` and `vcpkg` presets the WINDOW computed parity, the box and
    # the futures calendar while the TEST took the blocked branch and passed.
    # Three scanners live in the shipped UI with no coverage, and the test
    # reported green -- which is the failure mode a test exists to prevent.
    #
    # Given to the test on the same condition the window gets it, so the two
    # take the same branch in the same preset.
    if(TARGET altair_charges_toml)
        target_link_libraries(altair_pages_test PRIVATE altair_charges_toml)
        target_compile_definitions(altair_pages_test PRIVATE
            ALTAIR_HAVE_CHARGES_TOML=1
            ALTAIR_CHARGES_TOML="${CMAKE_SOURCE_DIR}/config/charges.toml")
    endif()
    set_target_properties(altair_pages_test PROPERTIES WIN32_EXECUTABLE OFF)
    add_test(NAME desktop_pages COMMAND altair_pages_test)
    # Reads dataset/ (not in git): CI runs `ctest -LE dataset`.
    set_tests_properties(desktop_pages PROPERTIES LABELS dataset)

    qt_add_executable(altair_model_job_test
        tests/test_model_job.cpp
        model_job.hpp)
    target_link_libraries(altair_model_job_test PRIVATE Qt6::Core)
    target_include_directories(altair_model_job_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    set_target_properties(altair_model_job_test PROPERTIES WIN32_EXECUTABLE OFF)
    add_test(NAME desktop_model_job COMMAND altair_model_job_test)

    qt_add_executable(altair_compute_page_test
        tests/test_compute_page.cpp
        quant_pages.hpp
        model_job.hpp)
    target_link_libraries(altair_compute_page_test PRIVATE
        altair_types altair_time altair_feed altair_analytics altair_models
        altair_risk altair_backtest altair_strategies
        Qt6::Core Qt6::Gui Qt6::Widgets)
    target_include_directories(altair_compute_page_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_compile_definitions(altair_compute_page_test PRIVATE
        ALTAIR_DATASET_DIR="${CMAKE_SOURCE_DIR}/dataset")
    if(TARGET altair_charges_toml)
        target_link_libraries(altair_compute_page_test PRIVATE altair_charges_toml)
        target_compile_definitions(altair_compute_page_test PRIVATE
            ALTAIR_HAVE_CHARGES_TOML=1
            ALTAIR_CHARGES_TOML="${CMAKE_SOURCE_DIR}/config/charges.toml")
    endif()
    set_target_properties(altair_compute_page_test PROPERTIES WIN32_EXECUTABLE OFF)
    add_test(NAME desktop_compute_page COMMAND altair_compute_page_test)
    set_tests_properties(desktop_compute_page PROPERTIES TIMEOUT 30 ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORM_PLUGIN_PATH=${Qt6_DIR}/../../../plugins/platforms")
    if(WIN32)
        set_tests_properties(desktop_compute_page PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()

    # P25-04. The order ticket's WRITER, checked against the same vectors
    # oms/ parses. It deliberately does NOT link altair_oms: the whole point
    # of the intent queue is that the two sides share a wire format and
    # nothing else, so a test that linked both would be checking a shared
    # header rather than the contract.
    qt_add_executable(altair_order_ticket_test
        tests/test_order_ticket.cpp
        order_ticket.hpp)
    target_link_libraries(altair_order_ticket_test PRIVATE
        altair_types altair_time Qt6::Core Qt6::Gui Qt6::Widgets)
    target_include_directories(altair_order_ticket_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_compile_definitions(altair_order_ticket_test PRIVATE
        ALTAIR_INTENT_VECTORS="${CMAKE_SOURCE_DIR}/oms/tests/vectors/intents.jsonl")
    set_target_properties(altair_order_ticket_test
        PROPERTIES WIN32_EXECUTABLE OFF)
    add_test(NAME desktop_order_ticket COMMAND altair_order_ticket_test)

    # CX02-B5. The halt request file: absent, present and UNREADABLE are three
    # states, unreadable reads as halted and never as "no halt requested", and
    # a failed write leaves the old file intact. A test target only -- nothing
    # is added to altair_desktop's links, so the gate-3 audit is unaffected.
    qt_add_executable(altair_kill_switch_test
        tests/test_kill_switch.cpp
        kill_switch.hpp)
    target_link_libraries(altair_kill_switch_test PRIVATE
        altair_types altair_time Qt6::Core Qt6::Gui Qt6::Widgets)
    target_include_directories(altair_kill_switch_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    set_target_properties(altair_kill_switch_test
        PROPERTIES WIN32_EXECUTABLE OFF)
    add_test(NAME desktop_kill_switch COMMAND altair_kill_switch_test)

    # P32-01. The terminal's WIRE. The merge itself is layout; the wire is the
    # part that was broken by construction -- the ticket's instrument list was
    # three tokens typed into a header and the watchlist held every instrument
    # in the Kite master, so anything but those three could not be requested at
    # all. A layout change could silently fail to fix that and look right.
    #
    # Counts emissions with a lambda rather than QSignalSpy: Qt6::Test is
    # not among the DLLs windeployqt puts beside these binaries, and linking a
    # component nobody deploys fails at RUN time with 0xC0000135 and no
    # message naming it. Needs a real QApplication rather than a
    # QCoreApplication, because QTableWidget is a widget.
    # P36-01. The Model Atlas page. The CMake audit checks every path in the
    # table; this checks the PAGE -- that the tree shows every row, that the
    # search matches across all four columns, and that "Hide absent" removes
    # exactly the absent rows. An off-by-one there would quietly delete a real
    # model from the reader's picture of the engine.
    # P37-04. The REAL bus against the REAL client, over a real socket.
    # Neither side mocked: the bug class this catches is the one where both
    # ends are self-consistently wrong. Needs Boost for the bus and Qt Network
    # for the client.
    find_package(Boost CONFIG QUIET COMPONENTS headers)
    if(Boost_FOUND)
        qt_add_executable(altair_price_client_test
            tests/test_price_client.cpp
            price_client.hpp)
        target_link_libraries(altair_price_client_test PRIVATE
            altair_types altair_time altair_server Boost::headers
            Qt6::Core Qt6::Network)
        target_include_directories(altair_price_client_test
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
        set_target_properties(altair_price_client_test
            PROPERTIES WIN32_EXECUTABLE OFF)
        if(WIN32)
            target_link_libraries(altair_price_client_test
                PRIVATE ws2_32 mswsock)
        endif()
        add_test(NAME desktop_price_client COMMAND altair_price_client_test)

        # The live market watch over the real bus and client: watch rows,
        # depth, time and sales, the live chain's market IV, SIM labelling.
        qt_add_executable(altair_live_market_test
            tests/test_live_market.cpp
            live_market.hpp
            price_client.hpp
            theme.hpp)
        target_link_libraries(altair_live_market_test PRIVATE
            altair_types altair_time altair_server altair_analytics altair_flags Boost::headers
            Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network)
        target_include_directories(altair_live_market_test
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
        set_target_properties(altair_live_market_test
            PROPERTIES WIN32_EXECUTABLE OFF)
        if(WIN32)
            target_link_libraries(altair_live_market_test
                PRIVATE ws2_32 mswsock)
        endif()
        add_test(NAME desktop_live_market COMMAND altair_live_market_test)

        # The paper order book behind + / − and its windows, and the market
        # watch's scrip add/remove.
        qt_add_executable(altair_paper_oms_test
            tests/test_paper_oms.cpp
            paper_oms.hpp
            paper_windows.hpp
            live_market.hpp
            price_client.hpp
            theme.hpp)
        target_link_libraries(altair_paper_oms_test PRIVATE
            altair_types altair_time altair_server altair_analytics altair_flags Boost::headers
            Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network)
        target_include_directories(altair_paper_oms_test
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
        set_target_properties(altair_paper_oms_test
            PROPERTIES WIN32_EXECUTABLE OFF)
        if(WIN32)
            target_link_libraries(altair_paper_oms_test
                PRIVATE ws2_32 mswsock)
        endif()
        add_test(NAME desktop_paper_oms COMMAND altair_paper_oms_test)
        set_tests_properties(desktop_paper_oms PROPERTIES TIMEOUT 60 ENVIRONMENT
            "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORM_PLUGIN_PATH=${Qt6_DIR}/../../../plugins/platforms")
        if(WIN32)
            set_tests_properties(desktop_paper_oms PROPERTIES ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
        endif()

        # The live models panel: engine state and paper trades from files,
        # positions marked tick by tick from the real bus and client.
        qt_add_executable(altair_live_models_test
            tests/test_live_models.cpp
            live_models.hpp
            live_market.hpp
            price_client.hpp
            theme.hpp)
        target_link_libraries(altair_live_models_test PRIVATE
            altair_types altair_time altair_server altair_analytics altair_flags Boost::headers
            Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network)
        target_include_directories(altair_live_models_test
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
        set_target_properties(altair_live_models_test
            PROPERTIES WIN32_EXECUTABLE OFF)
        if(WIN32)
            target_link_libraries(altair_live_models_test
                PRIVATE ws2_32 mswsock)
        endif()
        add_test(NAME desktop_live_models COMMAND altair_live_models_test)
        set_tests_properties(desktop_live_models PROPERTIES TIMEOUT 60 ENVIRONMENT
            "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORM_PLUGIN_PATH=${Qt6_DIR}/../../../plugins/platforms")
        if(WIN32)
            set_tests_properties(desktop_live_models PROPERTIES ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
        endif()
        set_tests_properties(desktop_live_market PROPERTIES TIMEOUT 60 ENVIRONMENT
            "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORM_PLUGIN_PATH=${Qt6_DIR}/../../../plugins/platforms")
        if(WIN32)
            set_tests_properties(desktop_live_market PROPERTIES ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
        endif()
    endif()

    qt_add_executable(altair_atlas_test
        tests/test_atlas.cpp
        atlas.hpp
        atlas_data.hpp)
    target_link_libraries(altair_atlas_test PRIVATE
        altair_types Qt6::Core Qt6::Gui Qt6::Widgets)
    target_include_directories(altair_atlas_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    set_target_properties(altair_atlas_test PROPERTIES WIN32_EXECUTABLE OFF)
    add_test(NAME desktop_atlas COMMAND altair_atlas_test)

    qt_add_executable(altair_pair_trading_test
        tests/test_pair_trading.cpp
        chain_panel.hpp
        data/master_lookup.hpp)
    target_link_libraries(altair_pair_trading_test PRIVATE
        altair_flags altair_types altair_time altair_instruments
        Qt6::Core Qt6::Gui Qt6::Widgets)
    target_include_directories(altair_pair_trading_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_compile_definitions(altair_pair_trading_test PRIVATE
        ALTAIR_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    set_target_properties(altair_pair_trading_test PROPERTIES WIN32_EXECUTABLE OFF)
    add_test(NAME desktop_pair_trading COMMAND altair_pair_trading_test)

    add_executable(altair_format_test tests/test_format.cpp)
    target_link_libraries(altair_format_test PRIVATE altair_flags Qt6::Core)
    target_include_directories(altair_format_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    set_target_properties(altair_format_test PROPERTIES WIN32_EXECUTABLE OFF)
    add_test(NAME desktop_format COMMAND altair_format_test)
    if(WIN32)
        set_tests_properties(desktop_format PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()

    qt_add_executable(altair_position_table_test
        tests/test_position_table.cpp
        position_table.hpp)
    target_link_libraries(altair_position_table_test PRIVATE
        altair_flags altair_types altair_time Qt6::Core Qt6::Gui)
    target_include_directories(altair_position_table_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    set_target_properties(altair_position_table_test PROPERTIES WIN32_EXECUTABLE OFF)
    add_test(NAME desktop_position_table COMMAND altair_position_table_test)
    if(WIN32)
        set_tests_properties(desktop_position_table PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()

    qt_add_executable(altair_funds_summary_test
        tests/test_funds_summary.cpp
        funds_summary.hpp)
    target_link_libraries(altair_funds_summary_test PRIVATE
        altair_flags altair_types altair_time Qt6::Core Qt6::Gui)
    target_include_directories(altair_funds_summary_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    set_target_properties(altair_funds_summary_test PROPERTIES WIN32_EXECUTABLE OFF)
    add_test(NAME desktop_funds_summary COMMAND altair_funds_summary_test)
    if(WIN32)
        set_tests_properties(desktop_funds_summary PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()

    # GETS-style Terminal: FYERS account/quote decoding, the Greek book, every
    # tab's rows and the typed snapshot the position table accepts. Fixtures
    # only; no dataset, broker or network.
    qt_add_executable(altair_gets_test
        tests/test_gets.cpp
        gets_data.hpp
        gets_tables.hpp
        gets_workspace.hpp
        funds_summary.hpp
        position_table.hpp)
    target_link_libraries(altair_gets_test PRIVATE
        altair_flags altair_types altair_time altair_instruments altair_analytics
        altair_risk Qt6::Core Qt6::Gui Qt6::Widgets)
    target_include_directories(altair_gets_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    set_target_properties(altair_gets_test PROPERTIES WIN32_EXECUTABLE OFF)
    add_test(NAME desktop_gets COMMAND altair_gets_test)
    if(WIN32)
        set_tests_properties(desktop_gets PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()

    # The Demo Trading page reads the band-fade and pairs CSVs: fixtures only,
    # no CLI is run.
    qt_add_executable(altair_demo_trading_test
        tests/test_demo_trading.cpp
        demo_trading_page.hpp
        helper_process.hpp
        theme.hpp)
    target_link_libraries(altair_demo_trading_test PRIVATE
        altair_flags Qt6::Core Qt6::Gui Qt6::Widgets)
    target_include_directories(altair_demo_trading_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    set_target_properties(altair_demo_trading_test PROPERTIES WIN32_EXECUTABLE OFF)
    add_test(NAME desktop_demo_trading COMMAND altair_demo_trading_test)
    set_tests_properties(desktop_demo_trading PROPERTIES TIMEOUT 60 ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORM_PLUGIN_PATH=${Qt6_DIR}/../../../plugins/platforms")
    if(WIN32)
        set_tests_properties(desktop_demo_trading PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()

    qt_add_executable(altair_arbitrage_workspace_test
        tests/test_arbitrage_workspace.cpp
        arbitrage_workspace.hpp)
    target_link_libraries(altair_arbitrage_workspace_test PRIVATE
        altair_flags altair_types altair_time Qt6::Core Qt6::Gui Qt6::Widgets)
    target_include_directories(altair_arbitrage_workspace_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    set_target_properties(altair_arbitrage_workspace_test PROPERTIES WIN32_EXECUTABLE OFF)
    add_test(NAME desktop_arbitrage_workspace COMMAND altair_arbitrage_workspace_test)
    if(WIN32)
        set_tests_properties(desktop_arbitrage_workspace PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()

    qt_add_executable(altair_terminal_test
        tests/test_terminal.cpp
        terminal.hpp
        panels.hpp
        order_ticket.hpp
        kill_switch.hpp
        # P32-07. live_feed.hpp declares a Q_OBJECT and terminal.hpp includes
        # it, so it has to be in THIS target's source list too -- AUTOMOC only
        # processes files a target names, and leaving it out links against a
        # moc generated for a different target and fails with three unresolved
        # vtable symbols rather than anything that says "add the header".
        live_feed.hpp
        watchlist.hpp
        # P39. Same reason, twice more: both declare a Q_OBJECT and terminal.hpp
        # now includes them.
        option_chain.hpp
        price_client.hpp)
    target_link_libraries(altair_terminal_test PRIVATE
        altair_types altair_time altair_feed altair_instruments
        altair_analytics altair_server
        Qt6::Core Qt6::Gui Qt6::Network Qt6::Widgets)
    target_include_directories(altair_terminal_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_compile_definitions(altair_terminal_test PRIVATE
        ALTAIR_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        ALTAIR_DATASET_DIR="${CMAKE_SOURCE_DIR}/dataset"
        ALTAIR_INTENT_FILE="${CMAKE_SOURCE_DIR}/data/order_intents.jsonl"
        ALTAIR_KILL_REQUEST_FILE="${CMAKE_SOURCE_DIR}/data/kill_request.json"
        ALTAIR_QUOTE_FILE="${CMAKE_SOURCE_DIR}/data/kite_quotes.json"
        ALTAIR_TICKS_FILE="${CMAKE_SOURCE_DIR}/data/kite_ticks.json"
        ALTAIR_DEV_CREDENTIALS)
    set_target_properties(altair_terminal_test PROPERTIES WIN32_EXECUTABLE OFF)
    add_test(NAME desktop_terminal COMMAND altair_terminal_test)
    # Check [5] prices the option chain from the instrument master and
    # dataset/ (neither in git): CI runs `ctest -LE dataset`.
    set_tests_properties(desktop_terminal PROPERTIES LABELS dataset)

    # P26-01. The Link Kite panel's two pure functions: token redaction and
    # pulling the login URL out of the subprocess's usage text. The QProcess
    # half is not tested here -- it launches a binary only the `net` preset
    # builds, and a test that skipped itself in the default preset would pass
    # by not running.
    qt_add_executable(altair_kite_link_test
        tests/test_kite_link.cpp
        kite_link.hpp
        login_outcome.hpp
        combined_account.hpp)
    target_link_libraries(altair_kite_link_test PRIVATE
        altair_types altair_time Qt6::Core Qt6::Gui Qt6::Widgets)
    target_include_directories(altair_kite_link_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_compile_definitions(altair_kite_link_test PRIVATE
        ALTAIR_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    set_target_properties(altair_kite_link_test PROPERTIES WIN32_EXECUTABLE OFF)
    add_test(NAME desktop_kite_link COMMAND altair_kite_link_test)
    set_tests_properties(desktop_kite_link PROPERTIES TIMEOUT 60 ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORM_PLUGIN_PATH=${Qt6_DIR}/../../../plugins/platforms")
    if(WIN32)
        set_tests_properties(desktop_kite_link PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()

    qt_add_executable(altair_bar_csv_test tests/test_bar_csv.cpp)
    # Qt6::Gui because market_clock.hpp uses QColor for the phase indicator.
    target_link_libraries(altair_bar_csv_test PRIVATE
        altair_types altair_time altair_feed Qt6::Core Qt6::Gui)
    target_include_directories(altair_bar_csv_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_compile_definitions(altair_bar_csv_test PRIVATE
        ALTAIR_DATASET_DIR="${CMAKE_SOURCE_DIR}/dataset")
    set_target_properties(altair_bar_csv_test PROPERTIES WIN32_EXECUTABLE OFF)
    add_test(NAME desktop_bar_csv COMMAND altair_bar_csv_test)
    # Reads dataset/ (not in git): CI runs `ctest -LE dataset`.
    set_tests_properties(desktop_bar_csv PROPERTIES LABELS dataset)

    # P11Q-07. Fits the two models the available data can fit, on that data.
    qt_add_executable(altair_fits_test tests/test_fits.cpp)
    target_link_libraries(altair_fits_test PRIVATE
        altair_types altair_time altair_feed altair_models altair_strategies
        Qt6::Core Qt6::Gui)
    target_include_directories(altair_fits_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_compile_definitions(altair_fits_test PRIVATE
        ALTAIR_DATASET_DIR="${CMAKE_SOURCE_DIR}/dataset")
    set_target_properties(altair_fits_test PROPERTIES WIN32_EXECUTABLE OFF)
    add_test(NAME desktop_fits COMMAND altair_fits_test)
    # Reads dataset/ (not in git): CI runs `ctest -LE dataset`.
    set_tests_properties(desktop_fits PROPERTIES LABELS dataset)
    # THE QT BIN DIR IS PREPENDED AT TEST TIME, NOT FROZEN AT CONFIGURE TIME.
    #
    # These three used `ENVIRONMENT "PATH=${ALTAIR_QT_ROOT}/bin;$ENV{PATH}"`,
    # and `$ENV{PATH}` in a CMake file is expanded when CMake CONFIGURES. So
    # each test ran with a PATH snapshotted from whatever shell last ran
    # cmake -- which REPLACED the live PATH ctest was given rather than adding
    # to it.
    #
    # Under the asan preset that was fatal and invisible. build.bat runs
    # vcvars, which puts the compiler's directory on PATH, and that directory
    # is where clang_rt.asan_dynamic-x86_64.dll lives. Every other test
    # inherited it and passed; these three threw it away with their frozen
    # PATH and could not load. Windows then raised a hard-error dialog owned by
    # csrss.exe -- not by the test, so it had no window to find -- and the
    # process sat blocked on an invisible box until ctest killed it at 600 s.
    #
    # That is why the asan run reported exactly these three as "failures", and
    # why a direct run from a shell passed them in under twelve seconds. It
    # was never a memory bug.
    #
    # ENVIRONMENT_MODIFICATION (CMake 3.22) edits the PATH ctest actually
    # hands the test, so the live PATH survives.
    if(WIN32)
        set_tests_properties(desktop_fits desktop_bar_csv desktop_filter
            PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:${ALTAIR_QT_ROOT}/bin")
    endif()
endif()

# macOS: the bundle is Altair.app, because app/ installs every helper into
# Altair.app/Contents/Helpers and the desktop finds them at ../Helpers from
# Contents/MacOS. Named after the target it came out as altair_desktop.app and
# the helpers landed in a second, empty bundle (P1-04, first macOS CI run).
if(APPLE)
    set_target_properties(altair_desktop PROPERTIES OUTPUT_NAME "Altair")
endif()

set_target_properties(altair_desktop PROPERTIES
    WIN32_EXECUTABLE ON      # no console window behind the UI
    MACOSX_BUNDLE ON
    MACOSX_BUNDLE_BUNDLE_NAME "Altair"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.thesmitshah.altair"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
)

install(TARGETS altair_desktop
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin)

qt_generate_deploy_app_script(
    TARGET altair_desktop
    OUTPUT_SCRIPT altair_deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR)
install(SCRIPT "${altair_deploy_script}")

# Copy the Qt DLLs next to the exe so it runs from the build tree without a
# PATH dance. Debug and release Qt DLLs differ; windeployqt picks the right set.
if(WIN32)
    find_program(ALTAIR_WINDEPLOYQT windeployqt HINTS ${Qt6_DIR}/../../../bin)
    if(ALTAIR_WINDEPLOYQT)
        add_custom_command(TARGET altair_desktop POST_BUILD
            COMMAND "${ALTAIR_WINDEPLOYQT}"
                    --no-translations --no-system-d3d-compiler --no-opengl-sw
                    "$<TARGET_FILE:altair_desktop>"
            COMMENT "windeployqt: staging Qt runtime next to altair_desktop"
            VERBATIM)
    endif()
endif()
