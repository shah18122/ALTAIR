# features/ - the versioned, horizon-banded feature registry and its builders.
#
# This is the modelling side of the tree. Nothing here places an order or
# knows a broker exists: features are computed from ticks and books and handed
# to models/, and the trade handler in oms/ decides what to do with the answer.

add_library(altair_features INTERFACE)
add_library(altair::features ALIAS altair_features)

target_include_directories(altair_features INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(altair_features INTERFACE
    altair_types
    altair_time
    altair_analytics
    altair_book
    altair_flags
)

if(ALTAIR_BUILD_TESTS)
    add_executable(altair_registry_test tests/test_registry.cpp)
    target_link_libraries(altair_registry_test PRIVATE altair_features altair_flags)
    add_test(NAME feature_registry COMMAND altair_registry_test)

    add_executable(altair_kinematics_test tests/test_kinematics.cpp)
    target_link_libraries(altair_kinematics_test PRIVATE altair_features altair_flags)
    add_test(NAME feature_kinematics COMMAND altair_kinematics_test)

    add_executable(altair_book_flow_test tests/test_book_flow.cpp)
    target_link_libraries(altair_book_flow_test PRIVATE altair_features altair_flags)
    add_test(NAME feature_book_flow COMMAND altair_book_flow_test)

    add_executable(altair_options_calendar_test tests/test_options_calendar.cpp)
    target_link_libraries(altair_options_calendar_test PRIVATE altair_features altair_flags)
    add_test(NAME feature_options_calendar COMMAND altair_options_calendar_test)
endif()
