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

find_package(Qt6 6.8 QUIET COMPONENTS Core Gui Svg Widgets)

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
    assets/altair.qrc
    main_window.hpp
    tick_model.hpp
    filter.hpp
    market_clock.hpp
    format.hpp
    auth.hpp
    feed_status.hpp
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
    live_feed.hpp
    live_forecast.hpp
    kite_link.hpp
    chain_panel.hpp
    analytics_panel.hpp
)

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
    altair_feed
    altair_analytics
    altair_models
    altair_risk
    altair_backtest
    Qt6::Core
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
    ALTAIR_ACCOUNT_FILE="${CMAKE_SOURCE_DIR}/data/kite_account.json"
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
if(ALTAIR_BUILD_TESTS)
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
        watchlist.hpp)
    target_link_libraries(altair_terminal_test PRIVATE
        altair_types altair_time altair_feed altair_instruments
        Qt6::Core Qt6::Gui Qt6::Widgets)
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

    # P26-01. The Link Kite panel's two pure functions: token redaction and
    # pulling the login URL out of the subprocess's usage text. The QProcess
    # half is not tested here -- it launches a binary only the `net` preset
    # builds, and a test that skipped itself in the default preset would pass
    # by not running.
    qt_add_executable(altair_kite_link_test
        tests/test_kite_link.cpp
        kite_link.hpp)
    target_link_libraries(altair_kite_link_test PRIVATE
        altair_types altair_time Qt6::Core Qt6::Gui Qt6::Widgets)
    target_include_directories(altair_kite_link_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_compile_definitions(altair_kite_link_test PRIVATE
        ALTAIR_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    set_target_properties(altair_kite_link_test PROPERTIES WIN32_EXECUTABLE OFF)
    add_test(NAME desktop_kite_link COMMAND altair_kite_link_test)

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
    if(WIN32)
        set_tests_properties(desktop_fits PROPERTIES
            ENVIRONMENT "PATH=${ALTAIR_QT_ROOT}/bin;$ENV{PATH}")
    endif()
    if(WIN32)
        set_tests_properties(desktop_bar_csv PROPERTIES
            ENVIRONMENT "PATH=${ALTAIR_QT_ROOT}/bin;$ENV{PATH}")
    endif()
    if(WIN32)
        # The test needs the Qt DLLs on PATH; point ctest at the Qt bin dir
        # rather than copying a second set next to the test binary.
        set_tests_properties(desktop_filter PROPERTIES
            ENVIRONMENT "PATH=${ALTAIR_QT_ROOT}/bin;$ENV{PATH}")
    endif()
endif()

set_target_properties(altair_desktop PROPERTIES
    WIN32_EXECUTABLE ON      # no console window behind the UI
    MACOSX_BUNDLE ON
)

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
