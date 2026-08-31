# broker/ -- broker session and authentication.
#
# The ONLY directory that touches a credential. feed/ needs an access token to
# open a socket and oms/ needs one to place an order, so the session cannot
# live in either without the other including it -- and a client that can
# include an OMS header is a client that can be made to trade.
#
# Header-only. The HTTP half (P2-10c) lands when vcpkg unblocks.

add_library(altair_broker INTERFACE)
add_library(altair::broker ALIAS altair_broker)

target_include_directories(altair_broker INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(altair_broker INTERFACE
    altair_types
    altair_time
    altair_flags
)

if(ALTAIR_BUILD_TESTS)
    add_executable(altair_kite_auth_test tests/test_kite_auth.cpp)
    target_link_libraries(altair_kite_auth_test PRIVATE altair_broker altair_flags)
    add_test(NAME kite_auth COMMAND altair_kite_auth_test)
endif()
