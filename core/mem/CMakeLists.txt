# core/mem — OS pages (P0-05a), arena + pool allocators (P0-05b).
# STATIC because page_alloc.cpp carries the platform code. The include root is
# exported so consumers write `#include <mem/...>`.

add_library(altair_mem STATIC
    page_alloc.cpp
)
add_library(altair::mem ALIAS altair_mem)

target_include_directories(altair_mem PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(altair_mem PUBLIC altair_types altair_flags)

if(ALTAIR_BUILD_TESTS)
    add_executable(altair_page_alloc_test tests/test_page_alloc.cpp)
    target_link_libraries(altair_page_alloc_test PRIVATE altair_mem altair_flags)
    add_test(NAME page_alloc COMMAND altair_page_alloc_test)

    add_executable(altair_arena_pool_test tests/test_arena_pool.cpp)
    target_link_libraries(altair_arena_pool_test PRIVATE altair_mem altair_flags)
    add_test(NAME arena_pool COMMAND altair_arena_pool_test)
endif()
