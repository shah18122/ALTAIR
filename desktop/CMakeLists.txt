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

find_package(Qt6 6.8 QUIET COMPONENTS Core Gui Widgets)

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
)

# READ-SIDE ONLY. See the header comment; this list is the safety property.
target_link_libraries(altair_desktop PRIVATE
    altair_types
    altair_time
    altair_feed
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)

target_include_directories(altair_desktop PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

# dataset/ is gitignored and regenerable, so its location is a build-time
# constant rather than something the UI hunts for at runtime.
target_compile_definitions(altair_desktop PRIVATE
    ALTAIR_DATASET_DIR="${CMAKE_SOURCE_DIR}/dataset")

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
# A human reading a diff can miss a line added to target_link_libraries. This
# cannot: it fails configure if an order-placing library ever appears.
get_target_property(_altair_ui_links altair_desktop LINK_LIBRARIES)
foreach(_forbidden altair_oms altair_broker altair::oms altair::broker)
    if(_forbidden IN_LIST _altair_ui_links)
        message(FATAL_ERROR
            "desktop/ links ${_forbidden}. The UI must not be able to place an "
            "order — see CLAUDE.md 'The in-process decision'. This is gate 3.")
    endif()
endforeach()

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
    add_executable(altair_desktop_chart_test tests/test_chart.cpp)
    # Qt6::Core for format.hpp's QString formatting in the axis regression.
    target_link_libraries(altair_desktop_chart_test PRIVATE
        altair_types altair_time altair_feed Qt6::Core)
    target_include_directories(altair_desktop_chart_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    add_test(NAME desktop_chart COMMAND altair_desktop_chart_test)

    # Runs against the REAL series in dataset/, not a fixture. dataset/ is
    # gitignored and regenerable, so the test skips loudly rather than failing
    # if it is absent -- see the guard in the test's own output.
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
