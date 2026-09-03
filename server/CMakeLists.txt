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
    add_executable(altair_emit_vectors tests/emit_vectors.cpp)
    target_link_libraries(altair_emit_vectors PRIVATE altair_server)
    target_compile_definitions(altair_emit_vectors PRIVATE
        ALTAIR_VECTOR_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors")
    add_test(NAME protocol_vectors COMMAND altair_emit_vectors)
endif()
