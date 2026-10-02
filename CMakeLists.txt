cmake_minimum_required(VERSION 3.28)

project(altair
    VERSION 0.1.0
    DESCRIPTION "Tick-to-tick multi-strategy trading engine for NSE & BSE"
    LANGUAGES CXX
)

# ─────────────────────────────────────────────────────────────────────────────
# Standard & global settings
# ─────────────────────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# ─────────────────────────────────────────────────────────────────────────────
# Options
# ─────────────────────────────────────────────────────────────────────────────
option(ALTAIR_BUILD_TESTS      "Build unit tests"            ON)
option(ALTAIR_BUILD_BENCH      "Build latency benchmarks"    ON)
option(ALTAIR_ENABLE_ASAN      "Address + UB sanitizer"      OFF)
option(ALTAIR_ENABLE_TSAN      "Thread sanitizer"            OFF)
option(ALTAIR_ENABLE_TORCH     "Build the ML tier (LibTorch)" OFF)
option(ALTAIR_NATIVE_ARCH      "Compile for the host CPU"    ON)

# Qt for desktop/. Not a hard dependency: if this is unset and Qt is not on
# CMAKE_PREFIX_PATH, desktop/ skips itself and the engine still builds. The
# default is where aqtinstall put it on this box; it is off C: because C: had
# 7.8 GB free against a 230 GB disk.
if(WIN32)
    set(_altair_qt_default "D:/Qt/6.8.3/msvc2022_64")
else()
    set(_altair_qt_default "")
endif()
set(ALTAIR_QT_ROOT "${_altair_qt_default}" CACHE PATH "Qt 6 install prefix")
unset(_altair_qt_default)
if(EXISTS "${ALTAIR_QT_ROOT}")
    list(APPEND CMAKE_PREFIX_PATH "${ALTAIR_QT_ROOT}")
endif()
option(ALTAIR_STRICT_INVARIANTS "Check conservation laws every tick" ON)

# ─────────────────────────────────────────────────────────────────────────────
# Warning / optimisation interface target — every Altair target links this
# ─────────────────────────────────────────────────────────────────────────────
add_library(altair_flags INTERFACE)

if(MSVC)
    target_compile_options(altair_flags INTERFACE
        /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /EHsc /utf-8
        /bigobj /wd4324           # 4324: structure padded due to alignas — intentional
        $<$<CONFIG:RelWithDebInfo>:/O2 /Oi /Ot /GS- /Zi>
        $<$<CONFIG:Release>:/O2 /Oi /Ot /GS->
    )
    if(ALTAIR_NATIVE_ARCH)
        target_compile_options(altair_flags INTERFACE /arch:AVX2)
    endif()
else()
    target_compile_options(altair_flags INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
        -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Wunused
        -Woverloaded-virtual -Wnull-dereference -Wdouble-promotion
        -Wformat=2 -Wimplicit-fallthrough
        $<$<CONFIG:RelWithDebInfo>:-O2 -g -fno-omit-frame-pointer>
        $<$<CONFIG:Release>:-O3 -fno-omit-frame-pointer>
    )
    if(ALTAIR_NATIVE_ARCH)
        target_compile_options(altair_flags INTERFACE -march=native -mtune=native)
    endif()
endif()

if(ALTAIR_STRICT_INVARIANTS)
    target_compile_definitions(altair_flags INTERFACE ALTAIR_STRICT_INVARIANTS=1)
else()
    # Define the OFF state explicitly so conservation.hpp can reject an
    # optimised build that would otherwise accidentally omit this safety flag.
    target_compile_definitions(altair_flags INTERFACE ALTAIR_STRICT_INVARIANTS=0)
endif()

# ─────────────────────────────────────────────────────────────────────────────
# Sanitizers
#
# BOTH OF THESE USED TO BE GUARDED ON `NOT MSVC`, AND THIS IS AN MSVC BOX.
#
# The `asan` and `tsan` presets therefore configured, built, and ran their whole
# suite with NO INSTRUMENTATION AT ALL -- a plain Debug build wearing a
# sanitizer's name. Every "asan green" and "tsan green" this project has
# recorded on Windows was a statement about a build that had never been
# sanitised, and Phase 0's gate log has said since the beginning that the
# lock-free memory orderings are "unvalidated ... the single largest
# correctness risk in the phase". They were even less validated than that
# admitted: the preset that appeared to check them did not exist.
#
# A silently-degraded check is worse than an absent one. It answers the
# question nobody asks again.
# ─────────────────────────────────────────────────────────────────────────────

if(ALTAIR_ENABLE_ASAN)
    if(MSVC)
        # MSVC has had AddressSanitizer since VS 2019 16.9 and it works.
        # /RTC1 -- which CMake puts in the default Debug flags -- is
        # INCOMPATIBLE with it and the compiler refuses the combination, so it
        # is stripped here rather than left for whoever next selects the
        # preset to decode.
        target_compile_options(altair_flags INTERFACE /fsanitize=address)
        foreach(_cfg "" _DEBUG _RELWITHDEBINFO)
            string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS${_cfg}
                   "${CMAKE_CXX_FLAGS${_cfg}}")
            string(REPLACE "/RTC1" "" CMAKE_C_FLAGS${_cfg}
                   "${CMAKE_C_FLAGS${_cfg}}")
        endforeach()
        message(STATUS "ASAN: ON (MSVC /fsanitize=address, /RTC1 stripped)")
    else()
        target_compile_options(altair_flags INTERFACE
            -fsanitize=address,undefined -fno-sanitize-recover=all)
        target_link_options(altair_flags INTERFACE
            -fsanitize=address,undefined)
        message(STATUS "ASAN: ON (address,undefined)")
    endif()
endif()

if(ALTAIR_ENABLE_TSAN)
    if(MSVC)
        # MSVC HAS NO ThreadSanitizer. There is no flag to add and no
        # workaround; clang-cl does not ship one either.
        #
        # This REFUSES rather than warning, because a warning at configure time
        # scrolls past and the suite then passes, and a passing suite named
        # `tsan` is a claim that the orderings were checked. They were not, and
        # cannot be, on this toolchain. Run the preset under GCC or Clang --
        # which is item 3 of Phase 0's re-gate list, and is still the way to
        # discharge it.
        message(FATAL_ERROR
            "ALTAIR_ENABLE_TSAN: MSVC has no ThreadSanitizer, so this preset "
            "cannot do what its name says. It used to configure anyway and "
            "run an UNINSTRUMENTED Debug build, which reported green and meant "
            "nothing.\n"
            "Run this preset on Linux with GCC or Clang. On Windows use the "
            "`asan` preset, which IS instrumented, or `debug` for a plain "
            "build.")
    endif()
    target_compile_options(altair_flags INTERFACE -fsanitize=thread)
    target_link_options(altair_flags    INTERFACE -fsanitize=thread)
    message(STATUS "TSAN: ON")
endif()

# ─────────────────────────────────────────────────────────────────────────────
# Dependencies (vcpkg manifest mode — see vcpkg.json)
# ─────────────────────────────────────────────────────────────────────────────
find_package(Threads REQUIRED)
# Uncommented progressively as phases land. Keep in lockstep with vcpkg.json.
# find_package(Boost           CONFIG REQUIRED COMPONENTS system)
# find_package(Eigen3          CONFIG REQUIRED)
# find_package(simdjson        CONFIG REQUIRED)
# find_package(tomlplusplus    CONFIG REQUIRED)
# find_package(xsimd           CONFIG REQUIRED)
# find_package(unofficial-uwebsockets CONFIG REQUIRED)

# Catch2 / Google Benchmark arrive with card P0-03 (vcpkg feature "testing").
# Until then tests are plain main() executables, so nothing is required here.
if(ALTAIR_BUILD_TESTS)
    include(CTest)
    enable_testing()
    # find_package(Catch2 3 CONFIG REQUIRED)      # from P0-03
endif()
if(ALTAIR_BUILD_BENCH)
    # find_package(benchmark CONFIG REQUIRED)     # from P0-03
endif()

# ─────────────────────────────────────────────────────────────────────────────
# One flat namespace, one declaration per name. P33-02.
#
# `namespace altair` holds everything, so two directories independently
# choosing the same obvious name is an ODR violation that only surfaces when
# somebody includes both headers -- which in this tree has meant months later,
# as a wall of errors pointing at the wrong file. Four times now. This fails
# CONFIGURE instead.
# ─────────────────────────────────────────────────────────────────────────────
include(cmake/NamespaceAudit.cmake)
altair_audit_namespace()

# ─────────────────────────────────────────────────────────────────────────────
# Hard rule 11: a fixed bound must refuse what exceeds it. P33-06.
#
# The rule was written after the same bug was found five times and enforced by
# review -- which is what had failed five times. This finds the SHAPE (a
# clamp) and requires that somebody wrote down which arm of the rule it takes.
# It cannot decide whether a bound is safe; it can decide whether anyone said
# so. See the header of the module for what it deliberately does not catch.
# ─────────────────────────────────────────────────────────────────────────────
include(cmake/BoundsAudit.cmake)
altair_audit_bounds()

# ─────────────────────────────────────────────────────────────────────────────
# P36-02. THE MODEL ATLAS MAY NOT DESCRIBE A TREE THAT HAS MOVED.
#
# desktop/atlas_data.hpp names the header every model lives in, and those paths
# are the page's whole value -- they are what turns a definition into something
# a reader can go and open. A renamed file turns the sentence into a lie that
# nothing else in the build would catch, because a table of strings compiles
# whatever the strings say.
# ─────────────────────────────────────────────────────────────────────────────
include(cmake/AtlasAudit.cmake)

# ─────────────────────────────────────────────────────────────────────────────
# Subdirectories — uncommented as each phase's cards land.
# The order below mirrors ROADMAP.md §12.
# ─────────────────────────────────────────────────────────────────────────────
add_subdirectory(core)              # Phase 0

add_subdirectory(instruments)       # Phase 1 — ContractSpec + spec store
add_subdirectory(broker)            # Phase 2 - the ONLY dir touching a credential
add_subdirectory(feed)              # Phase 0 replay skeleton; decoders in Phase 2
add_subdirectory(book)              # Phase 2 — L2 book skeleton (P2-08)
add_subdirectory(analytics)         # Phase 3 - greeks, IV, stats
add_subdirectory(risk)              # Phase 3 - cost calculator (P3-09)
add_subdirectory(oms)               # Phase 4 - THE TRADE HANDLER
add_subdirectory(features)          # Phase 5 - feature registry + builders
add_subdirectory(strategies)        # Phase 5 — arbitrage scanners (detect only)
add_subdirectory(backtest)          # Phase 6 — replayer, walk-forward, Monte Carlo
add_subdirectory(models)            # Phase 8 — labels, training, the model zoo
add_subdirectory(flagging)          # Phase 9 — scorecards, drift, auto-correction
add_subdirectory(server)            # Phase 11 — BACKEND ONLY, renders nothing
add_subdirectory(live)              # live terminal: universe, bars, live models, paper book
add_subdirectory(desktop)           # Phase 11Q — the Qt UI, in-process

# client/ is RETIRED and deliberately not built — see client/README.md
add_subdirectory(research)          # Phase 7 — paper pipeline (decides, does not trade)
add_subdirectory(app)               # main() — the `altair` binary

if(ALTAIR_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# Installable artifacts. macOS produces Altair.app plus credential-boundary
# helpers under Contents/Helpers; signing/notarisation remains an explicit
# release operation because no certificate is stored in this repository.
set(CPACK_PACKAGE_NAME "Altair")
set(CPACK_PACKAGE_VENDOR "Altair")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "Altair-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
if(APPLE)
    set(CPACK_GENERATOR "DragNDrop")
elseif(WIN32)
    set(CPACK_GENERATOR "ZIP")
else()
    set(CPACK_GENERATOR "TGZ")
endif()
include(CPack)

# ─────────────────────────────────────────────────────────────────────────────
# ASAN runtime next to every executable. AFTER every add_subdirectory, because
# it walks the targets they declared -- called earlier it would see none and
# stage nothing, which is the silent failure it exists to prevent.
# ─────────────────────────────────────────────────────────────────────────────
include(cmake/AsanRuntime.cmake)
altair_stage_asan_runtime()

# ─────────────────────────────────────────────────────────────────────────────
# Summary
# ─────────────────────────────────────────────────────────────────────────────
message(STATUS "──────────────────────────────────────────────")
message(STATUS " Altair ${PROJECT_VERSION}")
message(STATUS "   build type   : ${CMAKE_BUILD_TYPE}")
message(STATUS "   compiler     : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "   tests        : ${ALTAIR_BUILD_TESTS}")
message(STATUS "   benchmarks   : ${ALTAIR_BUILD_BENCH}")
message(STATUS "   ML tier      : ${ALTAIR_ENABLE_TORCH}")
message(STATUS "   invariants   : ${ALTAIR_STRICT_INVARIANTS}")
message(STATUS "──────────────────────────────────────────────")
