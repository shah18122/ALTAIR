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

    find_package(OpenSSL)
    if(OpenSSL_FOUND)
        add_executable(altair_websocket_test tests/test_websocket.cpp)
        target_link_libraries(altair_websocket_test PRIVATE altair_net OpenSSL::Crypto altair_flags)
        add_test(NAME websocket COMMAND altair_websocket_test)
    endif()
endif()

# TLS over ALTAIR's own TCP (OpenSSL on memory BIOs), and the kept-alive HTTPS
# order line on it (net/bypass_https.hpp). Only OpenSSL is needed, so both are
# built and tested on every machine; the links they run on are below.
find_package(OpenSSL)
if(OpenSSL_FOUND)
    add_library(altair_tls_stream STATIC tls_stream.cpp)
    target_link_libraries(altair_tls_stream PUBLIC altair_net OpenSSL::SSL OpenSSL::Crypto PRIVATE altair_flags)
    add_library(altair_bypass_https STATIC bypass_https.cpp)
    target_link_libraries(altair_bypass_https PUBLIC altair_tls_stream PRIVATE altair_flags)
    target_compile_options(altair_bypass_https PRIVATE -Wno-conversion -Wno-sign-conversion)
    # The FYERS WebSocket on any NetLink (a DPDK port in production).
    add_library(altair_bypass_ws_link STATIC bypass_ws_link.cpp)
    target_link_libraries(altair_bypass_ws_link PUBLIC altair_bypass_https PRIVATE altair_flags)
    target_compile_options(altair_bypass_ws_link PRIVATE -Wno-conversion -Wno-sign-conversion)
    if(ALTAIR_BUILD_TESTS)
        add_executable(altair_bypass_https_test tests/test_bypass_https.cpp)
        target_link_libraries(altair_bypass_https_test PRIVATE altair_bypass_https altair_bypass_ws_link altair_flags)
        target_compile_options(altair_bypass_https_test PRIVATE -Wno-conversion -Wno-sign-conversion)
        add_test(NAME bypass_https COMMAND altair_bypass_https_test)
    endif()
endif()

# AF_XDP: -DALTAIR_KERNEL_BYPASS=ON (needs libxdp-dev, libbpf-dev, clang).
option(ALTAIR_KERNEL_BYPASS "Build the AF_XDP kernel-bypass receive path" OFF)
if(ALTAIR_KERNEL_BYPASS)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBXDP REQUIRED IMPORTED_TARGET libxdp)
    pkg_check_modules(LIBBPF REQUIRED IMPORTED_TARGET libbpf)
    find_program(ALTAIR_BPF_CLANG clang REQUIRED)

    # The XDP program: compiled for the kernel's BPF VM, loaded at run time.
    set(xdp_obj ${CMAKE_CURRENT_BINARY_DIR}/altair_flow.bpf.o)
    set(ALTAIR_XDP_OBJECT ${xdp_obj} CACHE INTERNAL "the built XDP program")
    add_custom_command(
        OUTPUT ${xdp_obj}
        COMMAND ${ALTAIR_BPF_CLANG} -O2 -g -target bpf
                -I/usr/include/${CMAKE_LIBRARY_ARCHITECTURE}
                -c ${CMAKE_CURRENT_SOURCE_DIR}/xdp/altair_flow.bpf.c -o ${xdp_obj}
        DEPENDS xdp/altair_flow.bpf.c
        COMMENT "XDP program altair_flow.bpf.o")
    add_custom_target(altair_xdp_program ALL DEPENDS ${xdp_obj})

    add_library(altair_af_xdp STATIC af_xdp.cpp)
    target_link_libraries(altair_af_xdp PUBLIC altair_net PkgConfig::LIBXDP PkgConfig::LIBBPF PRIVATE altair_flags)
    add_dependencies(altair_af_xdp altair_xdp_program)
    # libxdp's inline ring helpers trip the project's strict conversion warnings.
    target_compile_options(altair_af_xdp PRIVATE -Wno-conversion -Wno-sign-conversion)

    add_executable(altair_xdp_rx tools/xdp_rx_main.cpp)
    target_link_libraries(altair_xdp_rx PRIVATE altair_af_xdp altair_flags)
    target_compile_options(altair_xdp_rx PRIVATE -Wno-conversion -Wno-sign-conversion)
    target_compile_definitions(altair_xdp_rx PRIVATE ALTAIR_XDP_OBJECT="${xdp_obj}")

    find_package(OpenSSL REQUIRED)   # altair_tls_stream, above

    add_executable(altair_xdp_tls_client tools/xdp_tls_client_main.cpp)
    target_link_libraries(altair_xdp_tls_client PRIVATE altair_af_xdp altair_tls_stream altair_flags)
    target_compile_options(altair_xdp_tls_client PRIVATE -Wno-conversion -Wno-sign-conversion)
    target_compile_definitions(altair_xdp_tls_client PRIVATE ALTAIR_XDP_OBJECT="${xdp_obj}")

    # The whole bypass stack: AF_XDP -> TCP -> TLS -> WebSocket.
    add_library(altair_bypass_ws STATIC bypass_ws.cpp)
    target_link_libraries(altair_bypass_ws PUBLIC altair_af_xdp altair_tls_stream PRIVATE altair_flags)
    target_compile_options(altair_bypass_ws PRIVATE -Wno-conversion -Wno-sign-conversion)

    add_executable(altair_bypass_ws_probe tools/bypass_ws_main.cpp)
    set_target_properties(altair_bypass_ws_probe PROPERTIES OUTPUT_NAME altair_bypass_ws)
    target_link_libraries(altair_bypass_ws_probe PRIVATE altair_bypass_ws altair_flags)
    target_compile_definitions(altair_bypass_ws_probe PRIVATE ALTAIR_XDP_OBJECT="${xdp_obj}")
endif()

# DPDK: -DALTAIR_DPDK=ON (needs libdpdk-dev). The port for a second ENI bound
# to vfio-pci; ARP is ALTAIR's own there (net/arp.hpp).
# The linux preset turns it on; without libdpdk-dev it says so and builds without it.
option(ALTAIR_DPDK "Build the DPDK port and its TLS client" OFF)
if(ALTAIR_DPDK)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBDPDK IMPORTED_TARGET libdpdk)
    if(NOT LIBDPDK_FOUND)
        message(WARNING "ALTAIR_DPDK=ON but libdpdk was not found (sudo apt install libdpdk-dev): the DPDK order line is not built")
        set(ALTAIR_DPDK OFF CACHE BOOL "Build the DPDK port and its TLS client" FORCE)
    endif()
endif()
if(ALTAIR_DPDK)
    find_package(OpenSSL REQUIRED)
    add_library(altair_dpdk_port STATIC dpdk_port.cpp)
    target_link_libraries(altair_dpdk_port PUBLIC altair_net PkgConfig::LIBDPDK)
    target_compile_definitions(altair_dpdk_port INTERFACE ALTAIR_DPDK=1)
    # DPDK's headers use what the project's warnings forbid.
    target_compile_options(altair_dpdk_port PRIVATE -w)
    add_executable(altair_dpdk_tls_client tools/dpdk_tls_client_main.cpp)
    target_link_libraries(altair_dpdk_tls_client PRIVATE altair_dpdk_port altair_tls_stream altair_flags)
    target_compile_options(altair_dpdk_tls_client PRIVATE -Wno-conversion -Wno-sign-conversion)
endif()
