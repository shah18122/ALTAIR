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
endif()

if(ALTAIR_ENABLE_ASAN AND NOT MSVC)
    target_compile_options(altair_flags INTERFACE -fsanitize=address,undefined -fno-sanitize-recover=all)
    target_link_options(altair_flags    INTERFACE -fsanitize=address,undefined)
endif()
if(ALTAIR_ENABLE_TSAN AND NOT MSVC)
    target_compile_options(altair_flags INTERFACE -fsanitize=thread)
    target_link_options(altair_flags    INTERFACE -fsanitize=thread)
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
# add_subdirectory(models)          # Phase 8
# add_subdirectory(flagging)        # Phase 9
# add_subdirectory(server)          # Phase 11 — BACKEND ONLY, renders nothing
# add_subdirectory(client)          # Phase 11 — DESKTOP ONLY, links no engine header
add_subdirectory(research)          # Phase 7 — paper pipeline (decides, does not trade)
add_subdirectory(app)               # main() — the `altair` binary

if(ALTAIR_BUILD_TESTS)
    add_subdirectory(tests)
endif()

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
