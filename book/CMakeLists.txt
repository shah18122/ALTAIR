# book/ - the L2 order book and microstructure (P2-08, P2-09).
# P2-08 is implemented; P2-09's estimators land next and may need a .cpp.

add_library(altair_book INTERFACE)
add_library(altair::book ALIAS altair_book)

target_include_directories(altair_book INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(altair_book INTERFACE
    altair_types
    altair_time
    altair_feed
    altair_instruments
    altair_flags
)

if(ALTAIR_BUILD_TESTS)
    add_executable(altair_l2_book_test tests/test_l2_book.cpp)
    target_link_libraries(altair_l2_book_test PRIVATE altair_book altair_flags)
    add_test(NAME l2_book COMMAND altair_l2_book_test)
endif()
