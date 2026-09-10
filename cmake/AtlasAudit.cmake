# cmake/AtlasAudit.cmake -- the Model Atlas may not describe a tree that moved.
#
# P36-02. desktop/atlas_data.hpp names a header for every model it says exists.
# Those paths are the whole value of the page: "GBDT lives in models/gbdt.hpp"
# is the sentence that turns a definition into something you can go and read.
# A renamed or deleted file turns that sentence into a lie that nothing else in
# the build would notice, because the atlas is a table of strings and strings
# compile.
#
# So every path is checked here, at configure time, exactly like the namespace
# and bounds audits. Three rules:
#
#   1. Every path named by a BUILT or PARTIAL row must exist.
#   2. An ABSENT row must name no file. A path beside "ABSENT" is a
#      contradiction -- either the thing exists or it does not.
#   3. Every path must be repo-relative with a forward slash, so the message
#      says something a reader can act on.
#
# VERIFIED BY PLANTING, not by reading. The standing lesson from P33: three
# checks in this repo shipped reporting clean while broken. This one was
# confirmed by adding a row naming a file that does not exist and watching
# configure fail, and by moving a path onto an Absent row and watching it fail
# for the other reason.

set(_atlas_file "${CMAKE_SOURCE_DIR}/desktop/atlas_data.hpp")
if(NOT EXISTS "${_atlas_file}")
    message(FATAL_ERROR "atlas audit: ${_atlas_file} is missing")
endif()

file(READ "${_atlas_file}" _atlas_text)

# Walk the text with FIND/SUBSTRING rather than splitting on newlines.
# `file(STRINGS)` and `string(REPLACE "\n" ";")` both mangle C++ source -- a
# semicolon inside a string literal becomes a list separator and the tail of
# the file silently collapses. P33-06 shipped a bounds audit that reported
# clean for exactly that reason.
set(_pos 0)
string(LENGTH "${_atlas_text}" _len)
set(_rows 0)
set(_built 0)
set(_absent 0)
set(_bad "")

while(_pos LESS _len)
    string(FIND "${_atlas_text}" "AtlasRow{" _at)
    if(_at LESS 0)
        break()
    endif()
    math(EXPR _after "${_at} + 9")
    string(SUBSTRING "${_atlas_text}" ${_after} -1 _atlas_text)
    string(LENGTH "${_atlas_text}" _len)
    set(_pos 0)

    # The row ends at the next "AtlasRow{" or at the end of the array.
    string(FIND "${_atlas_text}" "AtlasRow{" _next)
    if(_next LESS 0)
        set(_row "${_atlas_text}")
    else()
        string(SUBSTRING "${_atlas_text}" 0 ${_next} _row)
    endif()

    math(EXPR _rows "${_rows} + 1")

    # Status: exactly one of the three appears in a row.
    set(_status "")
    if(_row MATCHES "AtlasStatus::Implemented")
        set(_status "BUILT")
        math(EXPR _built "${_built} + 1")
    elseif(_row MATCHES "AtlasStatus::Partial")
        set(_status "PARTIAL")
    elseif(_row MATCHES "AtlasStatus::Absent")
        set(_status "ABSENT")
        math(EXPR _absent "${_absent} + 1")
    else()
        list(APPEND _bad "a row carries no AtlasStatus")
        continue()
    endif()

    # The file path is the quoted string containing a slash and ending .hpp.
    # Anchored on those two things so a description mentioning a filename in
    # prose cannot be mistaken for the path field.
    set(_path "")
    if(_row MATCHES "\"([A-Za-z_0-9]+/[A-Za-z_0-9]+[.]hpp)\"")
        set(_path "${CMAKE_MATCH_1}")
    endif()

    if(_status STREQUAL "ABSENT")
        if(NOT _path STREQUAL "")
            list(APPEND _bad
                 "an ABSENT row names ${_path} -- either it exists or it does not")
        endif()
    else()
        if(_path STREQUAL "")
            list(APPEND _bad "a ${_status} row names no file")
        elseif(NOT EXISTS "${CMAKE_SOURCE_DIR}/${_path}")
            list(APPEND _bad "${_path} does not exist (${_status} row)")
        endif()
    endif()
endwhile()

if(_rows EQUAL 0)
    # A parser that finds nothing must not report success. This is the exact
    # shape of the P33-06 failure: a regex that never matched, reporting clean.
    message(FATAL_ERROR
            "atlas audit: parsed ZERO rows from desktop/atlas_data.hpp. The "
            "file format changed and this check is no longer looking at "
            "anything.")
endif()

list(LENGTH _bad _bad_n)
if(_bad_n GREATER 0)
    string(REPLACE ";" "\n  - " _bad_text "${_bad}")
    message(FATAL_ERROR
            "atlas audit: ${_bad_n} problem(s) in desktop/atlas_data.hpp\n"
            "  - ${_bad_text}")
endif()

message(STATUS
        "altair: atlas audit clean -- ${_rows} models, ${_built} built, "
        "${_absent} absent, every path present")
