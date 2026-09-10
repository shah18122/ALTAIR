# server/ - BACKEND ONLY. uWebSockets, the binary delta-frame protocol, auth,
# session. Renders nothing and knows no pixels.
#
# Links no client/ header, because there is no client/ header to link: the
# client is a TypeScript SPA and the only thing the two share is this
# protocol's byte layout, checked by conformance vectors rather than by an
# include.
#
# The uWebSockets transport binding needs the vcpkg `ui` feature and is a
# separate card. The protocol itself is dependency-free and builds under every
# preset, which is the point -- the framing, sequencing and channel policy are
# testable with no socket anywhere near them.
add_library(altair_server INTERFACE)
add_library(altair::server ALIAS altair_server)

target_include_directories(altair_server INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(altair_server INTERFACE
    altair_types
    altair_time
)

if(ALTAIR_BUILD_TESTS)
    add_executable(altair_protocol_test tests/test_protocol.cpp)
    target_link_libraries(altair_protocol_test PRIVATE altair_server)
    add_test(NAME protocol COMMAND altair_protocol_test)

    # Regenerates server/tests/vectors/frames.txt, the ONLY thing client/ is
    # allowed to read out of this directory. It is a data file, not a header:
    # the client still links nothing here. Checked in, so client/ can be
    # tested without a C++ build present.
    # P37-01. The first payload the protocol ever carried. protocol.hpp
    # defines the envelope and named "quotes, greeks, book levels" as things
    # the State channel would hold; none of them had a byte layout until now.
    #
    # Its first check is a hand-written hex vector rather than a round trip,
    # because a round trip passes even when both directions are wrong in the
    # same way.
    add_executable(altair_price_payload_test tests/test_price_payload.cpp)
    target_link_libraries(altair_price_payload_test PRIVATE altair_server)
    add_test(NAME price_payload COMMAND altair_price_payload_test)

    # P37-02. The bus, over a real loopback socket.
    #
    # BOOST IS ASKED FOR BY NAME HERE, and that is a correction rather than a
    # preference. broker/ compiles Beast without ever naming Boost: it links
    # OpenSSL::SSL, whose imported target adds the vcpkg include directory,
    # and Boost's headers happen to live in that same directory. It works, and
    # it works for a reason unrelated to Boost -- so a build without OpenSSL,
    # or a vcpkg layout that split the two, would fail with a missing
    # boost/asio header and no clue why. The bus has no use for TLS, so it
    # says what it actually needs.
    find_package(Boost CONFIG QUIET COMPONENTS headers)
    if(Boost_FOUND)
        add_executable(altair_price_bus_test tests/test_price_bus.cpp)
        target_link_libraries(altair_price_bus_test
            PRIVATE altair_server Boost::headers)
        if(WIN32)
            # Boost.Asio auto-links ws2_32 on MSVC via a pragma, but mswsock
            # is needed for the overlapped path and is not always pulled in.
            target_link_libraries(altair_price_bus_test PRIVATE ws2_32 mswsock)
        endif()
        add_test(NAME price_bus COMMAND altair_price_bus_test)
        message(STATUS "altair: price bus ENABLED (Boost ${Boost_VERSION})")
    else()
        message(STATUS "altair: price bus test skipped -- no Boost. It needs "
                       "the `net` preset (build.bat net).")
    endif()

    add_executable(altair_emit_vectors tests/emit_vectors.cpp)
    target_link_libraries(altair_emit_vectors PRIVATE altair_server)
    target_compile_definitions(altair_emit_vectors PRIVATE
        ALTAIR_VECTOR_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors")
    add_test(NAME protocol_vectors COMMAND altair_emit_vectors)
endif()
