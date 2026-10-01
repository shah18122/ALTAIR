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
    add_executable(altair_queue_position_test tests/test_queue_position.cpp)
    target_link_libraries(altair_queue_position_test PRIVATE altair_book altair_flags)
    add_test(NAME queue_position COMMAND altair_queue_position_test)
    add_executable(altair_l2_book_test tests/test_l2_book.cpp)
    target_link_libraries(altair_l2_book_test PRIVATE altair_book altair_flags)
    add_test(NAME l2_book COMMAND altair_l2_book_test)

    add_executable(altair_microstructure_test tests/test_microstructure.cpp)
    target_link_libraries(altair_microstructure_test PRIVATE altair_book altair_flags)
    add_test(NAME microstructure COMMAND altair_microstructure_test)
    # Order flow imbalance and the depth study (book/depth_study.hpp).
    add_executable(altair_depth_study_test tests/test_depth_study.cpp)
    target_link_libraries(altair_depth_study_test PRIVATE altair_flags)
    target_include_directories(altair_depth_study_test PRIVATE ${CMAKE_SOURCE_DIR})
    add_test(NAME depth_study COMMAND altair_depth_study_test)

    add_executable(altair_flow_test tests/test_flow.cpp)
    target_link_libraries(altair_flow_test PRIVATE altair_book altair_flags)
    add_test(NAME flow COMMAND altair_flow_test)
endif()
