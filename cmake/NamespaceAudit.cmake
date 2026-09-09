# cmake/NamespaceAudit.cmake -- one flat namespace, one declaration per name.
#
# P33-02. THE FOURTH COLLISION IS WHY THIS EXISTS.
#
# Everything in the engine lives in `namespace altair`. That is a deliberate
# choice and it is fine right up to the moment two directories independently
# pick the same obvious name for two different things -- at which point every
# translation unit that includes both fails to compile, with an error message
# that names neither the collision nor the second declaration.
#
# It has now happened four times:
#
#   P23-08  kMaxBins        models/gbdt.hpp = 64,  flagging/drift.hpp = 32
#   P32-03  ScoreError      flagging/scorecard.hpp and strategies/score.hpp
#   P33-01  ScorecardError  the rename above, and a NEW header of mine
#   (and Qt's `signals` macro, which is the same failure from outside)
#
# Every one of them sat there for months, because nothing had reason to
# include both headers, and every one was found by a UI page putting two
# subsystems on the same screen. That is a terrible detector: it finds the
# collision at the moment somebody is trying to build a feature, and it finds
# it as a wall of errors pointing at a file that is not the problem.
#
# WHY CMAKE AND NOT A SCRIPT.
#
# A Python or shell scanner would be shorter. It would also add a build-time
# dependency to a project whose whole point is that it configures with nothing
# but a compiler and CMake, and CLAUDE.md is explicit that there is no Python
# in this system. `file(STRINGS ... REGEX)` does the job.
#
# WHAT IT DOES NOT CATCH, STATED SO NOBODY TRUSTS IT TOO FAR.
#
#   * Anything not declared at column zero. Member types nested inside a class
#     are scoped and safe, so this is correct, but it means a namespace-scope
#     declaration written with leading whitespace is invisible here.
#   * Functions. Overloads are legal and usually intended, so scanning them
#     would be almost all false positives.
#   * Two declarations in the SAME file. That is a compiler error already.
#   * Macros, which are not namespaced at all -- Qt's `signals` is the example
#     and QT_NO_KEYWORDS is the fix for it.
#
# It catches types, enums and namespace-scope constants, which is where all
# four real collisions have been.

function(altair_audit_namespace)
    set(_dirs core instruments broker feed book analytics risk oms features
              strategies backtest models flagging server)
    set(_all_names "")

    foreach(_dir IN LISTS _dirs)
        file(GLOB_RECURSE _headers "${CMAKE_SOURCE_DIR}/${_dir}/*.hpp")
        foreach(_h IN LISTS _headers)
            # tests/ and tools/ are excluded: a test may legitimately declare a
            # local helper with an obvious name, and no two tests are ever in
            # one translation unit.
            if(_h MATCHES "/tests/" OR _h MATCHES "/tools/")
                continue()
            endif()

            # Column zero only -- see the note above.
            file(STRINGS "${_h}" _decls
                 REGEX "^(enum class|struct|class) [A-Z][A-Za-z0-9_]*")
            file(STRINGS "${_h}" _consts
                 REGEX "^inline constexpr [^ ]+ k[A-Za-z0-9_]* *=")

            foreach(_line IN LISTS _decls _consts)
                set(_is_type FALSE)
                if(_line MATCHES "^(enum class|struct|class) ([A-Z][A-Za-z0-9_]*)")
                    set(_name "${CMAKE_MATCH_2}")
                    set(_is_type TRUE)
                elseif(_line MATCHES "^inline constexpr [^ ]+ (k[A-Za-z0-9_]*)")
                    set(_name "${CMAKE_MATCH_1}")
                else()
                    continue()
                endif()

                # A forward declaration is not a definition and may legitimately
                # repeat. `struct Foo;` ends in a semicolon and nothing else.
                # ONLY FOR TYPES. The first version applied this to
                # constants too, and every constant declaration ends in a
                # semicolon -- so it skipped all of them, and missed
                # altair::kMaxSectors sitting in two headers of one directory.
                # AND TWO BUGS LIVED IN THIS GUARD, BOTH FOUND BY
                # PROBING IT RATHER THAN BY READING IT.
                #
                # It first read "ends in a semicolon", applied to every
                # line. Every constant declaration ends in a semicolon,
                # so it skipped all of them and missed altair::kMaxSectors
                # sitting in two headers of the same directory.
                #
                # Restricted to types, it still read "ends in a
                # semicolon" -- so a one-line definition,
                # `struct Foo { int x; };`, looked exactly like a forward
                # declaration. A deliberate collision planted in
                # analytics/vix.hpp to test this check went undetected,
                # which is the one failure a check must not have:
                # reporting clean while a collision sits in the tree.
                #
                # The semicolon now has to follow the NAME.
                if(_is_type AND _line MATCHES
                   "^(enum class|struct|class) [A-Z][A-Za-z0-9_]*[ \t]*;")
                    continue()
                endif()

                file(RELATIVE_PATH _rel "${CMAKE_SOURCE_DIR}" "${_h}")
                if(DEFINED _altair_decl_${_name})
                    # Same file seen twice through two globs is not a clash.
                    if(NOT "${_altair_decl_${_name}}" STREQUAL "${_rel}")
                        list(APPEND _clashes
                             "  altair::${_name}\n      ${_altair_decl_${_name}}\n      ${_rel}")
                    endif()
                else()
                    set(_altair_decl_${_name} "${_rel}")
                    list(APPEND _all_names "${_name}")
                endif()
            endforeach()
        endforeach()
    endforeach()

    list(LENGTH _all_names _n)
    if(_clashes)
        list(REMOVE_DUPLICATES _clashes)
        list(LENGTH _clashes _c)
        string(REPLACE ";" "\n" _clash_text "${_clashes}")
        message(FATAL_ERROR
            "NAMESPACE COLLISION: ${_c} name(s) are declared at namespace "
            "scope in more than one engine header.\n\n"
            "${_clash_text}\n\n"
            "Everything here is in `namespace altair`, so two declarations of "
            "one name are an ODR violation: every translation unit that "
            "includes both fails to compile, with an error naming neither "
            "file. This has happened four times in this tree and every time "
            "it was found months later by a UI page that happened to include "
            "both.\n\n"
            "THE RULE THAT RESOLVES IT: the MORE SPECIFIC user takes the "
            "qualified name. strategies/score.hpp scores signals in general, "
            "so flagging/scorecard.hpp's error enum became ScorecardError.\n\n"
            "Rename one of them. Do not add an exception to this check.")
    endif()
    message(STATUS "altair: namespace audit clean -- ${_n} names, no clashes")
endfunction()
