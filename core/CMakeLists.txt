# core/ — Phase 0 foundation.
#
# Each subdirectory is added only once its task card has landed, so the tree
# always configures cleanly. Remove the EXISTS guard once all six are in.
#
#   P0-01  types, time
#   P0-02  mem
#   P0-03  lockfree
#   P0-04  log
#   P0-05  config
#   P0-06  invariant

set(ALTAIR_CORE_MODULES
    types       # P0-01  DONE
    time        # P0-02 timestamp · P0-03 tsc_clock · P0-04 exchange_ts
    mem         # P0-05
    lockfree    # P0-06
    log         # P0-07
    config      # P0-08
    invariant   # P0-09
)

set(ALTAIR_CORE_PRESENT "")
foreach(mod IN LISTS ALTAIR_CORE_MODULES)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${mod}/CMakeLists.txt")
        add_subdirectory(${mod})
        list(APPEND ALTAIR_CORE_PRESENT ${mod})
    endif()
endforeach()

message(STATUS "   core modules : ${ALTAIR_CORE_PRESENT}")

# Umbrella target — link this to get all of core.
add_library(altair_core INTERFACE)
add_library(altair::core ALIAS altair_core)
foreach(mod IN LISTS ALTAIR_CORE_PRESENT)
    target_link_libraries(altair_core INTERFACE altair_${mod})
endforeach()
target_link_libraries(altair_core INTERFACE altair_flags Threads::Threads)
