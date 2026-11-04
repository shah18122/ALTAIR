# net/ -- kernel-bypass networking for the FYERS feed (AF_XDP; DPDK later).
#
# The packet parser and TCP reassembler are header-only and always built, so
# they are tested on every machine. The AF_XDP socket needs libxdp + libbpf and
# clang for the XDP program; it is built only with -DALTAIR_KERNEL_BYPASS=ON and
# nothing in the live path uses it unless config says so.

add_library(altair_net INTERFACE)
add_library(altair::net ALIAS altair_net)
target_include_directories(altair_net INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(altair_net INTERFACE altair_flags)

if(ALTAIR_BUILD_TESTS)
    add_executable(altair_net_test tests/test_net.cpp)
    target_link_libraries(altair_net_test PRIVATE altair_net altair_flags)
    add_test(NAME net COMMAND altair_net_test)
endif()
