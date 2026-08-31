# core/config - versioned, hashed, hot-swappable config store (P0-08a) and
# the TOML loader (P0-08b).
#
# altair_config stays header-only and DEPENDENCY-FREE so a strategy that reads
# config does not drag a TOML parser into the hot path. altair_config_toml is a
# separate compiled target that links tomlplusplus, built only when vcpkg has
# provided it.

add_library(altair_config INTERFACE)
add_library(altair::config ALIAS altair_config)

target_include_directories(altair_config INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(altair_config INTERFACE
    altair_types
    altair_lockfree
    altair_flags
)

# P0-08b. Optional: the tree still builds with no vcpkg at all.
find_package(tomlplusplus CONFIG QUIET)
if(tomlplusplus_FOUND)
    add_library(altair_config_toml STATIC toml_source.cpp)
    add_library(altair::config_toml ALIAS altair_config_toml)
    target_link_libraries(altair_config_toml
        PUBLIC altair_config
        PRIVATE tomlplusplus::tomlplusplus altair_flags)
    message(STATUS "altair: tomlplusplus found - P0-08b TOML loader enabled")
else()
    message(STATUS "altair: tomlplusplus NOT found - P0-08b skipped "
                   "(configure with the `vcpkg` preset to enable it)")
endif()

if(ALTAIR_BUILD_TESTS)
    add_executable(altair_config_test tests/test_config.cpp)
    target_link_libraries(altair_config_test
        PRIVATE altair_config altair_flags Threads::Threads)
    add_test(NAME config COMMAND altair_config_test)

    if(tomlplusplus_FOUND)
        add_executable(altair_toml_source_test tests/test_toml_source.cpp)
        target_link_libraries(altair_toml_source_test
            PRIVATE altair_config_toml altair_flags)
        # Runs from the source root so it can find config/altair.toml.
        add_test(NAME toml_source COMMAND altair_toml_source_test
                 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    endif()
endif()
